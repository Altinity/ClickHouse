#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <ctime>
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
    extern const Event CasPartFolderManifestGets;
    extern const Event CasRefBatchFlushes;
    extern const Event CasRefBatchedMutations;
    extern const Event CasRefBatchScopeCuts;
    extern const Event CasRefQueueWaitMicroseconds;
    extern const Event CasRefRecoveryRestarts;
    extern const Event CasRefAppendWedged;
    extern const Event CasRefAppendUnwedged;
    extern const Event CasRefAppendDefiniteFailure;
    extern const Event CasRefSweepDeferred;
    extern const Event CasRefSweepRearmed;
    extern const Event CasRefStalePrecommitsReclaimed;
    extern const Event CasRefTableEvictions;
    extern const Event CasRefSnapshotPutBytes;
    extern const Event CasRefSnapshotTailLogs;
    extern const Event CasRefSnapshotPublishDispatched;
    extern const Event CasRefSnapshotPublishBackoff;
    extern const Event CasRefRecoverySealPublished;
    extern const Event CasDedupCacheHits;
    extern const Event CasDedupCacheMisses;
}

namespace DB::Cas
{

/// Runaway brake on CAS/conditional-write retry loops. These keys are single-owner (one writer per
/// namespace by construction), so a contention storm is impossible — the bound only catches a
/// pathological live-lock and never a legitimate steady state.
static constexpr size_t MAX_CAS_ATTEMPTS = 100;

/// Task 10: declared in CasStore.h (shared with CasBuild.cpp's promote). Wire: `u32 count | (u32
/// klen+bytes, u32 vlen+bytes)...`. `std::map` already iterates sorted by key, so this is a pure
/// function of the map's contents with no separate sort step. Reuses the shared `writeLenPrefixed`/
/// `readLenPrefixed` (`CasCodecUtil.h`, with their >UInt32 guard) rather than open-coding the same
/// length-prefixed-string shape a fourth time (already shared by `CasRefLogCodec`/`CasRefSnapshotCodec`).
String encodeMutableFilesPayload(const std::map<String, String> & files)
{
    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(static_cast<uint32_t>(files.size()), out);
    for (const auto & [k, v] : files)
    {
        writeLenPrefixed(out, k);
        writeLenPrefixed(out, v);
    }
    return out.str();
}

std::map<String, String> decodeMutableFilesPayload(const String & payload)
{
    std::map<String, String> files;
    if (payload.empty())
        return files;
    ReadBufferFromMemory in(payload.data(), payload.size());
    uint32_t count = 0;
    readBinaryLittleEndian(count, in);
    for (uint32_t i = 0; i < count; ++i)
    {
        const String k = readLenPrefixed(in);
        const String v = readLenPrefixed(in);
        files[k] = v;
    }
    return files;
}

Store::Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_)
    : pool_backend(std::move(backend_))
    , config(std::move(config_))
    , meta(std::move(meta_))
    /// Seed the monotone admitted-algo cache from the pool state `createOrValidate` already
    /// established (fresh create, steady-state member, or a just-completed admission union) --
    /// register-before-first-write (spec §5) means this Store's own `writeAlgo()` is ALWAYS a
    /// member by the time the constructor runs.
    , admitted_algos(meta.algos_used)
    /// Phase 3 T2/T3: `Layout` no longer captures a pool algo -- every blob key is built from a
    /// `BlobRef` (algo + digest) directly, so the constructor takes only the pool prefix.
    , pool_layout(config.pool_prefix)
{
    if (config.dedup_cache_bytes > 0)
        dedup_cache = std::make_unique<DedupCache>(
            "LRU", CurrentMetrics::end(), CurrentMetrics::end(),
            config.dedup_cache_bytes, DedupCache::NO_MAX_COUNT, DedupCache::DEFAULT_SIZE_RATIO);
    if (config.manifest_decode_cache_bytes > 0)
        manifest_cache = std::make_unique<ManifestDecodeCache>(
            "LRU", CurrentMetrics::end(), CurrentMetrics::end(),
            config.manifest_decode_cache_bytes, /*max_count=*/16384, ManifestDecodeCache::DEFAULT_SIZE_RATIO);

    /// Task 10: the ref-log writer path's retry controller. `config.boot_ms_fn` -- the SAME fake-clock
    /// seam the local write fence uses -- is reused here rather than adding a second clock knob; both
    /// are monotonic-ms clocks and tests that need deterministic deadline behavior already inject it.
    ref_request_controller = std::make_unique<CasRequestController>(pool_backend, config.cas_request_budget, config.boot_ms_fn);
}

void Store::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    ref_request_controller->setSleepFnForTest(std::move(sleep_fn));
}

bool Store::isAlgoAdmitted(BlobHashAlgo algo) const
{
    const auto v = static_cast<uint8_t>(algo);
    std::lock_guard lock(admitted_algos_mutex);
    return std::binary_search(admitted_algos.begin(), admitted_algos.end(), v);
}

std::vector<uint8_t> Store::refreshAdmittedAlgos()
{
    /// A direct GET+decode of `_pool_meta`, not a re-run of `createOrValidate`'s admission logic --
    /// this Store's OWN algo is already admitted (register-before-first-write, spec §5), so all this
    /// needs is the CURRENT authoritative `algos_used`, unioned into the monotone cache.
    const auto existing = pool_backend->get(pool_layout.poolMetaKey());

    std::lock_guard lock(admitted_algos_mutex);
    if (existing)
    {
        const PoolMeta fresh = decodePoolMeta(existing->bytes);
        for (uint8_t v : fresh.algos_used)
            if (!std::binary_search(admitted_algos.begin(), admitted_algos.end(), v))
            {
                admitted_algos.push_back(v);
                std::sort(admitted_algos.begin(), admitted_algos.end());
            }
    }
    return admitted_algos;
}

bool Store::dedupCacheContains(const BlobRef & ref) const
{
    /// Task 3 (Round-B §0.3 introspection): raw lookup counters on the presence cache itself, disabled
    /// (nullptr `dedup_cache`) means neither counter moves -- the short-circuit below never reaches the
    /// probe. `Build::putBlob` calls this seam up to twice on a genuine hit (once to pick the
    /// HEAD-first branch, once more just to attribute `CasBlobBodyPutAvoided` to the cache -- see
    /// CasBuild.cpp), so `CasDedupCacheHits` counts LOOKUPS, not distinct blobs or putBlob calls. A hit
    /// does not itself skip the HEAD that follows in putBlob's HEAD-first branch -- it steers the call
    /// onto that cheap branch instead of an unconditional body stream; the body PUT is what a hit
    /// actually avoids.
    if (!dedup_cache)
        return false;
    if (dedup_cache->contains(ref))
    {
        ProfileEvents::increment(ProfileEvents::CasDedupCacheHits);
        return true;
    }
    ProfileEvents::increment(ProfileEvents::CasDedupCacheMisses);
    return false;
}

void Store::dedupCacheAdd(const BlobRef & ref)
{
    if (dedup_cache)
        dedup_cache->set(ref, std::make_shared<DedupPresent>());
}

uint64_t Store::bootMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

uint64_t Store::bootMsNow() const
{
    return config.boot_ms_fn ? config.boot_ms_fn() : bootMs();
}

void Store::waitSleep(uint64_t ms) const
{
    if (config.wait_sleep_fn)
        config.wait_sleep_fn(ms);
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool Store::mayMutate() const
{
    return !mount_fence.lost.load(std::memory_order_acquire)
        && bootMsNow() < mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
}

void Store::tripMountLost()
{
    mount_fence.lost.store(true, std::memory_order_release);
}

bool Store::refAppendFenceOk() const
{
    /// RFC pre-attempt check: mount fence not lost AND now < lease_deadline AND now + attempt_timeout +
    /// lease_safety_margin < lease_deadline -- `mayMutate` only checks the first two; this adds the
    /// REMAINING-budget check so a controlled attempt is never even started unless it could plausibly
    /// finish (with its own safety margin) before the lease expires.
    if (mount_fence.lost.load(std::memory_order_acquire))
        return false;
    const uint64_t now = bootMsNow();
    const uint64_t deadline = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
    if (now >= deadline)
        return false;
    const uint64_t margin = config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms;
    return margin < deadline - now;
}

void Store::setMountDeadline(uint64_t deadline_boot_ms)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
}

