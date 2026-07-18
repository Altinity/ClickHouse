#include <Common/assert_cast.h>
#include <Common/Macros.h>
#include <Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.h>
#include <Disks/DiskObjectStorage/MetadataStorages/Local/MetadataStorageFromDisk.h>
#if CLICKHOUSE_CLOUD
    #include <Disks/DiskObjectStorage/MetadataStorages/Keeper/MetadataStorageFromKeeper.h>
#endif
#include <Disks/DiskObjectStorage/MetadataStorages/Plain/MetadataStorageFromPlainObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/Web/MetadataStorageFromStaticFilesWebServer.h>
#include <Disks/DiskLocal.h>
#include <Core/ServerUUID.h>
#include <Interpreters/Context.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NO_ELEMENTS_IN_CONFIG;
    extern const int UNKNOWN_ELEMENT_IN_CONFIG;
    extern const int INVALID_CONFIG_PARAMETER;
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

void checkSingleLocation(const ClusterConfigurationPtr & cluster)
{
    if (cluster->getConfiguration().size() > 1)
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Disk supports only single location clusters");
}

std::string getObjectKeyCompatiblePrefix(
    const ObjectStoragePtr & object_storage,
    const Poco::Util::AbstractConfiguration & config,
    const String & config_prefix)
{
    std::string prefix = config.getString(config_prefix + ".key_compatibility_prefix", object_storage->getCommonKeyPrefix());
    Macros::MacroExpansionInfo info;
    info.ignore_unknown = true;
    info.expand_special_macros_only = true;
    info.replica = Context::getGlobalContextInstance()->getMacros()->tryGetValue("replica");
    return Context::getGlobalContextInstance()->getMacros()->expand(prefix, info);
}

}

MetadataStorageFactory & MetadataStorageFactory::instance()
{
    static MetadataStorageFactory factory;
    return factory;
}

void MetadataStorageFactory::registerMetadataStorageType(const std::string & metadata_type, Creator creator)
{
    if (!registry.emplace(metadata_type, creator).second)
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "MetadataStorageFactory: the metadata type '{}' is not unique",
                        metadata_type);
    }
}

std::string MetadataStorageFactory::getCompatibilityMetadataTypeHint(
    const ClusterConfigurationPtr & cluster,
    const ObjectStorageRouterPtr & object_storages)
{
    switch (object_storages->takePointingTo(cluster->getLocalLocation())->getType())
    {
        case ObjectStorageType::S3:
        case ObjectStorageType::HDFS:
        case ObjectStorageType::Local:
        case ObjectStorageType::Azure:
            return "local";
        case ObjectStorageType::Web:
            return "web";
        default:
            return "";
    }
}

std::string MetadataStorageFactory::getMetadataType(
    const Poco::Util::AbstractConfiguration & config,
    const std::string & config_prefix,
    const std::string & compatibility_type_hint)
{
    if (compatibility_type_hint.empty() && !config.has(config_prefix + ".metadata_type"))
    {
        throw Exception(ErrorCodes::NO_ELEMENTS_IN_CONFIG, "Expected `metadata_type` in config");
    }

    return config.getString(config_prefix + ".metadata_type", compatibility_type_hint);
}

MetadataStoragePtr MetadataStorageFactory::create(
    const std::string & name,
    const Poco::Util::AbstractConfiguration & config,
    const std::string & config_prefix,
    const ClusterConfigurationPtr & cluster,
    const ObjectStorageRouterPtr & object_storages,
    const std::string & compatibility_type_hint) const
{
    const auto type = getMetadataType(config, config_prefix, compatibility_type_hint);
    const auto it = registry.find(type);

    if (it == registry.end())
    {
        throw Exception(ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG,
                        "MetadataStorageFactory: unknown metadata storage type: {}", type);
    }

    return it->second(name, config, config_prefix, cluster, object_storages);
}

static void registerMetadataStorageFromDisk(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("local", [](
        const std::string & name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        checkSingleLocation(cluster);

        auto metadata_path = config.getString(config_prefix + ".metadata_path",
                                              fs::path(Context::getGlobalContextInstance()->getPath()) / "disks" / name / "");
        auto metadata_keep_free_space_bytes = config.getUInt64(config_prefix + ".metadata_keep_free_space_bytes", 0);

        fs::create_directories(metadata_path);
        const auto db_disk = std::make_shared<DiskLocal>(name + "-metadata", metadata_path, metadata_keep_free_space_bytes, config, config_prefix);
        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        auto key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);
        auto key_generator = local_object_storage->createKeyGenerator();

        bool persistent_removal_log = config.getBool(config_prefix + ".persistent_removal_log", false);
        size_t metadata_removal_log_compaction_threshold = config.getUInt64(config_prefix + ".metadata_removal_log_compaction_threshold", 1000);
        return std::make_shared<MetadataStorageFromDisk>(db_disk, std::move(key_compatibility_prefix), std::move(key_generator), persistent_removal_log, metadata_removal_log_compaction_threshold);
    });
}

