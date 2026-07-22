#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHashingWriteBuffer.h>
#include <Disks/WriteMode.h>
#include <Core/Defines.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DB
{

/// Owns the metadata-side overlay for one object-storage transaction. Part files are accumulated by
/// routed namespace/ref, then published as manifest trees and refs by `commit`; verbatim namespace
/// and mountpoint files are written immediately when their buffers finalize. A part uses one
/// `Cas::PartWriteTxn`, created lazily before the first staged dependency, so the durable manifest
/// edge is established before the pool observes or uploads a new blob.
///
/// Content blobs are first represented by local or S3 staging objects and are uploaded only during
/// publication. Inline entries remain in the manifest tree when they fit `INLINE_CAP`. The
/// destructor removes private local staging, leaves aborted S3 staging for the mount-lease sweeper,
/// and abandons any open part builds; it never publishes an uncommitted ref.
class ContentAddressedTransaction : public IMetadataTransaction
{
public:
    /// Borrows the metadata storage for the transaction lifetime; staged builds and resources are
    /// owned by this transaction and finalized or abandoned by `commit`/destruction.
    explicit ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_);

    bool supportsChmod() const override { return false; }

    /// Publishes every staged part. Publication is ordered so each new blob is named by a durable
    /// precommit edge first; if a later part fails, only refs created by this call are compensated.
    void commit(const TransactionCommitOptionsVariant & options) override;

    /// Accepts only `NoCommitOptions`, delegates to `commit`, and returns its successful outcome.
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    /// Unsupported because content-addressed keys are generated from payload hashes, not paths.
    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    /// Returns no removal list: shared CAS objects are reclaimed only by garbage collection.
    StoredObjects getSubmittedForRemovalBlobs() override;

    /// Resolves a staged entry to its committed-style storage description. Pending blobs return no
    /// object because their bytes still reside in staging and must be read through the overlay path.
    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    /// Opens an inline entry, pending local/S3 staging object, or already uploaded blob for a
    /// read-your-writes operation; returns null when the path is outside this transaction's overlay.
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    /// Returns the logical size of a staged entry, including inline bytes and pending blob payloads.
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;
    /// Reports only inner staged directories. The bare part directory intentionally remains absent
    /// so cleanup of a temporary, dedup-rejected part does not treat it as a real directory.
    bool hasInFlightDirectory(const std::string & path) const override;
    /// Lists immediate child names visible in the staged directory overlay.
    std::vector<std::string> listInFlightDirectory(const std::string & path) const override;

    /// This legacy object-list operation has no content-addressed equivalent and throws.
    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    /// Creates the buffer used by the disk transaction for this path. The returned wrapper keeps
    /// `owner` alive until deferred finalization, because its callback captures this transaction;
    /// it also applies the content-addressed append and autocommit rules before selecting a blob,
    /// inline, or verbatim-file buffer. Part blobs cannot be published independently of their
    /// manifest, while an inline-eligible standalone part file may commit through the repoint path.
    std::unique_ptr<WriteBufferFromFileBase> tryCreateWriteBuffer(
        const std::shared_ptr<IDiskTransaction> & owner,
        const std::string & path, size_t buf_size, WriteMode mode,
        const WriteSettings & settings, bool autocommit) override;

    /// Creates the inner buffer for a content-addressed path. Part blobs are hashed while being
    /// staged, small metadata files are accumulated in memory and classified at finalize, and
    /// verbatim namespace or mountpoint files are read-modify-written when append is requested.
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    /// Directory creation is a metadata no-op because object storage has no directory objects.
    void createDirectory(const std::string & path) override;
    /// Recursive directory creation is likewise a no-op; entries create their own prefixes.
    void createDirectoryRecursive(const std::string & path) override;
    /// Drops a part ref, or does nothing for a non-part directory. A whole-part drop supersedes any
    /// per-file removal marks staged earlier in this transaction.
    void removeDirectory(const std::string & path) override;
    /// Removes refs, namespace files, and shadow objects while leaving shared CAS objects to GC.
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;
    /// Copies an entry between parts, preserving pending staging ownership and tokenless evidence.
    void createHardLink(const std::string & path_from, const std::string & path_to) override;
    /// These filesystem metadata operations are unsupported because CAS metadata is immutable and
    /// object storage exposes neither POSIX timestamps nor mode bits.
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;
    /// Re-keys or merges staged part entries without publishing early; non-part paths use the
    /// corresponding object-storage copy/remove semantics.
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;
    /// Moves a staged entry and its pending ownership, or copies/removes a verbatim object as needed.
    void moveFile(const std::string & path_from, const std::string & path_to) override;
    /// Replaces the destination while preserving the source's content-addressed ownership rules.
    void replaceFile(const std::string & path_from, const std::string & path_to) override;
    /// Unlinks a staged entry or records a removal mark for an already committed manifest; shared
    /// blobs are never deleted directly.
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;
    /// Truncation is not representable without rewriting the content and is unsupported.
    void truncateFile(const std::string & path, size_t size) override;

    /// Abandons open builds and cleans owned staging without publishing an uncommitted ref.
    ~ContentAddressedTransaction() override;

protected:
    ContentAddressedMetadataStorage & metadata_storage;

private:
    /// State for one routed part. The build is opened lazily at the first staged write so every
    /// manifest dependency is recorded by the same `Cas::PartWriteTxn`; pending blobs remain in
    /// staging until publication.
    struct PartStaging
    {
        /// Created lazily when a staged blob, inline entry, or adopted entry first needs publication.
        Cas::PartWriteTxnPtr build;
        std::vector<Cas::ManifestEntry> entries;   /// staged manifest entries (uploads + adoptions)
        /// Paths removed from a committed part's manifest by this transaction. Publication carries
        /// forward all other committed entries. If `removeDirectory` drops the whole ref in the same
        /// transaction, that ref-drop supersedes these marks and clears them.
        std::set<std::string> content_removed;
        bool published = false;                    /// set by publishStaging during commit(); the commit
                                                   /// loop is idempotent (never re-publishes a staging).

        /// Assigned once, the first time this (ns, ref) key is ever touched (`touchPart`), from a
        /// per-transaction monotone counter. `commit()` processes parts in ASCENDING `part_seq` order
        /// (the order this transaction first touched each key) rather than the incidental sort order
        /// of the `parts` map's (ns, ref) string key -- load-bearing whenever one part's publish must
        /// be observable (e.g. to a test hook modeling a concurrent writer, or a future dependent part)
        /// before a LATER-staged part's publish runs.
        uint64_t part_seq = 0;

        /// `staging_key` is either a private local temp path or an S3 staging object key returned by
        /// `CaContentWriteBuffer` at finalize. The backend selects cleanup and read-your-writes:
        /// local files are removed by this transaction, whereas aborted S3 objects must remain
        /// available to the mount-lease sweeper and to recovery of the promote source.
        struct PendingBlob { Cas::BlobRef ref; std::string staging_key; uint64_t size = 0; Cas::StagingBackend backend = Cas::StagingBackend::Local; };
        std::vector<PendingBlob> pending_blobs;    /// Staged blobs uploaded after the manifest edge is precommitted.
    };

    /// Keyed by (namespace string, ref name) — the routed identity, so live/detached/shadow
    /// stagings never collide.
    std::map<std::pair<std::string, std::string>, PartStaging> parts;
    /// Feeds `PartStaging::part_seq`; see its comment. Monotone for the lifetime of one transaction.
    uint64_t next_part_seq = 0;
    bool committed = false;
    bool failed = false;

    /// Memoizes, per (this transaction, ref), whether `unlinkFile` has already re-proven a committed
    /// ref's manifest body `ForceFresh`. The MergeTree fast-removal path unlinks every file of a part
    /// through ONE transaction right before `removeDirectory`: the first unlink's `ForceFresh` view
    /// proves the body once; the rest of the burst reuse that proof (`Cas::Freshness::CachedForLoad`)
    /// instead of paying one manifest-body HEAD per file. Cleared in `commit()`'s epilogue.
    std::unordered_set<String> force_fresh_validated_refs;

    /// Stage a CONTENT part file as a blob: record the pending upload + a tokenless dependency
    /// and add/replace its manifest entry. Shared by the streaming-blob path
    /// (Local or S3-staging, `backend` says which) and the always-Local inline-cap fallback.
    void stageBlobPartFile(const ContentAddressedMetadataStorage::Route & route,
                           const Cas::BlobRef & ref, size_t size, const std::string & staging_key,
                           Cas::StagingBackend backend);

    /// Builds the fixed-length CABL envelope header for a staging blob, with a fresh `incarnation_tag`,
    /// so the S3 staging object holds `[header][payload]` and the promote
    /// stays a verbatim server-side copy. `build_id` is left 0 (not known at stream time; diagnostic-only).
    std::string buildS3StagingBlobHeader(const ContentAddressedMetadataStorage::Route & route) const;

    /// Returns (and, when necessary, creates) the staging state for a routed namespace/ref.
    PartStaging & stagingFor(const ContentAddressedMetadataStorage::Route & r);
    /// Returns the staging state for `key`, creating it and stamping `part_seq` from the monotone
    /// counter on FIRST touch only. The single insertion point for `parts` so `part_seq` is assigned
    /// exactly once per key regardless of which call site (a routed write via `stagingFor`, or
    /// `moveDirectory`'s re-key onto a fresh destination key) first creates it.
    PartStaging & touchPart(const std::pair<std::string, std::string> & key);
    /// Finds existing staging state without creating an entry; returns nullptr when untouched.
    PartStaging * findStaging(const ContentAddressedMetadataStorage::Route & r);
    /// Finds the staged manifest entry for a routed file without consulting committed storage.
    const Cas::ManifestEntry * findStagedEntry(const ContentAddressedMetadataStorage::Route & r) const;
    /// Returns the pending (staged but not yet uploaded) blob for `ref`, or nullptr when it has
    /// already been uploaded or was never staged.
    const PartStaging::PendingBlob * findPendingBlob(const PartStaging & st, const Cas::BlobRef & ref) const;
    /// Returns the part build, creating it lazily with the routed ref as its intended destination.
    Cas::PartWriteTxn & buildFor(const ContentAddressedMetadataStorage::Route & r, PartStaging & st);
    /// Parses a disk path and maps a part-file path to its namespace/ref/file route.
    std::optional<ContentAddressedMetadataStorage::Route> routeOf(const std::string & path) const;

    /// Removes local staging files after commit or abort. On a successful commit it also removes
    /// S3 staging objects; aborted S3 objects are intentionally retained for lease-scoped cleanup.
    void cleanupPendingTempFiles() noexcept;

    /// Uploads through `st.build` only the pending blobs still referenced by `st.entries`. An entry
    /// removed by `unlinkFile` or `replaceFile` is skipped, but its staging resource is still cleaned
    /// by `cleanupPendingTempFiles`. Used for both new refs and committed-ref repoints.
    void uploadPendingBlobs(PartStaging & st);

    /// Adopts a manifest entry into another part while preserving its storage state. For a pending
    /// blob, `copy_pending` controls whether the staging record is copied (hardlink semantics) or
    /// has already been moved by the caller; either way the destination records a dependency. For
    /// an uploaded or committed blob, the destination records tokenless evidence and does not
    /// perform a pool read before precommit.
    ///
    /// `pb != nullptr` (pending, not yet uploaded):
    ///   - `copy_pending=true`  → push a copy of *pb into dst_st.pending_blobs (hardlink semantics:
    ///     both src and dst upload independently; src's copy is left in place by the caller).
    ///   - `copy_pending=false` → the pb record is already in dst_st (moved or already there);
    ///     just record the dep without any additional push.
    ///   In both cases: dst_build.recordPendingBlobDep(entry.file_hash, entry.file_size).
    ///
    void adoptStagedBlob(const PartStaging::PendingBlob * pb, const Cas::ManifestEntry & entry,
                         PartStaging & dst_st, Cas::PartWriteTxn & dst_build, bool copy_pending);

    /// Publishes one staged part, either by promoting a newly staged manifest or by repointing an
    /// existing ref after carrying its unchanged entries forward. It is idempotent within the commit
    /// loop and marks the staging as published. Writes the exact `Cas::CommitOutcome` into `out_slot`
    /// the INSTANT `promoteBuild`/`repointRef` confirms -- before any further throwable work (the
    /// scratch-build abandon, or a test hook) -- so `commit` can roll back precisely with
    /// `dropRefIfMatches` even when this call later throws. `out_slot` is left `std::nullopt` when this
    /// staging had nothing to publish or was already published earlier in this commit loop; `commit`
    /// preallocates one slot per part so this write is index-addressed and never grows a container
    /// (load-bearing once Task 5 calls this from multiple threads).
    void publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st,
                        std::optional<Cas::CommitOutcome> & out_slot);
};

}

