#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>
#include <Interpreters/Context_fwd.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>

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
    bool supportsTransactionalMutableFiles() const override { return true; }
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

    /// Resolve a part directory path (`store/<uuid[:3]>/<uuid>/<part>/`, relative to the disk) to the
    /// `part_id` named by THIS server's ref for that part, if the ref exists (else nullopt). Used by the
    /// fetch-by-relink SENDER (`DataPartsExchange::Service`) to read the authoritative content id of the
    /// part it is asked to send so it can transmit the id (not the bytes) for a same-pool receiver to
    /// relink against. A thin public wrapper over the private ref resolution.
    std::optional<ContentAddressed::PartId> getPartId(const std::string & part_path) const;

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

    /// The per-pool incremental reverse index (B9 / CA GC S1). The commit path adds a part's blob pins
    /// and the drop path removes them, so it tracks a per-process refcount over blob hashes. It is
    /// instrumentation only: the GC sweep validates it against the authoritative full-scan and logs
    /// drift, but never gates a deletion on it (the scan stays authoritative). Shared by reference with
    /// the background sweep (ContentAddressedGC) and the transaction commit/drop path.
    const std::shared_ptr<ContentAddressed::InMemoryBlobRefIndex> & blobRefIndex() const { return blob_ref_index; }

    /// Pin a blob object key for the lifetime of an in-flight transaction (B52). Must be called with
    /// gc_lock held so the pin is visible to a concurrent sweep before the caller's dedup-skip decision.
    void pinBlobLocked(const std::string & blob_key) { in_flight_pinned_blobs->insert(blob_key); }

    /// Release an in-flight blob pin (B52). Takes gc_lock internally; called after the ref is published
    /// (the ref now keeps the blob reachable) or when an uncommitted transaction is destroyed.
    void unpinBlob(const std::string & blob_key);

    /// ==== Fetch-by-relink (CAS replication Phase 2a) ====
    ///
    /// A handle on the durable cross-mounter `WriteSession` opened by a relink to PIN an existing part's
    /// blob set before this server publishes its own ref. It is the exact analogue of the write path's
    /// per-blob `WriteSession`, but seeded with a KNOWN hash set (the source part's manifest) instead of
    /// accumulating hashes as blobs are uploaded — the relink uploads no blobs (another replica already
    /// wrote them), so without this pin a concurrent source-ref drop + GC sweep could reclaim the blobs
    /// in the window before this server's ref is published (spec §4, the one true data-loss hole).
    struct RelinkPin
    {
        std::string session_id; /// the unique sessions/<id> key this pin is persisted at
        ContentAddressed::PartId part_id;
        bool open = false; /// false => no pin held (default-constructed / already released)
    };

    /// STEP 1 — PIN. Read `parts/<part_id>`'s manifest, open a durable `WriteSession` whose `pending` =
    /// every blob hash the manifest names, stamp this server's identity + an advisory lease, and persist
    /// it to `sessions/<id>` BEFORE returning. Once this returns, a GC sweep on ANY mounter treats those
    /// hashes as reachable (via `sessionPinnedBlobs`) for the lease lifetime, closing the relink window.
    /// Returns nullopt WITHOUT publishing anything if the manifest object is absent (the part_id this
    /// server was asked to relink does not exist in the pool — relink not possible, fall back to a byte
    /// fetch). A present-but-corrupt manifest throws (fail-closed).
    std::optional<RelinkPin> relinkPin(const ContentAddressed::PartId & part_id);

    /// STEP 2 — RE-VALIDATE. Under the held pin, confirm `parts/<part_id>` still exists AND every blob
    /// hash it names exists in `blobs/`. Returns false (relink not possible) if the manifest or any blob
    /// is missing — the caller must then release the pin and fall back to a byte fetch; a ref is NEVER
    /// published to a missing blob. A present-but-corrupt manifest throws (fail-closed).
    bool relinkRevalidate(const ContentAddressed::PartId & part_id) const;

    /// STEP 3 — PUBLISH. Write this server's ref `refKey(self, table_uuid, part_name)` -> part_id plus
    /// the per-ref sidecar (`refMetaKey` + the per-file mutable objects) from `sidecar_values` (the
    /// per-part mutable files uuid.txt / txn_version.txt / metadata_version.txt carried in the fetch
    /// header). Mirrors `ContentAddressedTransaction::commit`'s ref+sidecar publish: per-file objects and
    /// the bundle sidecar BEFORE the ref (the ref is the last, publishing step). No blob bytes are written.
    void relinkPublishRef(
        const std::string & table_uuid,
        const std::string & part_name,
        const ContentAddressed::PartId & part_id,
        const std::map<std::string, std::string> & sidecar_values);

    /// STEP 4 — RELEASE. Remove the durable pin's `sessions/<id>` object (best-effort, never throws): the
    /// published ref now keeps the blobs reachable, so the pin is no longer needed. Idempotent.
    void relinkReleasePin(RelinkPin & pin) noexcept;

    /// The full pin-before-publish relink (spec §4): PIN -> RE-VALIDATE -> PUBLISH -> RELEASE, in that
    /// strict order. Returns true if the ref was published (relink succeeded), false if the relink is not
    /// possible (the manifest or a blob is missing) — in which case NOTHING was published (no dangling
    /// ref) and the pin was released, so the caller falls back to a byte fetch. The 4 steps are also
    /// public so a test can interleave a concurrent source-ref drop + GC sweep BETWEEN pin and publish.
    bool relinkExistingPart(
        const std::string & table_uuid,
        const std::string & part_name,
        const ContentAddressed::PartId & part_id,
        const std::map<std::string, std::string> & sidecar_values);

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

    // FREEZE read side: resolve a frozen part to its part id via the SHADOW ref object at
    // shadowRefKey(shadow_table_dir, part_name). Mirrors readRefPartId but keys off the literal shadow
    // table dir (shadow/<backup>/store/<uuid[:3]>/<uuid>) instead of the live store/.../refs/ location.
    // Returns nullopt if the shadow ref is absent.
    std::optional<ContentAddressed::PartId> readShadowRefPartId(const std::string & shadow_table_dir, const std::string & part_name) const;

    // FREEZE read side: read and deserialize the SHADOW per-ref sidecar at
    // shadowRefMetaKey(shadow_table_dir, part_name) if present (else nullopt). Mirrors
    // readRefSidecarIfExists for the shadow namespace (overlays a frozen part's mutable per-part files).
    std::optional<ContentAddressed::RefSidecar> readShadowRefSidecarIfExists(const std::string & shadow_table_dir, const std::string & part_name) const;

    // Load and deserialize the manifest at partKey(part_id).
    // B18 fail-close: if the manifest object is absent, throw CORRUPTED_DATA (a live ref must
    // never point at a missing manifest); never treat it as "file doesn't exist".
    ContentAddressed::PartManifest loadPartManifestOrThrow(const ContentAddressed::PartId & part_id) const;

    // Read a small object (ref/manifest) into a string. Returns nullopt if the object is absent.
    std::optional<std::string> readSmallObjectIfExists(const std::string & key) const;

    // Generic directory-child derivation (shared by the live table-dir listing and the shadow
    // intermediate/table-dir listing): list every object under `prefix`, strip `prefix`, and collect
    // the immediate child component of each. When `skip_ref_meta` is set, per-ref sidecars (.meta keys
    // under a refs/ prefix) are skipped so a part dir never appears twice (once as <part>, once as
    // <part>.meta). Mirrors MetadataStorageFromPlainObjectStorage::listDirectory child-derivation.
    std::unordered_set<std::string> collectDirectoryChildren(const std::string & prefix, bool skip_ref_meta) const;

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

    /// Per-pool incremental reverse index (B9 / CA GC S1), one instance per pool alongside `gc_lock`.
    /// Maintained best-effort by the commit/drop path and validated (never gated on) by the sweep.
    /// Thread-safe internally; shared by reference with the sweep and the commit/drop path.
    const std::shared_ptr<ContentAddressed::InMemoryBlobRefIndex> blob_ref_index = std::make_shared<ContentAddressed::InMemoryBlobRefIndex>();

    /// Background pool garbage collector, present only on the disk-factory path (context non-null).
    ContentAddressedGCThreadPtr gc_thread;
};

}
