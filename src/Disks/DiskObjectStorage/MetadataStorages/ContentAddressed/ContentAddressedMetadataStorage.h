#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
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
/// Namespace mapping (M-W decision D-W1, shadow refined during T2):
///   live part      SERVER_ID/TABLE_UUID            ref = PART_DIR
///   detached part  SERVER_ID/detached/TABLE_UUID   ref = DETACHED_PART_DIR
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
        ContextPtr context_ = nullptr);

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

    /// The CA read entry, called by DiskObjectStorage::prepareRead BEFORE the generic
    /// storage-objects path: serves in-manifest bytes from memory, and blob-backed part files
    /// through a payload VIEW over the object (the CHCA envelope header is skipped via
    /// ReadBufferFromFileView — StoredObject carries no range, the recorded upstream delta).
    /// Returns false when the path has no CA-specific read plan (caller falls through; absent
    /// paths then fail in getStorageObjects exactly as before). NOTE: this path bypasses the
    /// pipeline cache stages — the CA disk runs cacheless, as the PoC did (B86).
    bool prepareReadPipeline(const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const;

    /// D-W1 namespace mapping (shared with the transaction — ONE definition).
    Cas::RootNamespace liveNamespace(const std::string & table_uuid) const;
    Cas::RootNamespace detachedNamespace(const std::string & table_uuid) const;
    static Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir);
    Cas::RootNamespace genericNamespace() const;

    /// The route of one parsed CA path: which namespace, which ref, which in-tree file. The single
    /// place the detached re-split (PoC B36 parser contract -> per-part detached refs) and the
    /// shadow mapping happen.
    struct Route
    {
        Cas::RootNamespace ns{""};
        std::string ref;    /// empty => the path is the namespace's container dir
        std::string file;   /// empty => the path is the part dir itself
    };
    std::optional<Route> route(const ContentAddressed::PartFilePath & p) const;

private:
    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
    const std::string local_scratch_path;
    const ContextPtr context;

    /// Set by startup (Store::open is fail-closed; empty store == not started).
    Cas::StorePtr cas_store;
    String pool_uuid;

    /// resolveRef + readTree for a routed path; nullopt when the ref is absent. Throws on a
    /// present-but-corrupt tree (fail closed, INV-NO-DANGLE surfaced).
    std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>
    resolveRouted(const Route & r) const;
};

}
