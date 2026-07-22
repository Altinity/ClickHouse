#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include <Common/thread_local_rng.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int INVALID_STATE;
    extern const int LOGICAL_ERROR;
}
}

namespace ProfileEvents
{
    extern const Event CasIdentityLost;
    extern const Event CasDataRootVanished;
}

namespace DB::Cas
{

CasMountRuntime::CasMountRuntime(
    BackendPtr backend_ptr_,
    const Layout & layout_,
    MountConfig config_,
    String server_root_id_,
    const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_,
    std::function<bool()> remount_attempt_)
    : backend_ptr(std::move(backend_ptr_))
    , layout(layout_)
    , config(std::move(config_))
    , server_root_id(std::move(server_root_id_))
    , event_sink(event_sink_)
    , cas_request_budget(cas_request_budget_)
    , remount_attempt(std::move(remount_attempt_))
{
}

uint64_t CasMountRuntime::bootMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

uint64_t CasMountRuntime::bootMsNow() const
{
    return config.boot_ms_fn ? config.boot_ms_fn() : bootMs();
}

void CasMountRuntime::waitSleep(uint64_t ms) const
{
    if (config.wait_sleep_fn)
        config.wait_sleep_fn(ms);
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool CasMountRuntime::mayMutate() const
{
    return !mount_fence.lost.load(std::memory_order_acquire)
        && bootMsNow() < mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
}

void CasMountRuntime::tripMountLost()
{
    mount_fence.lost.store(true, std::memory_order_release);
    /// A durable-effect caller admitted under the incarnation this trip just ended must never conclude
    /// the fence is fine again just because a LATER `armMountFence` happens to re-arm it (rev.7 [C2]).
    fence_generation.fetch_add(1, std::memory_order_acq_rel);
    /// The lease-loss event is exactly the `Live -> TransientNotLive` transition of the §1 state model.
    /// Idempotent and terminal-safe (a compare-exchange from `Live` only).
    noteLeaseLost();
}

void CasMountRuntime::checkFenceOrThrow(uint64_t admitted_generation) const
{
    if (!mayMutate() || fenceGeneration() != admitted_generation)
        throw Exception(ErrorCodes::INVALID_STATE,
            "mount lease not held — backing may be temporarily unreachable");
}

bool CasMountRuntime::refAppendFenceOk() const
{
    /// `mayMutate` checks the latch and deadline. The additional budget check prevents starting a
    /// controlled request that cannot plausibly finish, including its safety margin, before expiry.
    if (mount_fence.lost.load(std::memory_order_acquire))
        return false;
    const uint64_t now = bootMsNow();
    const uint64_t deadline = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
    if (now >= deadline)
        return false;
    const uint64_t margin = cas_request_budget.attempt_timeout_ms + cas_request_budget.lease_safety_margin_ms;
    return margin < deadline - now;
}

void CasMountRuntime::setMountDeadline(uint64_t deadline_boot_ms)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
}

void CasMountRuntime::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
    mount_fence.lost.store(false, std::memory_order_release);
    /// A fresh lease incarnation is a fresh generation too: a durable-effect caller admitted under the
    /// PRIOR incarnation must re-check and abort rather than ride this re-arm through (rev.7 [C2]).
    fence_generation.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t CasMountRuntime::minActive()
{
    std::lock_guard lk(builds_mutex);
    return active_build_seqs.empty() ? next_build_seq : *active_build_seqs.begin();
}

uint64_t CasMountRuntime::peekNextBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    return next_build_seq;
}

void CasMountRuntime::renewWatermarkOnce()
{
    /// A read-only runtime has no heartbeat to renew. Report that misuse instead of fabricating a keeper
    /// or silently treating the call as successful.
    if (!mount_keeper)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renewWatermarkOnce on a read-only Pool");
    mount_keeper->renewOnce();
}

uint64_t CasMountRuntime::allocateBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    const uint64_t s = next_build_seq++;
    active_build_seqs.insert(s);
    return s;
}

void CasMountRuntime::registerInflightBuild(uint64_t seq, const PartWriteTxnPtr & build)
{
    /// The caller owns the build's shared pointer. Keep only a weak reference here so the registry does
    /// not extend the build lifetime; publication, abandonment, or destruction removes the entry.
    std::lock_guard lk(builds_mutex);
    inflight_builds[seq] = build;
}

