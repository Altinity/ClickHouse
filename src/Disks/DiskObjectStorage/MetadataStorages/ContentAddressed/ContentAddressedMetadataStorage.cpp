#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
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
}

namespace
{

/// Wiring-reserved RefPayload.mutable_files keys (never real MergeTree files — dot-prefixed).
/// The publish wall-clock is now carried by the typed `RefPayload.published_at_ms` field (epoch ms).
bool isReservedMutableName(const std::string & name)
{
    return name.starts_with(".ca_");
}

/// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the same
/// recognizer the PoC used (B64: also matches the nested detached-staging shape). `file` here is
/// the ROUTED in-tree file path (the detached part prefix already split away).
std::optional<std::string> projectionDirPrefix(const std::string & file)
{
    if (file.empty())
        return std::nullopt;
    const auto last_slash = file.find_last_of('/');
    const std::string_view last_component
        = last_slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(last_slash + 1);
    if (last_component.ends_with(".proj") || last_component.ends_with(".tmp_proj"))
        return file + "/";
    return std::nullopt;
}

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
    String local_scratch_path_,
    ContextPtr context_,
    bool gc_enabled_,
    std::chrono::seconds gc_interval_,
    uint64_t root_shards_,
    String disk_name_,
    uint64_t dedup_cache_bytes_,
    uint64_t dedup_head_first_min_bytes_,
    uint64_t gc_snap_generations_to_keep_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
    , disk_name(!disk_name_.empty() ? disk_name_ : storage_path_prefix)
    , local_scratch_path(std::move(local_scratch_path_))
    , context(context_)
    , gc_enabled(gc_enabled_)
    , gc_interval(gc_interval_)
    , root_shards(root_shards_)
    , dedup_cache_bytes(dedup_cache_bytes_)
    , dedup_head_first_min_bytes(dedup_head_first_min_bytes_)
    , gc_snap_generations_to_keep(gc_snap_generations_to_keep_)
{
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
        e.children_cascaded = r.children_cascaded;
        e.forgotten_on_delete = r.forgotten_on_delete;
        e.forgotten_absent = r.forgotten_absent;
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
    auto backend = std::make_shared<Cas::ObjectStorageBackend>(object_storage, mode);

    /// EmulatedSingleProcess emulates the conditional-op / exact-token semantics in-process (local
    /// object storage has none). That emulation is per-process: two servers pointed at the SAME local
    /// pool (e.g. an NFS/shared mount) each keep independent token state and would silently violate
    /// the CAS invariants — the capability probe cannot detect this (each process passes it alone).
    /// Warn loudly so a shared-pool misconfiguration is visible (review #1 / B25).
    if (mode == Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess)
        LOG_WARNING(
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
    pool_config.background_watermark = (context != nullptr) && !read_only;
    pool_config.read_only = read_only;
    /// Creation-time only (the pool is authoritative on reopen): widening the shard fanout spreads
    /// manifest CAS writes across more keys, reducing per-key congestion + per-key overwrite-orphan
    /// pileup (#4). Existing pools keep their shard count.
    pool_config.root_shards = root_shards;
    pool_config.dedup_cache_bytes = dedup_cache_bytes;
    pool_config.dedup_head_first_min_bytes = dedup_head_first_min_bytes;
    pool_config.gc_snap_generations_to_keep = gc_snap_generations_to_keep;
    cas_store = Cas::Store::open(std::move(backend), std::move(pool_config));
    pool_uuid = Cas::u128ToHex(cas_store->poolMeta().pool_id);

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
    cas_store.reset();
}

const Cas::StorePtr & ContentAddressedMetadataStorage::store() const
{
    if (!cas_store)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: store accessed before startup");
    return cas_store;
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
    /// ONE canonical server token everywhere (Phase 6): the 32-hex `u128ToHex(serverIdToU128(server_id))`
    /// — exactly the form GC already uses for `precommitNs`/`serverWatermarkKey`. Using it as the
    /// server-prefix here lands a server's live/detached namespaces under the same `roots/<server-hex>/`
    /// subtree as its watermark and precommits, so "drop a server" is one subtree.
    return Cas::u128ToHex(serverIdToU128(server_id));
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
    /// The disk root always exists; otherwise a non-empty server-scoped mirrored LIST is the signal.
    if (canonical.empty())
        return true;
    const std::string scope = serverPrefix() + "/" + canonical + "/";
    return !store()->listMirroredChildren(scope).empty();
}

Cas::RootNamespace ContentAddressedMetadataStorage::liveNamespace(const std::string & table_uuid) const
{
    /// Path-mirroring (design §5.1): the namespace IS the table's canonical disk path with the
    /// content-addressed boundary marked by `@cas@` on the table-dir segment, prefixed by the
    /// canonical server token (`serverPrefix`). e.g. `<server-hex>/store/3f2/3f2a…@cas@`.
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

std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>
ContentAddressedMetadataStorage::resolveRouted(const Route & r) const
{
    auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);
    if (!resolved)
        return std::nullopt;
    /// A live ref to a missing/corrupt tree throws (INV-NO-DANGLE surfaced, never substituted).
    return std::make_pair(*resolved, store()->readTree(resolved->tree_id));
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
        /// A loose mountpoint object (design §5.2): a plain object at roots/<server>/<path>.
        return store()->getMountpointObject(server_id + "/" + path).has_value();
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
        auto resolved = store()->resolveRef(r->ns, r->ref);
        return resolved && !isReservedMutableName(r->file) && resolved->mutable_files.contains(r->file);
    }

    auto rt = resolveRouted(*r);
    if (!rt)
        return false;
    for (const auto & entry : rt->second)
        if (entry.name == r->file)
            return true;
    return false;
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    /// FREEZE shadow namespace — routed BEFORE the live branches (a shadow table dir also
    /// satisfies parseTableUuid).
    if (ContentAddressed::isShadowPath(path))
    {
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
            return store()->resolveRef(shadowNamespace(p->shadow_table_dir), p->part_name, /*allow_stale=*/true).has_value();
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
            return store()->resolveRef(r->ns, r->ref, /*allow_stale=*/true).has_value();
        /// A projection dir: at least one tree entry (or mutable file) under its prefix.
        if (r && !r->ref.empty())
        {
            if (auto prefix = projectionDirPrefix(r->file))
            {
                auto rt = resolveRouted(*r);
                if (!rt)
                    return false;
                for (const auto & entry : rt->second)
                    if (entry.name.starts_with(*prefix))
                        return true;
                for (const auto & [file, _] : rt->first.mutable_files)
                    if (file.starts_with(*prefix))
                        return true;
                return false;
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
    /// server-scoped mirrored LIST finds any object. Keeps `cd`/existence consistent with
    /// listDirectory so `clickhouse-disks` traversal behaves like a normal disk.
    return liveTreeDirHasChildren(path);
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    return existsFile(path) || existsDirectory(path);
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    if (auto bytes = tryGetInManifestBytes(path))
        return bytes->size();

    if (!ContentAddressed::isPartFilePath(path))
    {
        /// A loose mountpoint object (design §5.2): read and return its byte length.
        if (auto bytes = store()->getMountpointObject(server_id + "/" + path))
            return bytes->size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto rt = resolveRouted(*r);
    if (!rt)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    for (const auto & entry : rt->second)
        if (entry.name == r->file)
            return entry.file_size;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in tree of {}", r->file, path);
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
        auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);
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
            auto rt = resolveRouted(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""});
            if (!rt)
                return {};
            std::unordered_set<std::string> result;
            for (const auto & entry : rt->second)
                addFirstComponent(result, entry.name);
            for (const auto & [file, _] : rt->first.mutable_files)
                if (!isReservedMutableName(file))
                    addFirstComponent(result, file);
            return toVector(std::move(result));
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
            auto rt = resolveRouted(*r);
            if (!rt)
                return {};
            std::unordered_set<std::string> result;
            for (const auto & entry : rt->second)
                addFirstComponent(result, entry.name);
            for (const auto & [file, _] : rt->first.mutable_files)
                if (!isReservedMutableName(file))
                    addFirstComponent(result, file);
            return toVector(std::move(result));
        }
        /// A projection dir: inner names with the <proj>.proj/ prefix stripped.
        if (r && !r->ref.empty())
        {
            if (auto prefix = projectionDirPrefix(r->file))
            {
                auto rt = resolveRouted(*r);
                if (!rt)
                    return {};
                std::unordered_set<std::string> result;
                for (const auto & entry : rt->second)
                    if (entry.name.starts_with(*prefix))
                        result.emplace(entry.name.substr(prefix->size()));
                for (const auto & [file, _] : rt->first.mutable_files)
                    if (!isReservedMutableName(file) && file.starts_with(*prefix))
                        result.emplace(file.substr(prefix->size()));
                return toVector(std::move(result));
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
    /// container above a table dir): a server-scoped mirrored LIST. (`store/<u3>` is handled by the
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
        if (r && !r->ref.empty() && projectionDirPrefix(r->file))
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
        /// A loose mountpoint object: a real plain object at roots/<server>/<path>. The
        /// StoredObject key must be the PHYSICAL path (physicalKey-adjusted for Local backends).
        const std::string pool_key = store()->layout().mountpointObjectKey(server_id + "/" + path);
        if (store()->getMountpointObject(server_id + "/" + path))
            return {StoredObject(physicalKey(pool_key), path, getFileSize(path))};
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto rt = resolveRouted(*r);
    if (!rt)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    for (const auto & entry : rt->second)
    {
        if (entry.name != r->file)
            continue;
        const auto location = store()->locate(entry);
        /// StoredObject carries no range (the recorded upstream delta) — the PAYLOAD length is the
        /// size (what every size consumer wants); the header offset is applied by
        /// getBlobViewPlan's view window, the only byte-reading path.
        return {StoredObject(location.key, path, location.length)};
    }
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in tree of {}", r->file, path);
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
        auto resolved = store()->resolveRef(r->ns, r->ref);
        if (!resolved || isReservedMutableName(r->file))
            return std::nullopt;
        auto it = resolved->mutable_files.find(r->file);
        if (it == resolved->mutable_files.end())
            return std::nullopt;
        return it->second;
    }

    auto rt = resolveRouted(*r);
    if (!rt)
        return std::nullopt;
    for (const auto & entry : rt->second)
        if (entry.name == r->file && entry.placement == Cas::Placement::Inline)
            return entry.inline_bytes;
    return std::nullopt;
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
    auto rt = resolveRouted(*r);
    if (!rt)
        return std::nullopt;
    for (const auto & entry : rt->second)
    {
        if (entry.name != r->file)
            continue;
        const auto location = store()->locate(entry);
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

/// ==== IContentAddressedExchange (M-W T11; D-W4) ====

std::optional<String> ContentAddressedMetadataStorage::getPartTreeId(const String & part_path) const
{
    /// Sender side: the tree id THIS server's ref names for the part - the relink offer. No ref
    /// (or an unroutable path) means no offer; the sender streams bytes.
    if (!cas_store)
        return std::nullopt;
    auto p = ContentAddressed::parsePartFilePath(part_path);
    if (!p)
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->ref.empty() || !r->file.empty())
        return std::nullopt;
    auto resolved = store()->resolveRef(r->ns, r->ref, /*allow_stale=*/true);
    if (!resolved)
        return std::nullopt;
    return resolved->tree_id.string();
}

bool ContentAddressedMetadataStorage::adoptPart(
    const String & table_uuid, const String & part_name,
    const String & tree_id_hex, const std::map<String, String> & mutable_files)
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY, "Content-addressed disk is opened read-only: adoptPart is rejected");

    /// Receiver side: adopt-by-tree-id + publish this server's own ref. adoptTree OBSERVES the
    /// tree (cold reuse, 2026-06-12) - a tree reclaimed since the sender's offer surfaces as
    /// FILE_DOESNT_EXIST and the caller falls back to the byte fetch, exactly where the old
    /// 4-step pin protocol fell back. Publish-gate ABORTED propagates (a retryable fetch error).
    const Cas::TreeId tree{tree_id_hex};
    const auto ns = liveNamespace(table_uuid);
    auto build = store()->startBuild(
        Cas::BuildInfo{.intended_ref = ns.string() + "/" + part_name, .op = Cas::ProvenanceOp::Other});
    try
    {
        build->adoptTree(tree);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
            throw;
        build->abandon();
        return false;
    }
    Cas::RefPayload payload;
    payload.mutable_files = mutable_files;
    payload.published_at_ms = static_cast<uint64_t>(::time(nullptr)) * 1000;
    /// tree_size is not carried on the wire (B92: the adopt path publishes 0 until the size is
    /// recovered; no read path consumes it yet).
    /// B171: protect the adopted closure via a build-root precommit before the fail-closed publish,
    /// so GC cannot reclaim a tree/blob between this server's adopt and its own commit.
    build->precommit(tree);
    build->publish(ns, part_name, tree, std::move(payload));
    return true;
}

}
