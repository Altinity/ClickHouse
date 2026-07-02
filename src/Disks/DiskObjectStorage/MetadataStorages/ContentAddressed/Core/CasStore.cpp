#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <city.h>
#include <algorithm>
#include <thread>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}
}

namespace ProfileEvents
{
    extern const Event CasManifestBackpressureCount;
    extern const Event CasManifestBackpressureMicroseconds;
    extern const Event CasManifestHardLimitExceeded;
}

namespace DB::Cas
{

/// Runaway brake on CAS/conditional-write retry loops. These keys are single-owner (one writer per
/// namespace by construction), so a contention storm is impossible — the bound only catches a
/// pathological live-lock and never a legitimate steady state.
static constexpr size_t MAX_CAS_ATTEMPTS = 100;

Store::Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_)
    : pool_backend(std::move(backend_))
    , config(std::move(config_))
    , meta(std::move(meta_))
    , pool_layout(config.pool_prefix)
    , retire_view(pool_backend, pool_layout)
{
    if (config.dedup_cache_bytes > 0)
        dedup_cache = std::make_unique<DedupCache>(
            "LRU", CurrentMetrics::end(), CurrentMetrics::end(),
            config.dedup_cache_bytes, DedupCache::NO_MAX_COUNT, DedupCache::DEFAULT_SIZE_RATIO);
}

bool Store::dedupCacheContains(const UInt128 & blob_hash) const
{
    return dedup_cache && dedup_cache->contains(blob_hash);
}

void Store::dedupCacheAdd(const UInt128 & blob_hash)
{
    if (dedup_cache)
        dedup_cache->set(blob_hash, std::make_shared<DedupPresent>());
}

bool Store::mayMutate() const
{
    return !mount_fence.lost.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < mount_fence.deadline.load(std::memory_order_acquire);
}

void Store::tripMountLost()
{
    mount_fence.lost.store(true, std::memory_order_release);
}

void Store::setMountDeadline(std::chrono::steady_clock::time_point d)
{
    mount_fence.deadline.store(d, std::memory_order_release);
}

void Store::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, std::chrono::steady_clock::time_point deadline)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    mount_fence.deadline.store(deadline, std::memory_order_release);
    mount_fence.lost.store(false, std::memory_order_release);
}

