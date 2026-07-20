#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
#include <Disks/IDisk.h>
#include <IO/ReadBufferFromFileView.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadPipeline.h>
#include <Interpreters/Context.h>
#include <Interpreters/ContentAddressedGarbageCollectionLog.h>
#include <Interpreters/ContentAddressedLog.h>
#include <Common/CurrentThread.h>
#include <base/getThreadId.h>
#include <Common/DateLUT.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
    extern const int CORRUPTED_DATA;
    extern const int READONLY;
    extern const int BAD_ARGUMENTS;
    extern const int ABORTED;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

/// Canonical disk-relative path: components joined by single '/', no leading/trailing slashes.
/// Callers hand paths in both shapes (the Unfreezer walks shadow dirs WITH a trailing slash);
/// namespace strings and prefix matching need the canonical form.
std::string canonicalDiskPath(const std::string & path)
{
    std::string result;
    std::string component;
    auto flush = [&]
    {
        if (component.empty())
            return;
        if (!result.empty())
            result += '/';
        result += component;
        component.clear();
    };
    for (char c : path)
    {
        if (c == '/')
            flush();
        else
            component.push_back(c);
    }
    flush();
    return result;
}

/// "<first>/<rest...>" -> {first, rest} ({whole, ""} when there is no '/').
std::pair<std::string, std::string> splitFirstComponent(const std::string & s)
{
    const auto slash = s.find('/');
    if (slash == std::string::npos)
        return {s, ""};
    return {s.substr(0, slash), s.substr(slash + 1)};
}

void addFirstComponent(std::unordered_set<std::string> & out, const std::string & name)
{
    const auto slash = name.find('/');
    out.emplace(slash == std::string::npos ? name : name.substr(0, slash));
}

/// Drop a trailing `@cas@` content-addressing boundary marker from a mirrored path segment, so a
/// table-dir surfaces under its logical (unsuffixed) name in directory listings.
std::string stripCasArchiveSuffix(std::string s)
{
    const auto & suffix = Cas::kCasArchiveSuffix;
    if (s.size() >= suffix.size() && std::string_view(s).ends_with(suffix))
        s.resize(s.size() - suffix.size());
    return s;
}

std::vector<std::string> toVector(std::unordered_set<std::string> && set)
{
    return std::vector<std::string>(std::make_move_iterator(set.begin()), std::make_move_iterator(set.end()));
}

/// The server uuid string (with dashes) -> the core's UInt128 server id.
UInt128 serverIdToU128(const std::string & server_id)
{
    String hex;
    hex.reserve(32);
    for (char c : server_id)
        if (c != '-')
            hex += c;
    if (hex.size() == 32)
        return Cas::hexToU128(hex);
    /// Unit-test ids ("srv1") are not uuids — hash them stably.
    UInt128 r{};
    for (char c : server_id)
        r = r * 131 + static_cast<unsigned char>(c);
    return r == UInt128(0) ? UInt128(1) : r;
}

}

ContentAddressedMetadataStorage::ContentAddressedMetadataStorage(
    ObjectStoragePtr object_storage_,
    String storage_path_prefix_,
    String server_id_,
    String server_root_id_,
    String local_scratch_path_,
    ContextPtr context_,
    bool gc_enabled_,
    std::chrono::seconds gc_interval_,
    String disk_name_,
    uint64_t dedup_cache_bytes_,
    uint64_t dedup_head_first_min_bytes_,
    uint64_t gc_snap_generations_to_keep_,
    uint64_t gc_shards_,
    uint64_t manifest_sweep_list_budget_keys_,
    uint64_t manifest_sweep_delete_budget_keys_,
    uint64_t gcs_max_conditional_put_bytes_,
    uint64_t cas_part_folder_cache_bytes_,
    uint64_t cas_part_folder_cache_max_entries_,
    uint64_t cas_part_folder_cache_max_entry_bytes_,
    uint64_t manifest_decode_cache_bytes_,
    uint64_t gc_meta_pool_size_,
    Cas::StagingBackend staging_backend_,
    Cas::BlobHashAlgo blob_hash_algo_,
    bool blob_hash_allow_new_,
    bool skip_access_check_,
    uint64_t materialization_grace_ms_,
    Cas::PartFolderValidate part_folder_validate_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
    , server_root_id(std::move(server_root_id_))
    , disk_name(!disk_name_.empty() ? disk_name_ : storage_path_prefix)
    , local_scratch_path(std::move(local_scratch_path_))
    , context(context_)
    , gc_enabled(gc_enabled_)
    , gc_interval(gc_interval_)
    , dedup_cache_bytes(dedup_cache_bytes_)
    , dedup_head_first_min_bytes(dedup_head_first_min_bytes_)
    , gc_snap_generations_to_keep(gc_snap_generations_to_keep_)
    , gc_shards(gc_shards_)
    , manifest_sweep_list_budget_keys(manifest_sweep_list_budget_keys_)
    , manifest_sweep_delete_budget_keys(manifest_sweep_delete_budget_keys_)
    , gcs_max_conditional_put_bytes(gcs_max_conditional_put_bytes_)
    , cas_part_folder_cache_bytes(cas_part_folder_cache_bytes_)
    , cas_part_folder_cache_max_entries(cas_part_folder_cache_max_entries_)
    , cas_part_folder_cache_max_entry_bytes(cas_part_folder_cache_max_entry_bytes_)
    , manifest_decode_cache_bytes(manifest_decode_cache_bytes_)
    , gc_meta_pool_size(gc_meta_pool_size_)
    , staging_backend(staging_backend_)
    , blob_hash_algo(blob_hash_algo_)
    , blob_hash_allow_new(blob_hash_allow_new_)
    , skip_access_check(skip_access_check_)
    , materialization_grace_ms(materialization_grace_ms_)
    , part_folder_validate(part_folder_validate_)
{
}

Cas::StagingBackend ContentAddressedMetadataStorage::parseStagingBackend(const std::string & value)
{
    if (value == "local")
        return Cas::StagingBackend::Local;
    if (value == "s3")
        return Cas::StagingBackend::S3;
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "Unknown staging_backend value '{}' (expected 'local' or 's3')", value);
}

Cas::StagingBackend ContentAddressedMetadataStorage::parseStagingBackend(
    const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    return parseStagingBackend(config.getString(config_prefix + ".staging_backend", "local"));
}

Cas::PartFolderValidate ContentAddressedMetadataStorage::parsePartFolderValidate(const std::string & value)
{
    using PartFolderValidate = Cas::PartFolderValidate;
    if (value == "always")
        return {PartFolderValidate::Mode::Always, 0};
    if (value == "never")
        return {PartFolderValidate::Mode::Never, 0};
    if (value.starts_with("age "))
    {
        /// `std::from_chars` against an UNSIGNED type never accepts a leading '-' (unlike
        /// `std::stoull`, which silently negates modulo 2^64) -- a malformed/negative/non-digit/empty
        /// suffix falls through to the terminal throw below instead of wrapping into an astronomical
        /// age_seconds that behaves as skip-forever.
        const std::string age_str = value.substr(4);
        uint64_t age_seconds = 0;
        const auto [ptr, ec] = std::from_chars(age_str.data(), age_str.data() + age_str.size(), age_seconds);
        if (ec == std::errc{} && ptr == age_str.data() + age_str.size())
            return {PartFolderValidate::Mode::Age, age_seconds};
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "Unknown part_folder_validate value '{}' (expected 'always', 'never', or 'age <non-negative integer seconds>')", value);
}

Cas::PartFolderValidate ContentAddressedMetadataStorage::parsePartFolderValidate(
    const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    return parsePartFolderValidate(config.getString(config_prefix + ".part_folder_validate", "always"));
}

