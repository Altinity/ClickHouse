#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Interpreters/Context_fwd.h>
#include <base/defines.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Poco::Util { class AbstractConfiguration; }

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
    /// Constructs an unopened storage adapter. `local_scratch_path_` is a real server-local
    /// directory used when a write buffer must spill before hashing and upload; it is independent of
    /// the object-storage key prefix. A non-null `context_` enables the background GC scheduler on
    /// the disk-factory path; tests may pass null to disable system-log integration and scheduling.
    ContentAddressedMetadataStorage(
        ObjectStoragePtr object_storage_,
        String storage_path_prefix_,
        String server_id_,
        String server_root_id_,
        String local_scratch_path_,
        ContextPtr context_ = nullptr,
        bool gc_enabled_ = true,
        std::chrono::seconds gc_interval_ = std::chrono::seconds(60),
        String disk_name_ = {},
        uint64_t dedup_cache_bytes_ = 64ULL << 20,
        uint64_t dedup_head_first_min_bytes_ = 1ULL << 20,
        uint64_t gc_snap_generations_to_keep_ = 3,
        uint64_t gc_shards_ = 1,
        uint64_t manifest_sweep_list_budget_keys_ = 1000,
        uint64_t manifest_sweep_delete_budget_keys_ = 100,
        uint64_t gcs_max_conditional_put_bytes_ = 1ULL << 30,
        uint64_t cas_part_folder_cache_bytes_ = 64ULL << 20,
        uint64_t cas_part_folder_cache_max_entries_ = 10000,
        uint64_t cas_part_folder_cache_max_entry_bytes_ = 16ULL << 20,
        uint64_t manifest_decode_cache_bytes_ = 128ULL << 20,
        /// Bounds the pool used by GC's per-hash freshness-metadata writes.
        uint64_t gc_meta_pool_size_ = 16,
        /// Selects local or opt-in object-store staging. The trailing default preserves the existing
        /// local write path for callers that do not configure S3 staging.
        Cas::StagingBackend staging_backend_ = Cas::StagingBackend::Local,
        /// Selects the pool's blob content-hash function. The default `CityHash128` preserves the
        /// existing key encoding for positional callers.
        Cas::BlobHashAlgo blob_hash_algo_ = Cas::BlobHashAlgo::CityHash128,
        /// Allows `blob_hash_algo_` to be admitted to a pool whose persisted algorithm set does not
        /// contain it. The default is false, so an accidental algorithm mismatch fails closed.
        bool blob_hash_allow_new_ = false,
        /// Passes the per-disk `<skip_access_check>` policy to `Cas::PoolConfig`. The default false
        /// retains the normal boot-time access check.
        bool skip_access_check_ = false,
        /// Configures the grace period used when materializing after an unclean predecessor. The
        /// default matches `Cas::PoolConfig` and preserves existing startup behavior.
        uint64_t materialization_grace_ms_ = 30000,
        /// Configures when retained part-folder views must revalidate their manifest body. The
        /// default `Mode::Always` retains the strict validation behavior used by existing callers.
        Cas::PartFolderValidate part_folder_validate_ = {});

    /// Parses `staging_backend`, defaulting to `local`. Throws `BAD_ARGUMENTS` for an unrecognized
    /// value rather than silently selecting a backend.
    static Cas::StagingBackend parseStagingBackend(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

    /// Parses `part_folder_validate`, defaulting to `always`. The `age` form accepts only a
    /// non-negative integer number of seconds; malformed input and unknown modes throw
    /// `BAD_ARGUMENTS` instead of silently selecting a policy.
    static Cas::PartFolderValidate parsePartFolderValidate(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

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
    /// skip write probes and GC; failures in startup propagate.
    void startup() override;
    /// Stops the GC scheduler before releasing the part-folder facade and pool. Their destruction is
    /// synchronized with the accessors and synchronous GC entry points.
    void shutdown() override;

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

    /// Returns a shared-ownership snapshot of the opened pool. Throws `LOGICAL_ERROR` before `startup`.
    Cas::PoolPtr store() const;
    /// Returns the cached part-folder facade by reference. Throws `LOGICAL_ERROR` before `startup`.
    /// Committed part-folder reads and mutations go through this facade so cache validation remains
    /// centralized. The disk lifecycle contract requires callers not to retain this reference across
    /// a concurrent `shutdown`; the lifecycle mutex only serializes the pointer read with reset.
    Cas::CachedPartFolderAccess & partAccess() const;
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
    bool prepareInManifestRead(const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const;

    /// Translates a blob-backed part file to the physical blob
    /// object plus the payload WINDOW inside it (the CHCA envelope header occupies
    /// [0, payload_offset)). DiskObjectStorage::prepareRead composes the STANDARD object-storage
    /// pipeline over `object` (gather/caches/async prefetch — the same chain plain s3 disks get,
    /// so `MergeTreeReaderStream` right-mark bounds reach the object reader and its range
    /// requests stay drainable) and bounds it with the pipeline's FileView stage.
    /// nullopt = the path is not a blob-backed part file (caller falls through; absent paths
    /// then fail in getStorageObjects exactly as before).
    struct BlobViewPlan
    {
        StoredObject object;        /// physical blob key; logical path; readable extent (envelope + payload)
        size_t payload_offset = 0;  /// view left bound inside the blob
        size_t payload_end = 0;     /// view right bound (payload_offset + payload length)
    };
    /// Resolves a blob-backed path to its physical object and payload window. Returns nullopt for
    /// in-manifest, loose, directory, or otherwise unresolved paths.
    std::optional<BlobViewPlan> getBlobViewPlan(const std::string & path) const;

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

    /// Set by startup (Pool::open is fail-closed; empty store == not started).
    Cas::PoolPtr cas_store;
    /// The part-folder access facade: the normal path
    /// for committed part/projection reads and committed part-ref mutations. Constructed in
    /// startup right after Pool::open; reset in shutdown before cas_store.
    std::unique_ptr<Cas::CachedPartFolderAccess> part_access;
    String pool_uuid;
    std::unique_ptr<Cas::CasGcScheduler> gc_scheduler;
    /// Shared lifecycle mutex for scheduler creation/use/destruction and for pool/facade pointer
    /// reads versus reset. Synchronous rounds hold it for their full duration so teardown waits for a
    /// clean round completion; `gcHealth` follows the same shape.
    mutable std::mutex gc_scheduler_mutex;
    bool shutdown_called TSA_GUARDED_BY(gc_scheduler_mutex) = false;
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