StorePtr Store::open(BackendPtr backend, PoolConfig config)
{
    /// B168 P0: wrap the pool backend once, transparently, so EVERY CA S3 op — probe, pool-meta,
    /// writer, GC, watermark — flows through the per-namespace/op ProfileEvents chokepoint. The
    /// decorator only delegates and counts; it changes no behavior (read-only opens stay write-free).
    backend = std::make_shared<InstrumentedBackend>(std::move(backend));

    /// FAIL-CLOSED (design §6): the capability probe throws NOT_IMPLEMENTED on any failed check, and
    /// PoolMeta::createOrValidate is pool-authoritative — the config constants apply only at creation.
    Layout layout(config.pool_prefix);
    /// The probe writes and deletes throwaway keys to verify conditional-op enforcement. A read-only
    /// open must never mutate the pool it inspects; fsck only reads, so skip it. (Pool meta below is
    /// read-only when the pool already exists; a missing pool meta on a read-only backend fails closed.)
    if (!config.read_only)
    {
        /// B135: give each mount a PER-MOUNT UNIQUE probe key prefix so two servers mounting the SAME
        /// shared pool concurrently never collide on the (formerly fixed) `<pool>/_probe/token` /
        /// `<pool>/_probe/cas` keys. Without this, the loser of the `putIfAbsent` race aborts startup
        /// with PreconditionFailed (and the winner's cleanup delete can cascade into the loser). With a
        /// fresh random 128-bit id per `Store::open`, each mounter validates conditional-op support
        /// independently. A crashed mount leaves harmless `_probe/<rand>/...` debris under the `_probe/`
        /// namespace only (never the content planes) — acceptable.
        const UInt128 probe_uid = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
        runCapabilityProbe(*backend, config.pool_prefix + "/_probe/" + u128ToHex(probe_uid));
    }
    PoolMeta meta = PoolMeta::createOrValidate(*backend, layout, config.root_shards, config.blob_header_len);

    /// Private ctor: make_shared cannot reach it.
    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));

    /// Prime the writer-side retire view once (rare by construction; see RetireView).
    store->retire_view.refresh();

    /// Per-server watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random NONZERO
    /// value minted once per Store: GC checks it for equality only (a different epoch == a dead
    /// incarnation). It rides through the watermark protobuf codec (uint64 field — full range). For
    /// safety and to avoid the 0/UINT64_MAX sentinels, mask to 52 bits (collision-safe for an
    /// equality-only token) and re-draw on 0 (UINT64_MAX is the retired sentinel).
    constexpr uint64_t EPOCH_MASK = (1ULL << 52) - 1;
    store->process_epoch = (thread_local_rng() ^ (static_cast<uint64_t>(thread_local_rng()) << 32)) & EPOCH_MASK;
    if (store->process_epoch == 0)
        store->process_epoch = 1;

    /// W-ANCHOR: the per-server watermark must be durable BEFORE any object PUT. A read-only open
    /// must never mutate the pool (the probe is skipped above for the same reason), so the watermark
    /// — which writes the roots/<server-hex>/_watermark slot — is only constructed and anchored on a writable open.
    if (!store->config.read_only)
    {
        /// === Mount-safety startup protocol (spec §mount-safety; Phase 0 Task 7) ===
        /// STRICT ORDER: validate id → claim owner (identity) → allocate durable writer_epoch → claim
        /// the mount lease (liveness) + arm the local write fence → anchor the watermark. owner / epoch
        /// / mount / watermark are BOOTSTRAP-CONTROL writes: they establish the very right to write and
        /// run BEFORE the write fence gates ordinary data/ref/manifest mutations. Fail closed throughout.
        const String & srid = store->config.server_root_id;
        const UInt128 our_uuid = store->config.server_id;

        /// 1. The server_root_id is a clean relative path (mirrors the config-read validation; cheap to
        ///    re-check here so a Store opened directly in tests is held to the same contract).
        validateServerRootId(srid);

        /// 2. Owner anchor — IDENTITY (clock-free). A foreign uuid fails closed; an absent owner over a
        ///    non-empty subtree is CORRUPTED_DATA; a fresh empty root is claimed.
        claimOwnerOrThrow(*store->pool_backend, store->pool_layout, srid, our_uuid);

        /// 3. Durable-monotone writer_epoch — CAS-bump the sticky `epoch` object. THE BRIDGE: this
        ///    durable value REPLACES the random `process_epoch` for identity, so the watermark + every
        ///    manifest ref carries it (the random mint above stays for the read-only
        ///    path, which never reaches here). Phase 2's epoch-aware sweep reads this value.
        const uint64_t writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
        store->process_epoch = writer_epoch;

        /// 4. Mount lease — LIVENESS. Decide over the current mount object using a wall-clock `now_ms`.
        const auto now_ms = []() -> uint64_t
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
        const uint64_t ttl_ms = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());
        /// Poll twice per renew period so a live holder's renewal is always observed within the wait;
        /// margin = one poll interval (covers poll granularity + minor wall-clock skew). Derived from
        /// existing config — no new knob (spec §Config).
        const uint64_t poll_interval_ms = std::max<uint64_t>(
            1, static_cast<uint64_t>(store->config.mount_renew_period.count()) / 2);
        const uint64_t margin_ms = poll_interval_ms;
        const auto sleep_ms = [](uint64_t ms)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };
        /// Operator-visible log the moment startup decides to wait out a stale self-mount (the disk-open
        /// path blocks up to ~ttl here, so a silent block would be confusing).
        const auto on_wait_start = [&srid](const MountLease & held, uint64_t wait_deadline_ms)
        {
            LOG_INFO(getLogger("CasStore"),
                "CAS mount '{}': a stale mount lease is held by uuid={} epoch={} pid={} hostname={} "
                "(expires_at_ms={}); waiting for it to lapse, then reclaiming. If a second server is "
                "genuinely live, startup will abort once the wait bound (wait_deadline_ms={}) elapses.",
                srid, u128ToHex(held.server_uuid), held.writer_epoch, held.pid, held.hostname,
                held.expires_at_ms, wait_deadline_ms);
        };

        /// S13 crash-recovery: a hard-killed prior incarnation leaves a stale, unreleased mount lease.
        /// Rather than aborting, wait (bounded by ttl + margin) for that lease to lapse and reclaim it;
        /// a genuinely live second server keeps renewing and is reported as LiveDoubleStart. The reclaim
        /// is token-guarded (see claimMountAwaitingExpiry), so a live twin is never stolen from.
        const MountClaimResult claim = claimMountAwaitingExpiry(
            *store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
            [&now_ms]() { return now_ms(); }, ttl_ms, poll_interval_ms, margin_ms, sleep_ms, on_wait_start);
        if (claim.kind != MountClaimResult::Claimed)
        {
            /// LiveDoubleStart (waited out the bound → a live twin) or ForeignOwner → fail closed with
            /// the actionable, multi-line startup error (spec §mount-safety).
            throw Exception(ErrorCodes::ABORTED, "{}", mountDoubleStartMessage(srid, claim.body));
        }

        /// The mount object now holds OUR live (uuid, epoch) body. Construct + start the keeper, which
        /// ADOPTS that very (uuid, epoch) slot (Task 5) rather than self-tripping the double-start guard.
        Store * raw = store.get();
        /// Merged heartbeat (ack-floor redesign): the mount keeper now ALSO carries the per-server
        /// build-watermark floor (`minActive`) and the acked GC round. `minActive` is read off the
        /// keeper's state lock (it reaches into the Store's builds_mutex). The observed-round callback
        /// is a stub {0} until Task 5 wires the real view-load ack — a fresh mount has loaded nothing.
        store->mount_keeper = std::make_unique<MountLeaseKeeper>(
            store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
            store->config.mount_lease_ttl_ms, now_ms,
            [raw] { return raw->minActive(); }, [] { return uint64_t{0}; });
        /// Keeper ↔ fence coupling (spec §write-fence): on each successful background renew refresh the
        /// monotonic deadline; on a superseded/foreign renew failure latch the fence to lost. Set BEFORE
        /// startBackground so no renewal can fire before the callbacks are in place.
        store->mount_keeper->setFenceCallbacks(
            [raw, ttl_ms] { raw->setMountDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms)); },
            [raw] { raw->tripMountLost(); });
        store->mount_keeper->start();

        /// Arm the local write fence: cache (uuid, epoch) and set the monotonic deadline now + ttl. From
        /// here ordinary mutations (mutateShard) are fence-gated via mayMutate().
        store->armMountFence(our_uuid, writer_epoch,
            std::chrono::steady_clock::now() + store->config.mount_lease_ttl_ms);
        /// Gate the background renewer with `background_watermark`: it runs only in production
        /// (`background_watermark` = context != nullptr && !read_only), never in unit tests — which
        /// drive renewOnce explicitly and rely on the armed sub-TTL deadline, never on the loop. The
        /// keeper itself is still started above (it must claim/adopt the mount + arm the fence on every
        /// writable open); only the renewal thread is conditional. The merged heartbeat renews at
        /// `mount_renew_period` — the standalone watermark object (and its separate renew period) is
        /// gone; one beat now renews the lease, the floor and the acked round together.
        if (store->config.background_watermark)
            store->mount_keeper->startBackground(store->config.mount_renew_period);
    }

    return store;
}