ContentAddressedMetadataStorage * ContentAddressedMetadataStorage::tryFromDisk(const DiskPtr & disk)
{
    MetadataStoragePtr md;
    try
    {
        md = disk->getMetadataStorage();
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::NOT_IMPLEMENTED)
            return nullptr;
        throw;
    }
    if (!md || !md->isContentAddressed())
        return nullptr;
    return dynamic_cast<ContentAddressedMetadataStorage *>(md.get());
}

void ContentAddressedMetadataStorage::runOneGcRoundForTest()
{
    /// The pacing scheduler must be STABLE across calls: the lease's observation-window steal
    /// protocol compares consecutive observations of the SAME observer (gc_id), so an ad-hoc
    /// scheduler per call would acquire the lease on the first call and then back off forever
    /// ("incumbent alive" - its own previous incarnation). Recreating the scheduler for every call
    /// would therefore make every round after the first a silent no-op.
    /// Hold gc_scheduler_mutex for the whole round: a concurrent `shutdown` waits for the round to
    /// finish because clean GC completion takes priority over fast shutdown. pointer_mutex (a
    /// separate, briefly-held mutex) only guards the scheduler snapshot/creation below, so
    /// gcHealth/store/partAccess never block behind this round.
    std::lock_guard round_lock(gc_scheduler_mutex);
    if (shutdown_called)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Cannot run garbage collection after ContentAddressedMetadataStorage shutdown has begun");
    std::shared_ptr<Cas::CasGcScheduler> snapshot;
    {
        std::lock_guard ptr_lock(pointer_mutex);
        if (!gc_scheduler)
        {
            if (!cas_store)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "ContentAddressedMetadataStorage: store accessed before startup");
            gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
                cas_store, gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
                disk_name, makeGcRoundLogger());
        }
        snapshot = gc_scheduler;
    }
    snapshot->runOneRoundNow();
}

std::optional<Cas::CasGcScheduler::GcHealth> ContentAddressedMetadataStorage::gcHealth() const
{
    /// A brief pointer_mutex snapshot only -- this must NEVER wait behind gc_scheduler_mutex (an
    /// in-flight round can hold that for a long time; an unprivileged SELECT on
    /// system.content_addressed_mounts must not stall behind it). The snapshot keeps the scheduler
    /// alive via its own refcount even if `shutdown` concurrently resets the member, and
    /// CasGcScheduler::gcHealth() is itself lock-free (atomic reads), so calling it outside any lock
    /// here is safe.
    std::shared_ptr<Cas::CasGcScheduler> snapshot;
    {
        std::lock_guard lock(pointer_mutex);
        snapshot = gc_scheduler;
    }
    if (!snapshot)
        return std::nullopt;
    return snapshot->gcHealth();
}

Cas::GcRoundLogger ContentAddressedMetadataStorage::makeGcRoundLogger() const
{
    /// Unit tests pass a null context (no system logs); the scheduler then runs without a sink.
    if (!context)
        return {};
    auto ctx = context;
    /// The configured disk name (threaded from the metadata-storage factory); falls back to
    /// storage_path_prefix for callers that don't supply one (e.g. unit tests).
    const String disk = disk_name;
    return [ctx, disk](const Cas::GcRoundLogRecord & r)
    {
        auto log = ctx->getContentAddressedGarbageCollectionLog();
        if (!log)
            return;
        ContentAddressedGarbageCollectionLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        e.event_type = r.event_type == Cas::GcRoundLogRecord::EventType::Start
            ? ContentAddressedGarbageCollectionLogElement::START
            : ContentAddressedGarbageCollectionLogElement::FINISH;
        e.disk_name = r.disk_name.empty() ? disk : r.disk_name;
        e.srid = r.srid;
        e.gc_id = r.gc_id;
        e.trigger = r.trigger == Cas::GcRoundLogRecord::Trigger::Manual
            ? ContentAddressedGarbageCollectionLogElement::MANUAL
            : ContentAddressedGarbageCollectionLogElement::SCHEDULED;
        switch (r.outcome)
        {
            case Cas::GcRoundLogRecord::Outcome::Unknown:
                e.outcome = ContentAddressedGarbageCollectionLogElement::UNKNOWN;
                break;
            case Cas::GcRoundLogRecord::Outcome::Success:
                e.outcome = ContentAddressedGarbageCollectionLogElement::SUCCESS;
                break;
            case Cas::GcRoundLogRecord::Outcome::NotALeader:
                e.outcome = ContentAddressedGarbageCollectionLogElement::NOT_A_LEADER;
                break;
            case Cas::GcRoundLogRecord::Outcome::Failed:
                e.outcome = ContentAddressedGarbageCollectionLogElement::FAILED;
                break;
        }
        e.round = r.round;
        e.candidates_marked = r.candidates_marked;
        e.objects_deleted = r.objects_deleted;
        e.objects_absent = r.objects_absent;
        e.objects_replaced = r.objects_replaced;
        e.objects_spared = r.objects_spared;
        e.manifests_deleted = r.manifests_deleted;
        e.entries_condemned = r.entries_condemned;
        e.entries_graduated = r.entries_graduated;
        e.entries_redeleted = r.entries_redeleted;
        e.fence_outs = r.fence_outs;
        e.anomalies = r.anomalies;
        e.duration_ms = r.duration_ms;
        e.error = r.error;
        e.profile_events = r.profile_events;
        /// Best-effort: SystemLog::add never blocks GC; a full queue drops the row with a warning.
        log->add(std::move(e));
    };
}

Cas::CasEventSink ContentAddressedMetadataStorage::makeCasEventSink() const
{
    /// Unit tests pass a null context (no system logs); the Pool then runs without a sink.
    if (!context)
        return {};
    auto ctx = context;
    /// The configured disk name (threaded from the metadata-storage factory); falls back to
    /// storage_path_prefix for callers that don't supply one (e.g. unit tests).
    const String disk = disk_name;
    return [ctx, disk](Cas::CasEvent ev)
    {
        auto log = ctx->getContentAddressedLog();
        if (!log)
            return;
        ContentAddressedLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        e.event_type = toString(ev.type);
        e.disk_name = disk;
        e.namespace_ = std::move(ev.namespace_);
        e.ref_name = std::move(ev.ref_name);
        e.object_kind = toString(ev.object_kind);
        e.object_hash = std::move(ev.object_hash);
        e.token = std::move(ev.token);
        e.round = ev.round;
        e.gen = ev.gen;
        e.at_version = ev.at_version;
        e.outcome = std::move(ev.outcome);
        e.reason = std::move(ev.reason);
        e.thread_id = getThreadId();
        e.query_id = CurrentThread::getQueryId();
        e.detail = std::move(ev.detail);
        /// Best-effort: SystemLog::add never blocks the Core; a full queue drops the row with a warning.
        log->add(std::move(e));
    };
}