void Store::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
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
        if (!config.skip_access_check)
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
        else
        {
            /// skip_access_check: skip the access-check-class probe I/O (store preconditions + the
            /// `_probe/` round trip, both folded into runCapabilityProbe above) but NOT the
            /// single-attempt conditional-write gate — see `PoolConfig::skip_access_check`. Run it
            /// directly so a Native-mode backend with no working single-attempt client still fails
            /// closed at open instead of silently corrupting CAS state under blind retries later.
            /// The skipped store-precondition check also covers GCS bucket-versioning/delete-marker
            /// detection — that risk is purely environmental (slower GC reclaim, not data loss) and
            /// gets re-checked the next time this pool is opened without skip_access_check.
            backend->checkConditionalWriteSingleAttemptSupport();
        }
    }
    PoolMeta meta = PoolMeta::createOrValidate(
        *backend, layout, config.blob_header_len, config.blob_hash_algo, config.blob_hash_allow_new);
    const BlobHashAlgo write_algo = config.blob_hash_algo;   /// `config` is moved-from just below

    /// Private ctor: make_shared cannot reach it.
    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));

    /// Register-before-first-write, belt-and-braces (spec §5): `createOrValidate` above already
    /// admitted/validated the write algo, so the freshly-seeded cache must already contain it -- a
    /// violation here would mean a build/write could reach this Store naming an algo that was never
    /// durably admitted (the invariant this whole design rests on).
    chassert(store->isAlgoAdmitted(write_algo));

    /// Per-server watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random NONZERO
    /// value minted once per Store: GC checks it for equality only (a different epoch == a dead
    /// incarnation). It rides through the watermark protobuf codec (uint64 field — full range). For
    /// safety and to avoid the 0/UINT64_MAX sentinels, mask to 52 bits (collision-safe for an
    /// equality-only token) and re-draw on 0 (UINT64_MAX is the retired sentinel).
    constexpr uint64_t EPOCH_MASK = (1ULL << 52) - 1;
    store->process_epoch.store(
        (thread_local_rng() ^ (static_cast<uint64_t>(thread_local_rng()) << 32)) & EPOCH_MASK,
        std::memory_order_relaxed);
    if (store->process_epoch.load(std::memory_order_relaxed) == 0)
        store->process_epoch.store(1, std::memory_order_relaxed);

    /// W-ANCHOR: the per-server watermark must be durable BEFORE any object PUT. A read-only open
    /// must never mutate the pool (the probe is skipped above for the same reason), so the watermark
    /// — which rides inside the `gc/server-roots/<server_root_id>/mount` lease object — is only
    /// constructed and anchored on a writable open.
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
        /// Mutable: a GC fence of our fresh lease during open (expiry mid-open racing a GC round) is
        /// recoverable — a fence costs an epoch, so the fence-recovery loop below re-allocates a fresh
        /// writer_epoch and re-claims (P3.1 vector C; TLA+ `NoPermanentWedge`).
        uint64_t writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
        store->process_epoch.store(writer_epoch, std::memory_order_relaxed);

        /// 4. Mount lease — LIVENESS. Decide over the current mount object using a wall-clock `now_ms`.
        const auto now_ms = []() -> uint64_t
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
        const uint64_t ttl_ms = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());

        /// CAS request budget (RFC cas-s3-timeout-retry-control §required-timeout-model, Task 5): a
        /// writable mount refuses to open with a budget that could let a controlled attempt outlive the
        /// mount lease it is fenced under. Throws BAD_ARGUMENTS and aborts open on an inconsistent
        /// budget; logs the effective values once on success. The controller itself (Store's ref
        /// mutation paths) is wired in a later task — this validates the config invariant up front.
        validateCasRequestBudget(store->config.cas_request_budget, ttl_ms,
            static_cast<uint64_t>(store->config.mount_renew_period.count()));

        /// Poll twice per renew period so a live holder's renewal is always observed within the
        /// observation window. Derived from existing config — no new knob (spec §Config).
        const uint64_t poll_interval_ms = std::max<uint64_t>(
            1, static_cast<uint64_t>(store->config.mount_renew_period.count()) / 2);
        /// rev.6 Task 6: routes through `store->waitSleep` (which itself routes through
        /// `config.wait_sleep_fn` when a test injected one) rather than a bare `sleep_for` directly, so
        /// a test intercepting `wait_sleep_fn` observes every wait `open` can block on -- this
        /// observation poll AND the `materialization_grace_ms` wait below.
        const auto sleep_ms = [s = store.get()](uint64_t ms) { s->waitSleep(ms); };
        /// Operator-visible log the moment startup decides to watch a stale-looking self-mount (the
        /// disk-open path blocks up to ~threshold_ms here, so a silent block would be confusing). May
        /// fire more than once per open: the observation restarts (and re-logs) every time the watched
        /// lease's write-token changes before the full threshold elapses.
        const auto on_wait_start = [&srid](const MountLease & held, uint64_t threshold_ms)
        {
            LOG_INFO(getLogger("CasStore"),
                "CAS mount '{}': a stale-looking mount lease is held by uuid={} epoch={} pid={} "
                "hostname={} (expires_at_ms={}); observing its write-token for up to ~{} ms before "
                "reclaiming. If a second server is genuinely live, its renewals will keep restarting "
                "the observation and startup will eventually abort as a live double-start.",
                srid, u128ToHex(held.server_uuid), held.writer_epoch, held.pid, held.hostname,
                held.expires_at_ms, threshold_ms);
        };

        /// Mount-slot writer audit (the P1 "foreign writer" instrument): route every mount-slot
        /// write/conflict event through the Store's own sink. `setEventSink` runs AFTER `open`
        /// returns (the `ContentAddressedMetadataStorage` wiring), so an event fired synchronously
        /// during this very `open` call is dropped by the still-null sink — the same startup window
        /// every other B170 emission site in this file already has (`hasEventSink()`/`emitEvent()`).
        /// `s` outlives the lambda: it is captured by raw pointer into the keeper, a member of
        /// `Store` destroyed before the `Store` itself.
        const auto emit_mount_event = [s = store.get()](const CasEvent & e) { s->emitEvent(e); };

        Store * raw = store.get();

        /// S13 crash-recovery: a hard-killed prior incarnation leaves a stale, unreleased mount lease.
        /// Rather than aborting, OBSERVE that lease's write-token (never its stamped `expires_at_ms`
        /// against our wall clock) until it has held stable for the full rate-bound threshold, then
        /// reclaim it; a genuinely live second server keeps renewing the token and is (after bounded
        /// restarts) reported as LiveDoubleStart. The reclaim is token-guarded (see
        /// `claimMountAwaitingExpiry`), so a live twin is never stolen from.
        ///
        /// Fence-recovery loop (P3.1 vector C): if the GC fences our own fresh lease while we are opening
        /// (the lease expired mid-open — e.g. a slow first beat — and a GC round fenced it), that is a
        /// RECOVERABLE state, not a wedge: a fence costs an epoch, so allocate a fresh writer_epoch and
        /// re-claim. Bounded so a pathological fence storm still fails closed. The fence can surface two
        /// ways: `claimMount` observes an already-fenced own slot (`FencedSelf`), or the keeper's adopt
        /// races a fence between its GET and CAS (`MountFencedException` from `start()`).
        /// rev.6 Task 6: which certificate of death (if any) justified the reclaim FINALLY adopted below
        /// (the last iteration's `claim` before `break` -- `claim` itself is loop-scoped). Read after the
        /// loop to decide the materialization-grace wait.
        MountPriorState claimed_prior = MountPriorState::None;
        constexpr int max_fence_recoveries = 3;
        for (int fence_recovery = 0; ; ++fence_recovery)
        {
            const MountClaimResult claim = claimMountAwaitingExpiry(
                *store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
                [&now_ms]() { return now_ms(); }, [raw] { return raw->bootMsNow(); },
                ttl_ms, poll_interval_ms, sleep_ms, on_wait_start, emit_mount_event);
            if (claim.kind == MountClaimResult::FencedSelf)
            {
                if (fence_recovery >= max_fence_recoveries)
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS mount '{}': our own mount lease was GC-fenced repeatedly during open "
                        "({} recoveries exhausted) — a fresh writer_epoch kept being fenced before we "
                        "could adopt it. This should not persist; investigate GC fence-out timing.",
                        srid, max_fence_recoveries);
                writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
                store->process_epoch.store(writer_epoch, std::memory_order_relaxed);
                continue;
            }
            if (claim.kind != MountClaimResult::Claimed)
            {
                /// LiveDoubleStart (waited out the bound → a live twin) or ForeignOwner → fail closed
                /// with the actionable, multi-line startup error (spec §mount-safety).
                throw Exception(ErrorCodes::ABORTED, "{}", mountDoubleStartMessage(srid, claim.body));
            }
            claimed_prior = claim.prior;

            /// The mount object now holds OUR live (uuid, epoch) body. Construct + start the keeper, which
            /// ADOPTS that very (uuid, epoch) slot rather than self-tripping the double-start guard.
            /// The mount keeper carries the per-server build-watermark floor (`minActive`), read off the
            /// keeper's state lock via `prepareRenew`.
            store->mount_keeper = std::make_unique<MountLeaseKeeper>(
                store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
                store->config.mount_lease_ttl_ms, now_ms,
                [raw] { return raw->minActive(); },
                emit_mount_event);
            /// Keeper ↔ fence coupling (spec §write-fence): on each successful background renew refresh
            /// the monotonic deadline; on a superseded/foreign renew failure latch the fence to lost.
            /// Set BEFORE startBackground so no renewal can fire before the callbacks are in place.
            store->mount_keeper->setFenceCallbacks(
                [raw, ttl_ms] { raw->setMountDeadline(raw->bootMsNow() + ttl_ms); },
                [raw]
                {
                    raw->tripMountLost();
                    /// Liveness counterpart of the fence-out: recover in place as a FRESH incarnation.
                    raw->scheduleRemount();
                });
            try
            {
                store->mount_keeper->start();
            }
            catch (const MountFencedException &)
            {
                /// The GC fenced our fresh lease between the keeper's adopt GET and CAS. Recoverable:
                /// drop this keeper, take a fresh epoch, and re-claim.
                if (fence_recovery >= max_fence_recoveries)
                    throw;
                store->mount_keeper.reset();
                writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
                store->process_epoch.store(writer_epoch, std::memory_order_relaxed);
                continue;
            }
            break;
        }

        /// rev.6 Task 6 (spec §Late Predecessor PUT): a reclaim over a predecessor whose death was NOT
        /// proven clean may still have a conditional PUT from that predecessor in flight when this
        /// incarnation starts trusting its recovery listings -- `Fenced` and `UncleanObserved` are
        /// exactly the two `MountPriorState`s with no such proof (`Clean`, Task 5's drained farewell,
        /// and `None`, a fresh mount / same-epoch refresh with nothing to hand over, pay nothing).
        if (claimed_prior == MountPriorState::Fenced || claimed_prior == MountPriorState::UncleanObserved)
        {
            store->unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
            const uint64_t t_mat = store->config.materialization_grace_ms;
            LOG_INFO(getLogger("CasStore"),
                "Content-addressed mount {} follows an unclean predecessor; waiting {} ms "
                "(materialization grace) for the store to finalize or drop its accepted "
                "requests before trusting recovery listings", srid, t_mat);
            store->waitSleep(t_mat);
        }

        /// Arm the local write fence: cache (uuid, epoch) and set the boottime deadline now + ttl. From
        /// here ordinary ref mutations (appendRefOps) are fence-gated via mayMutate.
        store->armMountFence(our_uuid, writer_epoch,
            store->bootMsNow() + static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count()));
        /// Gate the background renewer with `background_watermark`: it runs only in production
        /// (`background_watermark` = context != nullptr && !read_only), never in unit tests — which
        /// drive renewOnce (or renewWatermarkOnce) explicitly and rely on the armed sub-TTL deadline,
        /// never on the loop. The keeper itself is still started above (it must claim/adopt the mount +
        /// arm the fence on every writable open); only the renewal thread is conditional. The merged
        /// heartbeat renews at `mount_renew_period` — one beat now renews the lease and the floor.
        if (store->config.background_watermark)
            store->mount_keeper->startBackground(store->config.mount_renew_period);

        store->live_writer_epoch.store(writer_epoch, std::memory_order_release);
    }

    return store;
}

Store::~Store()
{
    /// Refuse any further self-remount arming for the rest of teardown. Latched under the thread mutex
    /// (paired with scheduleRemount's checks) BEFORE the join below, so a keeper on_lost firing during
    /// teardown can never re-arm remount_thread after we join it.
    {
        std::lock_guard g(remount_thread_mutex);
        remount_shutting_down.store(true);
    }
    /// Stop the self-remount recovery loop FIRST: it may otherwise re-create the keeper below us.
    remount_stop.store(true);
    remount_cv.notify_all();
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }

    /// rev.6 Task 5 (spec §clean-release drain): the farewell marker `mount_keeper->stop()` writes is a
    /// certificate that no in-flight ref-log conditional PUT from this incarnation can land after it --
    /// a successor treats it as proof of a clean death (`MountPriorState::Clean`, no observation wait
    /// needed). Writing it without an actual drain would be a protocol-safety bug: an uncertain PUT this
    /// incarnation is still resolving could land AFTER the successor already reclaimed and started
    /// mutating. `drainRefLanesForShutdown` is the drain; bounded by one attempt's worth of budget plus
    /// the lease safety margin -- long enough for an in-flight attempt to resolve, never unbounded.
    const bool drained = drainRefLanesForShutdown(
        config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms);

    /// Retire the merged heartbeat on a clean Store teardown: stop() runs the keeper's terminal op,
    /// which stamps the lease already-expired (expires_at_ms = now) AND folds in the watermark
    /// farewell (min_active = UINT64_MAX). Stamping it expired lets a SAME-server reopen reclaim
    /// immediately (the durable epoch + owner stay sticky). A throw here (e.g. a foreign incarnation
    /// touched the slot) must not escape the dtor — log and continue tearing down.
    if (mount_keeper)
    {
        if (drained)
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
        else
        {
            /// Fail-closed: the drain could not certify every in-flight PUT resolved (a timeout or a
            /// live wedge), so writing the clean farewell would be a false certificate. No terminal op --
            /// the successor falls back to the (slower but safe) observation-based reclaim.
            LOG_WARNING(getLogger("CasStore"),
                "CAS store shutdown with an unresolved ref-log PUT: skipping the clean-release marker; "
                "the next mount will treat this end as unclean");
            mount_keeper->stopBackground();
        }
    }

    /// Belt-and-suspenders re-join. The shutting-down flag makes scheduleRemount a no-op above, so this
    /// is normally not joinable; it closes the residual window where a keeper on_lost between the first
    /// join and stop() observed the flag late.
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }
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

bool Store::tryRemountOnce()
{
    std::lock_guard serialize(remount_mutex);

    const String & srid = config.server_root_id;
    const UInt128 our_uuid = config.server_id;
    const auto now_ms = []() -> uint64_t
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
    const uint64_t poll_interval_ms = std::max<uint64_t>(
        1, static_cast<uint64_t>(config.mount_renew_period.count()) / 2);

    /// Best-effort round for the MountRemount audit event only (diagnostic, never correctness-relevant):
    /// `currentGcRound` is a live `gc/state` GET, which may itself fail on the very backend trouble that
    /// is causing this remount attempt to fail — never let that escalate into an uncaught throw out of
    /// a function whose contract is "returns bool, never throws".
    const auto round_for_event = [this]() -> uint64_t
    {
        try { return currentGcRound(); } catch (...) { return 0; }
    };

    /// The same startup protocol as Store::open steps 2-4, as a FRESH incarnation (the old one is
    /// dead by the fence-out contract and its keeper never re-mints). Open THROWS on any failure
    /// (startup is fail-closed); the remount RETURNS false instead — the recovery loop retries.
    try
    {
        claimOwnerOrThrow(*pool_backend, pool_layout, srid, our_uuid);
        const uint64_t writer_epoch = allocateWriterEpoch(*pool_backend, pool_layout, srid);

        /// Mount-slot writer audit: `this` is already fully open (setEventSink ran long ago), so
        /// unlike the initial `open`, every event fired below reaches the real sink immediately.
        const auto emit_mount_event = [this](const CasEvent & e) { emitEvent(e); };

        const auto sleep_ms = [](uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        const MountClaimResult claim = claimMountAwaitingExpiry(
            *pool_backend, pool_layout, srid, our_uuid, writer_epoch,
            now_ms, [this] { return bootMsNow(); }, ttl_ms, poll_interval_ms, sleep_ms,
            [&srid](const MountLease & held, uint64_t threshold_ms)
            {
                LOG_INFO(getLogger("CasStore"),
                    "CAS self-remount '{}': observing a stale-looking mount's write-token (uuid={} "
                    "epoch={} expires_at_ms={}) for up to ~{} ms before reclaiming",
                    srid, u128ToHex(held.server_uuid), held.writer_epoch, held.expires_at_ms, threshold_ms);
            },
            emit_mount_event);
        if (claim.kind != MountClaimResult::Claimed)
        {
            LOG_WARNING(getLogger("CasStore"),
                "CAS self-remount '{}': mount not claimable ({}); will retry", srid,
                claim.kind == MountClaimResult::ForeignOwner ? "foreign owner — never taking over"
                                                             : "a live twin holds the lease");
            return false;
        }

        /// rev.6 Task 7 (spec §Self-remount): pay `materialization_grace_ms` only when this incarnation's
        /// own ref lanes did NOT provably settle before the fence tripped -- an unresolved (still-wedged)
        /// ref-log conditional `PUT` from the dying epoch could still land after recovery starts trusting
        /// its listings. The common case (no in-flight `PUT` when the fence tripped) skips the wait.
        if (!refLanesSettledForRemount())
        {
            unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
            LOG_INFO(getLogger("CasStore"),
                "Self-remount of content-addressed mount {} with an unresolved ref-log PUT; waiting {} ms "
                "(materialization grace) before re-listing", srid, config.materialization_grace_ms);
            waitSleep(config.materialization_grace_ms);
        }

        /// Swap the keeper for the new incarnation. The old keeper's renewal loop already stopped on
        /// its failed renew; never run its terminal op (the slot now belongs to the new claim).
        Store * raw = this;
        if (mount_keeper)
            mount_keeper->stopBackground();
        mount_keeper = std::make_unique<MountLeaseKeeper>(
            pool_backend, pool_layout, srid, our_uuid, writer_epoch,
            config.mount_lease_ttl_ms, now_ms,
            [raw] { return raw->minActive(); },
            emit_mount_event);
        mount_keeper->setFenceCallbacks(
            [raw, ttl_ms] { raw->setMountDeadline(raw->bootMsNow() + ttl_ms); },
            [raw]
            {
                raw->tripMountLost();
                raw->scheduleRemount();
            });
        mount_keeper->start();

        /// Re-establish the ref-protocol incarnation BEFORE re-arming the fence (spec §Startup And
        /// Recovery / §write-fence). Order is load-bearing: `start()` refreshes the lease deadline but
        /// does NOT clear `lost`, so the fence stays closed here and no append/publish can race the swap.
        /// 1. Bump the live epoch so every subsequent `allocateRefTxnId` sorts strictly above any older
        ///    (dead-incarnation or twin) durable log. Do this BEFORE `armMountFence` so there is no window
        ///    where the gate is open while the epoch is still stale. Keep `process_epoch` (the identity
        ///    accessors) equal to it.
        live_writer_epoch.store(writer_epoch, std::memory_order_release);
        process_epoch.store(writer_epoch, std::memory_order_release);
        /// 2. Drain publishers and drop the cached tables so each re-recovers under the new epoch on next
        ///    touch (and any leader still holding an orphaned runtime fails closed). While the fence is lost.
        quiesceRefTablesForRemount();
        /// 3. Re-open the gate. From here appends allocate ids under the new epoch and touch fresh runtimes.
        armMountFence(our_uuid, writer_epoch, bootMsNow() + ttl_ms);
        if (config.background_watermark)
            mount_keeper->startBackground(config.mount_renew_period);

        LOG_INFO(getLogger("CasStore"),
            "CAS self-remount '{}': recovered as writer_epoch {} (fresh incarnation; older builds fail closed)",
            srid, writer_epoch);
        EventEmitter{*this}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::MountRemount;
            e.round = round_for_event();
            e.outcome = "ok";
            e.reason = "self-remount recovered a fresh mount incarnation after fence-out / renewal failure";
            e.detail = {{"writer_epoch", std::to_string(writer_epoch)},
                        {"server_root_id", srid}};
        });
        return true;
    }
    catch (...)
    {
        tryLogCurrentException(getLogger("CasStore"), "CAS self-remount attempt failed; will retry");
        EventEmitter{*this}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::MountRemount;
            e.round = round_for_event();
            e.outcome = "failed";
            e.reason = "self-remount attempt failed; the recovery loop retries with backoff";
            e.detail = {{"server_root_id", srid},
                        {"error", getCurrentExceptionMessage(/*with_stacktrace*/ false)}};
        });
        return false;
    }
}

