#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Interpreters/Context_fwd.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Poco::Util { class AbstractConfiguration; }

namespace DB
{

/// Per-disk CAS staging backend selection (design `docs/superpowers/specs/2026-07-11-cas-s3-native-staging-design.md`
/// §4, plan `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md` Task 0). `Local` (the
/// default) is BYTE-FOR-BYTE the current write path (the local `scratchPath` write-buffer spill) —
/// zero behavior change, no capability probe, no new code path taken (global constraint: OFF BY
/// DEFAULT). `S3` opts in to streaming large blobs directly to an S3-native staging key; it is still
/// subject to the mount-time conditional-copy capability probe (a later task), which can fail-close
/// back to `Local` when the object storage does not enforce conditional writes.
enum class StagingBackend
{
    Local,
    S3,
};

/// Content-addressed metadata storage — the M-W wiring (design 2026-06-11 section 4): a THIN
/// translator between the IMetadataStorage read surface and the Cas:: core. ClickHouse path
/// parsing (PartPathParser) + namespace mapping (D-W1) + StoredObjects construction; NO protocol
/// state, NO internals-exposing accessors — the protocol lives in Cas::Store/Build/Gc.
///
/// Namespace mapping (M-W decision D-W1, shadow refined during T2; detached folded in B181):
///   live part      SERVER_ID/TABLE_UUID            ref = PART_DIR
///   detached part  SERVER_ID/TABLE_UUID            ref = detached/DETACHED_PART_DIR
///   FREEZE shadow  the LITERAL shadow table dir     ref = PART_DIR
///                  (shadow/BACKUP/store/U3/UUID or shadow/BACKUP/data/DB/TBL — bijective with
///                  the disk path for both Atomic and non-Atomic layouts, so the shadow tree
///                  enumerates from Store::listNamespaces("shadow/..."))
///   generic files  SERVER_ID/_disk                  verbatim namespace files (access probes)
///
/// Mutable per-part files live IN RefPayload.mutable_files (D-W3) — no sidecar objects. Their
/// bytes (and future inline tree entries) are served through DiskObjectStorage::prepareRead's CA
/// branch via tryGetInManifestBytes; getStorageObjects returns a sized placeholder with an EMPTY
/// remote key for them (any consumer bypassing the prepareRead branch fails loudly, never reads
/// wrong bytes).
class ContentAddressedMetadataStorage final : public IMetadataStorage, public IContentAddressedExchange
{
public:
    /// local_scratch_path_: a real server-local directory for the write-buffer spill (hash before
    /// upload) — never derived from the object-storage key prefix. context_ non-null (the
    /// disk-factory path) enables the background GC scheduler (M-W T10); unit tests pass nullptr.
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
        /// Task 5: bounded pool size for GC's per-hash freshness-meta writes (condemn/spare/delete);
        /// see `PoolConfig::gc_meta_pool_size`.
        uint64_t gc_meta_pool_size_ = 16,
        /// S3-native staging Task 0 (config plumbing, no behavior change): see `StagingBackend`
        /// above. Trailing default keeps every existing positional call site (the disk factory,
        /// the gtests that stop at `context_`) compiling unmodified.
        StagingBackend staging_backend_ = StagingBackend::Local,
        /// CAS pluggable-blob-hash Phase 1 (design 2026-07-11-cas-pluggable-blob-hash-design.md):
        /// the pool's blob content-hash function, threaded into `Cas::PoolConfig` in `startup()`.
        /// Trailing default (`CityHash128`, byte-for-byte today's behavior) keeps every existing
        /// positional call site compiling unmodified.
        Cas::BlobHashAlgo blob_hash_algo_ = Cas::BlobHashAlgo::CityHash128,
        /// CAS mixed-algo pools (Phase 3 T4, design 2026-07-11-cas-mixed-algo-pools-design.md §5):
        /// opt-in for `blob_hash_algo_` to be ADMITTED into the pool's `algos_used` when it is not
        /// already a member. Trailing default `false` (fail-closed) keeps every existing positional
        /// call site compiling unmodified.
        bool blob_hash_allow_new_ = false,
        /// Boot-time "start now, fix later" (see `Cas::PoolConfig::skip_access_check`): read from the
        /// per-disk `<skip_access_check>` config directive by the factory and threaded into
        /// `Cas::PoolConfig` in `startup()`. Trailing default `false` keeps every existing positional
        /// call site compiling unmodified.
        bool skip_access_check_ = false,
        /// rev.6 Task 6: the conditional post-reclaim wait over an unclean predecessor, read from the
        /// per-disk `<materialization_grace_ms>` config directive and threaded into `Cas::PoolConfig`
        /// in `startup()`. See `Cas::PoolConfig::materialization_grace_ms`. Trailing default (the same
        /// 30000 default as the `PoolConfig` field) keeps every existing positional call site compiling
        /// unmodified.
        uint64_t materialization_grace_ms_ = 30000);