Cas::RoundReport ContentAddressedMetadataStorage::runGarbageCollectionRoundNow()
{
    checkNotReadOnly("GC round");
    if (!gc_enabled)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Garbage collection is not enabled on this content-addressed disk");
    /// Mirror runOneGcRoundForTest: a STABLE scheduler instance across calls (the lease's
    /// observation-window steal protocol compares consecutive observations of the same gc_id).
    /// Hold gc_scheduler_mutex for the whole round: a concurrent `shutdown` waits for the round to
    /// finish because clean GC completion takes priority over fast shutdown. pointer_mutex only
    /// guards the scheduler snapshot/creation below, so gcHealth/store/partAccess never block behind
    /// this round.
    std::lock_guard round_lock(gc_scheduler_mutex);
    if (shutdown_called)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Cannot run garbage collection after ContentAddressedMetadataStorage shutdown has begun");
    std::shared_ptr<Cas::CasGcScheduler> snapshot;
    {
        std::lock_guard ptr_lock(pointer_mutex);
        if (!gc_scheduler)
        {
            if (!cas_store)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "ContentAddressedMetadataStorage: store accessed before startup");
            gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
                cas_store, gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
                disk_name, makeGcRoundLogger());
        }
        snapshot = gc_scheduler;
    }
    return snapshot->runOneRoundNow(Cas::GcRoundLogRecord::Trigger::Manual);
}

Cas::RebuildReport ContentAddressedMetadataStorage::runGcRebuildNow(bool force) const
{
    checkNotReadOnly("GC rebuild");
    if (!gc_enabled)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Garbage collection is not enabled on this content-addressed disk");
    /// A one-shot Gc instance is fine here (unlike the scheduler's stable-instance requirement for
    /// the lease's observation-window steal protocol): rebuildBaseline does its own lease
    /// acquire/steal check internally and this command runs exactly one round.
    const UInt128 gc_id = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
    const auto cas_store_snapshot = store();
    Cas::Gc gc(cas_store_snapshot, gc_id, {}, {},
        getLogger(fmt::format("CasGc({})", cas_store_snapshot->poolConfig().server_root_id)));
    return gc.rebuildBaseline(force);
}

void ContentAddressedMetadataStorage::startup()
{
    if (cas_store)
        return;

    /// Observe-only mode (the disk's <readonly> config): skip the probe (a probe write would fail on
    /// a read-only backend), run no watermark, start no GC, and fail the mutating surface closed.
    read_only = object_storage->isReadOnly();

    /// Native mode rides real conditional ops (probed fail-closed by Pool::open); Local object
    /// storage has none, so the backend emulates exact token semantics in-process (single server).
    const auto mode = object_storage->getType() == ObjectStorageType::Local
        ? Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess
        : Cas::ObjectStorageBackend::Mode::Native;
    auto backend = std::make_shared<Cas::ObjectStorageBackend>(object_storage, mode, gcs_max_conditional_put_bytes);

    /// EmulatedSingleProcess emulates the conditional-op / exact-token semantics in-process (local
    /// object storage has none). That emulation is per-process: two servers pointed at the SAME local
    /// pool (e.g. an NFS/shared mount) each keep independent token state and would silently violate
    /// the CAS invariants — the capability probe cannot detect this (each process passes it alone).
    /// Make a shared-pool misconfiguration visible at INFO, not WARNING.
    /// An inline `disk = disk(... object_storage_type=local ...)` opens the disk on the QUERY thread, so
    /// a WARNING is forwarded to the client at the functional-test default `send_logs_level=warning` and
    /// fails EVERY such query (clickhouse-test fails a test on ANY client stderr). At INFO the message
    /// still lands in the server log for operator visibility but is not forwarded to client queries, so
    /// the ~15 CA-over-local stateless tests stop failing on a benign single-server note. (A genuinely
    /// shared local pool is a niche risk that would also surface via CAS/GC corruption; a future
    /// `system.warnings` entry could restore a louder, test-safe signal.)
    if (mode == Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess)
        LOG_INFO(
            getLogger("ContentAddressedMetadataStorage"),
            "Content-addressed disk over LOCAL object storage uses emulated in-process conditional "
            "operations — safe ONLY for a single server. Do NOT share this pool path between multiple "
            "ClickHouse servers (e.g. a shared/NFS mount): the CAS/GC invariants would break silently. "
            "Use an S3-backed pool for multi-server / shared deployments.");

    /// Key spaces per mode: the Emulated (Local) backend maps bare pool keys under
    /// getCommonKeyPrefix (the disk root dir), so the POOL prefix must be bucket-relative - strip
    /// the common prefix when the configured prefix carries it (the local factory passes the root
    /// path). Native passes keys through, so the configured prefix is used as-is (for S3 it
    /// already embeds the endpoint sub-path).
    String pool_prefix = storage_path_prefix;
    /// The configured prefix is an endpoint sub-path and usually carries a TRAILING slash
    /// ("content_addressed_s3/"); Cas::Layout joins components with '/', and a doubled slash in
    /// keys is backend-hostile (RustFS rejects "p//_probe" LIST prefixes with InvalidArgument -
    /// Some backends reject such prefixes while others merely tolerate them).
    while (!pool_prefix.empty() && pool_prefix.back() == '/')
        pool_prefix.pop_back();
    if (mode == Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess)
    {
        physical_key_prefix = object_storage->getCommonKeyPrefix();
        /// Slash-tolerant strip: the common prefix usually ends with '/', the pool prefix was
        /// just trimmed of trailing slashes - compare canonical forms.
        String common_trimmed = physical_key_prefix;
        while (!common_trimmed.empty() && common_trimmed.back() == '/')
            common_trimmed.pop_back();
        if (!common_trimmed.empty())
        {
            if (pool_prefix == common_trimmed)
                pool_prefix.clear();
            else if (pool_prefix.starts_with(common_trimmed + "/"))
                pool_prefix = pool_prefix.substr(common_trimmed.size() + 1);
        }
        if (pool_prefix.empty())
            pool_prefix = "ca";
    }

    Cas::PoolConfig pool_config;
    pool_config.pool_prefix = pool_prefix;
    pool_config.server_id = serverIdToU128(server_id);
    pool_config.server_root_id = server_root_id;
    pool_config.background_watermark = (context != nullptr) && !read_only;
    pool_config.read_only = read_only;
    pool_config.skip_access_check = skip_access_check;
    /// The node-local write algorithm: `PoolMeta::createOrValidate` accepts it with no write once
    /// it is a member of the pool's `algos_used`; a not-yet-admitted algo is admitted via
    /// `blob_hash_allow_new` or refused (BAD_ARGUMENTS, the default).
    pool_config.blob_hash_algo = blob_hash_algo;
    pool_config.blob_hash_allow_new = blob_hash_allow_new;
    pool_config.dedup_cache_bytes = dedup_cache_bytes;
    pool_config.dedup_head_first_min_bytes = dedup_head_first_min_bytes;
    pool_config.manifest_decode_cache_bytes = manifest_decode_cache_bytes;
    pool_config.gc_snap_generations_to_keep = gc_snap_generations_to_keep;
    pool_config.gc_shards = gc_shards;
    pool_config.manifest_sweep_list_budget_keys = manifest_sweep_list_budget_keys;
    pool_config.manifest_sweep_delete_budget_keys = manifest_sweep_delete_budget_keys;
    pool_config.gc_meta_pool_size = gc_meta_pool_size;
    pool_config.materialization_grace_ms = materialization_grace_ms;
    pool_config.event_sink = makeCasEventSink();
    cas_store = Cas::Pool::open(std::move(backend), std::move(pool_config));
    pool_uuid = Cas::u128ToHex(cas_store->poolMeta().pool_id);
    part_access = std::make_shared<Cas::CachedPartFolderAccess>(cas_store,
        Cas::CachedPartFolderAccess::CacheParams{
            .cache_bytes = cas_part_folder_cache_bytes,
            .max_entries = cas_part_folder_cache_max_entries,
            .max_entry_bytes = cas_part_folder_cache_max_entry_bytes,
            .validate = part_folder_validate});

    /// The optional mount-time capability probe for a write-once conditional server-side copy.
    /// Only relevant when this disk opted in to `staging_backend=s3`; `Local` (the default,
    /// global constraint: OFF BY DEFAULT) takes NO probe here — `conditional_copy_supported` simply
    /// stays at its `false` default and is never consulted on the local path. Skipped in
    /// observe-only/readonly mode: a probe write would fail on a read-only backend, exactly like the
    /// mandatory battery (`runCapabilityProbe`) above skips a read-only mount.
    ///
    /// Fail-close, never fail-open: an unsupported or non-enforcing backend just falls back to local
    /// staging (`conditional_copy_supported` stays `false`) — this is NOT a mount failure, unlike the
    /// mandatory battery, because `local` staging remains fully functional.
    if (staging_backend == Cas::StagingBackend::S3 && !read_only)
    {
        const String probe_prefix = physicalKey(pool_prefix + "/staging/" + server_root_id + "/probe");
        conditional_copy_supported = Cas::probeConditionalCopy(*object_storage, probe_prefix);
        if (!conditional_copy_supported)
            LOG_INFO(
                getLogger("ContentAddressedMetadataStorage"),
                "staging_backend=s3 requested but the object storage does not enforce conditional "
                "copy; falling back to local staging");

        /// Reclaim this mount's own leaked `staging/<server_root_id>/` debris (a promote whose staging-delete never
        /// ran, or an aborted transaction's never-promoted staging object — see
        /// `cleanupPendingTempFiles`) at mount start. Only runs when the S3 path is actually usable
        /// (`conditional_copy_supported`) — an unsupported/fail-closed-to-local mount never wrote any
        /// S3 staging objects under this prefix in the first place. LEASE-FENCE: `stagingKeyPrefix()`
        /// is keyed by THIS mount's own `server_root_id` (the SAME prefix construction the probe above
        /// and every staging key this mount ever mints use), so this sweep can never reach a different
        /// mount's in-flight staging (`Cas::sweepOwnMountStaging`'s own doc comment). GC excludes
        /// `staging/` entirely (a distinct top-level prefix from `blobs/` — see `CasLayout.h`), so this
        /// sweeper is the ONLY reclaimer of `staging/` debris.
        if (conditional_copy_supported)
            Cas::sweepOwnMountStaging(*object_storage, stagingKeyPrefix() + "/");
    }

    /// The background GC scheduler runs only on the disk-factory path (context non-null) and when
    /// enabled - the lease makes concurrent schedulers across mounters safe (work dedup), so no
    /// further gating is needed because the scheduler's lease coordinates concurrent mounters.
    if (context && gc_enabled && !read_only)
    {
        gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
            cas_store, gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
            disk_name, makeGcRoundLogger());
        gc_scheduler->start();
    }
}