void Store::scheduleRemount()
{
    /// Task 11 review follow-up: counted unconditionally, BEFORE the `background_watermark` gate below,
    /// so `scheduleRemountCallCountForTest` observes every entry regardless of whether a thread actually
    /// spawns -- see that accessor's comment for why this is the seam a fast unit test should use rather
    /// than driving a real self-remount attempt.
    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
    if (!config.background_watermark)
        return;   /// tests drive tryRemountOnce explicitly (the same gate as every background thread)
    if (remount_shutting_down.load() || remount_running.load())
        return;
    std::lock_guard g(remount_thread_mutex);
    if (remount_shutting_down.load() || remount_running.load())
        return;
    if (remount_thread.joinable())
        remount_thread.join();   /// a PREVIOUS recovery finished; reap it before starting a new one
    remount_running.store(true);
    remount_thread = ThreadFromGlobalPool([this]
    {
        uint64_t backoff_ms = 1000;
        while (!remount_stop.load())
        {
            if (tryRemountOnce())
                break;
            std::unique_lock lk(remount_cv_mutex);
            remount_cv.wait_for(lk, std::chrono::milliseconds(backoff_ms),
                                [this] { return remount_stop.load(); });
            backoff_ms = std::min<uint64_t>(backoff_ms * 2, 30000);
        }
        remount_running.store(false);
    });
}

bool Store::scheduleRemountForTest()
{
    scheduleRemount();
    std::lock_guard g(remount_thread_mutex);
    return remount_thread.joinable();
}

void Store::beginShutdownForTest()
{
    std::lock_guard g(remount_thread_mutex);
    remount_shutting_down.store(true);
}

void Store::renewWatermarkOnce()
{
    /// Renew the merged heartbeat (lease + build-watermark floor). A read-only open never anchored the
    /// keeper; there is nothing to renew (fail closed rather than fabricate one).
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
    inflight_builds.erase(seq);
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

    auto build = std::make_shared<Build>(shared_from_this(), build_id, seq, liveWriterEpoch(), std::move(info));
    /// Register for `dropNamespace`'s post-durable build cancellation (spec §Namespace Removal). weak_ptr:
    /// the wiring owns the returned shared_ptr; `retireBuildSeq` (publish/abandon/dtor) removes the entry.
    {
        std::lock_guard lk(builds_mutex);
        inflight_builds[seq] = build;
    }
    return build;
}

std::optional<Resolved> Store::resolveRef(const RootNamespace & ns, const String & ref_name, bool /*allow_stale*/)
{
    /// Task 10: read side of the snapshot+log protocol (spec §Table State / §Read-Only Consumers). The
    /// `allow_stale` staleness-tolerance knob no longer selects anything: this mounted writer is the
    /// ONLY writer of `ns`'s ref state (no external CAS token to go stale against, unlike the old
    /// per-shard decode cache), so the recovered-and-cached `RefTableState` is always this process's
    /// authoritative view. Kept as a parameter so existing callers compile unchanged.
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    /// Task 11 mount-time triggers: a table this mount only ever READS (never mutates) would otherwise
    /// never have its just-replayed tail/precommits checked -- `appendRefOps`'s own hoisted checks only
    /// fire for a table this mount WRITES to. Both are cheap (lock + comparison) on the warm path (the
    /// flag/threshold is already false after the table's first touch this mount); the sweep, if it DOES
    /// fire, runs synchronously here (safe: this call is not nested inside any queue leader's stack).
    /// Insulated (unlike appendRefOps's own hoisted call): a READ must not fail because a piggybacked
    /// maintenance action hit an uncertain PUT -- see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::lock_guard lock(rt->state_mutex);
    const auto it = rt->state.committed.find(ref_name);
    if (it == rt->state.committed.end())
        return std::nullopt;

    const RefCommittedRow & row = it->second;
    /// B170: a ref resolved to its manifest (the read-path entry point). object_hash is the manifest
    /// instance id the ref names; pairs with a later readManifest ReadMissing if that body is gone.
    if (hasEventSink())
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::RefResolve;
        _ev0.namespace_ = ns.string();
        _ev0.ref_name = ref_name;
        _ev0.object_kind = CasEventObjectKind::Manifest;
        _ev0.object_hash = manifestRefDebugString(row.manifest_ref);
        _ev0.outcome = "resolved";
        _ev0.reason = "read-side resolve of a ref to its part manifest";
        emitEvent(_ev0);
    }
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
        .manifest_size = 0,
        .mutable_files = decodeMutableFilesPayload(row.payload),
        .published_at_ms = row.published_at_ms,
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

std::shared_ptr<const PartManifest> Store::readManifestShared(const ManifestId & id)
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
            _ev1.object_kind = CasEventObjectKind::Manifest;
            _ev1.object_hash = manifestRefDebugString(id.ref);
            _ev1.outcome = "missing";
            _ev1.reason = "live ref names manifest but its object is missing (INV-NO-DANGLE)";
            _ev1.detail = {{"code", "FILE_DOESNT_EXIST"}, {"site", "readManifest"}};
            emitEvent(_ev1);
        }
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "live ref names manifest at {} but its object is missing — INV-NO-DANGLE", key);
    }

    if (manifest_cache)
        if (auto cached = manifest_cache->get(ManifestCacheKey{.manifest_id = id, .token = head.token}))
            return cached;

    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "manifest at {} vanished between head and get — INV-NO-DANGLE", key);
    ProfileEvents::increment(ProfileEvents::CasPartFolderManifestGets);

    PartManifest body = decodePartManifest(object->bytes);

    /// refMatchesBody: the journal ManifestRef must equal the body's self-described `ref`. A mismatch
    /// means the ref addresses the WRONG object (spec §Object Identity And Ownership).
    if (!refMatchesBody(id.ref, body))
    {
        if (hasEventSink())
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::CorruptDecode;
            _ev2.object_kind = CasEventObjectKind::Manifest;
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
            _ev3.object_kind = CasEventObjectKind::Manifest;
            _ev3.object_hash = manifestRefDebugString(id.ref);
            _ev3.outcome = "corrupt";
            _ev3.reason = "manifest body root_namespace_id does not match the owning namespace (manifestNamespaceMatches)";
            _ev3.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readManifest"}};
            emitEvent(_ev3);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS manifest at {} body root_namespace_id does not match the owning namespace — manifestNamespaceMatches", key);
    }

    auto decoded = std::make_shared<PartManifest>(std::move(body));
    if (manifest_cache)
        manifest_cache->set(ManifestCacheKey{.manifest_id = id, .token = head.token}, decoded);
    return decoded;
}

PartManifest Store::readManifest(const ManifestId & id)
{
    return *readManifestShared(id);
}

BlobLocation Store::locate(const ManifestEntry & entry) const
{
    /// A ranged read into the content object: the payload starts at a constant offset for blobs
    /// (the pool's fixed blob_header_len — no per-object header read). Inline carries no standalone
    /// object location (there is no Subtree placement on a part manifest).
    switch (entry.placement)
    {
        case EntryPlacement::Blob:
        {
            /// Phase 3 T2: the blob's object key is built directly from the entry's own `ref` (algo +
            /// digest) -- no pool-scoped codec needed, since the key no longer depends on a pool-wide
            /// width assumption.
            return BlobLocation{
                .key = pool_layout.blobKey(entry.ref),
                .offset = meta.blob_header_len,
                .length = entry.blob_size,
            };
        }
        case EntryPlacement::Inline:
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "entry placement {} has no blob location", static_cast<int>(entry.placement));
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "entry placement {} has no blob location", static_cast<int>(entry.placement));
}

std::map<String, Resolved> Store::listRefs(const RootNamespace & ns)
{
    /// Task 10: the whole ref set is a map iteration over this namespace's recovered-and-cached
    /// `RefTableState` (spec §Why One LIST Is Sufficient / §Startup And Recovery) -- an empty or
    /// never-touched namespace still costs exactly one `LIST` (recovery) and zero further requests;
    /// a warm namespace costs nothing at all (replacing the old per-shard LIST-then-HEAD-present-shards
    /// dance, since there is no longer a shard fan-out to rediscover on every call).
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    /// Task 11 mount-time triggers: see the identical comment in `resolveRef`. Insulated for the same
    /// reason -- see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::map<String, Resolved> result;
    std::lock_guard lock(rt->state_mutex);
    for (const auto & [ref_name, row] : rt->state.committed)
        result.emplace(ref_name, Resolved{
            .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
            .manifest_size = 0,
            .mutable_files = decodeMutableFilesPayload(row.payload),
            .published_at_ms = row.published_at_ms,
        });
    return result;
}

std::shared_ptr<Store::RefTableRuntime> Store::getRefTableRuntime(const RootNamespace & ns)
{
    std::lock_guard lock(ref_queue_mutex);
    auto & slot = ref_tables[ns.string()];
    if (!slot)
        slot = std::make_shared<RefTableRuntime>();
    return slot;
}