Store::~Store()
{
    /// Retire the merged heartbeat on a clean Store teardown: stop() runs the keeper's terminal op,
    /// which stamps the lease already-expired (expires_at_ms = now) AND folds in the watermark
    /// farewell (min_active = UINT64_MAX). Stamping it expired lets a SAME-server reopen reclaim
    /// immediately (the durable epoch + owner stay sticky). A throw here (e.g. a foreign incarnation
    /// touched the slot) must not escape the dtor — log and continue tearing down.
    if (mount_keeper)
    {
        try
        {
            mount_keeper->stop();
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CasStore"), "CAS mount-lease: release during Store teardown failed");
        }
    }
}

uint64_t Store::shardOf(const String & ref_name) const
{
    /// root_shards >= 1 is a PoolMeta invariant, so the modulus is always well-defined.
    return CityHash_v1_0_2::CityHash64(ref_name.data(), ref_name.size()) % meta.root_shards;
}

void Store::casPutObject(const String & full_key, const String & bytes)
{
    /// head + putIfAbsent/putOverwrite loop. Single-owner keys make a contention storm impossible;
    /// the bound is a runaway brake.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        HeadResult head = pool_backend->head(full_key);
        if (!head.exists)
        {
            if (pool_backend->putIfAbsent(full_key, bytes).outcome == PutOutcome::Done)
                return;
        }
        else
        {
            if (pool_backend->putOverwrite(full_key, bytes, head.token).outcome == PutOutcome::Done)
                return;
        }
        /// PreconditionFailed ⇒ the observed state changed under us; re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}'", full_key);
}

std::optional<String> Store::casGetObject(const String & full_key)
{
    std::optional<GetResult> result = pool_backend->get(full_key);
    if (!result)
        return std::nullopt;
    return result->bytes;
}

void Store::casRemoveObject(const String & full_key)
{
    /// head + deleteExact loop; no-op when absent. Single-owner keys; the bound is a runaway brake.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        const HeadResult head = pool_backend->head(full_key);
        if (!head.exists)
            return;
        const DeleteOutcome outcome = pool_backend->deleteExact(full_key, head.token);
        if (outcome.kind == DeleteOutcome::Kind::Deleted || outcome.kind == DeleteOutcome::Kind::NotFound)
            return;
        /// TokenMismatch: a concurrent rewrite — re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}' (runaway live-lock brake)", full_key);
}

void Store::putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes)
{
    casPutObject(pool_layout.namespaceFileKey(ns, name), bytes);
}

std::optional<String> Store::getNamespaceFile(const RootNamespace & ns, const String & name)
{
    return casGetObject(pool_layout.namespaceFileKey(ns, name));
}

std::vector<String> Store::listNamespaceFiles(const RootNamespace & ns)
{
    const String prefix = pool_layout.namespaceFilesPrefix(ns);
    std::vector<String> names;
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            /// Strip the prefix to yield the bare flat file name.
            if (listed.key.size() >= prefix.size() && listed.key.compare(0, prefix.size(), prefix) == 0)
                names.push_back(listed.key.substr(prefix.size()));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    /// The InMemoryBackend lists sorted, but sort explicitly to stay backend-agnostic.
    std::sort(names.begin(), names.end());
    return names;
}

uint64_t Store::minActive()
{
    std::lock_guard lk(builds_mutex);
    return active_build_seqs.empty() ? next_build_seq : *active_build_seqs.begin();
}

uint64_t Store::peekNextBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    return next_build_seq;
}

void Store::renewWatermarkOnce()
{
    /// After the ack-floor merge the build-watermark floor rides the merged mount-keeper beat (there
    /// is no standalone watermark object anymore), so one renewOnce re-reads `minActive` and stamps
    /// it into the mount body. A read-only open never anchored the keeper; there is nothing to renew
    /// (fail closed rather than fabricate one).
    if (!mount_keeper)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renewWatermarkOnce on a read-only Store");
    mount_keeper->renewOnce();
}

uint64_t Store::allocateBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    const uint64_t s = next_build_seq++;
    active_build_seqs.insert(s);
    return s;
}

void Store::retireBuildSeq(uint64_t seq)
{
    std::lock_guard lk(builds_mutex);
    active_build_seqs.erase(seq);
}

BuildPtr Store::startBuild(BuildInfo info)
{
    /// Mint a globally-unique build id from two thread_local_rng draws (random u128).
    const UInt64 hi = thread_local_rng();
    const UInt64 lo = thread_local_rng();
    const UInt128 build_id = (static_cast<UInt128>(hi) << 64) | lo;

    /// Strictly-increasing per-process build_seq carried by the Build (spec 2026-06-16). The Build is
    /// added to the active set here and retired on publish/abandon/dtor, so minActive — the GC floor
    /// the Store-owned watermark renews — tracks in-flight builds.
    const uint64_t seq = allocateBuildSeq();

    return std::make_shared<Build>(shared_from_this(), build_id, seq, process_epoch, std::move(info));
}

std::shared_ptr<const RootShard> Store::readShardDecoded(const RootNamespace & ns, uint64_t shard, bool allow_stale)
{
    const String key = pool_layout.rootShardKey(ns, shard);

    /// Pillar B TTL fast-path: a staleness-tolerant caller may reuse a recently-validated decode
    /// WITHOUT a HEAD. Only for PRESENT entries (absence is never TTL-cached — a just-created ref
    /// must be observable by force-fresh callers; staleness-tolerant callers re-validate on miss).
    if (allow_stale && config.shard_decode_cache_ttl_ms.count() > 0)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end())
        {
            const auto age = std::chrono::steady_clock::now() - it->second.validated_at;
            if (age < config.shard_decode_cache_ttl_ms)
                return it->second.shard;
        }
    }

    return coalescedReadShardDecoded(key);
}