#if CLICKHOUSE_CLOUD
static void registerMetadataStorageFromKeeper(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("keeper", [](
        const std::string & name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        auto component_guard = Coordination::setCurrentComponent("registerMetadataStorageFromKeeper");
        LOG_INFO(getLogger("registerDiskS3"), "Using DiskS3 with metadata keeper");

        std::string zookeeper_name = config.getString(config_prefix + ".zookeeper_name", "default");
        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        const auto key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);
        const auto key_generator = local_object_storage->createKeyGenerator();
        /// Yes, we place objects in metadata storage from keeper by prefix from s3 object keys.
        /// No reason, it just happened. Now it has to be preserved.
        auto keeper_prefix = key_compatibility_prefix;

        return std::make_shared<MetadataStorageFromKeeper>(
            name, zookeeper_name, keeper_prefix, key_compatibility_prefix, key_generator, config, config_prefix, Context::getGlobalContextInstance());
    });
}
#endif

static void registerPlainMetadataStorage(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("plain", [](
        const std::string & /* name */,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        checkSingleLocation(cluster);

        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        std::string key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);
        size_t object_metadata_cache_size = config.getUInt64(config_prefix + ".object_metadata_cache_size", 0);

        return std::make_shared<MetadataStorageFromPlainObjectStorage>(local_object_storage, key_compatibility_prefix, object_metadata_cache_size);
    });
}

static void registerPlainRewritableMetadataStorage(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("plain_rewritable", [](
        const std::string & /* name */,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        checkSingleLocation(cluster);

        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        std::string key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);

        return std::make_shared<MetadataStorageFromPlainRewritableObjectStorage>(local_object_storage, key_compatibility_prefix);
    });
}

