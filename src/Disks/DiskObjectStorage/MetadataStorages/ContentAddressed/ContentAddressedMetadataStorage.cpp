#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
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
    const auto & suffix = ContentAddressed::kCasArchiveSuffix;
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
    uint64_t root_shards_,
    String disk_name_,
    uint64_t dedup_cache_bytes_,
    uint64_t dedup_head_first_min_bytes_,
    uint64_t gc_snap_generations_to_keep_,
    uint64_t gc_shards_,
    uint64_t manifest_sweep_list_budget_keys_,
    uint64_t manifest_sweep_delete_budget_keys_,
    uint64_t manifest_soft_limit_,
    uint64_t manifest_hard_limit_,
    uint64_t manifest_max_delay_ms_,
    uint64_t gc_max_conditional_put_bytes_,
    uint64_t cas_part_folder_cache_bytes_,
    uint64_t cas_part_folder_cache_max_entries_,
    uint64_t cas_part_folder_cache_max_entry_bytes_,
    uint64_t manifest_decode_cache_bytes_,
    uint64_t gc_meta_pool_size_,
    StagingBackend staging_backend_,
    uint64_t s3_staging_min_bytes_)
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
    , root_shards(root_shards_)
    , dedup_cache_bytes(dedup_cache_bytes_)
    , dedup_head_first_min_bytes(dedup_head_first_min_bytes_)
    , gc_snap_generations_to_keep(gc_snap_generations_to_keep_)
    , gc_shards(gc_shards_)
    , manifest_sweep_list_budget_keys(manifest_sweep_list_budget_keys_)
    , manifest_sweep_delete_budget_keys(manifest_sweep_delete_budget_keys_)
    , manifest_soft_limit(manifest_soft_limit_)
    , manifest_hard_limit(manifest_hard_limit_)
    , manifest_max_delay_ms(manifest_max_delay_ms_)
    , gc_max_conditional_put_bytes(gc_max_conditional_put_bytes_)
    , cas_part_folder_cache_bytes(cas_part_folder_cache_bytes_)
    , cas_part_folder_cache_max_entries(cas_part_folder_cache_max_entries_)
    , cas_part_folder_cache_max_entry_bytes(cas_part_folder_cache_max_entry_bytes_)
    , manifest_decode_cache_bytes(manifest_decode_cache_bytes_)
    , gc_meta_pool_size(gc_meta_pool_size_)
    , staging_backend(staging_backend_)
    , s3_staging_min_bytes(s3_staging_min_bytes_)
{
}

StagingBackend ContentAddressedMetadataStorage::parseStagingBackend(
    const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    const std::string value = config.getString(config_prefix + ".cas_staging_backend", "local");
    if (value == "local")
        return StagingBackend::Local;
    if (value == "s3")
        return StagingBackend::S3;
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "Unknown cas_staging_backend value '{}' (expected 'local' or 's3')", value);
}

uint64_t ContentAddressedMetadataStorage::parseS3StagingMinBytes(
    const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    return config.getUInt64(config_prefix + ".cas_s3_staging_min_bytes", 64ULL << 20);
}

void ContentAddressedMetadataStorage::runOneGcRoundForTest()
{
    /// The pacing scheduler must be STABLE across calls: the lease's observation-window steal
    /// protocol compares consecutive observations of the SAME observer (gc_id), so an ad-hoc
    /// scheduler per call would acquire the lease on the first call and then back off forever
    /// ("incumbent alive" - its own previous incarnation). Found by the M-W retro: the original
    /// per-call one-shot made every round after the first a silent no-op.
    if (!gc_scheduler)
        gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(
            store(), gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
            disk_name, makeGcRoundLogger());
    gc_scheduler->runOneRoundNow();
}