std::shared_ptr<const RootShard> Store::coalescedReadShardDecoded(const String & key)
{
    std::shared_ptr<std::promise<std::shared_ptr<const RootShard>>> promise;
    std::shared_future<std::shared_ptr<const RootShard>> future;
    {
        std::lock_guard lock(shard_inflight_mutex);
        auto it = shard_inflight.find(key);
        if (it != shard_inflight.end())
        {
            future = it->second;   /// follower: wait on the in-flight leader
        }
        else
        {
            promise = std::make_shared<std::promise<std::shared_ptr<const RootShard>>>();
            future = promise->get_future().share();
            shard_inflight.emplace(key, future);
        }
    }

    if (!promise)
        return future.get();   /// follower: returns the leader's result (or rethrows the leader's exception)

    /// Leader: do the real work, publish to followers whether it succeeds or throws.
    try
    {
        auto result = loadShardDecoded(key);
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_value(result);
        return result;
    }
    catch (...)
    {
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_exception(std::current_exception());
        throw;
    }
}

std::shared_ptr<const RootShard> Store::loadShardDecoded(const String & key)
{
    /// Empty-manifest sentinel for the absent case — shared so callers can treat absent and present
    /// uniformly (no refs). Never mutated.
    static const std::shared_ptr<const RootShard> empty_shard = std::make_shared<const RootShard>();

    /// A `head` gets the current token without transferring/decoding the manifest body.
    const HeadResult h = pool_backend->head(key);
    if (!h.exists)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }

    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end() && it->second.token == h.token)
        {
            /// Token match: the cached decode is still valid. Stamp validated_at so TTL-tolerant
            /// callers can skip the next HEAD within the configured window.
            it->second.validated_at = std::chrono::steady_clock::now();
            return it->second.shard;
        }
    }

    /// Miss (or the shard was written since we last decoded it): fetch + decode once, then cache by
    /// the token the bytes actually came from (NOT the head token — a write could have landed in
    /// between; we cache what we decoded, which is self-consistent).
    ///
    /// Capture the per-key write counter BEFORE the get(): a committed write that bumps it while our
    /// get() is in flight means the bytes we are about to fetch may predate that write — and the
    /// write's invalidation erase has already run — so caching the decode now would poison the TTL
    /// fast-path with a stale entry the erase can no longer remove (read-your-writes coherence, B157).
    uint64_t seq_before_get;
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        seq_before_get = shard_write_seq[key];
    }

    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
    {
        /// Raced a deletion between head and get — treat as absent.
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }
    auto decoded = std::make_shared<const RootShard>(decodeRootShard(object->bytes));
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        /// Skip caching if a write committed during our get() (B157): the decode may be superseded
        /// and its invalidation erase has already run, so populating now would resurrect a stale
        /// entry. We still RETURN the decode to our own caller — a point-in-time view, acceptable
        /// for allow_stale; a force-fresh caller's own writes happen-before its head() so this only
        /// ever under-caches, never serves a caller its own stale write.
        if (shard_write_seq[key] == seq_before_get)
        {
            /// Bound memory on a long-lived server: a wholesale clear is fine — entries re-populate
            /// on demand (a stale entry would be re-validated by the head token check anyway).
            if (shard_decode_cache.size() >= SHARD_DECODE_CACHE_MAX_ENTRIES)
                shard_decode_cache.clear();
            shard_decode_cache[key] = ShardDecodeCacheEntry{
                .token = object->token,
                .shard = decoded,
                .validated_at = std::chrono::steady_clock::now()};
        }
    }
    return decoded;
}

std::optional<Resolved> Store::resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale)
{
    /// Read side (spec §6): no GC awareness, no tokens. The ref is pure manifest state — resolving it
    /// only reads the owning shard manifest; whether the named tree object is still present is checked
    /// later by readManifest (INV-NO-DANGLE surfaces there).
    const auto root = readShardDecoded(ns, shardOf(ref_name), allow_stale);
    auto it = root->refs.find(ref_name);
    if (it == root->refs.end())
        return std::nullopt;

    const RootRef & payload = it->second;
    /// B170: a ref resolved to its manifest (the read-path entry point). object_hash is the manifest
    /// instance id the ref names; pairs with a later readManifest ReadMissing if that body is gone.
    if (hasEventSink())
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::RefResolve;
        _ev0.namespace_ = ns.string();
        _ev0.ref_name = ref_name;
        _ev0.object_kind = CasEventObjectKind::Tree;
        _ev0.object_hash = manifestRefDebugString(payload.manifest_ref);
        _ev0.outcome = "resolved";
        _ev0.reason = "read-side resolve of a ref to its part manifest";
        emitEvent(_ev0);
    }
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = payload.manifest_ref},
        .manifest_size = 0,
        .mutable_files = payload.mutable_files,
        .published_at_ms = payload.published_at_ms,
    };
}

