#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHasher.h>
#include <Disks/WriteMode.h>
#include <Core/Defines.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DB
{

namespace ContentAddressed
{

/// Part files that must NOT be inlined into the tree: per-column data (`.bin`) and marks (`.mrk*`/
/// `.cmrk*`) — inlining them would force a full-part fetch and destroy column-read selectivity — plus
/// `primary.idx`, which can be large (a size-threshold inlining of small primary.idx is a follow-up).
/// Everything else (the small eager metadata files) is an inline candidate, subject to INLINE_CAP.
bool partFileMustStayBlob(std::string_view file_name);

}

/// The M-W write-path wiring: accumulates ClickHouse operations and maps them to ONE Cas::PartWriteTxn
/// per written part at commit (design 2026-06-11 section 4; plan D-W2).
///
/// T3 state: writeFile (content blobs through a spill+hash buffer into PartWriteTxn::putBlob; mutable
/// per-part bytes staged; verbatim namespace files durable on finalize) + commit (one
/// putTree+publish per staged part, with the typed `published_at_ms` stamp) + destructor
/// abandon. Remaining operations land task by task (T5 carry-forward/renames, T6 removals, T7
/// detached/ATTACH/FREEZE, T8 read-your-writes) — each with wiring tests; the SQL suites gate the
/// completed milestone (T13).
class ContentAddressedTransaction : public IMetadataTransaction
{
public:
    explicit ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_);

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;
    bool hasInFlightDirectory(const std::string & path) const override;
    std::vector<std::string> listInFlightDirectory(const std::string & path) const override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    /// [TXN-ONE-PIPELINE] The generic write-buffer hook (`DiskObjectStorageTransaction::writeFileImpl`
    /// calls this): CA owns its write because a blob key is a content hash, known only after the last
    /// byte. Returns the fully-wrapped buffer (hash-on-write + append RMW + inline/blob split +
    /// autocommit/lifetime pin via `owner`).
    std::unique_ptr<WriteBufferFromFileBase> tryCreateWriteBuffer(
        const std::shared_ptr<IDiskTransaction> & owner,
        const std::string & path, size_t buf_size, WriteMode mode,
        const WriteSettings & settings, bool autocommit) override;

    /// The inner content-addressed write buffer (hash-on-write / inline). `tryCreateWriteBuffer` wraps it.
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    void createDirectory(const std::string & path) override;
    void createDirectoryRecursive(const std::string & path) override;
    void removeDirectory(const std::string & path) override;
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;
    void createHardLink(const std::string & path_from, const std::string & path_to) override;
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;
    void moveFile(const std::string & path_from, const std::string & path_to) override;
    void replaceFile(const std::string & path_from, const std::string & path_to) override;
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;
    void truncateFile(const std::string & path, size_t size) override;

    ~ContentAddressedTransaction() override;

protected:
    ContentAddressedMetadataStorage & metadata_storage;

private:
    /// One part being assembled by this transaction (D-W2: ONE Cas::PartWriteTxn per written part,
    /// opened lazily at the FIRST staged write — W-ANCHOR requires the watermark durable
    /// before the first PUT, and buffer finalize uploads immediately).
    struct PartStaging
    {
        Cas::PartWriteTxnPtr build;                       /// nullptr until the first content upload
        std::vector<Cas::ManifestEntry> entries;   /// staged manifest entries (uploads + adoptions)
        std::set<std::string> content_removed;     /// all-tree-part-files Task 8 (B123 evolution, spec
                                                   /// 2026-07-14-cas-all-tree-part-files-design.md §6):
                                                   /// staged removal marks for a COMMITTED part's TREE
                                                   /// entries. Resolved at publish (publishStaging): a
                                                   /// repoint carries the committed manifest forward
                                                   /// minus these paths, UNLESS the same transaction also
                                                   /// drops the whole part (removeDirectory), in which
                                                   /// case the marks are superseded (see removeDirectory).
        bool published = false;                    /// set by publishStaging during commit(); the commit
                                                   /// loop is idempotent (never re-publishes a staging).

        /// `staging_key`: the local temp path (`StagingBackend::Local`) or the S3 staging object key
        /// (`StagingBackend::S3`, plan `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md`
        /// Task 4) that `CaContentWriteBuffer::on_finalized` handed back. `backend` selects which:
        /// `cleanupPendingTempFiles` only `fs::remove`s `Local` entries — an `S3` staging object is
        /// reclaimed by the promote path or the mount-lease sweeper (a later task), never here.
        struct PendingBlob { Cas::BlobRef ref; std::string staging_key; uint64_t size = 0; StagingBackend backend = StagingBackend::Local; };
        std::vector<PendingBlob> pending_blobs;    /// B188: spilled+hashed locally; uploaded post-precommit
    };

    /// Keyed by (namespace string, ref name) — the routed identity, so live/detached/shadow
    /// stagings never collide.
    std::map<std::pair<std::string, std::string>, PartStaging> parts;
    bool committed = false;

    /// Stage a CONTENT part file as a blob: record the pending upload + a tokenless dependency
    /// and add/replace its manifest entry. Shared by the streaming-blob path
    /// (Local or S3-staging, `backend` says which) and the always-Local inline-cap fallback.
    void stageBlobPartFile(const ContentAddressedMetadataStorage::Route & route,
                           const Cas::BlobRef & ref, size_t size, const std::string & staging_key,
                           StagingBackend backend);

    /// S3-native staging fix 2026-07-11: build the fixed-length CABL envelope header for a staging blob,
    /// with a FRESH `incarnation_tag`, so the S3 staging object holds `[header][payload]` and the promote
    /// stays a verbatim server-side copy. `build_id` is left 0 (not known at stream time; diagnostic-only).
    std::string buildS3StagingBlobHeader(const ContentAddressedMetadataStorage::Route & route) const;

    PartStaging & stagingFor(const ContentAddressedMetadataStorage::Route & r);
    PartStaging * findStaging(const ContentAddressedMetadataStorage::Route & r);
    const Cas::ManifestEntry * findStagedEntry(const ContentAddressedMetadataStorage::Route & r) const;
    /// B188: return a pointer to the pending (staged-but-not-yet-uploaded) blob for `hash`, or nullptr
    /// if not pending (already uploaded or never staged).
    const PartStaging::PendingBlob * findPendingBlob(const PartStaging & st, const Cas::BlobRef & ref) const;
    Cas::PartWriteTxn & buildFor(const ContentAddressedMetadataStorage::Route & r, PartStaging & st);
    std::optional<ContentAddressedMetadataStorage::Route> routeOf(const std::string & path) const;

    void cleanupPendingTempFiles() noexcept;   /// B188: remove all parts' pending temp files (commit/abort/dtor)

    /// B189/Task 4: upload every pending blob of `st` whose hash is still referenced by `st.entries`
    /// (an orphaned pending blob -- its entry removed by unlinkFile/replaceFile -- is skipped; its
    /// temp file is still cleaned by cleanupPendingTempFiles at commit end) through `st.build`.
    /// Shared by `publishStaging`'s normal path and its committed-ref repoint branch.
    void uploadPendingBlobs(PartStaging & st);

    /// B190 Task 4: unified adopt helper that collapses the 6 inline pending/uploaded dispatch
    /// blocks in createHardLink / moveFile (cross-part) / moveDirectory (two-build merge).
    ///
    /// `pb != nullptr` (pending, not yet uploaded):
    ///   - `copy_pending=true`  → push a copy of *pb into dst_st.pending_blobs (hardlink semantics:
    ///     both src and dst upload independently; src's copy is left in place by the caller).
    ///   - `copy_pending=false` → the pb record is already in dst_st (moved or already there);
    ///     just record the dep without any additional push.
    ///   In both cases: dst_build.recordPendingBlobDep(entry.file_hash, entry.file_size).
    ///
    /// `pb == nullptr` (uploaded / committed): dst_build.adoptEvidence(entry) — tokenless W-EVIDENCE,
    /// no pool HEAD/GET before precommit.
    void adoptStagedBlob(const PartStaging::PendingBlob * pb, const Cas::ManifestEntry & entry,
                         PartStaging & dst_st, Cas::PartWriteTxn & dst_build, bool copy_pending);

    /// Publish one staged part durably (putTree + publish, or a repoint for a standalone write/remove
    /// on an already-committed part) and mark it `published`. Idempotent: a no-op if already
    /// published. Returns true iff this call newly CREATED a ref that did not exist before (for
    /// commit()'s rollback tracking).
    bool publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st);
};

}