ContentAddressed::GcRoundLogger ContentAddressedMetadataStorage::makeGcRoundLogger() const
{
    /// Unit tests pass a null context (no system logs); the scheduler then runs without a sink.
    if (!context)
        return {};
    auto ctx = context;
    /// The configured disk name (threaded from the metadata-storage factory); falls back to
    /// storage_path_prefix for callers that don't supply one (e.g. unit tests).
    const String disk = disk_name;
    return [ctx, disk](const ContentAddressed::GcRoundLogRecord & r)
    {
        auto log = ctx->getContentAddressedGarbageCollectionLog();
        if (!log)
            return;
        ContentAddressedGarbageCollectionLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        e.event_type = r.event_type == ContentAddressed::GcRoundLogRecord::EventType::Start
            ? ContentAddressedGarbageCollectionLogElement::START
            : ContentAddressedGarbageCollectionLogElement::FINISH;
        e.disk_name = r.disk_name.empty() ? disk : r.disk_name;
        e.gc_id = r.gc_id;
        e.trigger = r.trigger == ContentAddressed::GcRoundLogRecord::Trigger::Manual
            ? ContentAddressedGarbageCollectionLogElement::MANUAL
            : ContentAddressedGarbageCollectionLogElement::SCHEDULED;
        switch (r.outcome)
        {
            case ContentAddressed::GcRoundLogRecord::Outcome::Unknown:
                e.outcome = ContentAddressedGarbageCollectionLogElement::UNKNOWN;
                break;
            case ContentAddressed::GcRoundLogRecord::Outcome::Success:
                e.outcome = ContentAddressedGarbageCollectionLogElement::SUCCESS;
                break;
            case ContentAddressed::GcRoundLogRecord::Outcome::NotALeader:
                e.outcome = ContentAddressedGarbageCollectionLogElement::NOT_A_LEADER;
                break;
            case ContentAddressed::GcRoundLogRecord::Outcome::Failed:
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
    /// Unit tests pass a null context (no system logs); the Store then runs without a sink.
    if (!context)
        return {};
    auto ctx = context;
    /// The configured disk name (threaded from the metadata-storage factory); falls back to
    /// storage_path_prefix for callers that don't supply one (e.g. unit tests).
    const String disk = disk_name;
    return [ctx, disk](const Cas::CasEvent & ev)
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
        e.namespace_ = ev.namespace_;
        e.ref_name = ev.ref_name;
        e.object_kind = toString(ev.object_kind);
        e.object_hash = ev.object_hash;
        e.token = ev.token;
        e.round = ev.round;
        e.gen = ev.gen;
        e.at_version = ev.at_version;
        e.outcome = ev.outcome;
        e.reason = ev.reason;
        e.thread_id = getThreadId();
        e.query_id = CurrentThread::getQueryId();
        e.detail = ev.detail;
        /// Best-effort: SystemLog::add never blocks the Core; a full queue drops the row with a warning.
        log->add(std::move(e));
    };
}

Cas::RoundReport ContentAddressedMetadataStorage::runGarbageCollectionRoundNow()
{
    if (read_only || !gc_enabled)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Garbage collection is not enabled on this content-addressed disk");
    /// Mirror runOneGcRoundForTest: a STABLE scheduler instance across calls (the lease's
    /// observation-window steal protocol compares consecutive observations of the same gc_id).
    if (!gc_scheduler)
        gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(
            store(), gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
            disk_name, makeGcRoundLogger());
    return gc_scheduler->runOneRoundNow(ContentAddressed::GcRoundLogRecord::Trigger::Manual);
}

Cas::RebuildReport ContentAddressedMetadataStorage::runGcRebuildNow(bool force)
{
    if (read_only || !gc_enabled)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Garbage collection is not enabled on this content-addressed disk");
    /// A one-shot Gc instance is fine here (unlike the scheduler's stable-instance requirement for
    /// the lease's observation-window steal protocol): rebuildBaseline does its own lease
    /// acquire/steal check internally and this command runs exactly one round.
    const UInt128 gc_id = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
    Cas::Gc gc(store(), gc_id);
    return gc.rebuildBaseline(force);
}

void ContentAddressedMetadataStorage::startup()
{
    if (cas_store)
        return;

    /// Observe-only mode (the disk's <readonly> config): skip the probe (a probe write would fail on
    /// a read-only backend), run no watermark, start no GC, and fail the mutating surface closed.
    read_only = object_storage->isReadOnly();

    /// Native mode rides real conditional ops (probed fail-closed by Store::open); Local object
    /// storage has none, so the backend emulates exact token semantics in-process (single server).
    const auto mode = object_storage->getType() == ObjectStorageType::Local
        ? Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess
        : Cas::ObjectStorageBackend::Mode::Native;
    auto backend = std::make_shared<Cas::ObjectStorageBackend>(object_storage, mode, gc_max_conditional_put_bytes);

    /// EmulatedSingleProcess emulates the conditional-op / exact-token semantics in-process (local
    /// object storage has none). That emulation is per-process: two servers pointed at the SAME local
    /// pool (e.g. an NFS/shared mount) each keep independent token state and would silently violate
    /// the CAS invariants — the capability probe cannot detect this (each process passes it alone).
    /// Make a shared-pool misconfiguration visible (review #1 / B25), but at INFO — NOT WARNING.
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
    /// T13 finding; MinIO and the emulation merely tolerated it).
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
    /// Creation-time only (the pool is authoritative on reopen): widening the shard fanout spreads
    /// manifest CAS writes across more keys, reducing per-key congestion + per-key overwrite-orphan
    /// pileup (#4). Existing pools keep their shard count.
    pool_config.root_shards = root_shards;
    pool_config.dedup_cache_bytes = dedup_cache_bytes;
    pool_config.dedup_head_first_min_bytes = dedup_head_first_min_bytes;
    pool_config.manifest_decode_cache_bytes = manifest_decode_cache_bytes;
    pool_config.gc_snap_generations_to_keep = gc_snap_generations_to_keep;
    pool_config.gc_shards = gc_shards;
    pool_config.manifest_sweep_list_budget_keys = manifest_sweep_list_budget_keys;
    pool_config.manifest_sweep_delete_budget_keys = manifest_sweep_delete_budget_keys;
    pool_config.manifest_soft_limit = manifest_soft_limit;
    pool_config.manifest_hard_limit = manifest_hard_limit;
    pool_config.manifest_max_delay_ms = manifest_max_delay_ms;
    pool_config.gc_meta_pool_size = gc_meta_pool_size;
    cas_store = Cas::Store::open(std::move(backend), std::move(pool_config));
    pool_uuid = Cas::u128ToHex(cas_store->poolMeta().pool_id);
    part_access = std::make_unique<ContentAddressed::CachedPartFolderAccess>(cas_store,
        ContentAddressed::CachedPartFolderAccess::CacheParams{
            .cache_bytes = cas_part_folder_cache_bytes,
            .max_entries = cas_part_folder_cache_max_entries,
            .max_entry_bytes = cas_part_folder_cache_max_entry_bytes});

    /// B170: bridge per-event CAS decisions to system.content_addressed_log (null sink when context
    /// is absent, e.g. unit tests — emitEvent is then a no-op single branch in the Core).
    cas_store->setEventSink(makeCasEventSink());

    /// The background GC scheduler runs only on the disk-factory path (context non-null) and when
    /// enabled - the lease makes concurrent schedulers across mounters safe (work dedup), so no
    /// further gating is needed (D-W5).
    if (context && gc_enabled && !read_only)
    {
        gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(
            cas_store, gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
            disk_name, makeGcRoundLogger());
        gc_scheduler->start();
    }
}

void ContentAddressedMetadataStorage::shutdown()
{
    if (gc_scheduler)
    {
        gc_scheduler->stop();
        gc_scheduler.reset();
    }
    part_access.reset();
    cas_store.reset();
}

const Cas::StorePtr & ContentAddressedMetadataStorage::store() const
{
    if (!cas_store)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: store accessed before startup");
    return cas_store;
}

ContentAddressed::CachedPartFolderAccess & ContentAddressedMetadataStorage::partAccess() const
{
    if (!part_access)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: partAccess accessed before startup");
    return *part_access;
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY, "Content-addressed disk is opened read-only: writes are rejected");
    return std::make_shared<ContentAddressedTransaction>(*this);
}