void Store::ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt)
{
    /// Held for the WHOLE recovery (including its I/O): there is nothing safe to do with an unrecovered
    /// table's `state` anyway, so a concurrent second caller for the SAME namespace blocking here is
    /// correct, not a missed-concurrency opportunity -- and this only affects each table's FIRST touch
    /// per mounted Store (spec §Startup And Recovery: "one LIST ... cache the resulting complete table
    /// state").
    {
    std::lock_guard lock(rt.state_mutex);
    /// Whole-table LRU stamp (spec §Byte, Memory, And CPU Budget): every touch -- warm or cold --
    /// marks this table most-recently-used so `enforceRefTableCacheBudget` evicts idle tables first.
    rt.last_touch_tick = ref_table_access_tick.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rt.recovered)
        return;

    for (uint64_t attempt = 0; ; ++attempt)
    {
        if (attempt > 0)
        {
            if (attempt > kRefRecoveryMaxRestarts)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot or "
                    "log object kept vanishing between its LIST and GET) — giving up; this bound is a "
                    "runaway brake against a pathological cleanup race, not an expected steady state",
                    ns.string(), attempt - 1);
            ++rt.recovery_restarts;
            ProfileEvents::increment(ProfileEvents::CasRefRecoveryRestarts);
        }

        /// spec §Why One LIST Is Sufficient: one namespace LIST returns every surviving snapshot, log,
        /// and `_cleanup` marker key.
        std::optional<RefTxnId> greatest_snapshot;
        std::vector<RefTxnId> log_ids;
        std::set<RefTxnId> cleanup_markers;
        const String prefix = pool_layout.refsNamespacePrefix(ns);
        String cursor;
        for (;;)
        {
            const ListPage page = pool_backend->list(prefix, cursor, /*limit=*/1000);
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = pool_layout.parseRefObjectKey(lk.key);
                if (!parsed)
                    continue;   /// not one of Task 10's ref-object keys (e.g. a legacy shard-number key)
                /// Trust the parsed `ns` only when it names EXACTLY this
                /// namespace -- the same checkNamespace-level guarantee the key builders enforce, not
                /// position math (the scoped LIST prefix already implies this in practice, but a listed
                /// key is untrusted input and is treated as such).
                if (parsed->ns != ns)
                    continue;
                switch (parsed->kind)
                {
                    case RefObjectKind::Cleanup:
                        cleanup_markers.insert(parsed->txn_id);
                        break;
                    case RefObjectKind::Log:
                        log_ids.push_back(parsed->txn_id);
                        break;
                    case RefObjectKind::Snap:
                        if (!greatest_snapshot || *greatest_snapshot < parsed->txn_id)
                            greatest_snapshot = parsed->txn_id;
                        break;
                }
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
        std::sort(log_ids.begin(), log_ids.end());

        /// rev.6 Task 8 (spec §recovery-seal): the greatest txn id this attempt's LIST observed across
        /// BOTH snapshots and logs -- used below to decide whether a dead-epoch region exists to seal.
        /// Recomputed fresh on every restart-on-vanish attempt, exactly like `greatest_snapshot`/`log_ids`.
        std::optional<RefTxnId> greatest_listed_id = greatest_snapshot;
        if (!log_ids.empty() && (!greatest_listed_id || *greatest_listed_id < log_ids.back()))
            greatest_listed_id = log_ids.back();

        bool vanished = false;
        std::optional<RefTableSnapshot> snapshot;
        uint64_t snapshot_body_bytes = 0;   /// weight of the recovered base (spec §Byte Budget); 0 = never-born base
        if (greatest_snapshot)
        {
            const auto got = pool_backend->get(pool_layout.refSnapshotKey(ns, *greatest_snapshot));
            if (!got)
                vanished = true;   /// covered by a newer snapshot published-before-delete; restart
            else
            {
                snapshot = decodeRefTableSnapshot(got->bytes, ns.string(), *greatest_snapshot);
                snapshot_body_bytes = got->bytes.size();
            }
        }

        std::vector<RefLogTxn> tail;
        std::vector<uint64_t> tail_bytes;
        if (!vanished)
        {
            for (const RefTxnId & id : log_ids)
            {
                if (greatest_snapshot && !(*greatest_snapshot < id))
                    continue;   /// at or below the selected snapshot: already covered
                const auto got = pool_backend->get(pool_layout.refLogKey(ns, id));
                if (!got)
                {
                    vanished = true;
                    break;
                }
                tail.push_back(decodeRefLogTxn(got->bytes, ns.string(), id));
                tail_bytes.push_back(got->bytes.size());
            }
        }

        if (vanished)
            continue;   /// restart-on-vanish (spec §Startup And Recovery): not corruption, retry fresh

        rt.state = replay(snapshot, tail);
        rt.cleanup_markers = std::move(cleanup_markers);

        /// rev.6 Task 8 (spec §recovery-seal): at an UNCLEAN mount, close every dead epoch THIS
        /// recovery discovered with an immediate snapshot -- the seal -- published BEFORE the table is
        /// exposed as recovered. This runs BEFORE `rt.recovered = true` below on purpose: a failed seal
        /// PUT throws here, leaving the table unrecovered, so the NEXT touch restarts recovery from
        /// scratch (fresh LIST, fresh replay, fresh seal attempt) rather than exposing a table whose
        /// dead-epoch region was never actually closed (spec: "failure fails closed").
        ///
        /// `seal_id` is the UPPER BOUND of the dead-epoch region, not the greatest listed id: the wedge
        /// discipline places the predecessor's one possible in-flight PUT strictly above everything it
        /// resolved, so only the epoch-closing bound is guaranteed to dominate it (spec §seal-id) --
        /// any later materialization from a dead epoch is born covered (`<=` seal_id) for every
        /// observer, forever. `sealed_from` records the greatest id this recovery actually listed, the
        /// observation horizon a later log past it is measured against (Task 12's late-log detector).
        const uint64_t my_epoch = liveWriterEpoch();
        const RefTxnId seal_id{my_epoch - 1, std::numeric_limits<uint64_t>::max()};
        const bool dead_region_nonempty =
            greatest_listed_id.has_value() && greatest_listed_id->writer_epoch < my_epoch;
        const bool already_sealed = snapshot.has_value() && !(snapshot->snapshot_id < seal_id);
        if (unclean_epoch_boundary_seen.load(std::memory_order_relaxed) && my_epoch >= 2
            && dead_region_nonempty && !already_sealed && rt.state.lifecycle == RefLifecycle::Live)
        {
            RefTableSnapshot seal = snapshotOf(rt.state, ns.string());
            seal.snapshot_id = seal_id;                     /// upper bound of the covered region
            seal.sealed_from = rt.state.greatest_applied;    /// == greatest_listed_id: nothing else was applied
            const String seal_bytes = encodeRefTableSnapshot(seal);
            const auto fence_ok = [this] { return refAppendFenceOk(); };
            const auto outcome = ref_request_controller->putIfAbsentControlled(
                pool_layout.refSnapshotKey(ns, seal_id), seal_bytes, fence_ok);
            if (outcome != CasWriteOutcome::Committed)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS recovery seal PUT for namespace '{}' did not commit; failing recovery closed "
                    "(the table stays unrecovered/non-writable; the next touch restarts recovery and "
                    "re-seals)", ns.string());
            ProfileEvents::increment(ProfileEvents::CasRefRecoverySealPublished);

            /// The seal now covers everything replayed so far: feed it into the SAME bookkeeping below
            /// (`rt.newest_snapshot_id`, the tail counters, cache weight) as if it were the recovered
            /// snapshot and the tail were empty -- exactly what a fresh recovery against the now-durable
            /// seal would find.
            snapshot = std::move(seal);
            tail.clear();
            tail_bytes.clear();
            snapshot_body_bytes = seal_bytes.size();
        }

        rt.recovered = true;

        /// rev.6 Task 10 (spec §publish-from-live): seed the tail counters from this SAME recovery
        /// pass -- `rt.state` above already IS `Replay(snapshot, tail)` (the mount-time trigger's
        /// candidate body needs no separate base+tail bookkeeping any more), so only the count and byte
        /// sum of the logs strictly above the recovered snapshot need seeding.
        rt.newest_snapshot_id = snapshot ? std::optional<RefTxnId>(snapshot->snapshot_id) : std::nullopt;
        rt.tail_count_since_snapshot.store(tail.size(), std::memory_order_relaxed);
        uint64_t seeded_tail_bytes = 0;
        for (uint64_t bytes : tail_bytes)
            seeded_tail_bytes += bytes;
        rt.tail_bytes_since_snapshot.store(seeded_tail_bytes, std::memory_order_relaxed);
        /// Task 11 (spec §Clean Up Old Precommits): dispatched once, from `appendRefOps`'s top level
        /// (never from here -- this call may itself be nested inside a queue leader's flush stack).
        rt.needs_stale_precommit_sweep = true;

        /// Per-table admission budgets (spec §Snapshot Format): pre-subtract this table's own wire
        /// overhead (`4 + ns.size()`, repeated once in a snapshot body and once in a removal txn body)
        /// plus a fixed Phase-1 safety margin from the raw hard limits, once, here.
        const uint64_t overhead = 4 + ns.string().size() + kRefAdmissionSafetyMargin;
        rt.snapshot_budget = overhead < ref_snapshot_max_bytes ? ref_snapshot_max_bytes - overhead : 0;
        rt.removal_budget = overhead < ref_removal_max_bytes ? ref_removal_max_bytes - overhead : 0;

        /// Cache-weight base (spec §Byte, Memory, And CPU Budget): the encoded body size of the recovered
        /// base snapshot, captured for free from the GET above. The tail bytes are tracked separately in
        /// `tail_bytes_since_snapshot`; the two sum to this table's estimated resident weight.
        rt.base_snapshot_bytes.store(snapshot_body_bytes, std::memory_order_relaxed);
        break;
    }
    }

    /// A NEW table was just materialized; enforce the whole-table cache budget, protecting this one
    /// (spec §Byte, Memory, And CPU Budget). Runs OUTSIDE `rt.state_mutex` (that scope closed above) so
    /// the pass -- which acquires `ref_queue_mutex` and try-locks other tables' `state_mutex` -- never
    /// nests this table's `state_mutex` under `ref_queue_mutex`.
    enforceRefTableCacheBudget(ns);
}

void Store::enforceRefTableCacheBudget(const RootNamespace & keep_ns)
{
    if (config.ref_table_cache_bytes == 0)
        return;   /// 0 = unbounded: eviction disabled

    /// Evicted runtimes are held alive here until AFTER every lock is released, so a runtime whose sole
    /// owner is its map slot is never destroyed while we still hold its `state_mutex` (that would destroy
    /// a locked mutex).
    std::vector<std::shared_ptr<RefTableRuntime>> evicted;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);

        /// Relaxed atomic reads: the `total` loop below reads this for EVERY table, including hot ones a
        /// concurrent append lane is mutating under `state_mutex` only (a cross-lock read). The gated
        /// candidate loop reads it too, but only for `use_count()==1` tables (no concurrent writer).
        const auto weightOf = [](const RefTableRuntime & rt)
        {
            return rt.base_snapshot_bytes.load(std::memory_order_relaxed)
                 + rt.tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        };

        uint64_t total = 0;
        for (const auto & [name, rt] : ref_tables)
            total += weightOf(*rt);
        if (total <= config.ref_table_cache_bytes)
            return;

        /// Idle candidates, least-recently-touched first. Idle == the map holds the SOLE `shared_ptr`
        /// (`use_count() == 1`: no in-flight caller, queued append, leader, or background publish holds a
        /// copy), no active queue leader, an empty pending queue, and not the just-recovered `keep_ns`.
        /// The `use_count() == 1` gate is what makes append-lane split-brain impossible: any thread that
        /// fetched this runtime keeps it non-evictable for as long as it holds the copy.
        struct Cand { String name; uint64_t tick; uint64_t weight; };
        std::vector<Cand> cands;
        for (const auto & [name, rt] : ref_tables)
        {
            if (name == keep_ns.string())
                continue;
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty())
                continue;
            cands.push_back(Cand{name, rt->last_touch_tick, weightOf(*rt)});
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand & a, const Cand & b) { return a.tick < b.tick; });

        for (const Cand & c : cands)
        {
            if (total <= config.ref_table_cache_bytes)
                break;
            auto it = ref_tables.find(c.name);
            if (it == ref_tables.end())
                continue;
            std::shared_ptr<RefTableRuntime> & rt = it->second;
            {
                /// `use_count() == 1` guarantees no other thread holds the runtime, so this try_lock
                /// cannot fail; the RAII scope releases `state_mutex` before `rt` is moved out. A wedged
                /// append lane is never evicted -- its uncertain in-flight PUT is not reconstructable from
                /// the durable objects, and re-recovery could re-allocate an id (spec §Writer-Side
                /// Linearization forbids this).
                std::unique_lock<std::mutex> slock(rt->state_mutex, std::try_to_lock);
                if (!slock.owns_lock() || rt->wedge.has_value())
                    continue;
            }
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty())
                continue;   /// re-check under the still-held ref_queue_mutex
            total -= c.weight;
            evicted.push_back(std::move(rt));   /// keep alive past the erase and lock release
            ref_tables.erase(it);
            ProfileEvents::increment(ProfileEvents::CasRefTableEvictions);
        }
    }
    /// `evicted` destructs the dropped runtimes here, with no lock held.
}

bool Store::refLanesSettledForRemount()
{
    /// Same wait mechanics as `drainRefLanesForShutdown`, without its `shutting_down` admission latch --
    /// self-remount mutations are already refused by the freshly-tripped mount fence, not an admission
    /// check, so there is nothing to latch here. Budget is exactly one attempt's worth: long enough for
    /// an in-flight leader to observe the tripped fence and settle, never unbounded.
    const uint64_t wait_budget_ms =
        config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms;

    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }

    /// Wait for every table's queue to go idle (no pending item, no active leader), bounded overall by
    /// `wait_budget_ms` -- mirrors `drainRefLanesForShutdown`'s own wait loop exactly (see its comments).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(ref_queue_mutex);
        for (const auto & rt : runtimes)
        {
            while (!(rt->pending.empty() && !rt->leader_active))
            {
                if (rt->cv.wait_until(lk, deadline) == std::cv_status::timeout
                    && !(rt->pending.empty() && !rt->leader_active))
                {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out)
                break;
        }
    }

    /// A queue going idle does NOT by itself prove no PUT is in flight -- see `drainRefLanesForShutdown`'s
    /// identical comment on this same check. Every table is checked regardless of `timed_out`, purely for
    /// a complete diagnostic; the return value already fails closed on either condition alone.
    bool any_wedge = false;
    for (const auto & rt : runtimes)
    {
        std::lock_guard<std::mutex> lock(rt->state_mutex);
        if (rt->wedge.has_value())
            any_wedge = true;
    }

    return !timed_out && !any_wedge;
}

void Store::quiesceRefTablesForRemount()
{
    /// Snapshot the current runtimes (copies keep them alive across the drain). New dispatches are
    /// already suppressed while the fence is lost (`maybeScheduleSnapshotPublish`'s fence guard), so the
    /// only publishers to drain are those dispatched before the fence dropped.
    std::vector<std::shared_ptr<RefTableRuntime>> tables;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        tables.reserve(ref_tables.size());
        for (auto & [name, rt] : ref_tables)
            tables.push_back(rt);
    }

    /// Wait for every in-flight background publisher to finish so none is mid-PUT when its runtime is
    /// detached. A publisher observes the lost fence (`fence_ok` false) and returns without committing,
    /// then decrements `pending_snapshot_publishes` under `state_mutex` and signals `publish_settle_cv`.
    for (auto & rt : tables)
    {
        std::unique_lock<std::mutex> slock(rt->state_mutex);
        rt->publish_settle_cv.wait(slock,
            [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
    }

    /// Detach every cached table. Mark it superseded FIRST (release, and before the caller re-arms the
    /// fence): a leader that raced in and holds one of these orphaned runtimes then fails closed at the
    /// `flushRefBatch` gate rather than allocating an id against a stale cache under the re-armed fence.
    /// Queued callers self-drain -- each `flushRefBatch` for a superseded runtime completes its whole
    /// carved batch with a retry error, so no caller hangs; the next touch creates a fresh runtime that
    /// re-recovers from the durable snapshot+log objects under `live_writer_epoch`. Dropping the map slot
    /// discards each runtime's in-memory wedge; `refLanesSettledForRemount` already consulted it above
    /// and `tryRemountOnce` paid `materialization_grace_ms` when it was unresolved, so nothing about this
    /// drop needs to be certified here.
    std::vector<std::shared_ptr<RefTableRuntime>> detached;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        detached.reserve(ref_tables.size());
        for (auto & [name, rt] : ref_tables)
        {
            rt->superseded_by_remount.store(true, std::memory_order_release);
            rt->cv.notify_all();   /// wake any waiter so it re-leads and fails closed against the flag
            detached.push_back(rt);
        }
        ref_tables.clear();
    }
    /// `detached` releases the map's references here (with no lock held); each runtime lives on only as
    /// long as an in-flight leader/caller still holds it.
}

uint64_t Store::refRecoveryRestartsForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->recovery_restarts;
}

bool Store::refLaneWedgedForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->wedge.has_value();
}

String Store::wedgedKeyForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->wedge ? rt->wedge->key : String{};
}

void Store::forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                              const String & key, const String & bytes)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    rt->wedge = RefAppendWedge{RefTxnId{writer_epoch, ref_sequence}, key, bytes};
}

bool Store::needsStalePrecommitSweepForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->needs_stale_precommit_sweep;
}

size_t Store::wedgedRefLaneCount()
{
    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }
    size_t wedged = 0;
    for (const auto & rt : runtimes)
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->wedge.has_value())
            ++wedged;
    }
    return wedged;
}