void ContentAddressedMetadataStorage::shutdown()
{
    /// Wait for any in-flight synchronous round to finish cleanly first (gc_scheduler_mutex is held
    /// for a round's whole duration) -- unchanged priority: clean GC completion over fast shutdown.
    std::lock_guard round_lock(gc_scheduler_mutex);
    shutdown_called = true;
    std::shared_ptr<Cas::CasGcScheduler> old_scheduler;
    {
        std::lock_guard ptr_lock(pointer_mutex);
        old_scheduler = std::move(gc_scheduler);
        gc_scheduler.reset();
        part_access.reset();
        cas_store.reset();
    }
    /// `stop` joins the background threads. Runs outside pointer_mutex (no reset left to race:
    /// gc_scheduler is already null) but still inside round_lock, so a NEW round can't start here.
    /// old_scheduler keeps the object alive regardless.
    if (old_scheduler)
        old_scheduler->stop();
}

Cas::PoolPtr ContentAddressedMetadataStorage::store() const
{
    Cas::PoolPtr snapshot;
    {
        std::lock_guard lock(pointer_mutex);
        snapshot = cas_store;
    }
    if (!snapshot)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: store accessed before startup");
    return snapshot;
}

std::shared_ptr<Cas::CachedPartFolderAccess> ContentAddressedMetadataStorage::partAccess() const
{
    std::shared_ptr<Cas::CachedPartFolderAccess> snapshot;
    {
        std::lock_guard lock(pointer_mutex);
        snapshot = part_access;
    }
    if (!snapshot)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: partAccess accessed before startup");
    return snapshot;
}

void ContentAddressedMetadataStorage::checkNotReadOnly(std::string_view what) const
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY,
            "Content-addressed disk is opened read-only: {} is rejected", what);
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    checkNotReadOnly("writes");
    return std::make_shared<ContentAddressedTransaction>(*this);
}

String ContentAddressedMetadataStorage::stagingKeyPrefix() const
{
    /// Mirrors the probe's own prefix construction (`startup`'s `probe_prefix` above), minus
    /// the probe's own `/probe` leaf — this is the writer-owned sibling subtree of the SAME
    /// `staging/<server_root_id>/` area. `store()` throws LOGICAL_ERROR before startup; every caller
    /// (writeFile, via a transaction) runs post-startup.
    return physicalKey(store()->poolConfig().pool_prefix + "/staging/" + server_root_id);
}

/// ==== namespace mapping ====

std::string ContentAddressedMetadataStorage::serverPrefix() const
{
    /// Live namespaces and mirrored live-tree files are rooted by the configured
    /// `server_root_id`, not by the ClickHouse ServerUUID-derived token. `ServerUUID` is only the
    /// mount owner token; `server_root_id` is the persistent layout identity.
    return server_root_id;
}

std::vector<std::string> ContentAddressedMetadataStorage::listLiveTreeChildren(const std::string & path) const
{
    const std::string canonical = canonicalDiskPath(path);
    const std::string scope = serverPrefix() + "/" + (canonical.empty() ? "" : canonical + "/");
    std::unordered_set<std::string> result;
    for (const auto & child : store()->listMirroredChildren(scope))
        result.emplace(stripCasArchiveSuffix(child));
    return toVector(std::move(result));
}

bool ContentAddressedMetadataStorage::liveTreeDirHasChildren(const std::string & path) const
{
    const std::string canonical = canonicalDiskPath(path);
    /// The disk root always exists; otherwise a non-empty server-root-scoped mirrored LIST is the signal.
    if (canonical.empty())
        return true;
    const std::string scope = serverPrefix() + "/" + canonical + "/";
    return !store()->listMirroredChildren(scope).empty();
}

Cas::RootNamespace ContentAddressedMetadataStorage::liveNamespace(const std::string & table_uuid) const
{
    /// Path mirroring: the namespace is the table's canonical disk path with the
    /// content-addressed boundary marked by `@cas@` on the table-dir segment, prefixed by the
    /// configured `server_root_id`. e.g. `<server_root_id>/store/3f2/3f2a…@cas@`.
    return Cas::RootNamespace{serverPrefix() + "/" + Cas::mirroredArchiveNamespace(table_uuid)};
}

bool ContentAddressedMetadataStorage::namespaceFilesReadable(const Cas::RootNamespace & ns) const
{
    return !store()->namespaceIsRemoved(ns);
}

Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir)
{
    /// The LITERAL shadow table dir (shadow/<backup>/store/<u3>/<uuid> or .../data/<db>/<tbl>):
    /// bijective with the disk path for both layouts, pool-global (backups are read by any
    /// replica), and the shadow tree enumerates from Pool::listNamespaces("shadow/").
    /// Canonicalize because the unfreezer can hand the directory a trailing slash.
    return Cas::RootNamespace{canonicalDiskPath(shadow_table_dir)};
}