/// ==== D-W1 namespace mapping ====

std::string ContentAddressedMetadataStorage::serverPrefix() const
{
    /// Phase 1 layout: live namespaces and mirrored live-tree files are rooted by the configured
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
    /// Path-mirroring (design §5.1): the namespace IS the table's canonical disk path with the
    /// content-addressed boundary marked by `@cas@` on the table-dir segment, prefixed by the
    /// configured `server_root_id`. e.g. `<server_root_id>/store/3f2/3f2a…@cas@`.
    return Cas::RootNamespace{serverPrefix() + "/" + ContentAddressed::mirroredArchiveNamespace(table_uuid)};
}

Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir)
{
    /// The LITERAL shadow table dir (shadow/<backup>/store/<u3>/<uuid> or .../data/<db>/<tbl>):
    /// bijective with the disk path for both layouts, pool-global (backups are read by any
    /// replica), and the shadow tree enumerates from Store::listNamespaces("shadow/").
    /// CANONICALIZED: the Unfreezer hands the dir with a trailing slash (T13 finding).
    return Cas::RootNamespace{canonicalDiskPath(shadow_table_dir)};
}


std::optional<ContentAddressedMetadataStorage::Route>
ContentAddressedMetadataStorage::route(const ContentAddressed::PartFilePath & p) const
{
    Route r;
    if (!p.backup_name.empty())
    {
        r.ns = shadowNamespace(p.shadow_table_dir);
        r.ref = p.part_name;
        r.file = p.file;
        return r;
    }
    if (p.part_name == ContentAddressed::kDetachedDirName)
    {
        /// The parser reports detached paths with part_name == "detached" and the real detached
        /// part dir as the first component of `file` (the PoC contract, B36). B181 folds detached
        /// INTO the table's OWN archive namespace: each detached part is a ref keyed by
        /// `detached/<part>` (vs a live `<part>`), so the re-split here keeps the table namespace
        /// and prepends the `detached/` ref prefix. An empty `p.file` (the bare `<table>/detached`
        /// container dir) yields an empty ref → the filtered-container listing path.
        r.ns = liveNamespace(p.table_uuid);
        auto [part, file] = splitFirstComponent(p.file);
        r.ref = part.empty() ? "" : std::string(ContentAddressed::kDetachedRefPrefix) + part;
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
        if (ref.starts_with(ContentAddressed::kDetachedRefPrefix))
            refs.push_back(ref);
    return refs;
}

/// ==== read surface ====

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            return store()->getNamespaceFile(liveNamespace(tf->table_uuid), tf->tail).has_value();
        /// A loose mountpoint object (design §5.2): a plain object at roots/<server_root_id>/<path>.
        /// Use a HEAD-based existence check (directory-safe), NOT a body read: the traversal in
        /// system.remote_data_paths probes existsFile on directory-shaped pool paths (e.g. `store`), and a
        /// body read (getMountpointObject) throws "Is a directory". A directory is not-a-file (B38).
        return store()->mountpointObjectExists(serverPrefix() + "/" + path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto r = route(*p);
    if (!r || r->file.empty())
        return false;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B): read-your-writes for a just-written mutable file — no TTL-stale manifest.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        return resolved && !ContentAddressed::PartFolderView::isReservedMutableName(r->file)
            && resolved->mutable_files.contains(r->file);
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
    return view && view->findFile(r->file);
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    /// FREEZE shadow namespace — routed BEFORE the live branches (a shadow table dir also
    /// satisfies parseTableUuid).
    if (ContentAddressed::isShadowPath(path))
    {
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
            return partAccess().existsRef(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""}.refKey(),
                                          ContentAddressed::Freshness::CachedForLoad);
        if (ContentAddressed::endsWithTableUuidPair(path))
            return !store()->listRefs(shadowNamespace(path)).empty();
        /// Intermediate dir (shadow/<bk>, shadow/<bk>/store, ...): exists iff the scoped S3
        /// LIST of the mirrored subtree finds any objects (design §5.3). A non-empty LIST means
        /// at least one shadow archive exists under this path; GC removes S3 objects for fully-
        /// dropped archives, so a bare LIST is a reliable existence signal without registry access.
        const std::string canonical = canonicalDiskPath(path);
        const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
        return !store()->listMirroredChildren(scope).empty();
    }

    /// The Atomic `store/<u3>` shard dir (see listDirectory): route to the generic existence signal
    /// before parseTableUuid/parseTableFilePath misclaim it as a non-Atomic table.
    if (ContentAddressed::isAtomicShardDir(path))
        return liveTreeDirHasChildren(path);

    if (auto uuid = ContentAddressed::parseTableUuid(path))
        /// Table dir exists iff it has at least one committed part (the PoC's refs-only rule).
        return !store()->listRefs(liveNamespace(*uuid)).empty();

    auto p = ContentAddressed::parsePartFilePath(path);
    if (p)
    {
        auto r = route(*p);
        /// The detached CONTAINER dir <table>/detached: exists iff it has at least one ref (B181).
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
            return !detachedRefNames(r->ns).empty();
        /// A part dir (live, detached, or shadow): exists iff its ref is present.
        if (r && !r->ref.empty() && r->file.empty())
            return partAccess().existsRef(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
        /// A projection dir: at least one tree entry (or mutable file) under its prefix.
        if (r && !r->ref.empty())
        {
            if (auto prefix = ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
            {
                auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
                return view && view->hasDirectory(*prefix);
            }
        }
    }

    /// A table-level SUBDIRECTORY (deduplication_logs/...): at least one verbatim file under it.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string prefix = tf->tail + "/";
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(tf->table_uuid)))
            if (name.starts_with(prefix))
                return true;
        return false;
    }

    /// A generic INTERMEDIATE live-tree directory (disk root, `store`, ...): exists iff a
    /// server-root-scoped mirrored LIST finds any object. Keeps `cd`/existence consistent with
    /// listDirectory so `clickhouse-disks` traversal behaves like a normal disk.
    return liveTreeDirHasChildren(path);
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    if (ContentAddressed::isPartFilePath(path))
    {
        auto p = ContentAddressed::parsePartFilePath(path);
        auto r = p ? route(*p) : std::nullopt;
        if (r && !r->ref.empty() && !r->file.empty() && !ContentAddressed::isMutablePerPartFile(r->file))
        {
            auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
            if (!view)
                return false;
            return view->hasFile(r->file) || view->hasDirectory(r->file + "/");
        }
    }
    return existsFile(path) || existsDirectory(path);
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto bytes = tryGetInManifestBytes(path))   /// verbatim table-level file
            return bytes->size();
        if (auto bytes = store()->getMountpointObject(serverPrefix() + "/" + path))
            return bytes->size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B): read-your-writes for a just-written mutable file.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        if (resolved && !ContentAddressed::PartFolderView::isReservedMutableName(r->file))
            if (auto it = resolved->mutable_files.find(r->file); it != resolved->mutable_files.end())
                return it->second.size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
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
        auto resolved = partAccess().resolve(r.refKey(), ContentAddressed::Freshness::CachedForLoad);
        if (!resolved)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
        if (resolved->published_at_ms == 0)
            return Poco::Timestamp(0);
        /// published_at_ms is epoch milliseconds; Poco::Timestamp::fromEpochTime takes seconds.
        return Poco::Timestamp::fromEpochTime(static_cast<time_t>(resolved->published_at_ms / 1000));
    };

    if (auto p = ContentAddressed::parsePartFilePath(path))
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
    if (ContentAddressed::isShadowPath(path))
    {
        /// Shadow PART dir: the frozen part's file names (first components).
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            auto view = partAccess().getView(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""}.refKey(),
                                             ContentAddressed::Freshness::CachedForLoad);
            return view ? view->listChildren("") : std::vector<std::string>{};
        }
        /// Shadow TABLE dir: the frozen part names.
        if (ContentAddressed::endsWithTableUuidPair(path))
        {
            std::vector<std::string> result;
            for (const auto & [ref, _] : store()->listRefs(shadowNamespace(path)))
                result.push_back(ref);
            return result;
        }
        /// Shadow INTERMEDIATE dir: enumerate children via a scoped S3 LIST of the mirrored
        /// subtree (design §5.3). A mirrored LIST naturally surfaces intermediate path segments
        /// AND `@cas@`-suffixed table dirs; strip the trailing `@cas@` for the logical view.
        /// Loose LIST is fine: the existing listRefs re-check filters out dropped-but-registered
        /// archives so they don't appear as false children.
        const std::string canonical = canonicalDiskPath(path);
        const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
        std::unordered_set<std::string> result;
        for (const auto & child : store()->listMirroredChildren(scope))
            result.emplace(stripCasArchiveSuffix(child));
        return toVector(std::move(result));
    }

    /// The Atomic `store/<u3>` shard dir: a pure intermediate dir whose only child is the
    /// uuid-anchored table dir. Its path shape collides with the non-Atomic `data/<db>` fallback of
    /// both parseTableUuid and parseTableFilePath, so it MUST be routed to the generic mirrored LIST
    /// BEFORE those branches claim it.
    if (ContentAddressed::isAtomicShardDir(path))
        return listLiveTreeChildren(path);

    /// Table dir: part names (live + B181 detached `detached/<part>` refs) + table-level verbatim
    /// file names; addFirstComponent collapses both to their first path segment (live part names and
    /// the single `detached` subdir, exactly like a nested verbatim file).
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        std::unordered_set<std::string> result;
        for (const auto & [ref, _] : store()->listRefs(liveNamespace(*uuid)))
            addFirstComponent(result, ref);
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(*uuid)))
            addFirstComponent(result, name);
        return toVector(std::move(result));
    }

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = route(*p);
        /// The detached CONTAINER dir: detached part names (prefix stripped; never files, B36/B181).
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
        {
            std::vector<std::string> result;
            for (const auto & ref : detachedRefNames(r->ns))
                result.push_back(ref.substr(ContentAddressed::kDetachedRefPrefix.size()));
            return result;
        }
        /// A part dir (live, detached part, shadow handled above): logical file names, nested
        /// keys collapsed to their first component (projections surface as ONE <proj>.proj entry).
        if (r && !r->ref.empty() && r->file.empty())
        {
            auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
            return view ? view->listChildren("") : std::vector<std::string>{};
        }
        /// A projection dir: inner names with the <proj>.proj/ prefix stripped.
        if (r && !r->ref.empty())
        {
            if (auto prefix = ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
            {
                auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
                return view ? view->listChildren(*prefix) : std::vector<std::string>{};
            }
        }
    }

    /// A table-level SUBDIRECTORY: verbatim files under <subdir>/, first-component collapsed.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string prefix = tf->tail + "/";
        std::unordered_set<std::string> result;
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(tf->table_uuid)))
            if (name.starts_with(prefix))
                addFirstComponent(result, name.substr(prefix.size()));
        return toVector(std::move(result));
    }

    /// A generic INTERMEDIATE live-tree directory (the disk root "", `store`, or any loose-file
    /// container above a table dir): a server-root-scoped mirrored LIST. (`store/<u3>` is handled by the
    /// early guard above, since its non-Atomic-table ambiguity would otherwise misroute it here too
    /// late, after parseTableUuid/parseTableFilePath have already claimed it.)
    return listLiveTreeChildren(path);
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
    /// CANNOT_RMDIR per removal (the PoC's B45). Same for a projection subdir (B60). The detached
    /// CONTAINER and TABLE dirs keep the listing-based emptiness (DROP TABLE's non-empty guard).
    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = route(*p);
        if (r && !r->ref.empty() && r->file.empty())
            return true;
        if (r && !r->ref.empty() && ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
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

    if (!ContentAddressed::isPartFilePath(path))
    {
        if (ContentAddressed::parseTableFilePath(path))
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "ContentAddressed: table-level verbatim file is in-manifest, not a storage object: {}", path);
        /// A loose mountpoint object: a real plain object at roots/<server_root_id>/<path>. The
        /// StoredObject key must be the PHYSICAL path (physicalKey-adjusted for Local backends).
        /// Probe with a HEAD (directory-safe, B38), NOT a body read: `system.remote_data_paths`
        /// may reach here on a directory-shaped pool path and a GET would throw "Is a directory".
        const std::string pool_key = store()->layout().mountpointObjectKey(serverPrefix() + "/" + path);
        if (store()->mountpointObjectExists(serverPrefix() + "/" + path))
            return {StoredObject(physicalKey(pool_key), path, getFileSize(path))};
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
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
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (existsFile(path))
            return getStorageObjects(path);
        return std::nullopt;
    }
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B), same contract as existsFile/tryGetInManifestBytes.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        if (!resolved || ContentAddressed::PartFolderView::isReservedMutableName(r->file))
            return std::nullopt;
        const auto it = resolved->mutable_files.find(r->file);
        if (it == resolved->mutable_files.end())
            return std::nullopt;
        /// Sized empty-key placeholder — same shape getStorageObjects returns for in-manifest bytes.
        return StoredObjects{StoredObject("", path, it->second.size())};
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
    if (!view)
        return std::nullopt;
    const auto * entry = view->findFile(r->file);
    if (!entry)
        return std::nullopt;
    if (entry->placement == Cas::EntryPlacement::Inline)
        return StoredObjects{StoredObject("", path, entry->inline_bytes.size())};
    const auto location = store()->locate(*entry);
    return StoredObjects{StoredObject(location.key, path, location.length)};
}