static void registerContentAddressedMetadataStorage(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("content_addressed", [](
        const std::string & name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        checkSingleLocation(cluster);

        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        std::string key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);

        /// Server-local scratch dir for the write-buffer spill. Mirrors how other metadata storages
        /// compute their local working dir (see the `local` registration above): it is a real
        /// filesystem path under the server data path, NEVER the object-storage key prefix (which for
        /// a remote object storage such as s3 is a remote key prefix and not a usable local path).
        auto local_scratch_path = config.getString(config_prefix + ".scratch_path",
                                                    fs::path(Context::getGlobalContextInstance()->getPath()) / "disks" / name / "cas_scratch" / "");
        /// A configured RELATIVE scratch path must be anchored to the server data path, NOT the
        /// process CWD (which varies by launch method) — otherwise the write-buffer spill lands in an
        /// unpredictable directory and orphans across restarts (review #1). The default is already
        /// absolute; only an explicit relative override needs anchoring.
        if (fs::path(local_scratch_path).is_relative())
            local_scratch_path = fs::path(Context::getGlobalContextInstance()->getPath()) / local_scratch_path;
        fs::create_directories(local_scratch_path);

        /// The incarnation-token pool is multi-writer by design (spec section 2): the publish gate +
        /// fence/recheck handshake make shared pools safe, and the GC lease dedups leaders — the old
        /// single-owner claim and its allow_shared_pool opt-in are gone (M-W D-W5/D-W6).
        auto global_context = Context::getGlobalContextInstance();
        const bool gc_enabled = config.getBool(config_prefix + ".gc_enabled", true);
        const uint64_t gc_interval_sec = config.getUInt64(config_prefix + ".gc_interval_sec", 60);
        const auto gc_interval = std::chrono::seconds(gc_interval_sec);
        /// CAS pluggable-blob-hash Phase 1/2 (design 2026-07-11-cas-pluggable-blob-hash-design.md §2):
        /// `blob_hash` selects the pool's blob content-hash function; default `cityhash128` keeps
        /// today's behavior byte-for-byte unchanged. `parseBlobHashAlgo` fails closed on an unknown
        /// spelling. `sha256` is now fully usable (Phase 2 Task 6 finished the variable-length digest
        /// write path: `objectKey`/`Build`'s dep map/`putBlob`/`logical_hash`/the event-log hash render/
        /// the inline-candidate hash all route through the pool-scoped `DigestCodec` at the pool's real
        /// width). The choice is fixed at pool creation; `PoolMeta::createOrValidate` is pool-authoritative
        /// on reopen and fails closed (BAD_ARGUMENTS) when this disagrees with the recorded algo.
        const Cas::BlobHashAlgo blob_hash_algo = Cas::parseBlobHashAlgo(config.getString(config_prefix + ".blob_hash", "cityhash128"));
        /// CAS mixed-algo pools (Phase 3 T4, design 2026-07-11-cas-mixed-algo-pools-design.md §5):
        /// admission of a NEW algo into an existing pool's `algos_used` is explicit opt-in -- a
        /// reopen whose `blob_hash` disagrees with the pool's recorded set fails closed
        /// (BAD_ARGUMENTS) unless this is set. Default 0 (fail-closed): a changed config alone must
        /// never silently turn a pool mixed.
        const bool blob_hash_allow_new = config.getBool(config_prefix + ".blob_hash_allow_new", false);
        /// Boot-time "start now, fix later" (see `Cas::PoolConfig::skip_access_check`): unlike the
        /// generic `IDisk::startup(bool)` global flag, this per-disk directive is read directly here
        /// because `IDisk::startupImpl()` drops the flag before `metadata_storage->startup()` runs.
        const bool skip_access_check = config.getBool(config_prefix + ".skip_access_check", false);
        const uint64_t dedup_cache_bytes = config.getUInt64(config_prefix + ".dedup_cache_bytes", 64ULL << 20);
        const uint64_t dedup_head_first_min_bytes = config.getUInt64(config_prefix + ".dedup_head_first_min_bytes", 1ULL << 20);
        const uint64_t gc_snap_generations_to_keep = config.getUInt64(config_prefix + ".gc_snap_generations_to_keep", 3);
        /// Phase 4: blob-hash-prefix reducer sharding. Default 1 (single-shard, identical to Phase 1d).
        /// Creation-time only: the pool's persisted GcState is authoritative on reopen.
        const uint64_t gc_shards = config.getUInt64(config_prefix + ".gc_shards", 1);
        if (gc_interval_sec == 0 || gc_shards == 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "content_addressed disk '{}': gc_interval_sec and gc_shards must be >= 1 (got {}, {})",
                name, gc_interval_sec, gc_shards);
        const uint64_t manifest_sweep_list_budget_keys = config.getUInt64(config_prefix + ".manifest_sweep_list_budget_keys", 1000);
        const uint64_t manifest_sweep_delete_budget_keys = config.getUInt64(config_prefix + ".manifest_sweep_delete_budget_keys", 100);
        /// Phase 0 (mount safety): the layout subtree identity is explicit and REQUIRED — no default,
        /// so a missing key throws a typed NO_ELEMENTS_IN_CONFIG exception (mirroring the `metadata_type`
        /// check above). `ServerUUID` is demoted to an owner token; the subtree a server owns is named
        /// by `server_root_id`. Validated immediately (fail closed). Macros expand here exactly as in
        /// the s3 `endpoint` (ObjectStorageFactory): on a multi-replica stand every replica mounts ONE
        /// shared pool (same endpoint) and must own a DISTINCT subtree, so the natural single-template
        /// config is `<server_root_id>{replica}</server_root_id>`. An unknown macro throws (fail closed).
        if (!config.has(config_prefix + ".server_root_id"))
            throw Exception(ErrorCodes::NO_ELEMENTS_IN_CONFIG,
                "Expected `server_root_id` in config for a content-addressed disk");
        const std::string server_root_id = global_context->getMacros()->expand(
            config.getString(config_prefix + ".server_root_id"));
        Cas::validateServerRootId(server_root_id);
        /// GCS single-PUT budget for conditional writes (generation-token stores only): the body
        /// is RAM-buffered up to this size; a bigger conditional write throws NOT_IMPLEMENTED
        /// (the compose-based path is a follow-up). Irrelevant on ETag stores (AWS et al).
        const uint64_t gcs_max_conditional_put_bytes = config.getUInt64(config_prefix + ".gcs_max_conditional_put_bytes", 1ULL << 30);
        /// Part-folder view cache (spec 2026-07-08-cas-part-folder-cache): ON by default;
        /// `part_folder_cache_bytes = 0` disables retention — a supported permanent
        /// operational configuration (the runbook disable switch), not only a debug aid.
        /// Config-key convention: the `content_addressed` block already scopes every key to this disk,
        /// so no key carries a redundant `cas_`/`ca_` prefix.
        const uint64_t part_folder_cache_bytes = config.getUInt64(config_prefix + ".part_folder_cache_bytes", 64ULL << 20);
        const uint64_t part_folder_cache_max_entries = config.getUInt64(config_prefix + ".part_folder_cache_max_entries", 10000);
        const uint64_t part_folder_cache_max_entry_bytes = config.getUInt64(config_prefix + ".part_folder_cache_max_entry_bytes", 16ULL << 20);
        /// §3 (spec 2026-07-13-cas-memory-s3-budget-optimizations-design.md): the ForceFresh body
        /// re-proof HEAD validation policy. Default `always` is byte-for-byte pre-§3 behavior --
        /// EVERY ForceFresh re-proves the manifest body via the mandatory HEAD.
        const auto part_folder_validate = ContentAddressedMetadataStorage::parsePartFolderValidate(config, config_prefix);
        /// Phase-5 (part-folder cache spec): byte bound for the manifest DECODE cache (Cas::Pool).
        /// `manifest_decode_cache_bytes = 0` disables decode caching entirely (diagnostic mode).
        const uint64_t manifest_decode_cache_bytes = config.getUInt64(config_prefix + ".manifest_decode_cache_bytes", 128ULL << 20);
        /// Task 5: bounded pool size for GC's per-hash freshness-meta writes (condemn/spare/delete) —
        /// a mass-DROP condemning ~1M blobs sequentially would take hours; see `PoolConfig::gc_meta_pool_size`.
        const uint64_t gc_meta_pool_size = config.getUInt64(config_prefix + ".gc_meta_pool_size", 16);
        /// rev.6 Task 6 (spec §Late Predecessor PUT): the conditional post-reclaim wait `Pool::open`
        /// pays over an unclean predecessor; see `Cas::PoolConfig::materialization_grace_ms`.
        const uint64_t materialization_grace_ms = config.getUInt64(config_prefix + ".materialization_grace_ms", 30000);
        /// S3-native staging (design 2026-07-11-cas-s3-native-staging-design.md §4, plan Task 0):
        /// `staging_backend` defaults to `local` — BYTE-FOR-BYTE the current write path, zero
        /// behavior change, no probe, no new code path taken (global constraint: OFF BY DEFAULT).
        const auto staging_backend = ContentAddressedMetadataStorage::parseStagingBackend(config, config_prefix);
        auto metadata_storage = std::make_shared<ContentAddressedMetadataStorage>(
            local_object_storage, key_compatibility_prefix, toString(ServerUUID::get()), server_root_id, local_scratch_path,
            global_context, gc_enabled, gc_interval, name, dedup_cache_bytes, dedup_head_first_min_bytes,
            gc_snap_generations_to_keep, gc_shards, manifest_sweep_list_budget_keys, manifest_sweep_delete_budget_keys,
            gcs_max_conditional_put_bytes,
            part_folder_cache_bytes, part_folder_cache_max_entries, part_folder_cache_max_entry_bytes,
            manifest_decode_cache_bytes, gc_meta_pool_size, staging_backend, blob_hash_algo, blob_hash_allow_new,
            skip_access_check, materialization_grace_ms, part_folder_validate);

        return metadata_storage;
    });
}

static void registerMetadataStorageFromStaticFilesWebServer(MetadataStorageFactory & factory)
{
    factory.registerMetadataStorageType("web", [](
        const std::string & /* name */,
        const Poco::Util::AbstractConfiguration & /* config */,
        const std::string & /* config_prefix */,
        const ClusterConfigurationPtr & cluster,
        const ObjectStorageRouterPtr & object_storages) -> MetadataStoragePtr
    {
        checkSingleLocation(cluster);

        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());

        return std::make_shared<MetadataStorageFromStaticFilesWebServer>(assert_cast<const WebObjectStorage &>(*local_object_storage));
    });
}

void registerMetadataStorages();

void registerMetadataStorages()
{
    auto & factory = MetadataStorageFactory::instance();
    registerMetadataStorageFromDisk(factory);
    registerPlainMetadataStorage(factory);
    registerPlainRewritableMetadataStorage(factory);
    registerContentAddressedMetadataStorage(factory);
    registerMetadataStorageFromStaticFilesWebServer(factory);
#if CLICKHOUSE_CLOUD
    registerMetadataStorageFromKeeper(factory);
#endif
}

void MetadataStorageFactory::clearRegistry()
{
    registry.clear();
}
}