    /// Parse `staging_backend` from the CAS disk config (default `local` — the OFF BY DEFAULT
    /// global constraint). Extracted as a static method (rather than inlined into
    /// `MetadataStorageFactory.cpp` like the neighboring `scratch_path` read) so the config-parsing
    /// logic is unit-testable without constructing the full disk factory. Throws BAD_ARGUMENTS on an
    /// unrecognized value (fail closed rather than silently defaulting to `local`).
    static StagingBackend parseStagingBackend(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

    /// Test/diagnostics: one synchronous GC round (creates an ad-hoc scheduler when disabled).
    void runOneGcRoundForTest();

    /// SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION: one synchronous GC round on the caller's thread,
    /// emitting Start + Finish rows to system.content_addressed_garbage_collection_log. Throws
    /// BAD_ARGUMENTS when GC is disabled (read-only disk or gc_enabled=false).
    Cas::RoundReport runGarbageCollectionRoundNow();

    /// SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE]: the gc/state disaster-recovery command (spec
    /// 2026-07-03). Constructs a fresh `Cas::Gc` with a freshly minted gc_id (the same one-shot
    /// pattern as `runGarbageCollectionRoundNow`/`CasGcScheduler::runOneRoundNow`) and calls
    /// `Gc::rebuildBaseline`. A refused rebuild (`report.performed == false`) writes nothing; the
    /// caller (the SYSTEM interpreter) is responsible for surfacing `report.refusal` loudly. Throws
    /// BAD_ARGUMENTS when GC is disabled (read-only disk or gc_enabled=false) — same gate as above.
    Cas::RebuildReport runGcRebuildNow(bool force);

    /// Per-disk GC health for system.content_addressed_mounts (B3). nullopt when no GC scheduler runs
    /// on this disk (GC disabled, read-only, or not started). Holds gc_scheduler_mutex for the whole
    /// call, which serializes it against shutdown()'s gc_scheduler.reset() under the same mutex -- an
    /// unprivileged SELECT on the system table can otherwise race a dangling scheduler mid-teardown.
    std::optional<ContentAddressed::CasGcScheduler::GcHealth> gcHealth() const;

    MetadataStorageType getType() const override { return MetadataStorageType::ContentAddressed; }
    const std::string & getPath() const override { return storage_path_full; }
    bool supportsChmod() const override { return false; }
    bool supportsStat() const override { return false; }
    bool isReadOnly() const override { return read_only; }
    bool isContentAddressed() const override { return true; }
    bool supportsTransactionalMutableFiles() const override { return true; }
    bool areBlobPathsRandom() const override { return false; }
    uint32_t getHardlinkCount(const std::string &) const override { return 0; }

    MetadataTransactionPtr createTransaction() override;

    /// Disk lifecycle (DiskObjectStorage start/stop): open/close the Cas::Store (fail-closed probe
    /// + pool-format check) and drive the GC scheduler.
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
    /// Single-lookup override (spec §Method Routing): the inherited default is existsFile +
    /// getStorageObjects — a two-read trap for CAS on the readFileIfExists path.
    std::optional<StoredObjects> getStorageObjectsIfExist(const std::string & path) const override;

    /// ==== IContentAddressedExchange (DataPartsExchange facade; relink lands in M-W T11) ====
    const String & getPoolUUID() const override { return pool_uuid; }
    std::optional<String> getPartManifestBytes(const String & part_path) const override;
    bool adoptPartFromManifest(
        const String & table_uuid, const String & part_name,
        const String & manifest_bytes, const std::map<String, String> & mutable_files) override;

    /// ==== wiring-internal surface (the transaction + the disk's prepareRead CA branch) ====

    /// The opened pool. Throws LOGICAL_ERROR before startup — every caller is post-startup.
    const Cas::StorePtr & store() const;
    /// The facade, by REFERENCE (dot-syntax call sites — the committed-ref style guard bans raw
    /// `->` mutation tokens in wiring). Throws LOGICAL_ERROR before startup, like store().
    ContentAddressed::CachedPartFolderAccess & partAccess() const;
    const std::string & serverId() const { return server_id; }
    const std::string & serverRootId() const { return server_root_id; }
    const std::string & scratchPath() const { return local_scratch_path; }
    /// S3-native staging Task 0 accessor — pure config plumbing, no behavior change. `writeFile`
    /// starts consulting this in a later task (Task 3/4 of the plan); today it is stored-but-unread.
    StagingBackend stagingBackend() const { return staging_backend; }
    /// Set by the mount-time conditional-copy capability probe (a later task). Defaults to `false`:
    /// conditional copy is assumed UNSUPPORTED until proven otherwise (fail-close), so consulting this
    /// accessor before the probe wiring lands can never mistakenly enable the S3 promote path.
    bool conditionalCopySupported() const { return conditional_copy_supported; }
    /// S3-native staging Task 4: raw access to the object storage, so `writeFile` can open a
    /// staging-object sink directly with `object_storage->writeObject(...)` — Task 4 predates the
    /// `Cas::Backend` seams (`Core/CasBackend.h::stageStream`) a later task adds. Only meaningful
    /// when `stagingBackend()==StagingBackend::S3 && conditionalCopySupported()`.
    const ObjectStoragePtr & objectStorage() const { return object_storage; }
    /// S3-native staging Task 4: the physical staging-key PREFIX for this pool's writer-owned staging
    /// area — `physicalKey(pool_prefix + "/staging/" + server_root_id)`, the SAME prefix construction
    /// the Task 3 capability probe uses for its own `<prefix>/probe` subtree (see `startup()`).
    /// Callers append their own unique leaf, e.g. `"/" + getRandomASCIIString(32) + ".tmp"`.
    String stagingKeyPrefix() const;

    /// Bytes that live INSIDE pool metadata rather than as their own object: a mutable per-part
    /// file's bytes from RefPayload.mutable_files, an Inline-placement tree entry, or a verbatim
    /// namespace file. nullopt = the path is blob-backed (a real storage object).
    std::optional<String> tryGetInManifestBytes(const std::string & path) const;

    /// The CA read entry, part 1 of 2, called by DiskObjectStorage::prepareRead BEFORE the generic
    /// storage-objects path: serves in-manifest bytes (mutable per-part files, inline entries,
    /// verbatim namespace files) from memory. Returns false when the path is not in-manifest.
    bool prepareInManifestRead(const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const;

    /// The CA read entry, part 2 of 2: a blob-backed part file translates to the physical blob
    /// object plus the payload WINDOW inside it (the CHCA envelope header occupies
    /// [0, payload_offset)). DiskObjectStorage::prepareRead composes the STANDARD object-storage
    /// pipeline over `object` (gather/caches/async prefetch — the same chain plain s3 disks get,
    /// so `MergeTreeReaderStream` right-mark bounds reach the object reader and its range
    /// requests stay drainable, B116) and bounds it with the pipeline's FileView stage.
    /// nullopt = the path is not a blob-backed part file (caller falls through; absent paths
    /// then fail in getStorageObjects exactly as before).
    struct BlobViewPlan
    {
        StoredObject object;        /// physical blob key; logical path; readable extent (envelope + payload)
        size_t payload_offset = 0;  /// view left bound inside the blob
        size_t payload_end = 0;     /// view right bound (payload_offset + payload length)
    };
    std::optional<BlobViewPlan> getBlobViewPlan(const std::string & path) const;

    /// A seekable reader over ONE blob payload (the object minus its envelope header) - the
    /// transaction's in-flight read-your-writes (B59). The committed read path goes through
    /// getBlobViewPlan + the pipeline instead.
    std::unique_ptr<ReadBufferFromFileBase> readBlobPayload(
        const Cas::BlobLocation & location, const std::string & path, const ReadSettings & settings) const;

    /// D-W1 namespace mapping (shared with the transaction — ONE definition). Detached parts (B181)
    /// live INSIDE this same table namespace as `detached/`-prefixed refs, not a sibling namespace.
    Cas::RootNamespace liveNamespace(const std::string & table_uuid) const;
    static Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir);

    /// A dropped-and-not-recreated table (ref-table lifecycle durably `Removed`) is GONE for readers: its
    /// table-level namespace files — `format_version.txt` and other verbatim files — must read as absent
    /// even while GC has not yet physically reclaimed them (deferred-GC removal, commit 318291fe5e5),
    /// mirroring how its parts already vanish via the ref state. Gate every namespace-file read on this.
    /// A never-born namespace is NOT hidden (fail-closed — only a KNOWN-removed table hides its files).
    bool namespaceFilesReadable(const Cas::RootNamespace & ns) const;

    /// The configured live-tree root prefix. Phase 1 makes layout identity explicit:
    /// live/detached namespaces and verbatim live-tree files are rooted by `server_root_id`, while
    /// `ServerUUID` remains only the mount owner token.
    std::string serverPrefix() const;
    // (genericNamespace removed — loose files are plain mountpoint objects, design §5.2)

    /// Enumerate the children of a GENERIC intermediate live-tree directory (the disk root "",
    /// `store`, the `store/<u3>` shard dir, or any loose-file container above a table dir) via a
    /// server-root-scoped mirrored S3 LIST of `roots/<server_root_id>/<path>/`. `@cas@`-suffixed table-dir
    /// segments are surfaced under their logical (unsuffixed) name. This is what makes top-down
    /// `clickhouse-disks` traversal of the live tree behave like a normal disk; concrete
    /// `store/<u3>/<uuid>/<part>/<file>` navigation is still served by the exact-shape branches.
    std::vector<std::string> listLiveTreeChildren(const std::string & path) const;
    bool liveTreeDirHasChildren(const std::string & path) const;

    /// The route of one parsed CA path: which namespace, which ref, which in-tree file. The single
    /// place the detached re-split (PoC B36 parser contract -> in-namespace `detached/`-prefixed
    /// refs, B181) and the shadow mapping happen.
    struct Route
    {
        Cas::RootNamespace ns{""};
        /// empty => the path is the namespace's container dir. For a detached part this is
        /// `detached/<part>` (a ref inside the table namespace, not a separate namespace).
        std::string ref;
        std::string file;   /// empty => the path is the part dir itself

        /// The (ns, ref) identity subset — what the part-folder access layer keys on.
        ContentAddressed::PartRefKey refKey() const { return {ns, ref}; }
    };
    std::optional<Route> route(const ContentAddressed::PartFilePath & p) const;

    /// Full `detached/<part>` ref names in a namespace (B181: detached parts fold into the table ns).
    std::vector<std::string> detachedRefNames(const Cas::RootNamespace & ns) const;

    /// C4: the ONE fixed dispatch order `existsDirectory`/`listDirectory` route a path through
    /// (shadow -> atomic-shard -> table-uuid -> part -> subdir -> generic), previously implemented
    /// twice and kept in sync by hand. `classifyDirectory` (private, below) computes it once; both
    /// callers then switch on the resulting shape. `DirShape`/`DirRoute` are nested-public only so
    /// `classifyDirectoryForTest` can name them from the test binary (mirroring the `Route` struct
    /// above); the classification logic itself stays private.
    enum class DirShape
    {
        ShadowPart,
        ShadowTable,
        ShadowIntermediate,
        AtomicShard,
        TableDir,
        DetachedContainer,
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
        std::optional<ContentAddressed::PartFilePath> p;
        std::optional<Route> r;
        std::optional<std::string> uuid;
        std::optional<ContentAddressed::TableFilePath> tf;
        std::optional<std::string> projection_prefix;
    };

    /// Test-only accessor (mirrors `conditionalWriteSettingsForTest`): exposes the private
    /// `classifyDirectory` so `gtest_ca_wiring` can pin the fixed dispatch order directly.
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
    const uint64_t gc_snap_generations_to_keep;  /// B174 gc/snap retention (0=keep all)
    const uint64_t gc_shards;                    /// Phase 4: blob-hash-prefix reducer shard count (creation-time only)
    const uint64_t manifest_sweep_list_budget_keys;
    const uint64_t manifest_sweep_delete_budget_keys;
    /// GCS single-PUT budget for conditional writes (generation-token stores only): threaded into
    /// the ObjectStorageBackend construction site in startup(). Irrelevant on ETag stores (AWS et al).
    const uint64_t gcs_max_conditional_put_bytes;
    /// Part-folder view cache settings (spec 2026-07-08-cas-part-folder-cache), threaded into the
    /// facade construction in startup(). `cas_part_folder_cache_bytes == 0` disables retention.
    const uint64_t cas_part_folder_cache_bytes;
    const uint64_t cas_part_folder_cache_max_entries;
    const uint64_t cas_part_folder_cache_max_entry_bytes;
    /// Phase 5 (part-folder cache spec): byte bound for the manifest DECODE cache (Cas::Store), threaded
    /// into PoolConfig in startup(). 0 disables decode caching entirely (diagnostic mode).
    const uint64_t manifest_decode_cache_bytes;
    /// Task 5: bounded pool size for GC's per-hash freshness-meta writes, threaded into PoolConfig.
    const uint64_t gc_meta_pool_size;
    /// S3-native staging Task 0 (config plumbing, no behavior change): see `StagingBackend` above.
    const StagingBackend staging_backend;
    /// CAS pluggable-blob-hash Phase 1: the pool's blob content-hash function, threaded into
    /// `Cas::PoolConfig` in `startup()`.
    const Cas::BlobHashAlgo blob_hash_algo;
    /// CAS mixed-algo pools (Phase 3 T4): opt-in for `blob_hash_algo` to be ADMITTED into the pool's
    /// `algos_used`, threaded into `Cas::PoolConfig` in `startup()`.
    const bool blob_hash_allow_new;
    /// Boot-time "start now, fix later": the per-disk `<skip_access_check>` directive, threaded into
    /// `Cas::PoolConfig` in `startup()`. See `Cas::PoolConfig::skip_access_check`.
    const bool skip_access_check;
    /// rev.6 Task 6: the per-disk `<materialization_grace_ms>` directive, threaded into `Cas::PoolConfig`
    /// in `startup()`. See `Cas::PoolConfig::materialization_grace_ms`.
    const uint64_t materialization_grace_ms;
    /// Set later by the mount-time conditional-copy capability probe (a later task) — NOT const.
    /// Defaults to false (fail-close): assumed unsupported until the probe proves otherwise.
    bool conditional_copy_supported = false;

    /// Set by startup (Store::open is fail-closed; empty store == not started).
    Cas::StorePtr cas_store;
    /// The part-folder access facade (spec 2026-07-08-cas-part-folder-cache): the ONLY normal path
    /// for committed part/projection reads and committed part-ref mutations. Constructed in
    /// startup right after Store::open; reset in shutdown before cas_store.
    std::unique_ptr<ContentAddressed::CachedPartFolderAccess> part_access;
    String pool_uuid;
    std::unique_ptr<ContentAddressed::CasGcScheduler> gc_scheduler;
    /// Guards the lazy `gc_scheduler` creation in `runOneGcRoundForTest`/`runGarbageCollectionRoundNow`
    /// against a racing manual `SYSTEM ... GC` on another query thread; the round itself runs OUTSIDE
    /// this lock (CasGcScheduler::runOneRoundNow has its own gc_round_mutex for that). Also taken around
    /// `shutdown()`'s `gc_scheduler.reset()` (NOT its `stop()`) and, `mutable`, for the FULL duration of
    /// the const `gcHealth()` accessor (B3) -- together these two make an unprivileged SELECT on
    /// system.content_addressed_mounts race-free against a concurrent disk shutdown. The lazy-creation
    /// call sites do NOT re-check for a post-shutdown state, so they can still resurrect a scheduler
    /// after `shutdown()` has run (pre-existing, out of scope here).
    mutable std::mutex gc_scheduler_mutex;
    /// Derived from object_storage->isReadOnly() at startup (the disk's <readonly> config). When set:
    /// the probe is skipped, no watermark, no GC scheduler, and the mutating surface fails closed.
    bool read_only = false;
    /// Joined in front of core keys for DIRECT object_storage reads. The Emulated (Local) backend
    /// maps bare pool keys under getCommonKeyPrefix; Native passes keys through - this member
    /// mirrors that rule so readBlobPayload reads exactly where the backend wrote ("" for Native).
    String physical_key_prefix;

    String physicalKey(const String & key) const
    {
        if (physical_key_prefix.empty())
            return key;
        if (physical_key_prefix.back() == '/')
            return physical_key_prefix + key;
        return physical_key_prefix + "/" + key;
    }

    /// C4: classify `path`'s directory shape by running the fixed dispatch order ONCE (shadow ->
    /// atomic-shard -> table-uuid -> part -> subdir -> generic), including the part-branch
    /// fall-through when no sub-shape matches. Pure path classification — consults no lifecycle
    /// state (e.g. `namespaceFilesReadable`); `existsDirectory`/`listDirectory` apply that gate
    /// themselves in their per-shape arms, exactly as before this refactor.
    DirRoute classifyDirectory(const std::string & path) const;

    /// Build the GC round sink: the std::function the scheduler calls per Start/Finish. Captures the
    /// ContextPtr, converts the POD GcRoundLogRecord into a ContentAddressedGarbageCollectionLogElement,
    /// and appends it to the SystemLog (best-effort). Returns an empty sink when context is null.
    ContentAddressed::GcRoundLogger makeGcRoundLogger() const;

    /// Build the per-event CAS audit sink (B170): the std::function the Store calls on every
    /// content-addressed decision. Captures the ContextPtr, converts the decoupled Core POD
    /// `Cas::CasEvent` into a ContentAddressedLogElement, and appends it to the SystemLog
    /// (best-effort). Returns an empty sink when context is null (unit tests).
    Cas::CasEventSink makeCasEventSink() const;
};

}