bool Store::drainRefLanesForShutdown(uint64_t wait_budget_ms)
{
    /// Latch FIRST, then snapshot under `ref_queue_mutex` (see the `shutting_down` member comment): this
    /// ordering is what makes the check in `appendRefOps` -- performed inside the SAME critical section
    /// as its `pending.push_back` -- race-free against the snapshot below, for both an already-cached
    /// table and one whose very first touch races this call.
    shutting_down.store(true, std::memory_order_release);

    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }

    /// Wait for every table's queue to go idle (no pending item, no active leader), bounded overall by
    /// `wait_budget_ms` -- `cv.wait_until` slices against one shared deadline, never a sleep. All the
    /// runtimes share the one `ref_queue_mutex` that guards `pending`/`leader_active` (see the
    /// `RefTableRuntime` field comments), so a single `lk` covers every table in the loop below; each
    /// table's OWN `cv` is what its leader/appendRefOps notifies on a state change, so the wait must
    /// target that specific `cv`, one table at a time.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(ref_queue_mutex);
        for (const auto & rt : runtimes)
        {
            while (!(rt->pending.empty() && !rt->leader_active))
            {
                if (rt->cv.wait_until(lk, deadline) == std::cv_status::timeout
                    && !(rt->pending.empty() && !rt->leader_active))
                {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out)
                break;
        }
    }

    /// A queue going idle does NOT by itself prove no PUT is in flight: a wedge is recorded (under
    /// `state_mutex`) strictly BEFORE the wedged item's caller is completed and the leader bookkeeping
    /// reset (see `flushRefBatch`'s `Unresolved` case), so this check -- performed AFTER the wait above
    /// -- observes it whenever the queue-idle wait itself raced a wedge. Every table is checked
    /// regardless of `timed_out`, purely for a complete diagnostic; the return value already fails
    /// closed on either condition alone.
    bool any_wedge = false;
    for (const auto & rt : runtimes)
    {
        std::lock_guard<std::mutex> lock(rt->state_mutex);
        if (rt->wedge.has_value())
            any_wedge = true;
    }

    return !timed_out && !any_wedge;
}

bool Store::observedNamespaceCleanupMarker(const RootNamespace & ns, const RefTxnId & remove_txn_id)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    if (rt->cleanup_markers.contains(remove_txn_id))
        return true;

    /// Warm-mount re-observation (Task 12): the recovery `LIST` that populated `cleanup_markers` may have
    /// run BEFORE GC's namespace-cleanup item published the `_cleanup/<remove_txn_id>` marker, so a
    /// warm-mounted writer that dropped a namespace and recreates it within the same mount lifetime would
    /// otherwise be rejected until it remounts. Do ONE exact-key backend check of the marker before
    /// answering; if it is durably present now, adopt it into the cached set. This preserves fail-close
    /// (a still-absent marker keeps recreation rejected -- evidence is refreshed, never assumed) and
    /// matches the recovery restart-on-vanish philosophy of consulting the durable object on a cache miss.
    const HeadResult head = backend().head(layout().refCleanupMarkerKey(ns, remove_txn_id));
    if (head.exists)
    {
        rt->cleanup_markers.insert(remove_txn_id);
        return true;
    }
    return false;
}

RefTxnId Store::appendRefOps(const RootNamespace & ns, MutationScope scope,
                             std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                             RootMutationOrigin origin, RootMutationKind kind)
{
    const auto rt = getRefTableRuntime(ns);
    /// Hoisted here (rather than left to `flushRefBatch`'s own idempotent call) so both Task 11
    /// triggers below run on the CALLING thread, strictly BEFORE this call enqueues its own item or
    /// becomes a queue leader -- `maybeSweepStalePrecommits`'s own nested `appendRefOps` calls are
    /// therefore always a fresh top-level invocation, never nested inside a leader's flush stack
    /// (which would deadlock the leader against itself).
    ensureRefTableRecovered(ns, *rt);
    maybeSweepStalePrecommits(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    auto item = std::make_shared<RefMutationItem>();
    item->scope = std::move(scope);
    item->build_ops = std::move(build_ops);
    item->origin = origin;
    item->kind = kind;

    const auto enqueued_at = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(ref_queue_mutex);
    /// rev.6 Task 5: refuse admission once a clean-release drain has begun (`drainRefLanesForShutdown`).
    /// Checked in the SAME critical section as the `pending.push_back` below -- the pairing that makes
    /// this race-free against the drain's snapshot-and-wait (see the `shutting_down` member comment).
    if (shutting_down.load(std::memory_order_acquire))
        throw Exception(ErrorCodes::ABORTED,
            "CAS store is shutting down — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id);
    rt->pending.push_back(item);

    while (!item->done)
    {
        if (!rt->leader_active)
        {
            rt->leader_active = true;
            lk.unlock();
            try
            {
                runRefQueueLeader(ns, rt, item);
            }
            catch (...)
            {
                /// Any exceptional exit from the leader loop (e.g. an unhandled CORRUPTED_DATA the flush's
                /// controller sites now surface loudly) must restore `leader_active`, or every queued and
                /// future `appendRefOps` caller for this table blocks forever in `cv.wait` -- a silent hang
                /// instead of a fail-closed error. Idempotent with the flush's own resets; rethrow so the
                /// corruption surfaces to this caller rather than being swallowed.
                lk.lock();
                rt->leader_active = false;
                rt->cv.notify_all();
                throw;
            }
            lk.lock();
            rt->leader_active = false;
            rt->cv.notify_all();
        }
        else
        {
            rt->cv.wait(lk);
        }
    }
    lk.unlock();

    ProfileEvents::increment(ProfileEvents::CasRefQueueWaitMicroseconds,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - enqueued_at).count());
    if (item->error)
        std::rethrow_exception(item->error);
    return item->committed_id;
}

void Store::runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                              const std::shared_ptr<RefMutationItem> & own)
{
    /// Fairness baton pass: serve flushes only until the caller's OWN item is done, then hand off to a
    /// woken waiter.
    while (true)
    {
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            if (own->done)
                return;
        }
        flushRefBatch(ns, rt);
    }
}

namespace
{
/// rev.6 Task 11 (spec §anomaly-policy background diagnostics): a tolerant, read-only peek at the
/// `RefLogTxn` wire header (`CasRefLogCodec.cpp`'s `u32 format_version | u32-len ns | u64 writer_epoch |
/// u64 ref_sequence | ...`) WITHOUT `decodeRefLogTxn`'s expected-value cross-check -- the whole point
/// of this diagnostic is that the body is NOT expected to match this key's identity. Never validates
/// `format_version`, never reads past the header (the op payload is irrelevant to identifying the
/// writer), and swallows any truncation/garbage: this is a background diagnostic only, never a decode
/// anything else depends on.
struct ForeignRefLogHeaderPeek
{
    String ns;
    uint64_t writer_epoch = 0;
    uint64_t ref_sequence = 0;
};

std::optional<ForeignRefLogHeaderPeek> peekForeignRefLogHeader(const String & bytes)
{
    try
    {
        ReadBufferFromMemory in(bytes.data(), bytes.size());
        uint32_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        ForeignRefLogHeaderPeek peek;
        peek.ns = readLenPrefixed(in);
        readBinaryLittleEndian(peek.writer_epoch, in);
        readBinaryLittleEndian(peek.ref_sequence, in);
        return peek;
    }
    catch (...)
    {
        return std::nullopt;
    }
}
}

void Store::reportImpossibleInterference(const String & key, const String & reason,
                                          const std::optional<String> & offending_ns)
{
    LOG_ERROR(getLogger("CasStore"),
        "CAS anomaly policy: impossible foreign interference for server_root '{}' (namespace='{}', key='{}'): "
        "{} -- fencing this mount closed and scheduling a remount",
        config.server_root_id, offending_ns.value_or(String{}), key, reason);

    EventEmitter{*this}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::ForeignInterference;
        if (offending_ns)
            e.namespace_ = *offending_ns;
        e.reason = reason;
        e.detail = {{"key", key}, {"server_root_id", config.server_root_id}};
    });

    /// spec §anomaly-policy: incidental-only detection, fail-closed reaction -- the SAME on_lost
    /// mechanics a foreign/superseded lease renewal already drives (the keeper's `setFenceCallbacks`
    /// lambda above).
    tripMountLost();
    scheduleRemount();

    /// Diagnosis off the critical path (spec §anomaly-policy): a background task may spend a FEW
    /// requests -- never the caller's thread, and never blocking this call's own return.
    /// `shared_from_this()` keeps the Store alive for the thread's lifetime (mirrors
    /// `maybeScheduleSnapshotPublish`'s dispatch).
    auto self = shared_from_this();
    try
    {
        ThreadFromGlobalPool([self, key]
        {
            try
            {
                const auto got = self->pool_backend->get(key);
                if (!got)
                {
                    LOG_ERROR(getLogger("CasStore"),
                        "CAS anomaly diagnostics: the offending object at '{}' had already vanished by the "
                        "time the background diagnostic GET ran", key);
                    return;
                }
                if (const auto peek = peekForeignRefLogHeader(got->bytes))
                    LOG_ERROR(getLogger("CasStore"),
                        "CAS anomaly diagnostics: offending object at '{}' ({} bytes) decodes as a ref-log "
                        "header: namespace='{}', writer_epoch={}, ref_sequence={}",
                        key, got->bytes.size(), peek->ns, peek->writer_epoch, peek->ref_sequence);
                else
                    LOG_ERROR(getLogger("CasStore"),
                        "CAS anomaly diagnostics: offending object at '{}' ({} bytes) does not decode as a "
                        "ref-log header -- raw and unidentified", key, got->bytes.size());
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasStore"),
                    "CAS anomaly diagnostics: background GET failed for '" + key + "'");
            }
        }).detach();
    }
    catch (...)
    {
        /// Pool exhaustion: best-effort diagnostics must never block the caller's own fail-closed throw.
        tryLogCurrentException(getLogger("CasStore"), "CAS anomaly diagnostics dispatch failed to launch for '" + key + "'");
    }
}