std::optional<ContentAddressedMetadataStorage::Route>
ContentAddressedMetadataStorage::route(const Cas::PartFilePath & p) const
{
    Route r;
    if (!p.backup_name.empty())
    {
        r.ns = shadowNamespace(p.shadow_table_dir);
        r.ref = p.part_name;
        r.file = p.file;
        return r;
    }
    if (p.part_name == Cas::kDetachedDirName)
    {
        /// The parser reports detached paths with part_name == "detached" and the real detached
        /// part dir as the first component of `file`. Detached parts share the table namespace and
        /// INTO the table's OWN archive namespace: each detached part is a ref keyed by
        /// `detached/<part>` (vs a live `<part>`), so the re-split here keeps the table namespace
        /// and prepends the `detached/` ref prefix. An empty `p.file` (the bare `<table>/detached`
        /// container dir) yields an empty ref → the filtered-container listing path.
        r.ns = liveNamespace(p.table_uuid);
        auto [part, file] = splitFirstComponent(p.file);
        r.ref = part.empty() ? "" : std::string(Cas::kDetachedRefPrefix) + part;
        r.file = file;
        return r;
    }
    if (p.part_name == Cas::kMovingDirName)
    {
        /// L1 (MOVE-to-CA fix): re-split exactly like detached, folding onto a `moving/`-PREFIXED
        /// ref (kMovingRefPrefix) -- NOT the part's final ref directly. Publishing the clone under
        /// the final ref before the mover's swap would break move crash-atomicity: a crash between
        /// the clone publication and swapClonedPart would leave a committed LIVE ref that never went
        /// through the swap, and moving/'s own startup cleanup couldn't distinguish that premature
        /// ref from a real live part. The staging ref keeps the pre-swap clone un-live; the mover's
        /// rename does a real ref repoint moving/<part> -> <part> (the same committed-ref-repoint
        /// path merge-result/delete_tmp renames already use). An empty p.file (the bare
        /// <table>/moving container dir) yields an empty ref, same convention as detached.
        r.ns = liveNamespace(p.table_uuid);
        auto [part, file] = splitFirstComponent(p.file);
        r.ref = part.empty() ? "" : std::string(Cas::kMovingRefPrefix) + part;
        r.file = file;
        return r;
    }
    r.ns = liveNamespace(p.table_uuid);
    r.ref = p.part_name;
    r.file = p.file;
    return r;
}

std::vector<std::string> ContentAddressedMetadataStorage::detachedRefNames(const Cas::RootNamespace & ns) const
{
    std::vector<std::string> refs;
    for (const auto & [ref, _] : store()->listRefs(ns))
        if (ref.starts_with(Cas::kDetachedRefPrefix))
            refs.push_back(ref);
    return refs;
}

std::vector<std::string> ContentAddressedMetadataStorage::movingRefNames(const Cas::RootNamespace & ns) const
{
    std::vector<std::string> refs;
    for (const auto & [ref, _] : store()->listRefs(ns))
        if (ref.starts_with(Cas::kMovingRefPrefix))
            refs.push_back(ref);
    return refs;
}

/// ==== read surface ====

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    if (!Cas::isPartFilePath(path))
    {
        if (auto tf = Cas::parseTableFilePath(path))
        {
            const auto ns = liveNamespace(tf->table_uuid);
            return namespaceFilesReadable(ns) && store()->getNamespaceFile(ns, tf->tail).has_value();
        }
        /// A loose mountpoint object is a plain object at roots/<server_root_id>/<path>.
        /// Use a HEAD-based existence check (directory-safe), NOT a body read: the traversal in
        /// system.remote_data_paths probes existsFile on directory-shaped pool paths (e.g. `store`), and a
        /// body read (getMountpointObject) throws "Is a directory". A directory is not a file.
        return store()->mountpointObjectExists(serverPrefix() + "/" + path);
    }

    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto r = route(*p);
    if (!r || r->file.empty())
        return false;

    /// Per-part files flow through the ordinary content path like any other file; no ForceFresh
    /// special case is needed.
    /// Safe to serve a CACHED view here: every committed-ref write that could have moved this entry
    /// (`repointRef`/`promoteBuild`) erases the cached view on success, so a stale hit is impossible
    /// by construction, not by freshness policy.
    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    return view && view->findFile(r->file);
}