std::optional<String> ContentAddressedMetadataStorage::tryGetInManifestBytes(const std::string & path) const
{
    if (!cas_store)
        return std::nullopt;

    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            return store()->getNamespaceFile(liveNamespace(tf->table_uuid), tf->tail);
        return std::nullopt;   /// loose files are plain objects, not in-manifest bytes (design §5.2)
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B): MVCC txn_version / mutable-file read — must not serve a TTL-stale manifest.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        if (!resolved || ContentAddressed::PartFolderView::isReservedMutableName(r->file))
            return std::nullopt;
        auto it = resolved->mutable_files.find(r->file);
        if (it == resolved->mutable_files.end())
            return std::nullopt;
        return it->second;
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
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
    if (!ContentAddressed::isPartFilePath(path))
        return std::nullopt;
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;
    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
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

/// ==== IContentAddressedExchange (M-W T11; B7 part_manifest_v1) ====

std::optional<String> ContentAddressedMetadataStorage::getPartManifestBytes(const String & part_path) const
{
    /// Sender side (B7): the committed part's encoded `PartManifest` body — the opaque payload the
    /// receiver decodes. Resolve the part path to its (ns, ref) exactly as the read surface does
    /// (route), resolve the committed ref to its ManifestId, read the immutable manifest, and re-encode
    /// it canonically. nullopt when the path is not a committed content-addressed part here (no ref =>
    /// no relink offer; the sender streams bytes). A live ref to a missing/corrupt manifest throws
    /// (INV-NO-DANGLE surfaced, never substituted) — the same fail-loud contract as partAccess().getView.
    auto p = ContentAddressed::parsePartFilePath(part_path);
    if (!p)
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->ref.empty())
        return std::nullopt;

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::ForceFresh);
    if (!view)
        return std::nullopt;
    return Cas::encodePartManifest(*view->manifest());
}