namespace DB::Cas
{

/// Part files that must NOT be inlined into the tree: per-column data (`.bin`) and marks (`.mrk*`/
/// `.cmrk*`) — inlining them would force a full-part fetch and destroy column-read selectivity — plus
/// `primary.idx`, which can be large (a size-threshold inlining of small primary.idx is a follow-up).
/// Everything else (the small eager metadata files) is an inline candidate, subject to INLINE_CAP.
bool partFileMustStayBlob(std::string_view file_name);

/// Writes a CONTENT part file while computing its content hash. The blob key is only known
/// once all bytes are written, so the buffer spills to a unique local temp file while hashing with
/// `hash_algo` (`Cas::makeBlobHashingWriteBuffer` — the pool's selectable blob-hash
/// function the wiring defines; the core never re-hashes payloads). `CityHash128` stays the thin
/// `HashingWriteBuffer` adapter (byte-for-byte unchanged); `XXH3_128` hashes with xxh3 instead. On
/// finalize it hands (hash_hex, size, temp_path)
/// to the owning transaction; the transaction owns the staging resource after finalize and uploads it
/// post-precommit, so finalizeImpl no longer removes it. cancelImpl and the destructor (on error
/// paths) still remove it.
///
/// In S3 staging mode:
/// a SECOND constructor streams directly to an already-opened object-store sink (an S3 staging
/// object) while hashing, instead of spilling to a local temp file — see its own doc comment below.
/// The local-temp-file constructor above is UNCHANGED byte-for-byte; this is an independent mode
/// selected only by which constructor the caller uses.
class CaContentWriteBuffer : public WriteBufferFromFileBase
{
public:
    using OnFinalized = std::function<void(const std::string & hash_hex, size_t size, const std::string & temp_path)>;