size_t Store::ManifestCacheKeyHash::operator()(const ManifestCacheKey & k) const
{
    /// Combine the manifest-id hash with the token's bytes + type. The token is part of the key so a
    /// re-incarnation under the same id misses (the immutable bytes changed identity).
    const size_t h1 = std::hash<ManifestId>{}(k.manifest_id);
    const size_t h2 = std::hash<String>{}(k.token.value);
    const size_t h3 = std::hash<uint8_t>{}(static_cast<uint8_t>(k.token.type));
    size_t h = h1;
    h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

PartManifest Store::readManifest(const ManifestId & id)
{
    /// A live ref naming a missing manifest body is INV-NO-DANGLE (spec §Read Path Scope: "fail-closed
    /// behavior when a committed ref names a missing manifest"). Never substitute an empty manifest.
    const String key = pool_layout.manifestKey(id);

    /// HEAD first for the current token: on a (id, token) cache hit, the immutable decode is reused
    /// with no get and no re-decode. A missing object surfaces INV-NO-DANGLE.
    const HeadResult head = pool_backend->head(key);
    if (!head.exists)
    {
        if (hasEventSink())
        {
            CasEvent _ev1;
            _ev1.type = CasEventType::ReadMissing;
            _ev1.object_kind = CasEventObjectKind::Tree;
            _ev1.object_hash = manifestRefDebugString(id.ref);
            _ev1.outcome = "missing";
            _ev1.reason = "live ref names manifest but its object is missing (INV-NO-DANGLE)";
            _ev1.detail = {{"code", "FILE_DOESNT_EXIST"}, {"site", "readManifest"}};
            emitEvent(_ev1);
        }
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "live ref names manifest at {} but its object is missing — INV-NO-DANGLE", key);
    }

    {
        std::lock_guard lock(manifest_cache_mutex);
        auto it = manifest_cache.find(ManifestCacheKey{.manifest_id = id, .token = head.token});
        if (it != manifest_cache.end())
            return *it->second;
    }

    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "manifest at {} vanished between head and get — INV-NO-DANGLE", key);

    PartManifest body = decodePartManifest(object->bytes);

    /// refMatchesBody: the journal ManifestRef must equal the body's self-described `ref`. A mismatch
    /// means the ref addresses the WRONG object (spec §Object Identity And Ownership).
    if (!refMatchesBody(id.ref, body))
    {
        if (hasEventSink())
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::CorruptDecode;
            _ev2.object_kind = CasEventObjectKind::Tree;
            _ev2.object_hash = manifestRefDebugString(id.ref);
            _ev2.outcome = "corrupt";
            _ev2.reason = "manifest body `ref` does not match the journal ManifestRef (refMatchesBody)";
            _ev2.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readManifest"}};
            emitEvent(_ev2);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS manifest at {} body ref does not match the journal ManifestRef — refMatchesBody", key);
    }

    /// manifestNamespaceMatches: the body's root_namespace_id must equal the owning root namespace. A
    /// mismatch is a cross-namespace dangle and would hand the debris sweep the wrong authority.
    if (!manifestNamespaceMatches(id.root_namespace, body))
    {
        if (hasEventSink())
        {
            CasEvent _ev3;
            _ev3.type = CasEventType::CorruptDecode;
            _ev3.object_kind = CasEventObjectKind::Tree;
            _ev3.object_hash = manifestRefDebugString(id.ref);
            _ev3.outcome = "corrupt";
            _ev3.reason = "manifest body root_namespace_id does not match the owning namespace (manifestNamespaceMatches)";
            _ev3.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readManifest"}};
            emitEvent(_ev3);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS manifest at {} body root_namespace_id does not match the owning namespace — manifestNamespaceMatches", key);
    }

    auto decoded = std::make_shared<const PartManifest>(std::move(body));
    {
        std::lock_guard lock(manifest_cache_mutex);
        /// Bound memory: a wholesale clear on overflow is fine — entries are pure immutable decode
        /// caches that re-populate on demand.
        if (manifest_cache.size() >= MANIFEST_CACHE_MAX_ENTRIES)
            manifest_cache.clear();
        manifest_cache[ManifestCacheKey{.manifest_id = id, .token = head.token}] = decoded;
    }
    return *decoded;
}

std::optional<ManifestEntry> Store::lookupPath(const PartManifest & manifest, const String & path) const
{
    for (const auto & entry : manifest.entries)
    {
        if (entry.path == path)
            return entry;
    }
    return std::nullopt;
}

std::vector<ManifestEntry> Store::listDirectory(const PartManifest & manifest, const String & dir_prefix) const
{
    std::vector<ManifestEntry> result;
    for (const auto & entry : manifest.entries)
    {
        if (dir_prefix.empty() || entry.path.starts_with(dir_prefix))
            result.push_back(entry);
    }
    return result;
}

BlobLocation Store::locate(const ManifestEntry & entry) const
{
    /// A ranged read into the content object: the payload starts at a constant offset for blobs
    /// (the pool's fixed blob_header_len — no per-object header read). Inline carries no standalone
    /// object location (there is no Subtree placement on a part manifest).
    switch (entry.placement)
    {
        case EntryPlacement::Blob:
            return BlobLocation{
                .key = pool_layout.blobKey(BlobId(u128ToHex(entry.blob_hash))),
                .offset = meta.blob_header_len,
                .length = entry.blob_size,
            };
        case EntryPlacement::Inline:
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "entry placement {} has no blob location", static_cast<int>(entry.placement));
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "entry placement {} has no blob location", static_cast<int>(entry.placement));
}

std::map<String, Resolved> Store::listRefs(const RootNamespace & ns)
{
    /// Refs are sharded by name across all root shards; the full ref set is the union over every shard.
    /// Listing tolerates a point-in-time snapshot: pass allow_stale=true to benefit from the TTL
    /// fast-path and skip a HEAD per shard when a recent decode is already cached.
    std::map<String, Resolved> result;
    for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
    {
        const auto root = readShardDecoded(ns, shard, /*allow_stale=*/true);
        for (const auto & [ref_name, payload] : root->refs)
        {
            result.emplace(ref_name, Resolved{
                .manifest_id = ManifestId{.root_namespace = ns, .ref = payload.manifest_ref},
                .manifest_size = 0,
                .mutable_files = payload.mutable_files,
                .published_at_ms = payload.published_at_ms,
            });
        }
    }
    return result;
}

