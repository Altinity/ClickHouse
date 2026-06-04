#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>
#include <Interpreters/Context_fwd.h>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace DB
{

// Content-addressed metadata storage: resolves part files via ref to part_id to manifest to blob.
// Read/resolve only in Phase 2; the write path is Phase 3.
class ContentAddressedMetadataStorage final : public IMetadataStorage
{
public:
    /// local_scratch_path_ is a real server-local filesystem directory used to spill part files
    /// while hashing them before upload. It must NOT be derived from the object-storage key prefix:
    /// for a remote object storage (e.g. s3) that prefix is a remote KEY prefix, not a local path.
    /// context_ is LAST and defaults to nullptr so unit tests that exercise the read/write paths
    /// directly can construct the storage without spinning up a background GC thread. When non-null
    /// (the disk-factory path), a ContentAddressedGCThread is created and driven by startup/shutdown.
    ContentAddressedMetadataStorage(
        ObjectStoragePtr object_storage_,
        String storage_path_prefix_,
        String server_id_,
        String local_scratch_path_,
        ContextPtr context_ = nullptr,
        bool allow_shared_pool_ = false);

    MetadataStorageType getType() const override { return MetadataStorageType::ContentAddressed; }
    const std::string & getPath() const override { return storage_path_full; }
    bool supportsChmod() const override { return false; }
    bool supportsStat() const override { return false; }
    bool isReadOnly() const override { return false; }
    bool isContentAddressed() const override { return true; }
    bool areBlobPathsRandom() const override { return false; }
    uint32_t getHardlinkCount(const std::string &) const override { return 0; }

    MetadataTransactionPtr createTransaction() override;

    /// Disk lifecycle hooks (called by DiskObjectStorage start/stop): drive the background GC thread.
    void startup() override;
    void shutdown() override;

    bool existsFile(const std::string & path) const override;
    bool existsDirectory(const std::string & path) const override;
    bool existsFileOrDirectory(const std::string & path) const override;
    uint64_t getFileSize(const std::string & path) const override;
    Poco::Timestamp getLastModified(const std::string & path) const override;
    std::vector<std::string> listDirectory(const std::string & path) const override;
    DirectoryIteratorPtr iterateDirectory(const std::string & path) const override;
    bool isDirectoryEmpty(const std::string & path) const override;
    StoredObjects getStorageObjects(const std::string & path) const override;

    const std::string & serverIdForTest() const { return server_id; }

    /// The pool's stable identity (`PoolMeta::pool_uuid`), resolved during `startup` (minted on a fresh
    /// claim, or read from the existing marker on re-mount / shared-accept). Empty before `startup`.
    /// Used by the fetch-by-relink path to confirm two replicas share a pool — endpoint+prefix
    /// string-matching is unsafe (false positives → mis-relink).
    const std::string & getPoolUUID() const { return pool_uuid; }

    /// Test hook: run one synchronous GC sweep round and wait for it (no-op if no GC thread).
    const ContentAddressedGCThreadPtr & gcThreadForTest() const { return gc_thread; }

    /// The per-pool in-process GC lock (B49). Shared with both the background sweep
    /// (ContentAddressedGC, via ContentAddressedGCThread) and every transaction commit
    /// (ContentAddressedTransaction::commit). Exposed so a test can build a ContentAddressedGC that
    /// truly excludes the same storage's commits (the production wiring threads the same shared_ptr).
    const std::shared_ptr<std::mutex> & gcLock() const { return gc_lock; }

    /// The per-pool set of in-flight blob object keys staged by transactions not yet committed (B52).
    /// Guarded by gc_lock. The sweep (which holds gc_lock) treats these as reachable so a blob a
    /// dedup-skipping insert decided to reuse cannot be reclaimed in the finalize -> commit window.
    const std::shared_ptr<std::set<std::string>> & inFlightPinnedBlobs() const { return in_flight_pinned_blobs; }

    /// Pin a blob object key for the lifetime of an in-flight transaction (B52). Must be called with
    /// gc_lock held so the pin is visible to a concurrent sweep before the caller's dedup-skip decision.
    void pinBlobLocked(const std::string & blob_key) { in_flight_pinned_blobs->insert(blob_key); }

    /// Release an in-flight blob pin (B52). Takes gc_lock internally; called after the ref is published
    /// (the ref now keeps the blob reachable) or when an uncommitted transaction is destroyed.
    void unpinBlob(const std::string & blob_key);

private:
    friend class ContentAddressedTransaction;

    // Resolve a part-file path to its manifest BlobEntry (carry-forward source for mutations).
    // Throws if the path is not a part file, the ref is absent, or the file is not in the manifest.
    ContentAddressed::BlobEntry resolveBlobEntry(const std::string & path) const;

    // Resolve a MUTABLE per-part file (uuid.txt / txn_version.txt / metadata_version.txt) path to its
    // raw bytes from the part's per-ref sidecar (carry-forward source for mutations). Throws if the
    // path is not such a file, the ref is absent, or the sidecar lacks the file (fail-close).
    std::string resolveMutableFileBytes(const std::string & path) const;

    // Read and deserialize the per-ref sidecar for (table_uuid, part_name) if present (else nullopt).
    std::optional<ContentAddressed::RefSidecar> readRefSidecarIfExists(const std::string & table_uuid, const std::string & part_name) const;

    // Resolve helpers: part file -> ref -> part_id -> manifest -> blob. All object keys are built
    // under storage_path_prefix (the object-storage common key prefix), the single source of truth
    // shared with ContentAddressedTransaction so the read and write sides cannot disagree.
    // No manifest cache in M1 (read each time); caching is a later optimization.

    // Read the ref object at refKey(server_id, table_uuid, part_name).
    // Returns nullopt if the ref object is absent, else the part id it names.
    std::optional<ContentAddressed::PartId> readRefPartId(const std::string & table_uuid, const std::string & part_name) const;

    // Load and deserialize the manifest at partKey(part_id).
    // B18 fail-close: if the manifest object is absent, throw CORRUPTED_DATA (a live ref must
    // never point at a missing manifest); never treat it as "file doesn't exist".
    ContentAddressed::PartManifest loadPartManifestOrThrow(const ContentAddressed::PartId & part_id) const;

    // Read a small object (ref/manifest) into a string. Returns nullopt if the object is absent.
    std::optional<std::string> readSmallObjectIfExists(const std::string & key) const;

    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
    /// Server-local scratch dir for the write-buffer spill (see ctor doc).
    const std::string local_scratch_path;
    /// Operator opt-in to mount a pool already owned by a different server (B11). Default false:
    /// fail closed on a second/concurrent mounter so background GC can never run un-coordinated.
    const bool allow_shared_pool;

    /// The pool's stable identity, resolved by `claimPoolOwnership` during `startup` (minted once by
    /// the first claimant, read back on every later mount). Empty until `startup` runs.
    std::string pool_uuid;

    /// Per-pool in-process GC lock (B49), shared with the background sweep and the commit path so a
    /// dedup-skipped blob cannot be reclaimed in the finalize -> commit window. Created eagerly so the
    /// unit tests that drive commits + a directly-built ContentAddressedGC can share it via gcLock().
    const std::shared_ptr<std::mutex> gc_lock = std::make_shared<std::mutex>();

    /// Per-pool set of blob object keys staged by in-flight (not-yet-committed) transactions (B52).
    /// Guarded by gc_lock and shared by reference with the background sweep so a blob a dedup-skipping
    /// insert decided to reuse is treated as reachable until the insert commits its ref (or aborts).
    const std::shared_ptr<std::set<std::string>> in_flight_pinned_blobs = std::make_shared<std::set<std::string>>();

    /// Background pool garbage collector, present only on the disk-factory path (context non-null).
    ContentAddressedGCThreadPtr gc_thread;
};

}