    /// Local-staging mode (today's default; BYTE-FOR-BYTE unchanged behavior). Buffer sizing mirrors
    /// the plain object-storage backends: with adaptive sizing on, the working buffer STARTS small
    /// and grows (what min_columns_to_activate_adaptive_write_buffer toggles — a wide part keeps its
    /// per-INSERT footprint small).
    CaContentWriteBuffer(
        std::string temp_dir,
        Cas::BlobHashAlgo hash_algo,
        size_t buf_size,
        bool use_adaptive_buffer_size,
        size_t adaptive_buffer_initial_size,
        OnFinalized on_finalized_);

    /// S3-native staging mode: `object_store_sink` is an ALREADY-OPENED write buffer over the staging
    /// object at `object_key` (e.g. `object_storage->writeObject(StoredObject(object_key), ...)`).
    ///
    /// `envelope_header` is the fixed-length (`blob_header_len`) CABL envelope header the transaction
    /// built for this staging blob. It is written to the sink FIRST —
    /// UNHASHED and NOT counted in the reported size — so the staging object holds `[header][payload]`
    /// and the promote can stay a verbatim server-side copy. Excluding the header from the hash is
    /// CRITICAL: the content key must be the pool's selected hash of `payload` alone (else the random
    /// `incarnation_tag` in the header would make every blob's key unique ⇒ zero dedup), and the reported
    /// blob size must be the
    /// PAYLOAD size (else the manifest `blob_size` would be payload+`blob_header_len`). Only the PAYLOAD
    /// bytes written through THIS buffer flow through `hashing` and `count()`.
    ///
    /// Bytes are hashed while streaming into `object_store_sink`; on finalize `on_finalized` receives
    /// `object_key` as its third argument (in place of a local temp path) and `getFileName()` returns
    /// it too. `cancelImpl` only cancels `object_store_sink` — it never attempts to delete the
    /// (possibly partially-written) staging object; reclaiming an orphaned staging object after a
    /// cancelled write belongs to the mount-lease sweeper, not this buffer.
    CaContentWriteBuffer(
        std::unique_ptr<WriteBufferFromFileBase> object_store_sink,
        std::string object_key,
        std::string envelope_header,
        Cas::BlobHashAlgo hash_algo,
        size_t buf_size,
        bool use_adaptive_buffer_size,
        size_t adaptive_buffer_initial_size,
        OnFinalized on_finalized_);