ContentAddressedMetadataStorage::DirRoute ContentAddressedMetadataStorage::classifyDirectory(const std::string & path) const
{
    DirRoute dr;

    /// FREEZE shadow namespace — routed BEFORE the live branches (a shadow table dir also
    /// satisfies parseTableUuid).
    if (Cas::isShadowPath(path))
    {
        if (auto p = Cas::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            dr.shape = DirShape::ShadowPart;
            dr.p = std::move(p);
            return dr;
        }
        if (Cas::endsWithTableUuidPair(path))
        {
            dr.shape = DirShape::ShadowTable;
            return dr;
        }
        dr.shape = DirShape::ShadowIntermediate;
        return dr;
    }

    /// The Atomic `store/<u3>` shard dir (see listDirectory): route to the generic existence signal
    /// before parseTableUuid/parseTableFilePath misclaim it as a non-Atomic table.
    if (Cas::isAtomicShardDir(path))
    {
        dr.shape = DirShape::AtomicShard;
        return dr;
    }

    if (auto uuid = Cas::parseTableUuid(path))
    {
        dr.shape = DirShape::TableDir;
        dr.uuid = std::move(uuid);
        return dr;
    }

    if (auto p = Cas::parsePartFilePath(path))
    {
        auto r = route(*p);
        /// The detached CONTAINER dir <table>/detached.
        if (r && r->ref.empty() && p->part_name == Cas::kDetachedDirName)
        {
            dr.shape = DirShape::DetachedContainer;
            dr.p = std::move(p);
            dr.r = std::move(r);
            return dr;
        }
        /// The moving CONTAINER dir <table>/moving (MOVE-to-CA fix): the mover's crash-cleanup
        /// (MergeTreeData.cpp, MOVING_DIR_NAME) existsDirectory/removeRecursive's this bare path
        /// at every table load to reclaim a staging ref left behind by an interrupted move.
        if (r && r->ref.empty() && p->part_name == Cas::kMovingDirName)
        {
            dr.shape = DirShape::MovingContainer;
            dr.p = std::move(p);
            dr.r = std::move(r);
            return dr;
        }
        /// A part dir (live, detached, or shadow).
        if (r && !r->ref.empty() && r->file.empty())
        {
            dr.shape = DirShape::PartDir;
            dr.p = std::move(p);
            dr.r = std::move(r);
            return dr;
        }
        /// A projection dir.
        if (r && !r->ref.empty())
        {
            if (auto prefix = Cas::PartFolderView::projectionDirPrefix(r->file))
            {
                dr.shape = DirShape::ProjectionDir;
                dr.p = std::move(p);
                dr.r = std::move(r);
                dr.projection_prefix = std::move(prefix);
                return dr;
            }
        }
        /// No sub-shape matched: fall through, identical to today's post-`if (p)` continuation.
    }

    /// A table-level SUBDIRECTORY (deduplication_logs/...).
    if (auto tf = Cas::parseTableFilePath(path))
    {
        dr.shape = DirShape::TableSubdir;
        dr.tf = std::move(tf);
        return dr;
    }

    /// A generic INTERMEDIATE live-tree directory (disk root, `store`, ...).
    dr.shape = DirShape::GenericIntermediate;
    return dr;
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    const DirRoute dr = classifyDirectory(path);
    switch (dr.shape)
    {
        case DirShape::ShadowPart:
            return partAccess()->existsRef(Route{shadowNamespace(dr.p->shadow_table_dir), dr.p->part_name, ""}.refKey(),
                                          Cas::Freshness::CachedForLoad);
        case DirShape::ShadowTable:
            return store()->hasAnyRefWithPrefix(shadowNamespace(path), "");
        case DirShape::ShadowIntermediate:
        {
            /// Intermediate dir (shadow/<bk>, shadow/<bk>/store, ...): exists iff SOME shadow namespace
            /// under this path still has a LIVE ref. A raw object LIST of the mirrored subtree would count
            /// tombstoned-but-not-yet-GC'd shard/manifest objects — CA removal is tombstone + deferred GC
            /// (`removeRecursive`/`dropNamespace` only tombstone; `Cas::Gc` physically deletes later) — so a
            /// just-`UNFREEZE`d backup dir would spuriously "exist" until a GC round runs. Instead
            /// enumerate the namespaces exactly as `removeRecursive` does (`listNamespaces(scope)`) and
            /// consult the tombstone-aware `listRefs` (as the `endsWithTableUuidPair` case above does), so
            /// existence is consistent with the ref-level signal and independent of GC timing.
            const std::string canonical = canonicalDiskPath(path);
            const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
            for (const auto & ns : store()->listNamespaces(scope))
                if (store()->hasAnyRefWithPrefix(Cas::RootNamespace{ns}, ""))
                    return true;
            return false;
        }
        case DirShape::AtomicShard:
            return liveTreeDirHasChildren(path);
        case DirShape::TableDir:
            /// A table directory exists iff it has at least one committed part.
            return store()->hasAnyRefWithPrefix(liveNamespace(*dr.uuid), "");
        case DirShape::DetachedContainer:
            /// Exists iff it has at least one reference.
            return store()->hasAnyRefWithPrefix(dr.r->ns, Cas::kDetachedRefPrefix);
        case DirShape::MovingContainer:
            /// Exists iff it has at least one staging ref (MOVE-to-CA fix, mirrors DetachedContainer).
            return store()->hasAnyRefWithPrefix(dr.r->ns, Cas::kMovingRefPrefix);
        case DirShape::PartDir:
            /// Exists iff its ref is present.
            return partAccess()->existsRef(dr.r->refKey(), Cas::Freshness::CachedForLoad);
        case DirShape::ProjectionDir:
        {
            /// At least one tree entry (or mutable file) under its prefix.
            auto view = partAccess()->getView(dr.r->refKey(), Cas::Freshness::CachedForLoad);
            return view && view->hasDirectory(*dr.projection_prefix);
        }
        case DirShape::TableSubdir:
        {
            /// At least one verbatim file under it.
            const auto ns = liveNamespace(dr.tf->table_uuid);
            if (!namespaceFilesReadable(ns))
                return false;
            const std::string prefix = dr.tf->tail + "/";
            for (const auto & name : store()->listNamespaceFiles(ns))
                if (name.starts_with(prefix))
                    return true;
            return false;
        }
        case DirShape::GenericIntermediate:
            /// Exists iff a server-root-scoped mirrored LIST finds any object. Keeps `cd`/existence
            /// consistent with listDirectory so `clickhouse-disks` traversal behaves like a normal disk.
            return liveTreeDirHasChildren(path);
    }
    return liveTreeDirHasChildren(path);   /// unreachable
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    if (Cas::isPartFilePath(path))
    {
        auto p = Cas::parsePartFilePath(path);
        auto r = p ? route(*p) : std::nullopt;
        if (r && !r->ref.empty() && !r->file.empty())
        {
            auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
            if (!view)
                return false;
            return view->hasFile(r->file) || view->hasDirectory(r->file + "/");
        }
    }
    return existsFile(path) || existsDirectory(path);
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    if (!Cas::isPartFilePath(path))
    {
        if (auto bytes = tryGetInManifestBytes(path))   /// verbatim table-level file
            return bytes->size();
        if (auto bytes = store()->getMountpointObject(serverPrefix() + "/" + path))
            return bytes->size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    if (auto size = view->fileSize(r->file))
        return *size;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", r->file, path);
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string & path) const
{
    /// Timestamps are DERIVED for content addressing: the part's publish wall-clock, stamped by
    /// the transaction into the typed `RefPayload.published_at_ms` field (epoch milliseconds).
    /// Every shape (part dir, detached part dir, projection dir, part file) reports its part's
    /// stamp; a part published without a stamp (published_at_ms == 0) reports the epoch (harmless:
    /// stamps only feed cleanup TTLs and system tables).
    auto resolve_stamp = [&](const Route & r) -> Poco::Timestamp
    {
        auto resolved = partAccess()->resolve(r.refKey(), Cas::Freshness::CachedForLoad);
        if (!resolved)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
        if (resolved->published_at_ms == 0)
            return Poco::Timestamp(0);
        /// published_at_ms is epoch milliseconds; Poco::Timestamp::fromEpochTime takes seconds.
        return Poco::Timestamp::fromEpochTime(static_cast<time_t>(resolved->published_at_ms / 1000));
    };

    if (auto p = Cas::parsePartFilePath(path))
    {
        auto r = route(*p);
        if (r && !r->ref.empty())
            return resolve_stamp(*r);
    }
    /// Table-level / generic verbatim files: no per-object mtime is kept — epoch.
    if (existsFile(path))
        return Poco::Timestamp(0);
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
}

std::vector<std::string> ContentAddressedMetadataStorage::listDirectory(const std::string & path) const
{
    const DirRoute dr = classifyDirectory(path);
    switch (dr.shape)
    {
        case DirShape::ShadowPart:
        {
            /// Shadow PART dir: the frozen part's file names (first components).
            auto view = partAccess()->getView(Route{shadowNamespace(dr.p->shadow_table_dir), dr.p->part_name, ""}.refKey(),
                                             Cas::Freshness::CachedForLoad);
            return view ? view->listChildren("") : std::vector<std::string>{};
        }
        case DirShape::ShadowTable:
        {
            /// Shadow TABLE dir: the frozen part names.
            std::vector<std::string> result;
            for (const auto & [ref, _] : store()->listRefs(shadowNamespace(path)))
                result.push_back(ref);
            return result;
        }
        case DirShape::ShadowIntermediate:
        {
            /// Enumerate children via a scoped LIST of the mirrored subtree. A
            /// mirrored LIST naturally surfaces intermediate path segments AND `@cas@`-suffixed
            /// table dirs; strip the trailing `@cas@` for the logical view. Loose LIST is fine: the
            /// existing listRefs re-check filters out dropped-but-registered archives so they don't
            /// appear as false children.
            const std::string canonical = canonicalDiskPath(path);
            const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
            std::unordered_set<std::string> result;
            for (const auto & child : store()->listMirroredChildren(scope))
                result.emplace(stripCasArchiveSuffix(child));
            return toVector(std::move(result));
        }
        case DirShape::AtomicShard:
            /// A pure intermediate dir whose only child is the uuid-anchored table dir. Its path
            /// shape collides with the non-Atomic `data/<db>` fallback of both parseTableUuid and
            /// parseTableFilePath, so it MUST be routed to the generic mirrored LIST BEFORE those
            /// branches claim it (see classifyDirectory).
            return listLiveTreeChildren(path);
        case DirShape::TableDir:
        {
            /// Part names (live and `detached/<part>` references) plus table-level verbatim
            /// file names; addFirstComponent collapses both to their first path segment (live part
            /// names and the single `detached` subdir, exactly like a nested verbatim file).
            const auto ns = liveNamespace(*dr.uuid);
            std::unordered_set<std::string> result;
            for (const auto & [ref, _] : store()->listRefs(ns))
                addFirstComponent(result, ref);
            /// A dropped table lists empty for its namespace files too (its refs are already gone
            /// via the ref state); only surface verbatim file names while the table is not removed.
            if (namespaceFilesReadable(ns))
                for (const auto & name : store()->listNamespaceFiles(ns))
                    addFirstComponent(result, name);
            return toVector(std::move(result));
        }
        case DirShape::DetachedContainer:
        {
            /// Detached part names (prefix stripped; never files).
            std::vector<std::string> result;
            for (const auto & ref : detachedRefNames(dr.r->ns))
                result.push_back(ref.substr(Cas::kDetachedRefPrefix.size()));
            return result;
        }
        case DirShape::MovingContainer:
        {
            /// Staging part names (prefix stripped), mirrors DetachedContainer.
            std::vector<std::string> result;
            for (const auto & ref : movingRefNames(dr.r->ns))
                result.push_back(ref.substr(Cas::kMovingRefPrefix.size()));
            return result;
        }
        case DirShape::PartDir:
        {
            /// A part dir (live, detached part, shadow handled separately): logical file names,
            /// nested keys collapsed to their first component (projections surface as ONE
            /// <proj>.proj entry).
            auto view = partAccess()->getView(dr.r->refKey(), Cas::Freshness::CachedForLoad);
            return view ? view->listChildren("") : std::vector<std::string>{};
        }
        case DirShape::ProjectionDir:
        {
            /// Inner names with the <proj>.proj/ prefix stripped.
            auto view = partAccess()->getView(dr.r->refKey(), Cas::Freshness::CachedForLoad);
            return view ? view->listChildren(*dr.projection_prefix) : std::vector<std::string>{};
        }
        case DirShape::TableSubdir:
        {
            /// Verbatim files under <subdir>/, first-component collapsed.
            const auto ns = liveNamespace(dr.tf->table_uuid);
            std::unordered_set<std::string> result;
            if (namespaceFilesReadable(ns))
                for (const auto & name : store()->listNamespaceFiles(ns))
                    if (name.starts_with(dr.tf->tail + "/"))
                        addFirstComponent(result, name.substr(dr.tf->tail.size() + 1));
            return toVector(std::move(result));
        }
        case DirShape::GenericIntermediate:
            /// The disk root "", `store`, or any loose-file container above a table dir: a
            /// server-root-scoped mirrored LIST. (`store/<u3>` is handled by AtomicShard above,
            /// since its non-Atomic-table ambiguity would otherwise misroute it here too late,
            /// after parseTableUuid/parseTableFilePath have already claimed it.)
            return listLiveTreeChildren(path);
    }
    return listLiveTreeChildren(path);   /// unreachable
}

DirectoryIteratorPtr ContentAddressedMetadataStorage::iterateDirectory(const std::string & path) const
{
    /// Mirror MetadataStorageFromPlainObjectStorage: iterateDirectory includes the path.
    auto names = listDirectory(path);
    std::vector<fs::path> fs_paths;
    fs_paths.reserve(names.size());
    for (const auto & child : names)
        fs_paths.push_back(fs::path(path) / child);
    return std::make_unique<StaticDirectoryIterator>(std::move(fs_paths));
}

bool ContentAddressedMetadataStorage::isDirectoryEmpty(const std::string & path) const
{
    /// A part directory's files are virtual (derived from the tree): report it EMPTY so
    /// DiskObjectStorage::removeDirectory proceeds straight to the ref-unlink instead of throwing
    /// CANNOT_RMDIR per removal. The same applies to a projection subdirectory. The detached
    /// CONTAINER and TABLE dirs keep the listing-based emptiness (DROP TABLE's non-empty guard).
    if (auto p = Cas::parsePartFilePath(path))
    {
        auto r = route(*p);
        if (r && !r->ref.empty() && r->file.empty())
            return true;
        if (r && !r->ref.empty() && Cas::PartFolderView::projectionDirPrefix(r->file))
            return true;
    }
    return !iterateDirectory(path)->isValid();
}

StoredObjects ContentAddressedMetadataStorage::getStorageObjects(const std::string & path) const
{
    /// In-manifest bytes (mutable per-part files, inline entries, verbatim namespace files) have
    /// no object of their own: DiskObjectStorage::prepareRead serves them via tryGetInManifestBytes
    /// BEFORE asking for storage objects. The sized empty-key placeholder below keeps size-only
    /// consumers working and makes any bypassing reader fail LOUDLY (never silently wrong bytes).
    if (auto bytes = tryGetInManifestBytes(path))
        return {StoredObject("", path, bytes->size())};

    if (!Cas::isPartFilePath(path))
    {
        if (Cas::parseTableFilePath(path))
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "ContentAddressed: table-level verbatim file is in-manifest, not a storage object: {}", path);
        /// A loose mountpoint object: a real plain object at roots/<server_root_id>/<path>. The
        /// StoredObject key must be the PHYSICAL path (physicalKey-adjusted for Local backends).
        /// Probe with a HEAD (directory-safe), not a body read: `system.remote_data_paths`
        /// may reach here on a directory-shaped pool path and a GET would throw "Is a directory".
        const std::string pool_key = store()->layout().mountpointObjectKey(serverPrefix() + "/" + path);
        if (store()->mountpointObjectExists(serverPrefix() + "/" + path))
            return {StoredObject(physicalKey(pool_key), path, getFileSize(path))};
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    if (const auto * entry = view->findFile(r->file))
    {
        const auto location = store()->locate(*entry);
        /// StoredObject carries no range (the recorded upstream delta) — the PAYLOAD length is the
        /// size (what every size consumer wants); the header offset is applied by
        /// getBlobViewPlan's view window, the only byte-reading path.
        return {StoredObject(location.key, path, location.length)};
    }
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", r->file, path);
}

std::optional<StoredObjects> ContentAddressedMetadataStorage::getStorageObjectsIfExist(const std::string & path) const
{
    /// Non-part shapes (verbatim table files, loose mountpoint objects) are rare paths — the
    /// generic two-step is fine for them.
    if (!Cas::isPartFilePath(path))
    {
        if (existsFile(path))
            return getStorageObjects(path);
        return std::nullopt;
    }
    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    if (!view)
        return std::nullopt;
    const auto * entry = view->findFile(r->file);
    if (!entry)
        return std::nullopt;
    if (entry->placement == Cas::EntryPlacement::Inline)
        return StoredObjects{StoredObject("", path, entry->size())};
    const auto location = store()->locate(*entry);
    return StoredObjects{StoredObject(location.key, path, location.length)};
}

std::optional<String> ContentAddressedMetadataStorage::tryGetInManifestBytes(const std::string & path) const
{
    Cas::PoolPtr store_snapshot;
    {
        std::lock_guard lock(pointer_mutex);
        store_snapshot = cas_store;
    }
    if (!store_snapshot)
        return std::nullopt;

    if (!Cas::isPartFilePath(path))
    {
        if (auto tf = Cas::parseTableFilePath(path))
        {
            const auto ns = liveNamespace(tf->table_uuid);
            return namespaceFilesReadable(ns) ? store_snapshot->getNamespaceFile(ns, tf->tail) : std::nullopt;
        }
        return std::nullopt;   /// loose files are plain objects, not in-manifest bytes
    }

    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    if (!view)
        return std::nullopt;
    return view->inlineBytes(r->file);
}

bool ContentAddressedMetadataStorage::prepareInManifestRead(
    const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const
{
    /// In-manifest bytes (mutable per-part files, inline entries, verbatim namespace files):
    /// served from memory — there is no object to read.
    auto bytes = tryGetInManifestBytes(path);
    if (!bytes)
        return false;

    const auto size = bytes->size();
    auto creator = [path, data = std::move(*bytes)](
        const StoredObject &, const ReadSettings &, bool, bool) -> std::unique_ptr<ReadBufferFromFileBase>
    {
        return std::make_unique<ReadBufferFromOwnMemoryFile>(path, data);
    };
    pipeline.setSource(std::move(creator), {StoredObject("", path, size)}, settings);
    return true;
}

std::optional<ContentAddressedMetadataStorage::BlobViewPlan> ContentAddressedMetadataStorage::getBlobViewPlan(
    const std::string & path) const
{
    if (!Cas::isPartFilePath(path))
        return std::nullopt;
    auto p = Cas::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;
    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::CachedForLoad);
    if (!view)
        return std::nullopt;
    if (const auto * entry = view->findFile(r->file))
    {
        const auto location = store()->locate(*entry);
        BlobViewPlan plan;
        /// bytes_size is the readable extent of THIS file's window, NOT the whole blob: a
        /// right-bounded read stops at payload_end, and a shared blob's bytes beyond it belong
        /// to other files. The caches key on the physical blob key, so payload ranges are
        /// shared between every part that references the same blob.
        plan.object = StoredObject(physicalKey(location.key), path, location.offset + location.length);
        plan.payload_offset = location.offset;
        plan.payload_end = location.offset + location.length;
        return plan;
    }
    return std::nullopt;
}

std::unique_ptr<ReadBufferFromFileBase> ContentAddressedMetadataStorage::readBlobPayload(
    const Cas::BlobLocation & location, const std::string & path, const ReadSettings & settings) const
{
    auto impl = object_storage->readObject(
        StoredObject(physicalKey(location.key), path, location.offset + location.length), settings);
    return std::make_unique<ReadBufferFromFileView>(
        std::move(impl), path, location.offset, location.offset + location.length);
}

/// ==== `IContentAddressedExchange` ====

std::optional<String> ContentAddressedMetadataStorage::getPartManifestBytes(const String & part_path) const
{
    /// Sender side: the committed part's encoded `PartManifest` body — the opaque payload the
    /// receiver decodes. Resolve the part path to its (ns, ref) exactly as the read surface does
    /// (route), resolve the committed ref to its ManifestId, read the immutable manifest, and re-encode
    /// it canonically. nullopt when the path is not a committed content-addressed part here (no ref =>
    /// no relink offer; the sender streams bytes). A live ref to a missing/corrupt manifest throws
    /// (INV-NO-DANGLE surfaced, never substituted) — the same fail-loud contract as partAccess()->getView.
    auto p = Cas::parsePartFilePath(part_path);
    if (!p)
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->ref.empty())
        return std::nullopt;

    auto view = partAccess()->getView(r->refKey(), Cas::Freshness::ForceFresh);
    if (!view)
        return std::nullopt;
    return Cas::encodePartManifest(*view->manifest());
}

