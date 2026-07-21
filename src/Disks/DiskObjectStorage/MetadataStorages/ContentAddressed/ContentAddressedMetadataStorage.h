#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Interpreters/Context_fwd.h>
#include <base/defines.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Poco::Util { class AbstractConfiguration; }

namespace DB
{
class IDisk;
using DiskPtr = std::shared_ptr<IDisk>;
}

namespace DB::Cas
{

/// Selects where a content-addressed blob is staged before it is published.
///
/// `Local` is the default and preserves the existing local scratch-file path byte for byte; it does
/// not run the conditional-copy probe. `S3` is opt-in and can stream large blobs to an object-store
/// staging key. That path is usable only after the mount-time probe has demonstrated write-once
/// conditional copy semantics. If the backend does not enforce those semantics, callers must stay
/// on `Local`: an unconditional copy could overwrite a live content-addressed blob.
enum class StagingBackend
{
    Local,
    S3,
};

}

namespace DB
{

/// Adapts ClickHouse's `IMetadataStorage` path-based interface to the content-addressed pool.
///
/// The class owns the pool and its cached part-folder facade for the lifetime of an opened disk. It
/// parses disk paths, maps them to pool namespaces and references, and translates manifest entries
/// into `StoredObjects` or in-memory read sources. Transaction and GC entry points are exposed here
/// because disk lifecycle and system-query code own those operations; the CAS protocol itself stays
/// in `Cas::Pool`, `Cas::PartWriteTxn`, and `Cas::Gc`.
///
/// Namespace mapping:
///   live part      SERVER_ID/TABLE_UUID            ref = PART_DIR
///   detached part  SERVER_ID/TABLE_UUID            ref = detached/DETACHED_PART_DIR
///   FREEZE shadow  the LITERAL shadow table dir     ref = PART_DIR
///                  (shadow/BACKUP/store/U3/UUID or shadow/BACKUP/data/DB/TBL — bijective with
///                  the disk path for both Atomic and non-Atomic layouts, so the shadow tree
///                  enumerates from `Pool::listNamespaces("shadow/...")`)
///   generic files  SERVER_ID/_disk                  verbatim namespace files (access probes)
///
/// Small per-part files (`uuid.txt`, `metadata_version.txt`, `txn_version.txt`, `checksums.txt`, ...)
/// are inline-placement manifest tree entries, not sidecar objects. Their bytes are served through
/// `DiskObjectStorage::prepareRead`'s CA branch via `tryGetInManifestBytes`; `getStorageObjects` returns a
/// sized placeholder with an EMPTY remote key for them (any consumer bypassing the prepareRead branch
/// fails loudly, never reads wrong bytes).
class ContentAddressedMetadataStorage final : public IMetadataStorage, public IContentAddressedExchange
{
public:
    /// Constructs an unopened storage adapter. `settings_` carries every tunable that used to be a
    /// positional parameter (see `ContentAddressedSettings`) -- it is the single source of defaults, so
    /// the constructor itself declares none. `server_root_id`/`scratch_path` (the local-scratch
    /// directory used when a write buffer must spill before hashing and upload, independent of the
    /// object-storage key prefix) are read from `settings_` rather than taken as their own parameters. A
    /// non-null `context_` enables the background GC scheduler on the disk-factory path; tests may pass
    /// null to disable system-log integration and scheduling. `disk_name_` falls back to
    /// `storage_path_prefix_` when empty, exactly as before this constructor collapsed.
    ContentAddressedMetadataStorage(
        ObjectStoragePtr object_storage_,
        String storage_path_prefix_,
        String server_id_,
        String disk_name_,
        ContextPtr context_,
        const ContentAddressedSettings & settings_);

    /// Parses a `staging_backend` value (`local` | `s3`). Throws `BAD_ARGUMENTS` for an unrecognized
    /// value rather than silently selecting a backend.
    static Cas::StagingBackend parseStagingBackend(const std::string & value);

