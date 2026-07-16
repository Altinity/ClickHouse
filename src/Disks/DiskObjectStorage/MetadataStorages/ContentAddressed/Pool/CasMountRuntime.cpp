#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBuild.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
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
}

bool CasMountRuntime::refAppendFenceOk() const
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
    /// Renew the merged heartbeat (lease + build-watermark floor). A read-only open never anchored the
    /// keeper; there is nothing to renew (fail closed rather than fabricate one).
    if (!mount_keeper)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renewWatermarkOnce on a read-only Store");
    mount_keeper->renewOnce();
}

uint64_t CasMountRuntime::allocateBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    const uint64_t s = next_build_seq++;
    active_build_seqs.insert(s);
    return s;
}

void CasMountRuntime::registerInflightBuild(uint64_t seq, const BuildPtr & build)
{
    /// Register for `dropNamespace`'s post-durable build cancellation (spec §Namespace Removal). weak_ptr:
    /// the wiring owns the returned shared_ptr; `retireBuildSeq` (publish/abandon/dtor) removes the entry.
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
    /// Collect the live shared_ptrs under `builds_mutex`, cancel OUTSIDE it
    /// (`cancelForNamespaceRemoval` only stores an atomic). Relocated verbatim from `dropNamespace`'s
    /// tail; invoked by `ref_ledger` through the `cancel_inflight_builds` callback once its removal
    /// transaction is durable (spec §Namespace Removal: "cancels local builds").
    std::vector<BuildPtr> builds_to_check;
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
    /// Per-server watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random NONZERO
    /// value minted once per Store: GC checks it for equality only (a different epoch == a dead
    /// incarnation). It rides through the watermark protobuf codec (uint64 field — full range). For
    /// safety and to avoid the 0/UINT64_MAX sentinels, mask to 52 bits (collision-safe for an
    /// equality-only token) and re-draw on 0 (UINT64_MAX is the retired sentinel).
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
    /// The mount object now holds OUR live (uuid, epoch) body. Construct + start the keeper, which
    /// ADOPTS that very (uuid, epoch) slot rather than self-tripping the double-start guard.
    /// The mount keeper carries the per-server build-watermark floor (`minActive`), read off the
    /// keeper's state lock via `prepareRenew`.
    const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
    mount_keeper = std::make_unique<MountLeaseKeeper>(
        backend_ptr, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); });
    /// Keeper ↔ fence coupling (spec §write-fence): on each successful background renew refresh
    /// the monotonic deadline; on a superseded/foreign renew failure latch the fence to lost.
    /// Set BEFORE startBackground so no renewal can fire before the callbacks are in place.
    mount_keeper->setFenceCallbacks(
        [this, ttl_ms] { setMountDeadline(bootMsNow() + ttl_ms); },
        [this]
        {
            tripMountLost();
            /// Liveness counterpart of the fence-out: recover in place as a FRESH incarnation.
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

void CasMountRuntime::scheduleRemount()
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
}

void CasMountRuntime::finishTeardown(bool drained)
{
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

}