void Store::flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// One flush = one carved batch through one attempted append. Contract: every ORDINARY outcome
    /// (validation reject, DefiniteFailure, Unresolved/wedge, Committed) lands in the affected items so
    /// waiters always wake, and this does NOT throw for any of them. The ONE
    /// exception is the provably-unreachable case where a DURABLY-committed transaction then fails to
    /// apply to the in-memory state (which the whole-item shape validation is supposed to preclude): that
    /// path completes every waiting survivor with the error, restores the leader bookkeeping, and RETHROWS
    /// a LOGICAL_ERROR (the object is already durable and every future recovery would re-hit it -- see the
    /// Committed-case catch below), so the caller learns this table's lane is bricked instead of hanging.
    auto complete_error = [&](const std::vector<std::shared_ptr<RefMutationItem>> & items, std::exception_ptr e)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        for (const auto & it : items)
        {
            it->error = e;
            it->done = true;
        }
        rt->cv.notify_all();
    };
    auto carve_all_pending = [&]() -> std::vector<std::shared_ptr<RefMutationItem>>
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        std::vector<std::shared_ptr<RefMutationItem>> all(rt->pending.begin(), rt->pending.end());
        rt->pending.clear();
        return all;
    };

    try
    {
        ensureRefTableRecovered(ns, *rt);
    }
    catch (...)
    {
        complete_error(carve_all_pending(), std::current_exception());
        return;
    }

    /// Local write fence (spec §write-fence): a superseded/paused writer must not race the live one.
    /// Fails the WHOLE queue -- every caller would have gotten the same refusal alone.
    if (!mayMutate())
    {
        complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return;
    }

    /// Self-remount re-incarnation (spec §Startup And Recovery): this runtime was detached by a
    /// `quiesceRefTablesForRemount` swap, so its cache is a stale (pre-remount) view. Fail the whole
    /// carved batch closed -- allocating an id / applying against this orphaned runtime under the
    /// re-armed fence would split-brain against the fresh runtime the next touch re-recovers. The
    /// superseded flag is ordered before the fence re-arm (release/acquire through `mayMutate`), so
    /// reaching this AFTER passing `mayMutate` above proves the swap happened.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// The pre-attempt / post-write fence gate for THIS flush's controller calls. It also folds in
    /// `superseded_by_remount` so the append PUT is airtight against a self-remount that lands between a
    /// leader's pre-allocate re-check and its PUT: `superseded` is published before `armMountFence`, so a
    /// leader whose PUT observes a LIVE fence (`refAppendFenceOk` true) necessarily observes the flag too
    /// (release/acquire through the fence) and reports Unresolved instead of committing against a stale cache.
    const auto fence_ok = [this, &rt] { return refAppendFenceOk() && !rt->superseded_by_remount.load(std::memory_order_acquire); };

    /// Resolve an outstanding wedge FIRST (spec §Writer-Side Linearization): "It does not start a later
    /// ref-log PUT for that table until the earlier result is resolved."
    {
        std::optional<RefAppendWedge> wedge_copy;
        {
            std::lock_guard lock(rt->state_mutex);
            wedge_copy = rt->wedge;
        }
        if (wedge_copy)
        {
            CasWriteOutcome resolved;
            try
            {
                resolved = ref_request_controller->resolveByExactGet(wedge_copy->key, wedge_copy->bytes);
            }
            catch (...)
            {
                /// `resolveByExactGet` throws CORRUPTED_DATA when the wedged key holds a DIFFERENT object
                /// than this attempt intended. Under rev.6 the mount lease makes this key exclusively ours,
                /// so this is not a possible protocol outcome -- it is the anomaly policy's incidental
                /// detection case (spec §anomaly-policy): fail closed LOUDLY (LOGICAL_ERROR, routed through
                /// `reportImpossibleInterference`) to every queued caller and KEEP the wedge, so the lane is
                /// left explicitly wedged for inspection rather than hanging every future caller.
                reportImpossibleInterference(wedge_copy->key,
                    fmt::format("ref-log wedge resolution for namespace '{}' txn {}-{} observed foreign bytes "
                        "at the wedged key ({})", ns.string(), wedge_copy->txn_id.writer_epoch,
                        wedge_copy->txn_id.ref_sequence, getCurrentExceptionMessage(/*with_stacktrace*/ false)),
                    ns.string());
                complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': impossible foreign interference observed at the "
                    "wedged key '{}' -- mount fenced closed and remount scheduled; see the anomaly "
                    "diagnostics log", ns.string(), wedge_copy->key)));
                return;
            }
            if (resolved == CasWriteOutcome::Committed)
            {
                std::lock_guard lock(rt->state_mutex);
                /// Apply BEFORE unwedging (spec: "a wedged append later observed durable is applied to
                /// cache before unwedging"). Guard against a re-entrant double-apply: only if this is
                /// still the SAME wedge (single leader per table makes a mismatch impossible, but the
                /// check costs nothing and documents the invariant).
                if (rt->wedge && rt->wedge->txn_id == wedge_copy->txn_id)
                {
                    const RefLogTxn wedged_txn = decodeRefLogTxn(wedge_copy->bytes, ns.string(), wedge_copy->txn_id);
                    applyRefLogTxn(rt->state, wedged_txn);
                    rt->wedge.reset();
                }
                ProfileEvents::increment(ProfileEvents::CasRefAppendUnwedged);
            }
            else
            {
                /// Still Unresolved: fail every CURRENTLY queued item with the SAME uncertainty
                /// exception and do not allocate a new id. A later call into this namespace's queue
                /// retries the resolve.
                complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                    "CAS ref-log append for namespace '{}' txn {}-{} is still UNCERTAIN — the append lane "
                    "stays wedged until the SAME key resolves durable or a conclusive rejection is observed",
                    ns.string(), wedge_copy->txn_id.writer_epoch, wedge_copy->txn_id.ref_sequence)));
                return;
            }
        }
    }

    /// Test-only (see `setRefPreCarveHookForTest`): a no-op in production.
    if (ref_pre_carve_hook_for_test)
        ref_pre_carve_hook_for_test();

    /// Carve a compatible batch (spec §Local Batching Queue). `lifecycle != Live` forces a solo carve:
    /// `namespace_birth` must run alone, and the flush already KNOWS the table's current lifecycle
    /// before carving (unlike a per-item property, which would need speculative undo).
    RefTableState working;
    bool table_live;
    {
        std::lock_guard lock(rt->state_mutex);
        working = rt->state;
        table_live = rt->state.lifecycle == RefLifecycle::Live;
    }

    std::vector<std::shared_ptr<RefMutationItem>> batch;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const size_t cap = table_live ? kMaxRefBatch : 1;
        std::set<String> seen_refs;
        while (!rt->pending.empty() && batch.size() < cap)
        {
            const auto & front = rt->pending.front();
            if (front->scope.kind == MutationScope::Kind::WholeShard)
            {
                if (!batch.empty())
                    break;
                batch.push_back(front);
                rt->pending.pop_front();
                break;
            }
            if (!seen_refs.insert(front->scope.ref_name).second)
            {
                ProfileEvents::increment(ProfileEvents::CasRefBatchScopeCuts);
                break;
            }
            batch.push_back(front);
            rt->pending.pop_front();
        }
    }
    if (batch.empty())
        return;   /// raced: everything was carved by a previous flush of this leader

    /// Per-item validation, in order, against `working` (per-request undo via `item_scratch`):
    /// business preconditions (thrown by `build_ops` itself) and the pre-encode admission budget both
    /// fail ONLY the offending item; survivors' ops accumulate into `final_ops` for one transaction.
    std::vector<RefOp> final_ops;
    std::vector<std::shared_ptr<RefMutationItem>> survivors;
    RefTxnId trial_id = working.greatest_applied;
    /// A never-born table's `greatest_applied` is `{0, 0}`; `applyRefLogTxn`'s strict-increase check
    /// only needs a strictly greater EPOCH to accept the first trial id (RefTxnId compares epoch
    /// first), and `admits`'s preview snapshot encoding rejects a zero epoch field regardless of
    /// sequence. `liveWriterEpoch()` is this incarnation's nonzero epoch -- the SAME source
    /// `allocateRefTxnId` stamps the real id with, so the trial preview and the persisted id never
    /// disagree on epoch; these trial ids are never persisted or compared outside this loop.
    if (trial_id.writer_epoch == 0)
        trial_id.writer_epoch = liveWriterEpoch();
    for (const auto & it : batch)
    {
        RefTableState item_scratch = working;
        try
        {
            std::vector<RefOp> item_ops = it->build_ops(working);

            /// Whole-item shape validation (prerequisite to Task 11's dropNamespace): the
            /// per-op loop below previews each op as its OWN single-op trial transaction, so a
            /// whole-transaction-shape rule like "remove_namespace must be the FINAL op" trivially
            /// passes on every singleton slice regardless of this item's REAL combined shape -- a
            /// malformed item (e.g. remove_namespace not last) would otherwise only be caught by the
            /// post-persist apply further below, AFTER its object is already durable (see that apply's
            /// own catch for why that would ALSO wedge this table's lane). Validate the item's COMPLETE
            /// ops array as ONE combined transaction, against a throwaway copy of the pre-item state,
            /// before doing any other per-op work -- exactly what the real persisted transaction will
            /// contain, using only the public two-phase `applyRefLogTxn` entry point (no need to reach
            /// into the state machine's private per-op helpers).
            if (!item_ops.empty())
            {
                RefTableState shape_check = working;
                RefTxnId shape_probe_id = trial_id;
                shape_probe_id.ref_sequence += 1;
                applyRefLogTxn(shape_check, RefLogTxn{ns.string(), shape_probe_id, item_ops});
            }

            for (const RefOp & op : item_ops)
            {
                /// Admission budget (spec §Snapshot Format): only STATE-GROWING ops need the check --
                /// an `owner_transition` installing a binding (add or promote) and `set_payload`.
                /// `namespace_birth` is exempt (it grows nothing, and a never-born state's preview has
                /// no meaningful "current snapshot" to encode); `remove_namespace` and a pure
                /// owner_transition removal shrink state and can never violate the budget.
                const bool state_growing = (op.kind == RefOpKind::OwnerTransition && op.new_binding.has_value())
                    || op.kind == RefOpKind::SetPayload;
                if (state_growing && !admits(item_scratch, op, rt->snapshot_budget, rt->removal_budget))
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' would exceed the table's admission budget "
                        "(snapshot_budget={} removal_budget={}) — refusing before any object is created",
                        ns.string(), rt->snapshot_budget, rt->removal_budget);
                /// Apply THIS op to item_scratch now (a single-op trial transaction) so a LATER op of
                /// the SAME item (e.g. namespace_birth immediately followed by its first
                /// owner_transition) is validated -- both here and by admits's own preview -- against
                /// a state that already reflects it, exactly as the real combined transaction will.
                trial_id.ref_sequence += 1;
                applyRefLogTxn(item_scratch, RefLogTxn{ns.string(), trial_id, {op}});
            }
            working = std::move(item_scratch);
            final_ops.insert(final_ops.end(), item_ops.begin(), item_ops.end());
            survivors.push_back(it);
        }
        catch (...)
        {
            complete_error({it}, std::current_exception());
        }
    }
    if (final_ops.empty())
    {
        /// Either every item failed validation (already completed via complete_error above, nothing
        /// left to do), or every survivor's own `build_ops` legitimately contributed ZERO ops (an
        /// idempotent no-op, e.g. precommitAdd/promote re-targeting a manifest already exactly
        /// committed). Survivors of the latter kind still need marking done -- with no new object
        /// created, `committed_id` is simply the table's current (unchanged) high-water mark.
        if (!survivors.empty())
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            for (const auto & it : survivors)
            {
                it->committed_id = working.greatest_applied;
                it->done = true;
            }
            rt->cv.notify_all();
        }
        return;
    }

    /// Self-remount re-check BEFORE allocating an id (spec §Startup And Recovery): the top-of-flush gate
    /// is passed once, but a leader can stall between it and here -- in `build_ops`' caller I/O -- across
    /// the whole fence-loss + remount window, then resume after `armMountFence`. Allocating {new_epoch,
    /// seq} now and PUTting it (its live `fence_ok` would pass) would persist a transaction validated
    /// against this orphaned runtime's STALE cache -- the C1 data-loss class. `superseded_by_remount` is
    /// published before the fence re-arm, so failing closed here (no id, no PUT, no wedge -- a safe gap,
    /// cache unchanged) keeps the durable log free of any stale-view transaction. The append `fence_ok`
    /// (which also checks the flag) is the airtight backstop for the narrow window past this point.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// Wedge hard contract (spec §anomaly-policy): at most one unresolved PUT per table, and the
    /// wedge-resolution block at the top of this flush either cleared `rt->wedge` (Committed) or already
    /// returned this whole batch closed (Unresolved / foreign interference) -- reaching HERE with a
    /// wedge STILL set is provably unreachable via any legitimate control flow. `chassert` catches it
    /// immediately in a debug build; the release-mode guard below refuses to allocate rather than trust
    /// an id minted against a lane that might still be uncertain, routing the violation into the same
    /// anomaly policy instead of an assert-only defense.
    {
        std::optional<String> wedged_key;
        {
            std::lock_guard lock(rt->state_mutex);
            chassert(!rt->wedge, "flushRefBatch: new-id allocation attempted while the ref-log lane was still wedged");
            if (rt->wedge)
                wedged_key = rt->wedge->key;
        }
        if (wedged_key)
        {
            reportImpossibleInterference(*wedged_key,
                fmt::format("flushRefBatch attempted new-id allocation for namespace '{}' while the lane "
                    "was still wedged -- the wedge hard contract was violated", ns.string()),
                ns.string());
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref-log append for namespace '{}': refusing to allocate a new ref-log id while the "
                "lane is wedged (wedge hard contract violated) -- mount fenced closed and remount scheduled",
                ns.string())));
            return;
        }
    }

    const RefTxnId id = allocateRefTxnId();
    const RefLogTxn final_txn{ns.string(), id, final_ops};
    String bytes;
    try
    {
        bytes = encodeRefLogTxn(final_txn);
    }
    catch (...)
    {
        complete_error(survivors, std::current_exception());
        return;
    }
    const String key = pool_layout.refLogKey(ns, id);

    CasWriteOutcome outcome;
    try
    {
        outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    }
    catch (...)
    {
        /// `putIfAbsentControlled` throws CORRUPTED_DATA when resolve-before-reissue observes a DIFFERENT
        /// object already at this txn's key -- a proven different-object conflict, not an unresolved PUT.
        /// Fail every survivor loudly and do NOT wedge (this is a conclusive rejection, not an uncertain
        /// outcome): the id is a safe gap, the cache is unchanged, and the lane stays usable.
        complete_error(survivors, std::current_exception());
        return;
    }
    switch (outcome)
    {
        case CasWriteOutcome::Committed:
        {
            try
            {
                std::lock_guard lock(rt->state_mutex);
                applyRefLogTxn(rt->state, final_txn);
                /// rev.6 Task 10 (spec §publish-from-live): this commit's own txn joins the
                /// applied-above-newest-snapshot tail counters -- the live `rt->state` just mutated
                /// above IS the next publish candidate's body, so there is no per-entry log to retain.
                rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                rt->tail_bytes_since_snapshot.fetch_add(bytes.size(), std::memory_order_relaxed);
            }
            catch (...)
            {
                /// Provably unreachable given the whole-item shape validation above (every item's
                /// COMPLETE ops array is already validated as one combined transaction before this
                /// point) -- but `final_txn` is now durably PUT regardless, so if this line throws
                /// anyway (a bug the pre-check did not anticipate), every future recovery would replay
                /// and re-throw on it, bricking this table forever. Fail every waiting survivor's own
                /// caller with a clear diagnostic instead of leaving them hung on an `item->done` that
                /// would otherwise never be set, then rethrow: `appendRefOps`'s own catch is the SOLE
                /// authority that resets `leader_active` on an exceptional exit from the leader loop, so
                /// this path must NOT reset it too (a double reset would open a two-leader window -- a
                /// waiter woken by the first reset could become leader before this frame unwinds).
                const String detail = getCurrentExceptionMessage(false);
                Exception rethrown(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': the durably-committed transaction {}-{} "
                    "failed to apply to the in-memory table state -- this should be provably "
                    "unreachable (every item's ops are validated as one combined transaction before any "
                    "object is created); the object is already durable and every future recovery will "
                    "hit the same failure: {}",
                    ns.string(), id.writer_epoch, id.ref_sequence, detail);
                complete_error(survivors, std::make_exception_ptr(rethrown));
                throw rethrown;
            }
            ProfileEvents::increment(ProfileEvents::CasRefBatchFlushes);
            ProfileEvents::increment(ProfileEvents::CasRefBatchedMutations, survivors.size());
            {
                std::lock_guard<std::mutex> g(ref_queue_mutex);
                for (const auto & it : survivors)
                {
                    it->committed_id = id;
                    it->done = true;
                }
                rt->cv.notify_all();
            }
            /// Task 11 (spec §writer-snapshot-publication): the threshold trigger -- off the lane,
            /// dispatched AFTER waking every waiter above so this commit's own callers are never
            /// delayed by it.
            maybeScheduleSnapshotPublish(ns, rt);
            return;
        }
        case CasWriteOutcome::DefiniteFailure:
        {
            ProfileEvents::increment(ProfileEvents::CasRefAppendDefiniteFailure);
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                "CAS ref-log append for namespace '{}' definitively failed (non-retryable rejection); "
                "cached state is unchanged and txn id {}-{} is a safe gap",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return;
        }
        case CasWriteOutcome::Unresolved:
        {
            {
                std::lock_guard lock(rt->state_mutex);
                rt->wedge = RefAppendWedge{id, key, bytes};
            }
            ProfileEvents::increment(ProfileEvents::CasRefAppendWedged);
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                "CAS ref-log append for namespace '{}' txn {}-{} is UNCERTAIN (retry budget exhausted) — "
                "the append lane is wedged until the SAME key resolves durable or a conclusive rejection "
                "is observed; this outcome is unproven, not failure",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return;
        }
    }
}