// ===== Merged from ContentAddressedWriteBuffers.h (merge #2) =====
namespace DB::ContentAddressed
{

/// Write buffer for a CONTENT part file (M-W T3). The blob key is the content hash, only known
/// once all bytes are written, so the buffer spills to a unique local temp file while hashing with
/// `hash_algo` (P1-T3a, `Cas::makeBlobHashingWriteBuffer` — the pool-wide selectable blob-hash
/// function the wiring defines; the core never re-hashes payloads). `CityHash128` stays the thin
/// `HashingWriteBuffer` adapter (byte-for-byte unchanged); `XXH3_128` hashes with xxh3 instead. On
/// finalize it hands (hash_hex, size, temp_path)
/// to the owning transaction; B188: the transaction owns the temp file post-finalize and uploads it
/// post-precommit, so finalizeImpl no longer removes it. cancelImpl and the destructor (on error
/// paths) still remove it.
///
/// S3-native staging (Task 4, plan `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md`):
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
    /// built for this staging blob (S3-native staging fix 2026-07-11). It is written to the sink FIRST —
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
    /// cancelled write is the mount-lease sweeper's job (a later task), not this buffer's.
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
    void nextImpl() override;
    void finalizeImpl() override;
    void cancelImpl() noexcept override;
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
    /// Built via `Cas::makeBlobHashingWriteBuffer(hash_algo, *sink)` (P1-T3a, pluggable blob hash):
    /// `CityHash128` is a thin adapter over the pre-existing `HashingWriteBuffer` convention (byte-for-byte
    /// unchanged); `XXH3_128` hashes with the pool's selected algo instead.
    std::unique_ptr<Cas::IHashingWriteBuffer> hashing;
    bool temp_ownership_transferred = false;   /// B188: set after on_finalized; the dtor skips removeTempFile
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
    void nextImpl() override;
    void finalizeImpl() override;

    OnInlined on_inlined;
    std::string accumulated;
};

}