    ~CaContentWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    /// Feeds bytes to the hashing/staging sink while preserving the base write-buffer contract.
    void nextImpl() override;
    /// Finalizes the sink, computes the content hash, and transfers the staging resource through
    /// `on_finalized`; after that callback succeeds, the transaction owns cleanup.
    void finalizeImpl() override;
    /// Cancels the sink and removes local staging. S3 staging is left for lease-scoped reclamation.
    void cancelImpl() noexcept override;
    /// Removes the local staging path when ownership has not been transferred to the transaction.
    void removeTempFile() noexcept;

    OnFinalized on_finalized;
    /// Local mode: the local temp file path (removed by removeTempFile). S3 mode: the staging
    /// object's key (never fs::remove'd — see is_s3_staging below).
    std::string temp_path;
    /// Selects the S3-staging semantics in cancelImpl/the destructor (skip local-file cleanup,
    /// since `temp_path` is a remote key, not a path on this filesystem). false (the default,
    /// local-temp-file constructor) is the pre-existing, byte-for-byte-unchanged behavior.
    bool is_s3_staging = false;
    /// The spill sink: a local WriteBufferFromFile (Local mode) or the caller-supplied object-store
    /// sink (S3 mode). Either way it is a SECOND per-stream buffer wrapped by `hashing` below.
    std::unique_ptr<WriteBufferFromFileBase> sink;
    /// Built via `Cas::makeBlobHashingWriteBuffer(hash_algo, *sink)`:
    /// `CityHash128` is a thin adapter over the pre-existing `HashingWriteBuffer` convention (byte-for-byte
    /// unchanged); `XXH3_128` hashes with the pool's selected algo instead.
    std::unique_ptr<Cas::IBlobHashingWriteBuffer> hashing;
    bool temp_ownership_transferred = false;   /// Set after successful `on_finalized`; the destructor skips local cleanup.
};

/// Write buffer for bytes that live INSIDE pool metadata (a small inline part file staged into the
/// manifest tree, or a verbatim namespace file PUT on finalize). Accumulates in memory (the bytes are
/// tiny) and hands them to the callback at finalize; the callback decides where they go and whether
/// they are durable immediately (verbatim) or at commit (staged into the part's manifest entries).
class CaInlineWriteBuffer : public WriteBufferFromFileBase
{
public:
    using OnInlined = std::function<void(std::string bytes)>;

    explicit CaInlineWriteBuffer(OnInlined on_inlined_);
    ~CaInlineWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    /// Appends bytes to the in-memory payload under the base write-buffer contract.
    void nextImpl() override;
    /// Hands the complete inline payload to the callback; the callback decides whether it is staged
    /// in a manifest or written immediately as a verbatim file.
    void finalizeImpl() override;

    OnInlined on_inlined;
    std::string accumulated;
};

}