void Store::maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// Never dispatch a publisher while the fence is lost (spec §write-fence): a publish is a
    /// conditional PUT that would fail `fence_ok` and return non-Committed anyway, and dispatching one
    /// during the self-remount window is exactly the stale-cache-publish race the remount quiesce closes
    /// -- with no dispatch here, `quiesceRefTablesForRemount` only has to drain publishers already in
    /// flight before the fence dropped, never a moving target.
    if (!mayMutate())
        return;

    /// Bound the read-triggered dispatch (spec §writer-snapshot-publication, C4). The whole decision --
    /// the threshold trigger, the single-in-flight gate, the backoff deadline -- and the
    /// `pending_snapshot_publishes` increment all happen under ONE `state_mutex` hold, so two racing
    /// dispatchers can never both admit a publish for this table.
    bool dispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        const uint64_t now = bootMsNow();
        if (rt->state.lifecycle == RefLifecycle::Live
            /// Single-in-flight gate: at most one background publish per table. A dropped trigger is
            /// re-evaluated on the next trigger (post-flush / mount-time / the next read), so no snapshot
            /// is permanently skipped -- the spec promises best-effort compaction, not a staleness bound.
            && rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0
            /// Backoff deadline: after a non-Committed publish, a saturated backend is not re-dispatched
            /// on the next read until the bounded backoff elapses (the read-triggered PUT-storm latch).
            && now >= rt->publish_backoff_until_ms)
        {
            /// rev.6 Task 10 (spec §publish-from-live): the threshold trigger reads the tail counters
            /// directly -- no walk, no age filter. `tail_count_since_snapshot`/`tail_bytes_since_snapshot`
            /// count ONLY applied txns strictly above `newest_snapshot_id` (maintained incrementally by
            /// every commit and every adoption, see `flushRefBatch`/`trySnapshotPublishOnce`), so
            /// `over_threshold` here is never true without a real, immediately-coverable candidate: unlike
            /// the deleted grace-window scheme, there is no longer a decoupling between "counted" and
            /// "coverable" that a separate candidate-advance check would need to close.
            const uint64_t publishable_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
            const uint64_t publishable_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
            const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
                || publishable_bytes > config.snapshot_log_bytes_threshold;
            if (over_threshold)
            {
                rt->pending_snapshot_publishes.fetch_add(1, std::memory_order_relaxed);
                dispatch = true;
            }
        }
    }
    if (!dispatch)
        return;
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPublishDispatched);

    /// Off the mutation hot path (spec §writer-snapshot-publication): `trySnapshotPublishOnce` never
    /// touches the append queue, so dispatching it onto an unrelated global-pool thread can never
    /// deadlock a flush leader. `shared_from_this()` keeps the Store alive for the thread's lifetime.
    auto self = shared_from_this();
    try
    {
        ThreadFromGlobalPool([self, ns, rt]
        {
            try
            {
                self->trySnapshotPublishOnce(ns);
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasStore"), "CAS background snapshot publish attempt failed");
            }
            {
                std::lock_guard lock(rt->state_mutex);
                rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
            }
            rt->publish_settle_cv.notify_all();
        }).detach();
    }
    catch (...)
    {
        /// The `ThreadFromGlobalPool` ctor can throw (pool exhaustion) AFTER the
        /// count was incremented. Undo the count (else `waitForSnapshotPublishSettleForTest` hangs and the
        /// leaked pending count wedges every later settle) and SWALLOW the failure: read-path callers
        /// (`resolveRef`/`listRefs`) invoke this OUTSIDE any insulation, and dispatching a background
        /// publish is a best-effort maintenance trigger -- it must never fail an otherwise-successful read
        /// (consistent with the `CasRefSweepDeferred` read-insulation adjudication). The next trigger
        /// reschedules; a mutation caller has already committed regardless.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        }
        rt->publish_settle_cv.notify_all();
        tryLogCurrentException(getLogger("CasStore"), "CAS background snapshot-publish dispatch failed to launch");
    }
}

void Store::advancePublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. Double the interval from `initial` up to `max` per consecutive
    /// non-Committed publish outcome; arm the deadline off the boottime clock (`bootMsNow`), so an
    /// injected test clock drives it deterministically and a VM-suspend cannot shorten it.
    rt.publish_backoff_ms = rt.publish_backoff_ms == 0
        ? config.snapshot_publish_backoff_initial_ms
        : std::min<uint64_t>(rt.publish_backoff_ms * 2, config.snapshot_publish_backoff_max_ms);
    rt.publish_backoff_until_ms = bootMsNow() + rt.publish_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPublishBackoff);
}

void Store::resetPublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A durable publish clears the cooldown.
    rt.publish_backoff_ms = 0;
    rt.publish_backoff_until_ms = 0;
}

void Store::waitForSnapshotPublishSettleForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::unique_lock lock(rt->state_mutex);
    rt->publish_settle_cv.wait(lock, [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
}

int Store::pendingSnapshotPublishesForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->pending_snapshot_publishes.load(std::memory_order_relaxed);
}

std::optional<RefTxnId> Store::newestPublishedSnapshotIdForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->newest_snapshot_id;
}

size_t Store::tailSinceSnapshotCountForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
}

namespace
{
/// rev.6 Task 10 (spec §publish-from-live): a clamped-to-zero fetch-subtract for the tail counters.
/// `trySnapshotPublishOnce` is public and NOT serialized against itself (spec: "publishes are NOT
/// serialized, so two overlapping attempts can finish out of order") -- the T11 monotonic guard below
/// skips a stale (superseded) adoption's subtraction outright, but it cannot see a SMALLER-candidate
/// attempt that lands its adoption BEFORE a larger-candidate one already in flight: that ordering would
/// have the larger attempt's `captured_count`/`captured_bytes` double-count the smaller one's
/// already-subtracted region. A plain `fetch_sub` would then underflow the unsigned counter, wrapping it
/// to near `UINT64_MAX` and permanently re-latching the C4 storm trigger on every subsequent read -- a
/// release-build regression of the exact bug this rev fixes. Clamping to zero instead settles for a
/// benign, self-healing under-count (a delayed next dispatch; the NEXT publish always captures the true
/// live state fresh, so snapshot CONTENT is never affected) over an unsafe wraparound.
void clampedCounterSub(std::atomic<uint64_t> & counter, uint64_t amount)
{
    uint64_t old_value = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(old_value, old_value > amount ? old_value - amount : 0,
        std::memory_order_relaxed))
    {
    }
}
}

bool Store::trySnapshotPublishOnce(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);

    /// rev.6 Task 10 (spec §publish-from-live): ONE copy of the live state, at a txn boundary -- no
    /// replay, no per-entry retention. The tail counters are captured in the SAME critical section so
    /// adoption below subtracts exactly what this attempt's candidate actually covers.
    RefTableState candidate_state;
    RefTxnId candidate_x;
    uint64_t captured_count = 0;
    uint64_t captured_bytes = 0;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Live)
            return false;   /// nothing to (re)publish here; dropNamespace publishes its own Removed snapshot
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < rt->state.greatest_applied))
            return false;   /// nothing above the newest snapshot
        candidate_state = rt->state;
        candidate_x = rt->state.greatest_applied;
        captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
        captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
    }

    const RefTableSnapshot snap = snapshotOf(candidate_state, ns.string());
    String bytes;
    try
    {
        bytes = encodeRefTableSnapshot(snap);
    }
    catch (...)
    {
        /// Failure Handling: "Snapshot create fails: keep all logs; writer recovery remains unchanged."
        /// Treat like any other non-Committed outcome: arm the backoff so a persistent encode failure
        /// does not re-dispatch on every read (spec §writer-snapshot-publication).
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    const String key = pool_layout.refSnapshotKey(ns, candidate_x);
    const auto fence_ok = [this] { return refAppendFenceOk(); };
    const CasWriteOutcome outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
    {
        /// DefiniteFailure/Unresolved: DO NOT prune (no durable covering snapshot -- pruning the tail
        /// without one is data loss). Arm the bounded per-table backoff so the read path does not
        /// re-dispatch this full-snapshot encode+PUT until it elapses -- the read-triggered PUT-storm
        /// latch breaker (spec §writer-snapshot-publication). A later trigger past the deadline retries.
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPutBytes, bytes.size());   /// spec §writer-snapshot-publication

    {
        std::lock_guard lock(rt->state_mutex);
        /// A durable publish clears any backoff: progress was made this attempt (even if the T11
        /// monotonic guard below skips the in-memory adoption because a newer snapshot already won).
        resetPublishBackoff(*rt);
        /// Monotonic adoption guard (CRITICAL): publishes are NOT serialized, so two
        /// overlapping attempts can finish out of order (this OLDER-candidate attempt landing its PUT
        /// after a NEWER one already adopted). Adopting the older `candidate_x` here would REGRESS
        /// `newest_snapshot_id` below what a newer attempt already advanced it to -- the next published
        /// snapshot would then omit committed transactions and recovery would lose refs. Skip the
        /// in-memory adoption (and the counter subtraction below) whenever a newer-or-equal snapshot is
        /// already adopted; the already-durable `_snap/<candidate_x>` object is harmless (readers pick
        /// the greatest snapshot, GC reclaims covered ones). This also keeps the Removed path monotonic:
        /// a stale Live attempt can never drag `newest_snapshot_id` back below a `remove_txn_id` that
        /// `publishRemovedSnapshotNow` already adopted (`remove_txn_id` is allocated after every Live
        /// txn, so it is always the greatest and this guard trips).
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < candidate_x))
            return true;
        /// rev.6 Task 10 (spec §publish-from-live): subtract exactly the counters captured at copy time
        /// -- more appends (or even another publish's own commits) may have landed on the LIVE counters
        /// since, and only those should remain uncovered. Clamped (see `clampedCounterSub`): an
        /// out-of-order adoption ordering the guard above does not catch (a SMALLER candidate that
        /// adopts before a LARGER one already in flight) could otherwise subtract an already-subtracted
        /// region and underflow the unsigned counter.
        clampedCounterSub(rt->tail_count_since_snapshot, captured_count);
        clampedCounterSub(rt->tail_bytes_since_snapshot, captured_bytes);
        /// logs-per-table-after-snapshot (spec §implementation-impact): the tail this publish compacted.
        ProfileEvents::increment(ProfileEvents::CasRefSnapshotTailLogs, captured_count);
        rt->newest_snapshot_id = candidate_x;
        /// Cache-weight base (spec §Byte, Memory, And CPU Budget): the new base is exactly the snapshot
        /// we just encoded and PUT, so its body size is the fresh base weight -- no re-encode needed.
        rt->base_snapshot_bytes.store(bytes.size(), std::memory_order_relaxed);
    }
    return true;
}

void Store::sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// A read-only caller (resolveRef/listRefs) must not fail its OWN
    /// otherwise-successful read because a piggybacked maintenance action (the stale-precommit sweep)
    /// hit an uncertain PUT -- the read asked for none of that; a mutation path (appendRefOps's own
    /// top-level hoisted call, which calls `maybeSweepStalePrecommits` directly, uncaught) keeps
    /// propagating instead, since it must not proceed past a wedged lane anyway. Swallowing here does
    /// NOT drop the sweep (S13 fix, triage `.superpowers/sdd/s13-triage-report.md`): the failed
    /// attempt already re-armed `needs_stale_precommit_sweep` (with a bounded cooldown) inside
    /// `maybeSweepStalePrecommits`, so a later read/mutation trigger on THIS mount retries until a
    /// sweep completes verified clean -- the old drop-the-shot behavior left a dead incarnation's
    /// precommit bindings (and the manifests they protect from the GC orphan sweep) live forever on a
    /// long-lived mount whenever the single attempt burned in the post-restart error window.
    try
    {
        maybeSweepStalePrecommits(ns, rt);
    }
    catch (...)
    {
        ProfileEvents::increment(ProfileEvents::CasRefSweepDeferred);
        tryLogCurrentException(getLogger("CasStore"),
            "CAS stale-precommit sweep deferred for namespace '" + ns.string()
                + "' (a read-only caller observed the failure and is proceeding with its own read)");
    }
}

void Store::maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    {
        std::lock_guard lock(rt->state_mutex);
        if (!rt->needs_stale_precommit_sweep)
            return;
        /// S13 fix: a failed attempt armed a cooldown; do not re-attempt (and do not touch the flag)
        /// until it elapses -- the bounded-backoff storm latch, same shape as `publish_backoff_until_ms`.
        /// The boottime clock is injectable (`boot_ms_fn`), so tests drive this deterministically.
        if (bootMsNow() < rt->precommit_sweep_backoff_until_ms)
            return;
        /// Cleared FIRST: `sweepStalePrecommitsNow`'s own `appendRefOps` calls re-enter this same
        /// top-level check (via `appendRefOps`'s hoisted call), and must see it already cleared. This
        /// clear is for RE-ENTRANCY only, never consumption: any non-clean outcome re-arms below.
        rt->needs_stale_precommit_sweep = false;
    }
    try
    {
        sweepStalePrecommitsNow(ns, rt);
    }
    catch (...)
    {
        /// S13 fix (triage `.superpowers/sdd/s13-triage-report.md`, run 20260713T172032_S13_seed42): a
        /// failed or partial sweep must NOT consume the shot. Under kill-chaos the single attempt lands
        /// exactly inside the post-restart error window (an uncertain PUT, a fence blip), and with no
        /// retry the dead incarnation's durable precommit bindings -- and the manifests
        /// `activeManifestKeys` protects for them -- leaked forever on a long-lived mount (GC has no
        /// backstop by spec §Responsibility Boundary). Re-arm with a bounded backoff and rethrow: the
        /// read path insulates the caller (`sweepStalePrecommitsForRead`), the mutation path propagates
        /// as before.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->needs_stale_precommit_sweep = true;
            advancePrecommitSweepBackoff(*rt);
        }
        throw;
    }
    /// Verified clean: `sweepStalePrecommitsNow` returns only after a full pass over the live state
    /// found zero stale bindings, so the flag stays cleared for the rest of this mount; reset the
    /// failure cooldown too.
    std::lock_guard lock(rt->state_mutex);
    resetPrecommitSweepBackoff(*rt);
}

void Store::advancePrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. S13 fix, mirroring `advancePublishBackoff`: double the interval
    /// from `initial` up to `max` per consecutive failed sweep attempt; arm the deadline off the
    /// boottime clock (`bootMsNow`), so an injected test clock drives it deterministically and a
    /// VM-suspend cannot shorten it.
    rt.precommit_sweep_backoff_ms = rt.precommit_sweep_backoff_ms == 0
        ? config.precommit_sweep_backoff_initial_ms
        : std::min<uint64_t>(rt.precommit_sweep_backoff_ms * 2, config.precommit_sweep_backoff_max_ms);
    rt.precommit_sweep_backoff_until_ms = bootMsNow() + rt.precommit_sweep_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CasRefSweepRearmed);
}