    /// Reads `staging_backend` from `config`, defaulting to `local`, and parses it. Kept only as a
    /// thin wrapper around the string-taking overload for callers that still hold a config reference.
    static Cas::StagingBackend parseStagingBackend(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

    /// Parses a `part_folder_validate` value (`always` | `never` | `age <seconds>`). The `age` form
    /// accepts only a non-negative integer number of seconds; malformed input and unknown modes throw
    /// `BAD_ARGUMENTS` instead of silently selecting a policy.
    static Cas::PartFolderValidate parsePartFolderValidate(const std::string & value);

    /// Reads `part_folder_validate` from `config`, defaulting to `always`, and parses it. Kept only as
    /// a thin wrapper around the string-taking overload for callers that still hold a config reference.
    static Cas::PartFolderValidate parsePartFolderValidate(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

    /// Returns the content-addressed metadata storage backing `disk`, or nullptr if `disk` is not
    /// content-addressed. Plain (non-object-storage) disks do not implement `getMetadataStorage` at
    /// all and throw `NOT_IMPLEMENTED`; that is treated as "not content-addressed" rather than
    /// propagated. Any other exception from `getMetadataStorage` is rethrown. Centralizes the
    /// detection lambda duplicated across `InterpreterSystemQuery` and
    /// `StorageSystemContentAddressedMounts`; callers there have not yet been migrated to it.
    static ContentAddressedMetadataStorage * tryFromDisk(const DiskPtr & disk);

    /// Runs one synchronous GC round for tests and diagnostics. If the scheduler is not running,
    /// this lazily creates one so repeated calls retain the same lease-observation history.
    void runOneGcRoundForTest();

    /// Runs one synchronous GC round on the caller's thread and emits Start and Finish rows to
    /// `system.content_addressed_garbage_collection_log`. Throws `BAD_ARGUMENTS` when GC is disabled
    /// by read-only mode or configuration.
    Cas::RoundReport runGarbageCollectionRoundNow();

    /// Rebuilds the GC baseline for the `SYSTEM` disaster-recovery command. Each invocation uses a
    /// fresh GC identity because `rebuildBaseline` performs its own lease check. A refused rebuild
    /// (`report.performed == false`) writes nothing; the `SYSTEM` interpreter surfaces the refusal.
    /// Throws `BAD_ARGUMENTS` when GC is disabled by read-only mode or configuration.
    Cas::RebuildReport runGcRebuildNow(bool force) const;

    /// The `SYSTEM CONTENT ADDRESSED FSCK` handler (Task 7): dormant-only, read-only independent
    /// reachability audit. Requires `mount_state == Dormant` (else `INVALID_STATE`, directing the
    /// operator to `SYSTEM CONTENT ADDRESSED UNMOUNT` first) -- a live mount keeps serving traffic that
    /// an FSCK scan must never race against. Opens a TEMPORARY observe-only pool view via
    /// `openPoolView(/* observe_only= */ true)` (no probe, no watermark, no GC scheduler -- exactly the
    /// existing `read_only` startup path), runs `Cas::runFsck` over it, and destroys the view before
    /// returning: nothing from this scan is published to `cas_store`/`part_access`/`mount_state`.
    Cas::FsckReport runFsckOnDormant(bool detail) const;

    /// Returns per-disk GC health for `system.content_addressed_mounts`. Returns nullopt when this
    /// disk has no scheduler because GC is disabled, the disk is read-only, or startup has not run.
    /// Holds `gc_scheduler_mutex` for the entire call so a concurrent system-table query cannot
    /// observe a scheduler while `shutdown` destroys it.
    std::optional<Cas::CasGcScheduler::GcHealth> gcHealth() const;

    MetadataStorageType getType() const override { return MetadataStorageType::ContentAddressed; }
    const std::string & getPath() const override { return storage_path_full; }
    bool supportsChmod() const override { return false; }
    bool supportsStat() const override { return false; }
    bool isReadOnly() const override { return read_only; }
    bool isContentAddressed() const override { return true; }

    /// Fail-close gate shared by every mutating entry point (transactions, GC round, GC rebuild,
    /// pool-member decommission): an observe-only (`<readonly>`) disk must reject them all.
    void checkNotReadOnly(std::string_view what) const;

    /// Content-addressed transactions are eager staging overlays: each mutating disk-transaction
    /// method reaches the metadata transaction immediately rather than entering the FIFO queue.
    bool transactionIsStagingOverlay() const override { return true; }
    bool supportsAtomicFileWrites() const override { return true; }
    bool supportsTransactionalMutableFiles() const override { return true; }
    bool areBlobPathsRandom() const override { return false; }
    uint32_t getHardlinkCount(const std::string &) const override { return 0; }

    /// Creates a write transaction bound to this storage. Throws `READONLY` before allocating one
    /// when the disk was opened read-only.
    MetadataTransactionPtr createTransaction() override;

    /// Opens the pool, validates its format and starts the optional GC scheduler. Read-only disks
    /// skip write probes and GC; failures in startup propagate. Runs exactly once, single-threaded,
    /// strictly before this object is exposed to any other thread (no other method can be called
    /// concurrently with it) -- TSA_NO_THREAD_SAFETY_ANALYSIS is deliberate here, not a bypass of a
    /// real risk: pointer_mutex/gc_scheduler_mutex exist to guard concurrent access AFTER startup,
    /// which is definitionally impossible during it.
    void startup() TSA_NO_THREAD_SAFETY_ANALYSIS override;
    /// Stops the GC scheduler before releasing the part-folder facade and pool. Their destruction is
    /// synchronized with the accessors and synchronous GC entry points.
    void shutdown() override;

    /// Idempotent, resumable quiescence barrier for the `SYSTEM CONTENT ADDRESSED UNMOUNT` handler
    /// (Task 6): moves `Mounted -> Unmounting -> Dormant`. Gates new operations immediately (the
    /// `Unmounting` flip happens before anything else), stops the GC scheduler, then waits for
    /// outstanding `store()`/`partAccess()` snapshots to drop their references before releasing the
    /// pool. A no-op when already `Dormant`. Throws `TIMEOUT_EXCEEDED` if snapshots are still live
    /// after `drain_timeout_ms` -- the disk then stays `Unmounting` (new operations stay refused) and
    /// a retried call resumes the drain from where it left off, rather than restarting the whole
    /// unmount. Does not consult `shutdown_called`; unlike `shutdown()`, this is not a terminal path.
    void unmountSynchronously(uint64_t drain_timeout_ms = 30000) TSA_NO_THREAD_SAFETY_ANALYSIS;
    /// Idempotent explicit mount for the `SYSTEM CONTENT ADDRESSED MOUNT` handler (Task 6):
    /// `Dormant -> Mounted` via `startup()`. A no-op when already `Mounted`. Throws `INVALID_STATE`
    /// when called during an incomplete `Unmounting` -- mounting on top of an unfinished unmount could
    /// let two live pool snapshots exist under different mount generations at once; the caller must
    /// finish (or retry) `unmountSynchronously` first.
    void mountExplicitly() TSA_NO_THREAD_SAFETY_ANALYSIS;

    /// Test-only fault-injection hook. When set, `startup` invokes it right before it publishes
    /// `cas_store`/`part_access`/`gc_scheduler`/`pool_uuid`/`conditional_copy_supported` -- everything
    /// up to that point (opening the pool, building the part-folder facade, running the capability
    /// probe, starting the GC scheduler) has already happened into locals, so throwing here proves a
    /// late startup failure publishes nothing and a retry can still succeed. Left empty (a no-op) in
    /// production.
    std::function<void()> startup_fault_injection_for_test;

    /// Tests whether a path is represented by an inline manifest entry, namespace file, or loose
    /// mountpoint object.
    bool existsFile(const std::string & path) const override;
    /// Tests whether a path names a virtual part, table, shadow, or mirrored live-tree directory.
    bool existsDirectory(const std::string & path) const override;
    /// Tests both file and directory interpretations of a path.
    bool existsFileOrDirectory(const std::string & path) const override;
    /// Returns the logical payload size, excluding a blob envelope.
    uint64_t getFileSize(const std::string & path) const override;
    /// Returns the part publication time; other existing files use epoch time because their mtime is
    /// not retained by the content-addressed namespace.
    Poco::Timestamp getLastModified(const std::string & path) const override;
    /// Lists logical children of a virtual or mirrored directory.
    std::vector<std::string> listDirectory(const std::string & path) const override;
    /// Iterates over `listDirectory` results with each child joined to `path`.
    DirectoryIteratorPtr iterateDirectory(const std::string & path) const override;
    /// Reports virtual part and projection directories as empty so removal unlinks their ref; table
    /// and container directories use their listing.
    bool isDirectoryEmpty(const std::string & path) const override;
    /// Maps a logical path to its storage object. Inline entries return a sized empty-key placeholder
    /// and must be served by the CA read branch.
    StoredObjects getStorageObjects(const std::string & path) const override;
    /// Performs one manifest lookup for part files instead of the inherited `existsFile` plus
    /// `getStorageObjects` sequence.
    std::optional<StoredObjects> getStorageObjectsIfExist(const std::string & path) const override;

    /// ==== `IContentAddressedExchange` (interserver relinking facade) ====
    const String & getPoolUUID() const override { return pool_uuid; }
    /// Returns the canonical encoded manifest for a committed part, or nullopt when the path is not
    /// a committed CA part. Missing or corrupt committed state propagates as an exception.
    std::optional<String> getPartManifestBytes(const String & part_path) const override;
    /// Adopts a peer-supplied manifest into this server's namespace without transferring blob bodies.
    /// Returns false for decode or retryable publication failures so the caller can perform a byte
    /// fetch; read-only disks throw `READONLY`.
    bool adoptPartFromManifest(
        const String & table_uuid, const String & part_name, const String & manifest_bytes) override;

    /// ==== wiring-internal surface (the transaction + the disk's prepareRead CA branch) ====

    /// Returns a shared-ownership snapshot of the opened pool. Throws `INVALID_STATE` when the disk
    /// is not mounted (before the first `startup`, after `shutdown`, or -- from Task 5 on -- during
    /// or after an explicit `SYSTEM CONTENT ADDRESSED UNMOUNT`). A thin wrapper over `poolAccess()`.
    Cas::PoolPtr store() const;
    /// Returns a shared-ownership snapshot of the cached part-folder facade. Throws `INVALID_STATE`
    /// under the same not-mounted condition as `store()`. Committed part-folder reads and mutations
    /// go through this facade so cache validation remains centralized. Returning a `shared_ptr`
    /// snapshot (not a reference) means the returned handle keeps the facade alive via its own
    /// refcount even if `shutdown` concurrently resets the member -- unlike a reference, which would
    /// dangle the instant `shutdown`'s reset runs. A thin wrapper over `poolAccess()`.
    std::shared_ptr<Cas::CachedPartFolderAccess> partAccess() const;
    const std::string & serverRootId() const { return server_root_id; }
    const std::string & scratchPath() const { return local_scratch_path; }
    /// Returns the configured staging backend. `Local` is the behavior-preserving default; callers
    /// must also check `conditionalCopySupported` before using S3 promotion.
    Cas::StagingBackend stagingBackend() const { return staging_backend; }
    /// Returns the mount-time conditional-copy capability result. It starts false and becomes true
    /// only after the backend proves write-once copy semantics, so S3 promotion fails closed.
    bool conditionalCopySupported() const { return conditional_copy_supported; }
    /// Returns the underlying object storage for an S3 staging writer. It is meaningful only when
    /// `stagingBackend` is `S3` and `conditionalCopySupported` is true.
    const ObjectStoragePtr & objectStorage() const { return object_storage; }
    /// Returns the physical prefix for this pool's writer-owned staging area. It is the same
    /// `pool_prefix/staging/server_root_id` subtree used by the capability probe; callers append a
    /// unique leaf and must not use the probe object itself.
    String stagingKeyPrefix() const;

    /// Bytes that live INSIDE pool metadata rather than as their own object: an Inline-placement
    /// manifest tree entry, or a verbatim namespace file. nullopt = the path is blob-backed (a real
    /// storage object).
    std::optional<String> tryGetInManifestBytes(const std::string & path) const;

    /// The CA read entry called by `DiskObjectStorage::prepareRead` before the generic
    /// storage-objects path: serves in-manifest bytes (mutable per-part files, inline entries,
    /// verbatim namespace files) from memory. Returns false when the path is not in-manifest.
    /// Declared on `IContentAddressedExchange` (the narrow seam `prepareRead` casts to); `BlobViewPlan`
    /// is likewise inherited from there.
    bool prepareInManifestRead(const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const override;

    /// Resolves a blob-backed path to its physical object and payload window. Returns nullopt for
    /// in-manifest, loose, directory, or otherwise unresolved paths.
    std::optional<BlobViewPlan> getBlobViewPlan(const std::string & path) const override;

    /// Creates a seekable reader over one blob payload, excluding its envelope. Transactions use
    /// this for read-your-writes; committed reads use `getBlobViewPlan` and the normal pipeline.
    std::unique_ptr<ReadBufferFromFileBase> readBlobPayload(
        const Cas::BlobLocation & location, const std::string & path, const ReadSettings & settings) const;

    /// Maps a live table UUID to its pool namespace. Detached parts share that namespace and use
    /// `detached/`-prefixed references rather than a sibling namespace.
    Cas::RootNamespace liveNamespace(const std::string & table_uuid) const;
    /// Canonicalizes a literal shadow-table directory into the pool namespace used by freeze and
    /// unfreeze paths. A trailing slash is ignored.
    static Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir);

    /// A dropped-and-not-recreated table (ref-table lifecycle durably `Removed`) is GONE for readers: its
    /// table-level namespace files — `format_version.txt` and other verbatim files — must read as absent
    /// even while GC has not yet physically reclaimed them (namespace removal is deferred to GC),
    /// mirroring how its parts already vanish via the ref state. Gate every namespace-file read on this.
    /// A never-born namespace is NOT hidden (fail-closed — only a KNOWN-removed table hides its files).
    bool namespaceFilesReadable(const Cas::RootNamespace & ns) const;

    /// Returns the root prefix for mirrored live-tree objects. The persistent layout identity is
    /// `server_root_id`; `ServerUUID` remains only the mount-owner token.
    std::string serverPrefix() const;

    /// Enumerate the children of a GENERIC intermediate live-tree directory (the disk root "",
    /// `store`, the `store/<u3>` shard dir, or any loose-file container above a table dir) via a
    /// server-root-scoped mirrored S3 LIST of `roots/<server_root_id>/<path>/`. `@cas@`-suffixed table-dir
    /// segments are surfaced under their logical (unsuffixed) name. This is what makes top-down
    /// `clickhouse-disks` traversal of the live tree behave like a normal disk; concrete
    /// `store/<u3>/<uuid>/<part>/<file>` navigation is still served by the exact-shape branches.
    std::vector<std::string> listLiveTreeChildren(const std::string & path) const;
    /// Tests whether the server-root-scoped mirrored subtree has at least one child. The disk root is
    /// always considered present.
    bool liveTreeDirHasChildren(const std::string & path) const;

    /// Resolves one parsed path to its namespace, reference, and in-tree file. Detached paths are
    /// re-split here so their references remain in the table namespace with a `detached/` prefix;
    /// shadow paths map to a namespace derived from the literal shadow directory.
    struct Route
    {
        Cas::RootNamespace ns{""};
        /// empty => the path is the namespace's container dir. For a detached part this is
        /// `detached/<part>` (a ref inside the table namespace, not a separate namespace).
        std::string ref;
        std::string file;   /// empty => the path is the part dir itself

        /// The (ns, ref) identity subset — what the part-folder access layer keys on.
        Cas::PartRefKey refKey() const { return {ns, ref}; }
    };
    /// Converts a parsed path into the namespace/reference/file tuple used by the part-folder
    /// facade. Returns nullopt only when the parsed path cannot be routed.
    std::optional<Route> route(const Cas::PartFilePath & p) const;

    /// Returns full `detached/<part>` reference names in a namespace.
    std::vector<std::string> detachedRefNames(const Cas::RootNamespace & ns) const;

    /// Returns full `moving/<part>` staging-reference names in a namespace. Move recovery enumerates
    /// these names and removes entries left by an interrupted move.
    std::vector<std::string> movingRefNames(const Cas::RootNamespace & ns) const;

    /// `existsDirectory` and `listDirectory` use one fixed dispatch order to route a path through
    /// (shadow -> atomic-shard -> table-uuid -> part -> subdir -> generic), previously implemented
    /// twice and kept in sync by hand. `classifyDirectory` (private, below) computes it once; both
    /// callers then switch on the resulting shape. `DirShape` and `DirRoute` remain public only so
    /// `classifyDirectoryForTest` can expose the classification to wiring tests; the logic stays
    /// private.
    enum class DirShape
    {
        ShadowPart,
        ShadowTable,
        ShadowIntermediate,
        AtomicShard,
        TableDir,
        DetachedContainer,
        MovingContainer,
        PartDir,
        ProjectionDir,
        TableSubdir,
        GenericIntermediate,
    };

    struct DirRoute
    {
        /// Defaulted so a future classifyDirectory return path that forgets to set it fails safe
        /// (a defined shape) instead of switching on an indeterminate enum (UB). Matches the
        /// existing unreachable-fallthrough choice at the bottom of existsDirectory/listDirectory.
        DirShape shape = DirShape::GenericIntermediate;
        std::optional<Cas::PartFilePath> p;
        std::optional<Route> r;
        std::optional<std::string> uuid;
        std::optional<Cas::TableFilePath> tf;
        std::optional<std::string> projection_prefix;
    };

    /// Test-only accessor exposing the private directory classification so wiring tests can pin the
    /// dispatch order directly.
    DirRoute classifyDirectoryForTest(const std::string & path) const { return classifyDirectory(path); }

private:
    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
    const std::string server_root_id;
    const std::string disk_name;
    const std::string local_scratch_path;
    const ContextPtr context;

    const bool gc_enabled;
    const std::chrono::seconds gc_interval;
    const uint64_t dedup_cache_bytes;            /// P1 known-present cache byte cap (0=off)
    const uint64_t dedup_head_first_min_bytes;   /// P2 HEAD-before-PUT size threshold (0=off)
    const uint64_t gc_snap_generations_to_keep;  /// Number of GC snapshots retained (0 means keep all).
    const uint64_t gc_shards;                    /// Blob-hash-prefix reducer shard count, fixed at pool creation.
    const uint64_t manifest_sweep_list_budget_keys;
    const uint64_t manifest_sweep_delete_budget_keys;
    /// GCS single-PUT budget for conditional writes (generation-token stores only): threaded into
    /// the ObjectStorageBackend construction site in startup(). Irrelevant on ETag stores (AWS et al).
    const uint64_t gcs_max_conditional_put_bytes;
    /// Part-folder view cache settings. `cas_part_folder_cache_bytes == 0` disables retention.
    const uint64_t cas_part_folder_cache_bytes;
    const uint64_t cas_part_folder_cache_max_entries;
    const uint64_t cas_part_folder_cache_max_entry_bytes;
    /// Byte bound for the manifest decode cache in `Cas::Pool`. Zero disables decode caching.
    const uint64_t manifest_decode_cache_bytes;
    /// Bounded pool size for GC's per-hash freshness-metadata writes.
    const uint64_t gc_meta_pool_size;
    /// Configured staging backend; `Local` preserves the existing write path.
    const Cas::StagingBackend staging_backend;
    /// Blob content-hash function passed to `Cas::PoolConfig`.
    const Cas::BlobHashAlgo blob_hash_algo;
    /// Whether `blob_hash_algo` may be admitted into the pool's persisted `algos_used` set.
    const bool blob_hash_allow_new;
    /// Per-disk `<skip_access_check>` policy passed to `Cas::PoolConfig`.
    const bool skip_access_check;
    /// Grace period for materialization after an unclean predecessor, passed to `Cas::PoolConfig`.
    const uint64_t materialization_grace_ms;
    /// Policy controlling when retained part-folder views revalidate their manifest body.
    const Cas::PartFolderValidate part_folder_validate;
    /// Set by the mount-time conditional-copy capability probe — not const because the result is
    /// unavailable until startup.
    /// Defaults to false (fail-close): assumed unsupported until the probe proves otherwise.
    bool conditional_copy_supported = false;

    /// Explicit disk lifecycle. `startup()` moves `Dormant -> Mounted` as the last step of its single
    /// publish section; `shutdown()` moves back to `Dormant` where it resets the pointers. From Task 5
    /// on, `unmountSynchronously`/`mountExplicitly` drive `Mounted <-> Unmounting <-> Dormant` too.
    /// `poolAccess()` is the ONE place that checks this against `Mounted` before handing out a
    /// snapshot, so every operation on the pool or part-folder facade is gated the same way.
    enum class MountState : uint8_t
    {
        Mounted,
        Unmounting,
        Dormant,
    };

    /// A single coherent snapshot of the pool and its cached part-folder facade, taken under ONE
    /// `pointer_mutex` acquisition (see `poolAccess()`) so no caller can observe `pool` from one mount
    /// generation and `part_access` from another -- the two used to be fetched by two separate calls
    /// to `store()`/`partAccess()` at some call sites, each taking its own `pointer_mutex` snapshot.
    struct PoolAccessSnapshot
    {
        Cas::PoolPtr pool;
        std::shared_ptr<Cas::CachedPartFolderAccess> part_access;
    };

    /// Set by startup (Pool::open is fail-closed; empty store == not started). shared_ptr so
    /// store()/partAccess() can return a by-value snapshot under `pointer_mutex` (see below) instead
    /// of a reference that could dangle across a concurrent `shutdown` reset.
    Cas::PoolPtr cas_store TSA_GUARDED_BY(pointer_mutex);
    /// The part-folder access facade: the normal path
    /// for committed part/projection reads and committed part-ref mutations. Constructed in
    /// startup right after Pool::open; reset in shutdown before cas_store. shared_ptr for the same
    /// snapshot-safety reason as cas_store.
    std::shared_ptr<Cas::CachedPartFolderAccess> part_access TSA_GUARDED_BY(pointer_mutex);
    /// The mount lifecycle state; see `MountState` above. Starts `Dormant` (nothing published yet) --
    /// this makes the pre-`startup` `poolAccess()` refusal uniform with the post-`shutdown`/unmounted
    /// one: an access before/without a mount is an operational condition, not a programming invariant.
    MountState mount_state TSA_GUARDED_BY(pointer_mutex) = MountState::Dormant;
    String pool_uuid;
    /// shared_ptr so `runGarbageCollectionRoundNow`/`runOneGcRoundForTest` can take a snapshot under
    /// `pointer_mutex`, release it, and run the (long) round via the snapshot -- never holding
    /// `pointer_mutex` itself for the round's duration, so `gcHealth`/`store`/`partAccess` never
    /// block behind an in-flight round.
    std::shared_ptr<Cas::CasGcScheduler> gc_scheduler TSA_GUARDED_BY(pointer_mutex);
    /// Outermost lock, taken only by the explicit lifecycle operations: `mountExplicitly`,
    /// `unmountSynchronously` (Task 5), and the FSCK handler (Task 7). Not acquired anywhere yet in
    /// this task -- declared now so its place in the lock order is fixed before those operations
    /// exist. Lock order when nested locks are needed: `lifecycle_mutex` -> `gc_scheduler_mutex` ->
    /// `pointer_mutex`, never the reverse.
    mutable std::mutex lifecycle_mutex;
    /// Serializes ONE synchronous GC round at a time and makes `shutdown` wait for an in-flight round
    /// to finish cleanly (clean GC completion has priority over fast shutdown) -- held for the WHOLE
    /// round. Deliberately NOT the same mutex as `pointer_mutex` below: this one can be held for a
    /// long time, so nothing that only needs a brief pointer snapshot may share it.
    mutable std::mutex gc_scheduler_mutex;
    bool shutdown_called TSA_GUARDED_BY(gc_scheduler_mutex) = false;
    /// Guards ONLY reads/writes of `cas_store`/`part_access`/`gc_scheduler`/`mount_state` themselves
    /// (snapshot, create-if-absent, reset) -- always held briefly. Lock ordering when more than one of
    /// these is needed (the round entry points, `shutdown`, and -- outermost -- `lifecycle_mutex`):
    /// `lifecycle_mutex` first (if held at all), then `gc_scheduler_mutex`, then `pointer_mutex`
    /// nested inside, never the reverse.
    mutable std::mutex pointer_mutex;
    /// Derived from object_storage->isReadOnly() at startup (the disk's <readonly> config). When set:
    /// the probe is skipped, no watermark, no GC scheduler, and the mutating surface fails closed.
    bool read_only = false;
    /// Joined in front of core keys for DIRECT object_storage reads. The Emulated (Local) backend
    /// maps bare pool keys under getCommonKeyPrefix; Native passes keys through - this member
    /// mirrors that rule so readBlobPayload reads exactly where the backend wrote ("" for Native).
    String physical_key_prefix;

    /// Adds the local-backend common prefix to a pool key when direct object-storage I/O requires
    /// the physical key; native backends use the key unchanged.
    String physicalKey(const String & key) const
    {
        if (physical_key_prefix.empty())
            return key;
        if (physical_key_prefix.back() == '/')
            return physical_key_prefix + key;
        return physical_key_prefix + "/" + key;
    }

    /// The one place that takes a `{pool, facade}` snapshot under a SINGLE `pointer_mutex`
    /// acquisition and checks it against `MountState::Mounted`. Throws `INVALID_STATE` when the disk
    /// is not mounted (`mount_state != Mounted`, including a not-yet-started or a `!cas_store`
    /// pre-publish window) or unmounted (no `cas_store`). `store()`/`partAccess()` are thin wrappers
    /// over this; every other caller that needs BOTH the pool and the facade for one logical
    /// operation must call this ONCE and use both fields from the same snapshot, rather than calling
    /// `store()` and `partAccess()` separately -- otherwise it could straddle two mount generations
    /// once unmount/mount (Task 5) can change `cas_store`/`part_access` after startup.
    PoolAccessSnapshot poolAccess() const;

    /// Builds and throws the `INVALID_STATE` "disk is not mounted" exception `poolAccess()` throws,
    /// naming `state` and directing the operator to `SYSTEM CONTENT ADDRESSED MOUNT`. Shared with the
    /// synchronous GC round entry points (`runOneGcRoundForTest`/`runGarbageCollectionRoundNow`):
    /// since `SYSTEM CONTENT ADDRESSED UNMOUNT` (Task 6) makes `Dormant` reachable from SQL, a GC
    /// round dispatched at an unmounted disk must surface this same operator-facing refusal instead
    /// of the `LOGICAL_ERROR` those entry points threw before this existed (which aborts the process
    /// under debug/ASan builds -- a programming-invariant response to what is now a normal,
    /// SQL-reachable operational state). Callers must already hold `pointer_mutex` and pass the
    /// `mount_state` they read under it.
    [[noreturn]] void throwNotMounted(MountState state) const;

    /// Non-throwing counterpart to `poolAccess()`'s mount check, used ONLY by the read-only
    /// existence/enumeration surface (`existsFile`/`existsDirectory`/`existsFileOrDirectory`/
    /// `isDirectoryEmpty`/`listDirectory`/`iterateDirectory`/`getStorageObjectsIfExist`). A generic
    /// server sweep iterates ALL disks and probes each with an existence call (e.g.
    /// `DatabaseCatalog::dropTableFinally` calls `existsDirectory` on every disk); a Dormant CA disk
    /// answering that surface as absent/empty — rather than throwing `INVALID_STATE` via `store()` —
    /// keeps those CA-agnostic sweeps from wedging while any CA disk is unmounted. Content/size/mutation
    /// entry points deliberately do NOT consult this and stay fail-close (they still throw).
    bool isMounted() const;

    /// A pool opened as a standalone, UNPUBLISHED view: never touches `cas_store`/`part_access`/
    /// `gc_scheduler`/`mount_state` -- the caller owns it entirely and drops it when done.
    struct PoolView
    {
        Cas::PoolPtr pool;
        /// The direct-object-storage key prefix for this view's backend (see `physicalKey`'s own
        /// doc comment): populated for the Emulated (Local) backend, empty ("") for Native. Returned
        /// rather than written to the `physical_key_prefix` member so this helper stays side-effect-free
        /// and callable from a `const` method (`runFsckOnDormant`).
        String physical_key_prefix;
        /// The resolved (bucket-relative, trailing-slash-trimmed) pool prefix passed into
        /// `Cas::PoolConfig::pool_prefix` -- `startup()`'s S3-staging capability probe below needs the
        /// SAME resolved value to build its own probe key, so it is returned here rather than
        /// recomputed a second time.
        String pool_prefix;
    };

    /// Builds the backend + `Cas::PoolConfig` and opens a pool exactly as `startup()` does, shared by
    /// its live-mount path and `runFsckOnDormant`'s TEMPORARY observe-only scan. `observe_only` forces
    /// `PoolConfig::read_only` and disables the background watermark regardless of the disk's own
    /// `<readonly>` config -- an FSCK scan must never probe/schedule against a pool it does not own the
    /// lifecycle of, exactly like the `read_only` disk path startup() already has (no probe, no
    /// watermark, no GC). Never touches `cas_store`/`part_access`/`gc_scheduler`/`mount_state`/
    /// `physical_key_prefix`/`pool_uuid`/`conditional_copy_supported`; `startup()` applies its own
    /// result to those members itself, in its single publish step.
    PoolView openPoolView(bool observe_only) const;

    /// Classifies `path`'s directory shape by running the fixed dispatch order once (shadow ->
    /// atomic-shard -> table-uuid -> part -> subdir -> generic), including the part-branch
    /// fall-through when no sub-shape matches. Pure path classification — consults no lifecycle
    /// state (e.g. `namespaceFilesReadable`); `existsDirectory`/`listDirectory` apply that gate
    /// themselves in their per-shape arms, exactly as before this refactor.
    DirRoute classifyDirectory(const std::string & path) const;

    /// Build the GC round sink: the std::function the scheduler calls per Start/Finish. Captures the
    /// ContextPtr, converts the POD GcRoundLogRecord into a ContentAddressedGarbageCollectionLogElement,
    /// and appends it to the SystemLog (best-effort). Returns an empty sink when context is null.
    Cas::GcRoundLogger makeGcRoundLogger() const;

    /// Builds the per-event CAS audit sink: the `std::function` the pool calls on every
    /// content-addressed decision. Captures the ContextPtr, converts the decoupled Core POD
    /// `Cas::CasEvent` into a ContentAddressedLogElement, and appends it to the SystemLog
    /// (best-effort). Returns an empty sink when context is null (unit tests).
    Cas::CasEventSink makeCasEventSink() const;
};

}