void CasMountRuntime::retireBuildSeq(uint64_t seq)
{
    std::lock_guard lk(builds_mutex);
    active_build_seqs.erase(seq);
    inflight_builds.erase(seq);
}

void CasMountRuntime::cancelInflightBuildsForNamespace(const RootNamespace & ns)
{
    /// The removal callback is invoked only after the namespace-removal transaction is durable. Keep
    /// cancellation outside `builds_mutex`; `cancelForNamespaceRemoval` changes the build's atomic
    /// cancellation state and does not require the registry lock.
    std::vector<PartWriteTxnPtr> builds_to_check;
    {
        std::lock_guard lk(builds_mutex);
        for (const auto & entry : inflight_builds)
            if (auto build = entry.second.lock())
                builds_to_check.push_back(std::move(build));
    }
    for (const auto & build : builds_to_check)
        build->cancelForNamespaceRemoval(ns);
}

void CasMountRuntime::mintRandomProcessEpoch()
{
    /// Mint a nonzero equality-only identity. Keep it away from the zero/unarmed and UINT64_MAX/retired
    /// sentinels; 52 random bits are sufficient for the expected collision risk of this token.
    constexpr uint64_t EPOCH_MASK = (1ULL << 52) - 1;
    process_epoch.store(
        (thread_local_rng() ^ (static_cast<uint64_t>(thread_local_rng()) << 32)) & EPOCH_MASK,
        std::memory_order_relaxed);
    if (process_epoch.load(std::memory_order_relaxed) == 0)
        process_epoch.store(1, std::memory_order_relaxed);
}

void CasMountRuntime::setProcessEpoch(uint64_t v, std::memory_order order)
{
    process_epoch.store(v, order);
}

void CasMountRuntime::setLiveWriterEpoch(uint64_t v)
{
    live_writer_epoch.store(v, std::memory_order_release);
}

void CasMountRuntime::installKeeper(UInt128 our_uuid, uint64_t writer_epoch, const std::function<uint64_t()> & now_ms)
{
    /// The mount object already contains this runtime's live `(uuid, epoch)` body. Construct the keeper
    /// to adopt that exact slot rather than triggering its double-start guard. The keeper reads the
    /// build-watermark floor through `minActive` while preparing each renewal.
    const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
    mount_keeper = std::make_unique<MountLeaseKeeper>(
        backend_ptr, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); },
        std::chrono::milliseconds(cas_request_budget.lease_safety_margin_ms));
    /// Install the fence callbacks before any background renewal can run: successful renewals extend the
    /// local BOOTTIME deadline, while a superseded or foreign renewal latches the fence and starts recovery.
    mount_keeper->setFenceCallbacks(
        [this, ttl_ms] { setMountDeadline(bootMsNow() + ttl_ms); },
        [this]
        {
            tripMountLost();
            /// Recover as a fresh incarnation; a fenced `(uuid, writer_epoch)` pair is never resurrected.
            scheduleRemount();
        });
}

void CasMountRuntime::keeperStart()
{
    mount_keeper->start();
}

void CasMountRuntime::keeperReset()
{
    mount_keeper.reset();
}

void CasMountRuntime::keeperStartBackground(std::chrono::milliseconds period)
{
    mount_keeper->startBackground(period);
}

void CasMountRuntime::keeperStopBackground()
{
    mount_keeper->stopBackground();
}

void CasMountRuntime::setUncleanEpochBoundarySeenAt(uint64_t v)
{
    unclean_epoch_boundary_seen_at.store(v, std::memory_order_relaxed);
}

bool CasMountRuntime::isVanished() const
{
    const PoolLifecycle s = lifecycle();
    return s == PoolLifecycle::VanishedErased
        || s == PoolLifecycle::VanishedReplaced
        || s == PoolLifecycle::VanishedForgotten;
}

void CasMountRuntime::noteLeaseLost()
{
    /// `Live -> TransientNotLive`, and nothing else. A compare-exchange FROM `Live` leaves every other
    /// state untouched, so a terminal state is never downgraded and a repeated call is a no-op. This is
    /// the only transition the keeper thread performs, and it needs no lock because of that discipline.
    PoolLifecycle expected = PoolLifecycle::Live;
    pool_lifecycle.compare_exchange_strong(
        expected, PoolLifecycle::TransientNotLive, std::memory_order_acq_rel, std::memory_order_acquire);
}