void Store::resetPrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A verified-clean sweep clears the cooldown.
    rt.precommit_sweep_backoff_ms = 0;
    rt.precommit_sweep_backoff_until_ms = 0;
}

void Store::sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// Task 11 (spec §Clean Up Old Precommits): after a fresh mount fence and recovery, this writer
    /// knows the exact stale precommit bindings -- their `manifest_ref.writer_epoch` predates this
    /// incarnation's live writer_epoch, i.e. they belong to a build from a superseded incarnation that
    /// can never be promoted. Removed with ordinary exact `owner_transition(old_binding, none)`
    /// operations, chunked to `ref_txn_max_ops` per transaction. Interruption is harmless: each chunk
    /// re-reads the LIVE state, so a partial sweep just leaves fewer stale bindings for the next chunk
    /// (a later retry on this mount, or the next mount's recovery) to find; nothing here can loop
    /// forever since only OLDER-epoch bindings ever qualify, and this writer's own new work always uses
    /// `liveWriterEpoch()` -- which a self-remount bumps in lockstep with the threshold below, so a
    /// remount's fresh precommits survive.
    ///
    /// A GC-side backstop stays deliberately OUT of this fix (S13, triage
    /// `.superpowers/sdd/s13-triage-report.md`): the spec's §Responsibility Boundary assigns
    /// precommit-binding cleanup to the WRITER -- GC never mutates another writer's ref-table state, so
    /// a leader-side reclaim would be a new protocol capability (a spec question about GC-authored
    /// ref-log transactions and their fencing), not a bugfix. The retry-until-clean loop above is the
    /// writer-side answer; the follow-up (a GC visibility counter for "live precommit binding below the
    /// mount-lease epoch", possibly a real backstop behind a spec revision) is tracked in
    /// `utils/ca-soak/scenarios/BACKLOG.md` entry S13-20260713T172032-3.
    const uint64_t live_epoch = liveWriterEpoch();
    while (true)
    {
        std::vector<std::pair<String, ManifestRef>> chunk;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : rt->state.precommits)
            {
                if (mref.writer_epoch >= live_epoch)
                    continue;
                chunk.emplace_back(ref_name, mref);
                if (chunk.size() >= ref_txn_max_ops)
                    break;
            }
        }
        if (chunk.empty())
            return;

        appendRefOps(ns, MutationScope::wholeShard(),
            [chunk](const RefTableState & state) -> std::vector<RefOp>
            {
                std::vector<RefOp> ops;
                for (const auto & [ref_name, mref] : chunk)
                    if (state.precommits.contains({ref_name, mref}))
                    {
                        RefOp op;
                        op.kind = RefOpKind::OwnerTransition;
                        op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                        ops.push_back(op);
                    }
                return ops;
            },
            RootMutationOrigin::Writer, RootMutationKind::ReclaimPrecommit);

        /// S13 fix / INTROSPECTION-1: audit each binding this sweep reclaimed, so
        /// system.content_addressed_log records the reclaim and the S13 card's "abandoned precommits
        /// reclaimed" counter is falsifiable (it had ZERO emit sites before). A binding gathered above
        /// that is GONE from the live state after the committed append was reclaimed by this sweep's
        /// work -- either this chunk's own ops or this lane's just-resolved wedged predecessor txn (a
        /// PRIOR attempt of this same sweep whose ack was lost); one still present was skipped by the
        /// builder (raced by another owner transition) and will be gathered again next iteration.
        /// Collected under the lock, emitted outside it (the sink forwards to the SystemLog).
        std::vector<std::pair<String, ManifestRef>> reclaimed;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : chunk)
                if (!rt->state.precommits.contains({ref_name, mref}))
                    reclaimed.emplace_back(ref_name, mref);
        }
        ProfileEvents::increment(ProfileEvents::CasRefStalePrecommitsReclaimed, reclaimed.size());
        for (const auto & [ref_name, mref] : reclaimed)
        {
            EventEmitter{*this}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::PrecommitReclaim;
                e.namespace_ = ns.string();
                e.ref_name = ref_name;
                e.object_kind = CasEventObjectKind::Root;
                e.object_hash = manifestRefDebugString(mref);
                e.reason = "stale-precommit sweep: dangling precommit of a superseded writer incarnation "
                           "reclaimed by the successor's fenced sweep (spec §Clean Up Old Precommits)";
                e.detail = {{"stale_writer_epoch", std::to_string(mref.writer_epoch)},
                            {"live_writer_epoch", std::to_string(live_epoch)}};
            });
        }
    }
}

void Store::publishRemovedSnapshotNow(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    RefTxnId remove_id;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Removed || !rt->state.remove_txn_id)
            return;
        remove_id = *rt->state.remove_txn_id;
        if (rt->newest_snapshot_id && *rt->newest_snapshot_id == remove_id)
            return;   /// already published this exact Removed snapshot
    }

    RefTableSnapshot removed_snap;
    removed_snap.ns = ns.string();
    removed_snap.snapshot_id = remove_id;
    removed_snap.lifecycle = RefLifecycle::Removed;
    removed_snap.remove_txn_id = remove_id;
    const String bytes = encodeRefTableSnapshot(removed_snap);
    const String key = pool_layout.refSnapshotKey(ns, remove_id);
    const auto fence_ok = [this] { return refAppendFenceOk(); };
    const CasWriteOutcome outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
        return;   /// best-effort; Task 12's namespace-cleanup item republishes idempotently later

    std::lock_guard lock(rt->state_mutex);
    rt->newest_snapshot_id = remove_id;
    rt->tail_count_since_snapshot.store(0, std::memory_order_relaxed);
    rt->tail_bytes_since_snapshot.store(0, std::memory_order_relaxed);
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
    /// Task 10 (spec §Remove Committed Ref): one `owner_transition` removal ref-log transaction. The
    /// exact committed binding must exist; `build_ops` reads it off the CURRENT batch-validation state,
    /// so a concurrently-co-batched publish/drop of a DIFFERENT ref sees a consistent view.
    ManifestRef dropped_ref;
    const RefTxnId txn_id = appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.committed.find(ref_name);
            if (it == state.committed.end())
                /// Fail-closed (no silent no-op): this item's own exception, the batch survives.
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "dropRef: no such ref {} in namespace {}", ref_name, ns.string());

            dropped_ref = it->second.manifest_ref;
            RefOp op;
            op.kind = RefOpKind::OwnerTransition;
            op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, dropped_ref};
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::Drop);

    /// B170: the ref was dropped (a removal op GC folds as a true removal, Task 12). object_hash is the
    /// manifest the ref named, so a part's "publish -> drop" life is reconstructable from the rows.
    if (hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::RefDrop;
        _ev3.namespace_ = ns.string();
        _ev3.ref_name = ref_name;
        _ev3.object_kind = CasEventObjectKind::Manifest;
        _ev3.object_hash = manifestRefDebugString(dropped_ref);
        _ev3.at_version = txn_id.ref_sequence;
        _ev3.outcome = "ok";
        _ev3.reason = "dropRef: appended an owner_transition removal ref-log transaction";
        emitEvent(_ev3);
    }
}

void Store::updateRefPayload(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RefMutableFilesUpdate &)> mutator)
{
    /// Task 10 (spec §Update Payload): one `set_payload` ref-log transaction. EVERY change (even
    /// payload-only) is an explicit logged operation -- the immutable append-only log has no other way
    /// to record it (spec: "The operation replaces the complete mutable payload and does not change the
    /// manifest edge").
    appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.committed.find(ref_name);
            if (it == state.committed.end())
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "updateRefPayload: no such ref {} in namespace {}", ref_name, ns.string());

            /// The mutator edits only the mutable payload; the carrier deliberately carries no
            /// `manifest_ref`, so a reachability change is structurally impossible here (it goes through
            /// publish/drop instead). The persisted encoding is one opaque `payload` blob.
            RefMutableFilesUpdate update;
            update.mutable_files = decodeMutableFilesPayload(it->second.payload);
            update.published_at_ms = it->second.published_at_ms;

            mutator(update);

            RefOp op;
            op.kind = RefOpKind::SetPayload;
            op.ref_name = ref_name;
            op.expected_manifest_ref = it->second.manifest_ref;
            op.payload = encodeMutableFilesPayload(update.mutable_files);
            op.published_at_ms = update.published_at_ms;
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::UpdateRefPayload);
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

bool Store::mountpointObjectExists(const String & key)
{
    /// HEAD (metadata), not a body GET: the probed path may resolve to a DIRECTORY (e.g. the `store`
    /// pool sub-dir traversed by system.remote_data_paths). The backend's metadata path treats a
    /// directory as not-an-object (B38), so this returns false instead of a body read throwing EISDIR.
    return pool_backend->head(pool_layout.mountpointObjectKey(key)).exists;
}

void Store::removeMountpointObject(const String & key)
{
    casRemoveObject(pool_layout.mountpointObjectKey(key));
}

bool Store::namespaceIsRemoved(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard<std::mutex> lock(rt->state_mutex);
    /// A genuinely-removed namespace is `Removed` WITH a `remove_txn_id` (RemoveNamespace sets both). The
    /// never-born default is also `Removed` but carries no `remove_txn_id`, and a recreated one is `Live`
    /// (NamespaceBirth resets the marker) — so `remove_txn_id.has_value()` is the exact born-then-removed
    /// discriminator, matching promote's own `state.remove_txn_id` recreation guard.
    return rt->state.lifecycle != RefLifecycle::Live && rt->state.remove_txn_id.has_value();
}

void Store::dropNamespace(const RootNamespace & ns)
{
    /// Task 11 (spec §Namespace Removal): one body transaction naming an exact `owner_transition`
    /// removal for every committed ref and precommit, followed by `remove_namespace` -- the removal
    /// class shares the bigger complete-table byte budget (encodeRefLogTxn's own `checkBudget`, keyed
    /// off the presence of a `RemoveNamespace` op) and is exempt from the ordinary per-op admission
    /// check (it only ever shrinks state; see `flushRefBatch`'s `state_growing` filter).
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    {
        /// spec: "Repeated API removal observes the cached Removed state and returns success without
        /// appending a second transaction." `RefTableState::lifecycle` cannot distinguish "genuinely
        /// removed" from "never born" (both default to `Removed` -- see the representation note on
        /// `RefTableState`), so a never-touched namespace's drop is ALSO a harmless no-op here, which
        /// is the correct behavior either way (nothing to remove).
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Live)
            return;
    }

    appendRefOps(ns, MutationScope::wholeShard(),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            if (state.lifecycle != RefLifecycle::Live)
                return {};   /// raced: another caller already removed it since our check above

            std::vector<RefOp> ops;
            for (const auto & [ref_name, row] : state.committed)
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, row.manifest_ref};
                ops.push_back(op);
            }
            for (const auto & [ref_name, mref] : state.precommits)
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                ops.push_back(op);
            }
            RefOp remove;
            remove.kind = RefOpKind::RemoveNamespace;
            ops.push_back(remove);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::DropNamespace);

    /// spec §Namespace Removal (line 666): "After the transaction is durable, it applies the same
    /// operations to memory, cancels local builds, and rejects further ordinary mutations." Reaching here
    /// means the removal is durable (this call's, or a concurrent caller's whose durable result the append
    /// lane observed) -- a FAILED append would have thrown above, so cancellation is only ever reached
    /// after durability (spec 667-668: a failed append leaves the namespace Live and propagates). Cancel
    /// every in-flight build TARGETING this namespace so its next op fails closed (`requireAlive`),
    /// preventing it from promoting/precommitting a fresh owner into (or staging more debris in) the
    /// just-removed namespace. The append lane is the real linearization authority (an `owner_transition`
    /// on a non-Live namespace is rejected by the state machine regardless); this stops wasted work early
    /// and surfaces a clear error. Builds in OTHER namespaces self-filter (no-op). Collect the live
    /// shared_ptrs under the lock, cancel OUTSIDE it (`cancelForNamespaceRemoval` only stores an atomic).
    std::vector<BuildPtr> builds_to_check;
    {
        std::lock_guard lk(builds_mutex);
        for (const auto & entry : inflight_builds)
            if (auto build = entry.second.lock())
                builds_to_check.push_back(std::move(build));
    }
    for (const auto & build : builds_to_check)
        build->cancelForNamespaceRemoval(ns);

    /// spec §Namespace Removal: "After the removal transaction is durable, the writer also publishes
    /// the constant-size Removed snapshot"; best-effort here (the removal itself already succeeded) --
    /// Task 12's namespace-cleanup item republishes it idempotently if this writer stops first.
    try
    {
        publishRemovedSnapshotNow(ns);
    }
    catch (...)
    {
        tryLogCurrentException(getLogger("CasStore"), "CAS dropNamespace: publishing the Removed snapshot failed (best-effort)");
    }

    /// The writer performs NO physical deletion of ref-log/snapshot objects or verbatim namespace files
    /// -- GC's namespace-cleanup item ({namespace, remove_txn_id}, Pending->Completed) owns that reclaim,
    /// keyed off the durable `remove_namespace` this call just appended (spec §Clean Old Ref Objects).
    /// Until GC reclaims it, a dropped namespace's ref-log objects and verbatim files remain as debris.
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

    /// `cas/refs/`: ref-object keys are `<pool_prefix>/cas/refs/<ns>/_log|_snap|_cleanup/<txn-id>`.
    /// `parseRefObjectKey` recognizes them and yields the owning namespace; any key it does not
    /// recognize is skipped (it is not one of this pool's ref objects).
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
                if (const auto parsed = pool_layout.parseRefObjectKey(key))
                {
                    if (parsed->ns.string().starts_with(prefix))
                        found.insert(parsed->ns.string());
                }
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
    /// Phase 1: a namespace's presence is split across two physical subtrees — its ref-log/snapshot
    /// objects live under `cas/refs/<ns>/_log|_snap|_cleanup/…` while its verbatim files and PLAIN
    /// mountpoint objects stay under `roots/<ns>/_files/…` / `roots/<key>`. The browse therefore
    /// UNIONs the next-segment names from BOTH subtrees so a namespace discoverable only by its ref
    /// objects (the common case — mutable per-part files ride inside the ref payload, not as
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