void Store::mutateShard(const RootNamespace & ns, uint64_t shard, std::function<void(RootShard &)> mutate,
                        uint64_t * out_committed_version, RootMutationOrigin origin, RootMutationKind kind,
                        ShardIncarnation birth_incarnation, std::function<uint64_t()> birth_floor_provider)
{
    /// Local write fence (spec §write-fence, Phase 0 Task 6): the shared mutable ref shards are the
    /// state the fence most protects. A superseded/paused writer (lease lost or local deadline passed)
    /// must not race the live one. Purely local — no S3 read. Permissive until Task 7 arms it, so
    /// existing flows (GC/build/bootstrap writes) are unaffected.
    if (!mayMutate())
        throw Exception(ErrorCodes::ABORTED,
            "CAS mount lost / lease expired — refusing to mutate ref shard for server_root '{}'",
            config.server_root_id);

    const String key = pool_layout.rootShardKey(ns, shard);

    /// B164b: at most one backpressure delay per mutation call. After the first delay + retry, the
    /// subsequent iteration's fresh read may still be above the soft limit but we commit anyway
    /// (admission is paced, never permanently blocked).
    bool delayed_once = false;

    const uint64_t soft_limit = config.manifest_soft_limit;
    const uint64_t hard_limit = config.manifest_hard_limit;
    const uint64_t max_delay_ms = config.manifest_max_delay_ms;
    const bool backpressure_active = (hard_limit > soft_limit) && (max_delay_ms > 0);

    /// Rate-limiter for soft-limit warnings — at most one per 30s to avoid log spam under pressure.
    static std::atomic<std::chrono::steady_clock::time_point> last_soft_warn{
        std::chrono::steady_clock::time_point::min()};

    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        /// Re-read inside the loop so `mutate` always edits the FRESH manifest: on a Conflict retry the
        /// previous attempt's edits are discarded and re-applied to the winner's state, so a journal
        /// append is never double-appended.
        auto [root, token] = readShard(ns, shard);
        /// Task 2: stamp the birth incarnation on the create-if-absent path (token == nullopt means
        /// the shard object did not exist before this call). Once the shard exists the incarnation is
        /// immutable — subsequent mutations leave it untouched. Callers that never create a first
        /// object (drop, fence, updateRefPayload, dropNamespace) pass the default `{}`, which is the
        /// unstamped sentinel (a no-op when the shard already carries a real incarnation). A CAS
        /// Conflict retry re-reads a now-present shard, so the stamp is skipped on retries correctly.
        /// Task 5: stamp the birth floor (fence_round) on the same create-if-absent path. A NEWBORN
        /// shard born during GC round R is fenced to R so the `promote` gate forces a retire-view
        /// refresh before committing any blob belonging to the new shard — the registry-free THM-NO-RETURN
        /// create-ordering guarantee. `birth_floor_provider` is LAZY: it is only invoked here (newborn
        /// path) so the common existing-shard path incurs ZERO extra S3 round-trips. The provider is
        /// called at most ONCE per `mutateShard` call (cached in `floor` below); a CAS-retry that
        /// re-reads a now-present shard takes the `token` branch and skips this block entirely.
        /// Callers that never create a first object (GC fence, drop, dropNamespace) pass nullptr —
        /// `fence_round` stays at the default 0 (no-op, shard already exists anyway).
        /// `fence_round = 0` (fresh pool, no GC) is a valid value; assigned unconditionally.
        if (!token)
        {
            root.incarnation = birth_incarnation;
            if (birth_floor_provider)
            {
                const uint64_t floor = birth_floor_provider();
                root.fence_round = floor;
            }
        }
        mutate(root);
        ++root.shard_version;
        String body = encodeRootShard(root);

        /// Hard limit — fail-closed before any consequential write, regardless of origin.
        if (body.size() >= hard_limit)
        {
            ProfileEvents::increment(ProfileEvents::CasManifestHardLimitExceeded);
            throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                "manifest {} size {} reached hard limit {} (kind={})",
                key, body.size(), hard_limit, toString(kind));
        }

        /// Soft limit — warning log + optional backpressure delay for Writer.
        if (body.size() >= soft_limit)
        {
            /// Rate-limited soft-limit warning (at most one per 30s).
            const auto now = std::chrono::steady_clock::now();
            auto last = last_soft_warn.load(std::memory_order_relaxed);
            if (now - last > std::chrono::seconds(30))
            {
                if (last_soft_warn.compare_exchange_strong(last, now))
                {
                    LOG_WARNING(getLogger("CasStore"),
                        "manifest {} size {} crossed soft limit {} (hard={}, kind={}, origin={})",
                        key, body.size(), soft_limit, hard_limit,
                        toString(kind), toString(origin));
                }
            }

            /// B164b: delay only Writer mutations, only when backpressure is configured, and at most
            /// once per mutation call.
            if (origin == RootMutationOrigin::Writer && backpressure_active && !delayed_once)
            {
                /// Linear delay: 0 at soft_limit → max_delay_ms near the hard limit.
                const double fraction = static_cast<double>(body.size() - soft_limit)
                                      / static_cast<double>(hard_limit - soft_limit);
                uint64_t delay_ms = static_cast<uint64_t>(fraction * static_cast<double>(max_delay_ms));

                if (delay_ms > 0)
                {
                    delayed_once = true;
                    const auto delay = std::chrono::milliseconds(delay_ms);

                    ProfileEvents::increment(ProfileEvents::CasManifestBackpressureCount);
                    ProfileEvents::increment(
                        ProfileEvents::CasManifestBackpressureMicroseconds,
                        std::chrono::duration_cast<std::chrono::microseconds>(delay).count());

                    LOG_DEBUG(getLogger("CasStore"),
                        "manifest backpressure: ns/shard={}/{} size={} soft={} hard={} delay={}ms kind={}",
                        ns.string(), shard, body.size(), soft_limit, hard_limit,
                        delay_ms, toString(kind));

                    if (backpressure_delay_hook)
                        backpressure_delay_hook(delay);
                    else
                        std::this_thread::sleep_for(delay);

                    /// Retry with a fresh read — avoids CAS with a stale token and re-applies the
                    /// mutation to the latest shard state.
                    continue;
                }
            }
        }

        if (pool_backend->casPut(key, body, token).outcome == CasOutcome::Committed)
        {
            /// Read-your-writes (Pillar B): invalidate this shard's decode cache so a same-Store
            /// allow_stale read (bounded-TTL fast-path) cannot serve the pre-write decode. A LOCAL
            /// write is reflected immediately; cross-node staleness stays bounded by the TTL.
            /// Bumping shard_write_seq under the SAME lock fences any in-flight reader (B157): a
            /// reader whose get() straddled this casPut sees the seq change before it populates and
            /// declines to cache its now-superseded decode (which the erase below could not remove).
            {
                std::lock_guard cache_lock(shard_decode_cache_mutex);
                ++shard_write_seq[key];
                shard_decode_cache.erase(key);
            }
            if (out_committed_version)
                *out_committed_version = root.shard_version;
            return;
        }
        /// Conflict ⇒ someone committed under us; re-read and re-apply the whole mutate.
    }
    throw Exception(ErrorCodes::ABORTED, "manifest CAS contention on {}", key);
}