void CasMountRuntime::noteRemounted()
{
    /// `TransientNotLive -> Live` on a successful reclaim. A compare-exchange FROM `TransientNotLive`
    /// never revives `IdentityLost` or a `Vanished` state ([D3]) and is a no-op if already `Live`.
    PoolLifecycle expected = PoolLifecycle::TransientNotLive;
    pool_lifecycle.compare_exchange_strong(
        expected, PoolLifecycle::Live, std::memory_order_acq_rel, std::memory_order_acquire);
}

void CasMountRuntime::enterIdentityLost()
{
    /// `TransientNotLive -> IdentityLost`, one way. The compare-exchange FROM `TransientNotLive` gives
    /// the brief's "from TransientNotLive only" precondition, idempotency (a second call finds the state
    /// already `IdentityLost` and its exchange fails), and safety against a concurrent keeper
    /// `noteLeaseLost` (which only ever moves `Live -> TransientNotLive`, never away from it). Crucially,
    /// this does NOT set `vanished_intent`: `IdentityLost` is non-absorbing ([C1]) — the lifecycle
    /// observer must keep running so a progressive erase can still complete into `VanishedErased`.
    PoolLifecycle expected = PoolLifecycle::TransientNotLive;
    if (!pool_lifecycle.compare_exchange_strong(
            expected, PoolLifecycle::IdentityLost, std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    ProfileEvents::increment(ProfileEvents::CasIdentityLost);
    LOG_WARNING(getLogger("CasPool"),
        "Content-addressed pool '{}' entered IdentityLost: the pool sentinels (_pool_meta and the owner "
        "anchor) are authoritatively absent while the pool prefix still holds objects (a live erase in "
        "progress). Store-class access now fails loud; a low-rate, non-mutating observer keeps probing. "
        "Recover by restart or SYSTEM CONTENT ADDRESSED FORGET — a matching-sentinel restore does NOT "
        "auto-revive this disk.",
        server_root_id);
}

void CasMountRuntime::enterVanished(PoolLifecycle which, const String & reason)
{
    /// Validate the target BEFORE mutating any state — `enterVanished` takes only the three terminal
    /// values; fail loud on a call-site bug rather than store a non-terminal value or mislabel it.
    const char * label = nullptr;
    switch (which)
    {
        case PoolLifecycle::VanishedErased:    label = "erased"; break;
        case PoolLifecycle::VanishedReplaced:  label = "replaced"; break;
        case PoolLifecycle::VanishedForgotten: label = "forgotten"; break;
        case PoolLifecycle::Live:
        case PoolLifecycle::TransientNotLive:
        case PoolLifecycle::IdentityLost:
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CasMountRuntime::enterVanished called with a non-terminal lifecycle value");
    }

    /// Publish the terminal-intent latch FIRST (spec §3). Its exchange doubles as the idempotency guard:
    /// the FIRST terminal transition wins, and a second call returns without re-storing or re-logging.
    if (vanished_intent.exchange(true, std::memory_order_acq_rel))
        return;

    /// An unconditional store is safe now: the latch above serializes terminal transitions, and no
    /// non-terminal transition can move a `Vanished` state (their compare-exchanges are keyed on
    /// `Live`/`TransientNotLive`), so this value is absorbing.
    pool_lifecycle.store(which, std::memory_order_release);

    ProfileEvents::increment(ProfileEvents::CasDataRootVanished);
    LOG_WARNING(getLogger("CasPool"),
        "Content-addressed pool '{}' entered Vanished({}): {}. The disk stays registered but store-class "
        "access now fails loud with a typed error (truth); restart re-registers the name.",
        server_root_id, label, reason);
}

void CasMountRuntime::scheduleRemount()
{
    /// Count every entry before checking whether background work is enabled. Tests can therefore observe
    /// the keeper's loss callback without depending on a recovery thread being spawned.
    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
    if (!config.background_watermark)
        return;
    /// A fully-terminal `Vanished` pool never claims/allocates/writes again (spec §3): the keeper
    /// callback must not arm a recovery thread. It reads the terminal-intent latch (`vanished_intent`,
    /// published FIRST by `enterVanished`, and by Task 10's FORGET) alongside the settled state
    /// `isVanished()`, so the earliest possible terminal signal is honored. `IdentityLost` is NOT terminal
    /// here — it sets no latch, so its non-mutating observer keeps running through this same recovery loop.
    if (remount_shutting_down.load() || remount_running.load()
        || vanished_intent.load(std::memory_order_acquire) || isVanished())
        return;
    std::lock_guard g(remount_thread_mutex);
    if (remount_shutting_down.load() || remount_running.load()
        || vanished_intent.load(std::memory_order_acquire) || isVanished())
        return;
    if (remount_thread.joinable())
        remount_thread.join();   /// Reap a previous recovery before starting a new one.
    remount_running.store(true);
    remount_thread = ThreadFromGlobalPool([this]
    {
        setThreadName(ThreadName::CAS_REMOUNT);
        uint64_t backoff_ms = 1000;
        /// Exit at any step boundary once a fully-terminal transition is intended (spec §3). It checks the
        /// terminal-intent latch (`vanished_intent`, published FIRST by `enterVanished`, and by Task 10's
        /// FORGET before it joins this thread) alongside the settled state `isVanished()`, so the loop
        /// bails at the earliest terminal signal rather than only after the state store lands. An
        /// `IdentityLost` pool sets no latch and is NOT vanished, so this loop keeps running as its
        /// low-rate observer: `remount_attempt` (the identity gate) returns false without claiming while
        /// it stays demoted, and promotes one-way to `VanishedErased` (Task 6), which then trips this exit.
        while (!remount_stop.load() && !vanished_intent.load(std::memory_order_acquire) && !isVanished())
        {
            if (remount_attempt())
                break;
            std::unique_lock lk(remount_cv_mutex);
            remount_cv.wait_for(lk, std::chrono::milliseconds(backoff_ms),
                                [this] { return remount_stop.load(); });
            backoff_ms = std::min<uint64_t>(backoff_ms * 2, 30000);
        }
        remount_running.store(false);
    });
}

bool CasMountRuntime::scheduleRemountForTest()
{
    scheduleRemount();
    std::lock_guard g(remount_thread_mutex);
    return remount_thread.joinable();
}

void CasMountRuntime::beginShutdownForTest()
{
    std::lock_guard g(remount_thread_mutex);
    remount_shutting_down.store(true);
}

void CasMountRuntime::stopRemountThread()
{
    /// Refuse further recovery arming under the same mutex used by `scheduleRemount`, before joining.
    /// Thus a keeper callback racing with teardown cannot re-arm the recovery thread after the join.
    {
        std::lock_guard g(remount_thread_mutex);
        remount_shutting_down.store(true);
    }
    /// Stop recovery first; it could otherwise recreate the keeper while the heartbeat is being retired.
    remount_stop.store(true);
    remount_cv.notify_all();
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }
}

void CasMountRuntime::finishTeardown(bool drained)
{
    /// On a drained teardown, `stop` writes an already-expired lease and the watermark farewell
    /// (`min_active = UINT64_MAX`). This lets the same server reclaim immediately while retaining the
    /// durable owner and epoch. A failure, such as another incarnation touching the slot, must not escape
    /// destruction; log it and continue teardown.
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
                tryLogCurrentException(getLogger("CasPool"), "CAS mount-lease: release during Pool teardown failed");
            }
        }
        else
        {
            /// If draining did not certify that every in-flight PUT resolved, a clean farewell would be
            /// false evidence. Stop background renewal without a terminal operation so the successor uses
            /// the slower but safe observation-based reclaim path.
            LOG_WARNING(getLogger("CasPool"),
                "CAS store shutdown with an unresolved ref-log PUT: skipping the clean-release marker; "
                "the next mount will treat this end as unclean");
            mount_keeper->stopBackground();
        }
    }

    /// The second join closes the residual window where a keeper loss callback observed the shutdown gate
    /// late during the heartbeat stop operation.
    {
        std::lock_guard g(remount_thread_mutex);
        if (remount_thread.joinable())
            remount_thread.join();
    }
}

}