/// TRUST MODEL: adopting a part from a peer-supplied manifest is exactly as trusted as an ordinary
/// ReplicatedMergeTree interserver part fetch. The interserver HTTP channel — not a per-blob ACL — is
/// the trust boundary: a malicious or MITM peer on that channel can already serve arbitrary part bytes
/// that the receiver adopts, in both the byte-streaming and the relink path. Table-level RBAC never
/// defended against a hostile peer, so relink-by-manifest adds no new trust surface. (See the retracted
/// umbrella "RBAC bypass" finding.)
bool ContentAddressedMetadataStorage::adoptPartFromManifest(
    const String & table_uuid, const String & part_name, const String & manifest_bytes)
{
    checkNotReadOnly("adoptPartFromManifest (interserver relink receiver)");

    /// Receiver side. Sender identity is non-authoritative: we ignore the decoded
    /// ManifestRef, root_namespace_id and payload_digest, and use ONLY the entries. We run a normal
    /// LOCAL build (the proven republishRef sequence) over the SHARED-pool blobs — adopted by hash via
    /// adoptEvidence, NO blob body transferred — then stage a FRESH receiver-local ManifestId in the
    /// receiver namespace, `precommitAdd`, and promote. Promotion trusts the adopted leaves
    /// via the durable manifest edge (no per-file HEAD/loadMeta probe); a genuinely-absent adopted blob is
    /// an invariant violation caught by fsck, not here — the ordinary
    /// ReplicatedMergeTree interserver trust. A retryable promote failure (a body-absent precommit, a
    /// precommit that is no longer the live owner, or a ref conflict => `ABORTED` or `NETWORK_ERROR`, the
    /// retry-later class), a manifest decode failure, or any other error returns false, publishing NOTHING,
    /// so the caller byte-fetches instead.

    Cas::PartManifest decoded;
    try
    {
        decoded = Cas::decodePartManifest(manifest_bytes);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::CORRUPTED_DATA)
            throw;
        LOG_INFO(getLogger("ContentAddressedMetadataStorage"), "Relink of part {} not possible: transferred manifest failed to decode ({}); "
            "caller falls back to a byte fetch", part_name, e.message());
        return false;
    }

    /// The RECEIVER namespace, derived from THIS server's table_uuid — never the sender's
    /// root_namespace_id (which is foreign to this server's path-mirroring identity).
    const Cas::RootNamespace receiver_ns = liveNamespace(table_uuid);

    try
    {
        /// Sender identity is NON-AUTHORITATIVE: only the entries are used. The blobs are already
        /// in the shared pool (referenced by hash) — publishEntries adopts them as tokenless
        /// W-EVIDENCE; promotion trusts them through the durable manifest edge (no per-blob re-probe).
        ///
        /// There is a commit-before-release gap. The relink sender is
        /// fire-and-forget (DataPartsExchange.cpp:256-259 releases the source part before this commit), so
        /// there is no in-degree overlap: if THIS precommitAdd edge-PUT stalls across >= 2 GC folds while
        /// the source's Outdated part is concurrently collected and the blob has no other ref, the blob can
        /// be reclaimed under us → a dangling committed manifest (fsck-detected). deleteExact covers every
        /// token-CHANGE recovery; only this same-token tail remains. A retention floor for read-replica
        /// snapshots is the intended protection for the source manifest while this asynchronous relink
        /// is in progress, but that protocol is not wired here yet; the current same-token tail remains
        /// an acknowledged fsck-detectable risk.
        /// Do NOT build a bespoke relink handshake — the Poco interserver transport is half-duplex.
        partAccess()->publishEntries({receiver_ns, part_name}, decoded.entries, Cas::ProvenanceOp::Attach);
        return true;
    }
    catch (const Exception & e)
    {
        /// `ABORTED` or `NETWORK_ERROR` means a body-absent precommit, a precommit binding that is no longer
        /// the live owner, or a ref conflict: retryable, the caller byte-fetches. Both codes are part of
        /// the retry-later path so merge backoff can engage. Promotion trusts the adopted pool blobs through the durable manifest edge,
        /// so an absent or condemned blob is an fsck finding, not a relink abort. Any other exception:
        /// fail safe to a byte fetch too (publish
        /// nothing), but log it as it is not the expected retryable path.
        if (e.code() == ErrorCodes::ABORTED || e.code() == ErrorCodes::NETWORK_ERROR)
            LOG_INFO(getLogger("ContentAddressedMetadataStorage"), "Relink of part {} deferred (body-absent precommit, "
                "precommit not the live owner, or a ref conflict): {}; caller falls back to a byte fetch", part_name, e.message());
        else
            LOG_WARNING(getLogger("ContentAddressedMetadataStorage"), "Relink of part {} failed with an unexpected error: {}; caller falls back to a "
                "byte fetch", part_name, e.message());
        return false;
    }
}

}
