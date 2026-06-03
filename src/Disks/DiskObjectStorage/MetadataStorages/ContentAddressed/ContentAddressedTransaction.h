#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/WriteMode.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace DB
{

namespace ContentAddressed
{

/// Write buffer for the content-addressed disk (a write-path impl detail used only by
/// ContentAddressedTransaction, so it lives here).
///
/// The normal object-storage write path picks the remote object key up front and streams
/// straight to remote. Content addressing cannot do that: the key is the content hash, which
/// is only known once all bytes have been written, and object storage has no rename.
///
/// So this buffer spills incoming bytes to a unique local temp file while accumulating the
/// same `cityHash128` ClickHouse uses for `checksums.txt` file hashes. On finalize it derives
/// the key `blobs/<hash>` and uploads the temp file there exactly once, using put-if-absent:
/// if an object with that key already exists the upload is skipped and the existing object is
/// reused (identical content deduplicates to the same blob).
class ContentAddressedWriteBuffer : public WriteBufferFromFileBase
{
public:
    /// Invoked from finalizeImpl once the content hash is known and the blob has been uploaded
    /// (or found already present). Lets the owning transaction record (logical_file -> blob). The
    /// hash is handed over as a typed BlobHash so the transaction cannot confuse it with an object key.
    using OnFinalized = std::function<void(const BlobHash & blob_hash, size_t size)>;

    /// key_prefix_ is the object-storage common key prefix to prepend to the blob key; an empty
    /// prefix yields the bare blobs/<hash> key. It is threaded from the owning transaction so the
    /// blob is uploaded exactly where the read side resolves it.
    ContentAddressedWriteBuffer(ObjectStoragePtr object_storage_, std::string key_prefix_, std::string temp_dir_, OnFinalized on_finalized_ = {});
    ~ContentAddressedWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

    /// Valid after finalize: lowercase hex of the cityHash128 of the written content.
    const std::string & getBlobHash() const { return blob_hash; }
    /// Valid after finalize: number of bytes written.
    size_t getSize() const { return size; }

private:
    void nextImpl() override;
    void finalizeImpl() override;
    void removeTempFile() noexcept;

    /// Publish the spilled temp file to the final content-hash blob key so a concurrent reader/writer
    /// never observes a partially-written object (B41). For the local backend (in-place writes) this
    /// uploads to a unique temp object key in the same directory and atomically renames it onto the
    /// final key; for an atomic-PUT backend (S3/Azure) it uploads the final key in one shot.
    void uploadBlobAtomically(const std::string & key);

    ObjectStoragePtr object_storage;
    std::string key_prefix;
    std::string temp_path;
    OnFinalized on_finalized;

    std::unique_ptr<WriteBufferFromFile> temp_file;
    std::unique_ptr<HashingWriteBuffer> hashing;

    std::string blob_hash;
    size_t size = 0;
};

/// Write buffer for a MUTABLE per-part file (uuid.txt / txn_version.txt / metadata_version.txt).
///
/// Unlike a content file, a mutable file must NOT be content-addressed: two parts with identical
/// content share one manifest, but each keeps its own mutable bytes. So this buffer does NOT upload a
/// blob — it accumulates the bytes in memory and, on finalize, hands them to the owning transaction
/// to store inline in that part's per-ref sidecar (no orphan blob is ever created). The bytes are
/// tiny (a uuid / a small integer), so in-memory accumulation is appropriate.
class ContentAddressedInlineWriteBuffer : public WriteBufferFromFileBase
{
public:
    /// Invoked from finalizeImpl with the fully-accumulated file bytes.
    using OnInlined = std::function<void(std::string bytes)>;

    explicit ContentAddressedInlineWriteBuffer(OnInlined on_inlined_);
    ~ContentAddressedInlineWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    void nextImpl() override;
    void finalizeImpl() override;

    OnInlined on_inlined;
    std::string accumulated;
};

}

// Transaction for the content-addressed metadata storage.
//
// As part files are written, each ContentAddressedWriteBuffer reports its content hash back
// through a finalize callback, and the transaction accumulates a (logical_file -> BlobEntry) map.
// Carried-forward files (mutations / hardlinks) copy their source BlobEntry without re-uploading.
// At commit the transaction derives the part_id from the accumulated blobs, writes the
// parts/<part_id> manifest (put-if-absent) and publishes the ref.
class ContentAddressedTransaction : public IMetadataTransaction
{
protected:
    ContentAddressedMetadataStorage & metadata_storage;

public:
    /// key_prefix_ is the object-storage common key prefix, taken from the metadata storage
    /// (its storage_path_prefix) so the write side keys objects exactly where the read side looks.
    /// Never re-derive it from object_storage->getCommonKeyPrefix() here: a config
    /// key_compatibility_prefix override would otherwise make read and write disagree.
    ///
    /// local_scratch_path_ is a real server-local filesystem directory where the write buffer spills
    /// each part file while hashing it, before upload. It is a local path, NOT an object key prefix:
    /// for a remote object storage (e.g. s3) the key prefix is not a usable local path.
    ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_, std::string key_prefix_, std::string local_scratch_path_);

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    // Open a write buffer for a file.
    //
    // For a part file (<uuid[:3]>/<uuid>/<part>/<file>) this returns a content-addressed buffer
    // that hashes + uploads the content on finalize and, via its callback, records the resulting
    // blob under the path's logical file name (manifest + ref are published at commit).
    //
    // For a non-part / table-level file (e.g. <uuid[:3]>/<uuid>/format_version.txt) this returns a
    // plain object-storage write buffer that writes the bytes verbatim to a direct object key
    // (tableFileKey) — no content addressing, no ref, no manifest. Such objects are durable on
    // finalize and are not tracked by commit.
    //
    // The buffer-size and settings arguments mirror the disk-layer writeFile signature; content
    // addressing ignores append mode, so only Rewrite is meaningful here.
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    // No-op directory operations: object storage has no real directories; existsDirectory derives
    // from the refs/objects prefix. Mirrors how the plain-rewritable transaction treats directories.
    void createDirectory(const std::string & path) override;
    void createDirectoryRecursive(const std::string & path) override;
    void removeDirectory(const std::string & path) override;

    // Recursive removal = pointer-unlink + deferred GC.
    //
    // DROP TABLE and outdated-part cleanup reach this via DiskObjectStorageTransaction's
    // removeSharedRecursive / removeRecursive at commit. For a content-addressed pool, removal
    // deletes only the POINTER objects that the incoming path covers — the per-(server,table) refs
    // and the verbatim table-level / generic disk-level files — and NEVER the shared backing objects
    // (blobs/ content, parts/ manifests). Those are reclaimed later by the Phase-4 reachability GC, so
    // an orphaned blob/manifest leaks until then. That is the known, accepted M1 state (merges already
    // leak this way).
    //
    // The should_remove_objects predicate governs whether the SHARED backing objects (blobs) are
    // deleted. For content addressing the answer is always "no" (deferred GC), and the ref metadata
    // is always removed, so the predicate does not gate ref deletion here. Deletion is scoped
    // strictly by table_uuid / path so dropping one table never touches another table's refs in the
    // shared pool.
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;

    // Carry a file forward from an already-committed (or in-flight) source part without
    // re-uploading: resolve the source blob and record it under the destination logical file.
    //
    // This is the production carry-forward entry point: the disk layer's
    // DiskObjectStorageTransaction::createHardLink delegates to metadata_transaction->createHardLink,
    // so this override is what real mutations / ATTACH reach when hardlinking unchanged files.
    void createHardLink(const std::string & path_from, const std::string & path_to) override;

    // ==== Derived per-file metadata: no-ops for content-addressing ====
    //
    // For a content-addressed disk, per-file timestamps, permission bits, read-only flags and
    // hardlink counts are not stored — they are derived (or simply absent) when a part is resolved
    // via ref -> part_id -> manifest -> blob. None of them participate in CA resolution, so the
    // commit-replay calls that the INSERT / merge code path records for them are no-ops here.
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;

    // Move/rename of a part directory. MergeTree writes a part under a temporary directory name
    // (e.g. tmp_insert_<part>) and renames it to the final <part> name at commit. Content
    // addressing keys recorded blobs by their in-part file name only, and the (table_uuid,
    // part_name) target is re-pinned to the destination so the ref is published under the final
    // part name. The blobs are already content-addressed, so no objects are moved in storage.
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;

    // Move/rename of a single file within a part (e.g. column rename during a part write).
    // Re-key the recorded blob from the source in-part file name to the destination one; no object
    // is moved in storage since the blob is content-addressed.
    void moveFile(const std::string & path_from, const std::string & path_to) override;

    // Unlink a logical file. For a part file still being assembled in this commit, drop the
    // recorded blob so it is excluded from the manifest. For files that were never recorded (or
    // non-part files), there is nothing CA-resolution-relevant to remove, so it is a no-op.
    // The underlying content blob, if shared, is reclaimed by GC, not by an unlink here.
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;

    // Truncate is meaningless for an immutable content-addressed blob; MergeTree does not truncate
    // committed part files, so this is a no-op (it would only ever be reached for a logical file
    // that is not part of CA resolution).
    void truncateFile(const std::string & path, size_t size) override;

    // Backwards-compatible alias used by the direct-call tests; forwards to createHardLink.
    void createHardLinkFrom(const std::string & from, const std::string & to) { createHardLink(from, to); }

private:
    // Record (logical_file -> blob) for the part being written; the part_id is derived from this.
    void recordBlob(const std::string & path, ContentAddressed::BlobEntry entry);
    // Pin/verify the (table_uuid, part_name) all files of one commit must agree on.
    void rememberTarget(const std::string & path);

    // DETACH PARTITION: re-publish a committed part directory as a detached ref. The detached ref is
    // named "detached" and carries the source part's manifest blob entries and mutable sidecar files
    // re-keyed under the detached part directory component, then the source ref is unlinked. Content
    // blobs and the source manifest are untouched (immutable). See moveDirectory (B36).
    void republishCommittedPartIntoDetached(
        const ContentAddressed::PartFilePath & src_committed, const ContentAddressed::PartFilePath & dst_detached);

    // DETACHED part rename within the detached namespace (detached/<old> -> detached/<new>), used by
    // DROP DETACHED PARTITION ("deleting_" rename before removal) and ATTACH ("attaching_" rename). The
    // detached parts share the one "detached" ref whose keys are <detached_part>/<file> (B36/B46); this
    // re-keys only this part's <old>/ prefix to <new>/ in the shared manifest + sidecar bundle (and the
    // per-file sidecar objects), leaving the other detached parts intact. No content blobs move. Reached
    // only from moveDirectory.
    void rekeyDetachedPartDir(const std::string & table_id, const std::string & old_dir, const std::string & new_dir);

    // RENAME TABLE / cross-engine table move: re-key every ref + per-ref sidecar (and verbatim
    // table-level file) from the source table identifier to the destination table identifier, then
    // unlink the source pointer objects. The table identifier is part of the ref/store object key, so
    // a rename changes it (e.g. Ordinary data/db/mt -> Atomic <uuid>) and the read at the new identity
    // would otherwise find no ref (B40). The shared blobs/manifests are content-addressed and
    // untouched; only the small pointer objects move. Reached only from moveDirectory.
    void republishTableRefs(const std::string & src_table_id, const std::string & dst_table_id);

    // Rename the ref (+ per-ref sidecar objects) of a COMMITTED part directory within one table:
    // <uuid>/<src_part> -> <uuid>/<dst_part>. MergeTree renames a committed part to delete_tmp_<part>
    // before removing it (DataPartStorageOnDiskBase::remove), and renames merged/mutated parts, via
    // disk->moveDirectory with NOTHING staged in this transaction. Content addressing has no rename, so
    // re-key the small pointer objects (ref, bundle <part>.meta, per-file <part>.<file>.meta) from the
    // source name to the destination name; the shared blobs/manifest are content-addressed and
    // untouched. Returns true if a committed source ref was found and re-keyed. Without this the source
    // ref survives the "remove", so the part is rediscovered on the next ATTACH (B45).
    bool renameCommittedPartRef(
        const ContentAddressed::PartFilePath & src, const ContentAddressed::PartFilePath & dst);

    // Authoritative removal of a single part directory <uuid[:3]>/<uuid>/<part>: unlink the part's
    // ref and its per-ref sidecar objects (the bundle <part>.meta and each per-file <part>.<file>.meta),
    // keeping the shared blobs/manifest for deferred GC. Returns true if the path was a regular part
    // directory and was handled here. Shared by removeRecursive (slow path) and removeDirectory (the
    // MergeTree fast removal path ends with removeDirectory(<part>), so this is where a complete part's
    // ref MUST be unlinked or it is rediscovered on the next ATTACH — B45).
    bool unlinkPartDirRefs(const std::string & path);

    /// Object-storage common key prefix; authoritative copy from the metadata storage (see ctor).
    const std::string key_prefix;
    /// Server-local scratch dir for the write-buffer spill (see ctor).
    const std::string local_scratch_path;
    std::map<std::string, ContentAddressed::BlobEntry> recorded;
    /// Mutable per-part files (isMutablePerPartFile): their raw bytes, stored inline in the per-ref
    /// sidecar at commit instead of the shared manifest. Keyed by in-part logical file name.
    std::map<std::string, std::string> recorded_mutable;
    std::string table_uuid;
    std::string part_name;
};

}
