#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Interpreters/Context_fwd.h>
#include <memory>
#include <optional>
#include <string>

namespace DB
{

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
        String local_scratch_path_,
        ContextPtr context_ = nullptr,
        bool gc_enabled_ = true,
        std::chrono::seconds gc_interval_ = std::chrono::seconds(60),
        uint64_t root_shards_ = 8,
        String disk_name_ = {});

    /// Test/diagnostics: one synchronous GC round (creates an ad-hoc scheduler when disabled).
    void runOneGcRoundForTest();

    /// SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION: one synchronous GC round on the caller's thread,
    /// emitting Start + Finish rows to system.content_addressed_garbage_collection_log. Throws
    /// BAD_ARGUMENTS when GC is disabled (read-only disk or gc_enabled=false).
    Cas::RoundReport runGarbageCollectionRoundNow();

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

    /// ==== IContentAddressedExchange (DataPartsExchange facade; relink lands in M-W T11) ====
    const String & getPoolUUID() const override { return pool_uuid; }
    std::optional<String> getPartTreeId(const String & part_path) const override;
    bool adoptPart(
        const String & table_uuid, const String & part_name,
        const String & tree_id_hex, const std::map<String, String> & mutable_files) override;

    /// ==== wiring-internal surface (the transaction + the disk's prepareRead CA branch) ====

    /// The opened pool. Throws LOGICAL_ERROR before startup — every caller is post-startup.
    const Cas::StorePtr & store() const;
    const std::string & serverId() const { return server_id; }
    const std::string & scratchPath() const { return local_scratch_path; }

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

    /// The canonical server prefix used for server-scoped namespaces: the 32-hex
    /// `u128ToHex(serverIdToU128(server_id))` — the SAME token GC uses for its watermark and
    /// precommit namespace (`precommitNs`, `serverWatermarkKey`). A server's mutable control state
    /// (live/detached namespaces, watermark, precommits) all live under this one token so dropping a
    /// server is one `roots/<server-hex>/` subtree.
    std::string serverPrefix() const;
    // (genericNamespace removed — loose files are plain mountpoint objects, design §5.2)

    /// Enumerate the children of a GENERIC intermediate live-tree directory (the disk root "",
    /// `store`, the `store/<u3>` shard dir, or any loose-file container above a table dir) via a
    /// server-scoped mirrored S3 LIST of `roots/<server-hex>/<path>/`. `@cas@`-suffixed table-dir
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
    };
    std::optional<Route> route(const ContentAddressed::PartFilePath & p) const;

private:
    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
    const std::string disk_name;
    const std::string local_scratch_path;
    const ContextPtr context;

    const bool gc_enabled;
    const std::chrono::seconds gc_interval;
    const uint64_t root_shards;   /// creation-time shard fanout (#4); applied in startup()'s Store::open

    /// Set by startup (Store::open is fail-closed; empty store == not started).
    Cas::StorePtr cas_store;
    String pool_uuid;
    std::unique_ptr<ContentAddressed::CasGcScheduler> gc_scheduler;
    /// Derived from object_storage->isReadOnly() at startup (the disk's <readonly> config). When set:
    /// the probe is skipped, no heartbeats, no GC scheduler, and the mutating surface fails closed.
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

    /// resolveRef + readTree for a routed path; nullopt when the ref is absent. Throws on a
    /// present-but-corrupt tree (fail closed, INV-NO-DANGLE surfaced).
    std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>
    resolveRouted(const Route & r) const;

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