uint64_t Store::currentGcRound() const
{
    /// Task 5: read `gc/state` once (no CAS loop — a point-in-time read is sufficient; a concurrent
    /// GC advance only makes the returned round larger, which is strictly more conservative for the
    /// `precommitAdd` self-floor). Returns 0 when absent (pool never GC'd — no round to floor to).
    const auto state_bytes = pool_backend->get(pool_layout.gcStateKey());
    if (!state_bytes)
        return 0;
    return decodeGcState(state_bytes->bytes).round;
}

void Store::dropRef(const RootNamespace & ns, const String & ref_name)
{
    /// Drop = the same CAS shape as publish (spec rev. 15 §Root Journal Format): remove refs[name],
    /// append a RootOwnerEvent whose old_binding is the committed binding being removed and whose
    /// new_binding is none (true removal ⇒ GC folds -1 + cleanup of the named manifest).
    ManifestRef dropped_ref;
    uint64_t at_version = 0;
    mutateShard(ns, shardOf(ref_name), [&](RootShard & root)
    {
        auto it = root.refs.find(ref_name);
        if (it == root.refs.end())
            /// Fail-closed (no silent no-op): the throw propagates out of mutateShard and aborts the drop.
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "dropRef: no such ref {} in namespace {}", ref_name, ns.string());

        dropped_ref = it->second.manifest_ref;
        at_version = root.shard_version + 1;
        root.refs.erase(it);
        /// transition_version is the NEW shard_version this attempt commits — the helper bumps AFTER
        /// mutate, so here the post-commit version is root.shard_version + 1.
        root.journal.push_back(RootOwnerEvent{
            .transition_version = root.shard_version + 1,
            .old_binding = OwnerBinding{
                .owner_kind = OwnerKind::Committed, .ref_name = ref_name,
                .build_id = UInt128(0), .manifest_ref = dropped_ref},
            .new_binding = std::nullopt});
    }, nullptr, RootMutationOrigin::Writer, RootMutationKind::Drop);
    /// B170: the ref was dropped (a removal RootOwnerEvent GC folds as a true removal). object_hash is
    /// the manifest the ref named, so a part's "publish -> drop" life is reconstructable from the rows.
    if (hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::RefDrop;
        _ev3.namespace_ = ns.string();
        _ev3.ref_name = ref_name;
        _ev3.object_kind = CasEventObjectKind::Tree;
        _ev3.object_hash = manifestRefDebugString(dropped_ref);
        _ev3.at_version = at_version;
        _ev3.outcome = "ok";
        _ev3.reason = "dropRef: removed the ref and appended a removal RootOwnerEvent";
        emitEvent(_ev3);
    }
}

void Store::updateRefPayload(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RootRef &)> mutator)
{
    /// Mutable-fields-only update (design §3): no reachability change ⇒ NO journal event.
    mutateShard(ns, shardOf(ref_name), [&](RootShard & root)
    {
        auto it = root.refs.find(ref_name);
        if (it == root.refs.end())
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "updateRefPayload: no such ref {} in namespace {}", ref_name, ns.string());

        const ManifestRef old_manifest_ref = it->second.manifest_ref;

        RootRef payload = it->second;
        mutator(payload);

        /// A reachability change is not allowed on this path: it would need a journal event (use
        /// publish/drop instead). Throwing here aborts before casPut — the manifest stays untouched.
        if (!(payload.manifest_ref == old_manifest_ref))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "updateRefPayload must not change manifest_ref; use publish/drop");

        it->second = std::move(payload);
    }, nullptr, RootMutationOrigin::Writer, RootMutationKind::UpdateRefPayload);
}

void Store::removeNamespaceFile(const RootNamespace & ns, const String & name)
{
    casRemoveObject(pool_layout.namespaceFileKey(ns, name));
}

void Store::putMountpointObject(const String & key, const String & bytes)
{
    casPutObject(pool_layout.mountpointObjectKey(key), bytes);
}

std::optional<String> Store::getMountpointObject(const String & key)
{
    return casGetObject(pool_layout.mountpointObjectKey(key));
}

void Store::removeMountpointObject(const String & key)
{
    casRemoveObject(pool_layout.mountpointObjectKey(key));
}

void Store::dropNamespace(const RootNamespace & ns)
{
    /// Tombstone every PRESENT shard, then delete the verbatim files. GC removes the manifest OBJECTS
    /// themselves later once the shard is empty + tombstoned + fully folded (Task 6). Spec §4: untouched
    /// (absent) shards stay absent — we never mint a manifest just to hold a tombstone.
    ///
    /// Task 6: for every present shard (with or without refs) we append ref-removal events (one per
    /// former ref) FOLLOWED by the drop-namespace tombstone as the LAST journal event. The tombstone
    /// event (`is_tombstone = true`, both bindings absent) is the in-band signal GC reads to decide
    /// whether the shard object is eligible for reclaim: empty + tombstone + fully folded ⇒ deleteExact.
    for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
    {
        const auto [root, token] = readShard(ns, shard);
        if (!token)
            continue;   /// absent shard: stays absent, no manifest minted

        mutateShard(ns, shard, [](RootShard & shard_root)
        {
            /// Append one removal RootOwnerEvent per former ref (iterate before clearing), then clear
            /// all refs. Each event: old_binding = the committed binding being removed / new_binding none.
            for (const auto & [ref_name, payload] : shard_root.refs)
                shard_root.journal.push_back(RootOwnerEvent{
                    .transition_version = shard_root.shard_version + 1,
                    .old_binding = OwnerBinding{
                        .owner_kind = OwnerKind::Committed, .ref_name = ref_name,
                        .build_id = UInt128(0), .manifest_ref = payload.manifest_ref},
                    .new_binding = std::nullopt});
            shard_root.refs.clear();
            /// Task 6: append the tombstone as the LAST journal event (after all removal events so the
            /// fold must cover every -1 removal before reaching the tombstone, preventing phantom in-degree).
            shard_root.journal.push_back(RootOwnerEvent{
                .transition_version = shard_root.shard_version + 1,
                .old_binding = std::nullopt,
                .new_binding = std::nullopt,
                .is_tombstone = true});
        }, nullptr, RootMutationOrigin::Writer, RootMutationKind::DropNamespace);
    }

    /// Delete the verbatim files: list → head for the token → deleteExact (single-owner, bounded retry).
    const String prefix = pool_layout.namespaceFilesPrefix(ns);
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
            /// Single-owner head → deleteExact with the bounded TokenMismatch-retry: exactly the
            /// casRemoveObject contract (no-op on absent, idempotent on NotFound, ABORTED on live-lock).
            casRemoveObject(listed.key);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }

    /// Evict this namespace's shard entries from the read-path decode cache: the namespace is gone,
    /// so they would never be re-read (and thus never revalidated/replaced) — a slow leak otherwise.
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
            shard_decode_cache.erase(pool_layout.rootShardKey(ns, shard));
    }
}