bool ContentAddressedMetadataStorage::adoptPartFromManifest(
    const String & table_uuid, const String & part_name,
    const String & manifest_bytes, const std::map<String, String> & mutable_files)
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY, "Content-addressed disk is opened read-only: adoptPartFromManifest is rejected");

    /// Receiver side (B7 part_manifest_v1). Sender identity is NON-AUTHORITATIVE: we ignore the decoded
    /// ManifestRef, root_namespace_id and payload_digest, and use ONLY the entries. We run a normal
    /// LOCAL build (the proven republishRef sequence) over the SHARED-pool blobs — adopted by hash via
    /// adoptEvidence, NO blob body transferred — then stage a FRESH receiver-local ManifestId in the
    /// RECEIVER namespace, precommitAdd, and promote (fail-closed blob revalidation). Any retryable
    /// failure (decode/build/stage/precommit/promote, incl. a condemned/absent blob => ABORTED) returns
    /// false, publishing NOTHING, so the caller falls back to the byte fetch.

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
        /// W-EVIDENCE and promote re-proves each fail-closed (the proven republish sequence).
        partAccess().publishEntries({receiver_ns, part_name}, decoded.entries, mutable_files,
                                    Cas::ProvenanceOp::Attach);
        return true;
    }
    catch (const Exception & e)
    {
        /// ABORTED = a referenced blob is absent/condemned, or the precommit binding is not the live
        /// owner: retryable, the caller byte-fetches. Any other exception: fail SAFE to a byte fetch
        /// too (publish nothing), but log it as it is not the expected retryable path.
        if (e.code() == ErrorCodes::ABORTED)
            LOG_INFO(getLogger("ContentAddressedMetadataStorage"), "Relink of part {} aborted (a referenced blob is absent/condemned in the shared "
                "pool, or the precommit is not live): {}; caller falls back to a byte fetch", part_name, e.message());
        else
            LOG_WARNING(getLogger("ContentAddressedMetadataStorage"), "Relink of part {} failed with an unexpected error: {}; caller falls back to a "
                "byte fetch", part_name, e.message());
        return false;
    }
}

}