std::pair<RootShard, std::optional<Token>> Store::readShard(const RootNamespace & ns, uint64_t shard)
{
    /// An absent shard manifest means the shard holds no refs yet — a fresh, empty manifest with no
    /// token (nothing to CAS against). This is normal state, NOT a fallback masking an error.
    std::optional<GetResult> object = pool_backend->get(pool_layout.rootShardKey(ns, shard));
    if (!object)
        return {RootShard{}, std::nullopt};
    return {decodeRootShard(object->bytes), object->token};
}

std::vector<String> Store::listNamespaces(const String & prefix)
{
    /// LIST-based discovery authority (Task 4): enumerate distinct full namespace strings
    /// from ref shards under `cas/refs/` UNION verbatim-file namespaces under `roots/`.
    ///
    /// Consistency requirement: the backend must give read-your-writes LIST enumeration.
    /// InMemoryBackend: guaranteed (in-memory map). S3: strongly consistent since 2021.
    /// RustFS: to confirm in soak.
    std::unordered_set<String> found;

    /// `cas/refs/`: keys are `<pool_prefix>/cas/refs/<ns>/<shard>` (shard is a decimal integer).
    /// Strip the pool prefix, then extract ns = everything before the last '/'.
    {
        const String base = pool_layout.casRefsPrefix() + prefix;
        String cursor;
        for (;;)
        {
            ListPage page = pool_backend->list(base, cursor, /*limit*/ 1000);
            for (const ListedKey & listed : page.keys)
            {
                const String & key = listed.key;
                if (!key.starts_with(base))
                    continue;
                const std::string_view rest(key.data() + pool_layout.casRefsPrefix().size(),
                    key.size() - pool_layout.casRefsPrefix().size());
                const size_t last_slash = rest.rfind('/');
                if (last_slash == std::string_view::npos)
                    continue;
                const String ns_str(rest.substr(0, last_slash));
                if (!ns_str.empty() && ns_str.starts_with(prefix))
                    found.insert(ns_str);
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }

    /// `roots/`: verbatim-file keys are `<pool_prefix>/roots/<ns>/_files/<name>`.
    /// Extract ns = everything before `/_files/`.
    {
        const String base = pool_layout.rootsPrefix() + prefix;
        String cursor;
        for (;;)
        {
            ListPage page = pool_backend->list(base, cursor, /*limit*/ 1000);
            for (const ListedKey & listed : page.keys)
            {
                const String & key = listed.key;
                if (!key.starts_with(pool_layout.rootsPrefix()))
                    continue;
                const std::string_view rest(key.data() + pool_layout.rootsPrefix().size(),
                    key.size() - pool_layout.rootsPrefix().size());
                const size_t files_pos = rest.find("/_files/");
                if (files_pos == std::string_view::npos)
                    continue;   /// plain mountpoint object (not a namespaced verbatim file); skip
                const String ns_str(rest.substr(0, files_pos));
                if (!ns_str.empty() && ns_str.starts_with(prefix))
                    found.insert(ns_str);
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }

    return {found.begin(), found.end()};
}

std::vector<String> Store::listMirroredChildren(const String & prefix)
{
    /// Loose LIST of the mirrored subtree (design §5.3). Returns the distinct next-path-segment
    /// names. NOT authoritative — callers must re-check `listRefs` per candidate before surfacing
    /// it. GC uses LIST-based discovery (`cas/refs/` prefix) rather than a registry.
    ///
    /// Phase 1: a namespace's presence is split across two physical subtrees — its ref shards live
    /// under `cas/refs/<ns>/<shard>` (the relocation target) while its verbatim files and PLAIN
    /// mountpoint objects stay under `roots/<ns>/_files/…` / `roots/<key>`. The browse therefore
    /// UNIONs the next-segment names from BOTH subtrees so a namespace discoverable only by its ref
    /// shards (the common case — mutable per-part files ride inside the RootRef payload, not as
    /// verbatim `_files`) is still surfaced.
    std::unordered_set<String> children;
    const String roots_full = pool_layout.rootsPrefix() + prefix;       /// e.g. <pool>/roots/shadow/
    const String refs_full = pool_layout.casRefsPrefix() + prefix;      /// e.g. <pool>/cas/refs/shadow/
    for (const String & full : {roots_full, refs_full})
    {
        String cursor;
        while (true)
        {
            ListPage page = pool_backend->list(full, cursor, /*limit*/ 1000);
            for (const ListedKey & listed : page.keys)
            {
                const String & key = listed.key;
                if (!key.starts_with(full))
                    continue;
                const std::string_view rest(key.data() + full.size(), key.size() - full.size());
                const size_t slash = rest.find('/');
                const std::string_view seg = slash == std::string_view::npos ? rest : rest.substr(0, slash);
                if (!seg.empty())
                    children.emplace(seg);
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }
    return {children.begin(), children.end()};
}

}
