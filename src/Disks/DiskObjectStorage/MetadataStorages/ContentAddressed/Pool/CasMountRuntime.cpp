#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Common/Exception.h>
#include <Common/LockMemoryExceptionInThread.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include <Common/thread_local_rng.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <type_traits>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace ProfileEvents
{
    extern const Event CASIdentityLost;
    extern const Event CASDataRootVanished;
    extern const Event CASMountLeaseLost;
    extern const Event CASMountLeaseExpired;
    extern const Event CASMountRenewalAttempts;
    extern const Event CASMountRenewalRetries;
    extern const Event CASMountRenewalResolved;
    extern const Event CASMountRenewalRecovered;
    extern const Event CASMountRenewalDeadlineExceeded;
}

namespace DB::Cas
{

void reportMountRenewCompletion(const MountRenewResult & result, std::optional<uint64_t> expired_ms) noexcept;
void configureMountRenewObservability(
    const String * server_root_id, const CasEventSink * event_sink, bool deferred) noexcept;

namespace
{
/// Wall-clock seconds since epoch — the `since` timestamp the lifecycle snapshot reports (spec §7). A
/// wall clock, deliberately unlike the fence's `CLOCK_BOOTTIME`: this is an operator-facing DateTime, not
/// an interval measured across a possible VM suspend.
int64_t wallClockNowSeconds()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
}

CasMountRuntime::CasMountRuntime(
    BackendPtr backend_ptr_,
    CasRequests & mount_requests_,
    CasRequests & farewell_requests_,
    CasRequests & lease_requests_,
    const Layout & layout_,
    MountConfig config_,
    String server_root_id_,
    const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_,
    std::function<bool()> remount_attempt_)
    : backend_ptr(std::move(backend_ptr_))
    , mount_requests(mount_requests_)
    , farewell_requests(farewell_requests_)
    , lease_requests(lease_requests_)
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
    /// FORGET publishes terminal intent before tripping the fence. That deliberate decommission is not
    /// an operational loss/recovery generation and must not pass through the transient-loss accounting
    /// edge. Ordinary external or renewal loss has no terminal intent and retains the single CAS winner.
    if (!vanishedIntentPublished())
        (void)noteLeaseLost();
}

void CasMountRuntime::tripFenceWithoutOperationalLoss()
{
    mount_fence.lost.store(true, std::memory_order_release);
    fence_generation.fetch_add(1, std::memory_order_acq_rel);
}

void CasMountRuntime::checkFenceOrThrow(uint64_t admitted_generation) const
{
    if (mayMutate() && fenceGeneration() == admitted_generation)
        return;
    const String subject = fmt::format("content-addressed pool '{}'", server_root_id);
    if (const std::optional<String> expired = leaseExpiredRefusal(admitted_generation))
        throwCasTransientUnavailable(subject, *expired);
    /// [D5]: tell only what is known here. A tripped fence (or a bumped generation) means this node no
    /// longer holds the mount incarnation the caller was admitted under -- but this same guard trips for a
    /// transient lease blip AND for a deliberate terminal decommission (FORGET) or a lost identity, and this
    /// code cannot tell them apart. So the CONDITION must NOT promise recovery ("temporarily unreachable"
    /// would misdiagnose the terminal case); it names both possibilities and points at the authoritative
    /// lifecycle. The CLASS is the write plane's uniform transient one (its 32 sibling write-transient sites
    /// already mint it): under genuine ambiguity the refusal must be retried, never consumed as damage.
    throwCasTransientUnavailable(
        subject,
        "mount fence tripped: the durable write is refused because this node no longer holds the mount "
        "incarnation it was admitted under -- either a lease loss the disk auto-recovers from, or a "
        "FORGET decommission / lost identity that does NOT recover; consult "
        "system.cas_mounts for the disk's lifecycle before retrying");
}

Fence::Admit CasMountRuntime::admit(uint64_t admitted_generation, uint64_t needed_ms) const
{
    if (mount_fence.lost.load(std::memory_order_acquire) || fenceGeneration() != admitted_generation)
        return Fence::Admit::LostOrRearmed;
    const uint64_t now = bootMsNow();
    return budgetAdmits(mount_fence.deadline_boot_ms.load(std::memory_order_acquire), now, needed_ms);
}

Fence::Admit CasMountRuntime::budgetAdmits(uint64_t deadline_boot_ms, uint64_t now_boot_ms, uint64_t needed_ms) const
{
    if (now_boot_ms >= deadline_boot_ms)
        return Fence::Admit::NoBudget;
    /// Compared by subtraction rather than as the sum `needed_ms + margin`, which can wrap for an
    /// absurd configuration and then read as if there were room.
    const uint64_t remaining = deadline_boot_ms - now_boot_ms;
    if (needed_ms >= remaining || cas_request_budget.lease_safety_margin_ms >= remaining - needed_ms)
        return Fence::Admit::NoBudget;
    return Fence::Admit::Ok;
}

uint64_t CasMountRuntime::refAppendReservationMs() const
{
    /// Two envelopes: a write and its settlement read, which is what `writeLoop` reserves.
    const uint64_t envelope_ms = cas_request_budget.attemptEnvelopeMs();
    return envelope_ms > std::numeric_limits<uint64_t>::max() / 2 ? std::numeric_limits<uint64_t>::max() : 2 * envelope_ms;
}

bool CasMountRuntime::refAppendFenceOk() const
{
    return admit(fenceGeneration(), refAppendReservationMs()) == Fence::Admit::Ok;
}

std::optional<uint64_t> CasMountRuntime::leaseExpiredAt(uint64_t now_boot_ms) const
{
    if (lifecycle() != PoolLifecycle::Live || mount_fence.lost.load(std::memory_order_acquire))
        return std::nullopt;
    const uint64_t deadline = mount_fence.deadline_boot_ms.load(std::memory_order_acquire);
    if (now_boot_ms < deadline)
        return std::nullopt;
    return std::min(deadline, lease_expired_at_boot_ms.load(std::memory_order_acquire));
}

std::optional<uint64_t> CasMountRuntime::leaseExpiredSinceBootMs() const
{
    return leaseExpiredAt(bootMsNow());
}

std::optional<String> CasMountRuntime::leaseExpiredRefusal(uint64_t admitted_generation) const
{
    if (fenceGeneration() != admitted_generation || !leaseExpiredSinceBootMs())
        return std::nullopt;
    return String("the mount lease expired and no renewal has restored it yet; "
                  "writes resume when a renewal restores it");
}

String CasMountRuntime::lastRenewFailure() const
{
    std::lock_guard lock(renew_failure_mutex);
    return last_renew_failure;
}

void CasMountRuntime::noteRenewRequest(const MountRenewRequestEvent & event) noexcept
{
    warnOnceIfLeaseExpired(event);
    if (!event.failed)
    {
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalAttempts);
        if (event.request_no > 1)
            ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRetries);
        return;
    }
    try
    {
        {
            std::lock_guard lock(renew_failure_mutex);
            last_renew_failure = event.failure_text;
        }
        /// `waitUntilArmed` re-reads the fence clock on this wake-up. Passing through `driver_mutex` orders
        /// the notification after its predicate check, so the wake-up is not lost.
        {
            std::lock_guard lock(driver_mutex);
        }
        driver_cv.notify_all();
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// A lost diagnostic must not end the renewal.
    }
}

void CasMountRuntime::warnOnceIfLeaseExpired(const MountRenewRequestEvent & event) noexcept
{
    try
    {
        const uint64_t now = bootMsNow();
        const std::optional<uint64_t> expired_at = leaseExpiredAt(now);
        if (!expired_at || *expired_at == lease_expiry_warned_at_boot_ms.load(std::memory_order_relaxed))
            return;
        lease_expiry_warned_at_boot_ms.store(*expired_at, std::memory_order_relaxed);
        const String last_failure = event.failed ? event.failure_text : lastRenewFailure();
        LOG_WARNING(getLogger("CasPool"),
            "CAS mount lease of '{}' expired {} ms ago and no renewal has restored it yet; "
            "the renewal keeps retrying and writes are refused until it succeeds. Last failed request: {}",
            server_root_id, now - *expired_at, last_failure.empty() ? "none" : last_failure);
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// A lost diagnostic must not end the renewal.
    }
}

std::optional<CasMountRuntime::RestoredLease> CasMountRuntime::publishRenewedDeadline(uint64_t deadline_boot_ms)
{
    const uint64_t now = bootMsNow();
    const std::optional<uint64_t> expired_at = leaseExpiredAt(now);
    if (deadline_boot_ms <= now)
    {
        /// The carried instant goes first: a concurrent snapshot reads the deadline and this instant
        /// separately, and must never see the new deadline without it.
        lease_expired_at_boot_ms.store(
            expired_at.value_or(std::numeric_limits<uint64_t>::max()), std::memory_order_release);
        setMountDeadline(deadline_boot_ms);
        return std::nullopt;
    }
    setMountDeadline(deadline_boot_ms);
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    String ended_failure;
    {
        std::lock_guard lock(renew_failure_mutex);
        ended_failure.swap(last_renew_failure);
    }
    if (!expired_at)
        return std::nullopt;
    ProfileEvents::incrementNoTrace(ProfileEvents::CASMountLeaseExpired);
    return RestoredLease{.expired_ms = now - *expired_at, .last_failure = std::move(ended_failure)};
}

void CasMountRuntime::setMountDeadline(uint64_t deadline_boot_ms)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
}

void CasMountRuntime::armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms)
{
    mount_fence.server_uuid = server_uuid;
    mount_fence.writer_epoch = writer_epoch;
    armFence(deadline_boot_ms, /*report_live=*/false);
}

void CasMountRuntime::armFence(uint64_t deadline_boot_ms, bool report_live)
{
    mount_fence.deadline_boot_ms.store(deadline_boot_ms, std::memory_order_release);
    lease_expired_at_boot_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    /// A fresh lease incarnation is a fresh generation too: a durable-effect caller admitted under the
    /// PRIOR incarnation must re-check and abort rather than ride this re-arm through.
    fence_generation.fetch_add(1, std::memory_order_acq_rel);
    if (report_live)
        noteRemounted();
    if (arm_mount_fence_interposition_hook_for_test)
        arm_mount_fence_interposition_hook_for_test();
    /// Open the gate LAST. A caller that observes `lost == false` with acquire semantics must also see
    /// the fresh generation; publishing the latch first exposes one admission window in which the dead
    /// generation looks live again.
    mount_fence.lost.store(false, std::memory_order_release);
}

void CasMountRuntime::beginReclaim()
{
    std::lock_guard lock(driver_mutex);
    reclaim_generation = remount_requested_generation;
    /// A request can come without a trip; latching here keeps the pool from running not `Live` on an
    /// armed fence. Atomics only.
    tripMountLost();
}

bool CasMountRuntime::armIfAdmissible(uint64_t deadline_boot_ms)
{
    std::lock_guard lock(driver_mutex);
    remount_handled_generation = std::max(remount_handled_generation, reclaim_generation);
    const bool arm = canArm(deadline_boot_ms);
    if (arm)
    {
        armFence(deadline_boot_ms, /*report_live=*/true);
    }
    else
    {
        /// An open starts unarmed, which admits writes; a claim that does not admit a ref append must
        /// not leave it so. Latched before the deadline is published, so no reader sees the claim's
        /// deadline on an open fence. A reclaim arrives here already latched by `beginReclaim`.
        if (!mount_fence.lost.load(std::memory_order_acquire))
            tripFenceWithoutOperationalLoss();
        setMountDeadline(deadline_boot_ms);
    }
    driver_cv.notify_all();
    return arm;
}

bool CasMountRuntime::waitUntilArmed(uint64_t timeout_ms) const
{
    const uint64_t started = bootMsNow();
    const uint64_t give_up = started > std::numeric_limits<uint64_t>::max() - timeout_ms
        ? std::numeric_limits<uint64_t>::max()
        : started + timeout_ms;
    std::unique_lock lock(driver_mutex);
    while (true)
    {
        if (!mount_fence.lost.load(std::memory_order_acquire))
            return true;
        if (workers_stop_requested || remountTerminal() || !workers_started)
            return false;
        const uint64_t now = bootMsNow();
        if (now >= give_up)
            return false;
        /// The fence clock can be injected and move with no real time passing, so every wake-up re-reads
        /// it: an arm, a failed renewal request, a stop and a terminal publication all notify.
        driver_cv.wait_for(lock, std::chrono::milliseconds(give_up - now));
    }
}

bool CasMountRuntime::canArm(uint64_t deadline_boot_ms) const
{
    /// The clock is read last and only when every other term holds.
    return !workers_stop_requested
        && !remountTerminal()
        && remount_requested_generation <= remount_handled_generation
        && budgetAdmits(deadline_boot_ms, bootMsNow(), refAppendReservationMs()) == Fence::Admit::Ok;
}

bool CasMountRuntime::lossNeedsNewRequest() const
{
    /// A request up to `reclaim_generation` was snapshotted by a reclaim that latched before this loss,
    /// so it does not cover the loss.
    return remount_requested_generation <= std::max(remount_handled_generation, reclaim_generation);
}

void CasMountRuntime::checkRenewerOwner() const
{
    if (workers_started && lease_thread_id != std::this_thread::get_id())
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "CAS mount runtime: only the lease thread may drive the renewer while it runs");
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
    (void)renewRenewerOnce(RenewCaller::Direct);
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

void CasMountRuntime::installRenewer(
    UInt128 our_uuid,
    uint64_t writer_epoch,
    const std::function<uint64_t()> & now_ms)
{
    std::unique_ptr<MountLeaseRenewer> replaced = std::make_unique<MountLeaseRenewer>(
        mount_requests, farewell_requests, lease_requests, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); },
        std::chrono::milliseconds(cas_request_budget.lease_safety_margin_ms),
        [this] { return bootMsNow(); });

    /// The previous renewer is destroyed after the unlock.
    std::lock_guard lock(driver_mutex);
    checkRenewerOwner();
    std::swap(mount_renewer, replaced);
}

uint64_t CasMountRuntime::startRenewer()
{
    MountLeaseRenewer * renewer = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        checkRenewerOwner();
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer without a renewer");
        if (mount_renewer->state() != MountLeaseRenewerState::New)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer requires a New renewer");
        renewer = mount_renewer.get();
    }
    return renewer->start([this] { return !renewalCancelled(); });
}

MountRenewOperationEnvironment CasMountRuntime::renewalEnvironment(RenewCaller caller)
{
    const bool loop = caller == RenewCaller::Loop;
    return MountRenewOperationEnvironment{
        .boot_ms = [this] { return bootMsNow(); },
        .live = [this, caller]
        {
            return renewalLive(caller) && (!config.renewal_live_for_test || config.renewal_live_for_test());
        },
        .cancelled = [this] { return renewalCancelled(); },
        /// Only the loop keeps renewing past the lease; startup, remount and direct renewals stay
        /// bounded by it.
        .policy = loop ? MountRenewPolicy::UntilDefinitive : MountRenewPolicy::LeaseBound,
        /// The loop counts its requests as they are sent, so an outage shows while it lasts.
        .on_request = loop
            ? std::function<void(const MountRenewRequestEvent &)>(
                  [this](const MountRenewRequestEvent & event) { noteRenewRequest(event); })
            : nullptr,
    };
}

bool CasMountRuntime::renewalLive(RenewCaller caller) const
{
    std::lock_guard lock(driver_mutex);
    if (workers_stop_requested)
        return false;
    if (caller != RenewCaller::Loop)
        return true;
    /// A lost fence alone does not end it: the renewal that makes an open or a reclaim ready runs under
    /// one, and every trip that must end it comes with a request, a terminal lifecycle or a stop.
    return remount_requested_generation <= remount_handled_generation && !remountTerminal();
}

bool CasMountRuntime::renewalCancelled() const
{
    std::lock_guard lock(driver_mutex);
    return workers_stop_requested;
}

void CasMountRuntime::sleepInterruptibly(uint64_t ms)
{
    std::unique_lock lock(driver_mutex);
    driver_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return workers_stop_requested; });
}

void CasMountRuntime::consumeRenewResult(const MountRenewResult & result, RenewCaller caller)
{
    /// The loop counts its requests as they are sent (`noteRenewRequest`). Every other renewal counts
    /// them here, from its result.
    if (caller != RenewCaller::Loop && result.attempts_sent > 0)
    {
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalAttempts, result.attempts_sent);
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRetries, result.attempts_sent - 1);
    }
    if (result.resolved_by_read)
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalResolved);

    if (result.outcome == MountRenewOutcome::Committed && (result.attempts_sent > 1 || result.resolved_by_read))
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalRecovered);
    if (result.outcome == MountRenewOutcome::Terminal
        && result.deadline_source == GaveUp::Source::Lease)
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountRenewalDeadlineExceeded);

    /// One step under `driver_mutex`. The loop reads the remount generations only after it, so no
    /// reclaim can arm between a commit and the publication of its deadline.
    std::optional<RestoredLease> restored;
    {
        std::lock_guard lock(driver_mutex);
        if (result.outcome == MountRenewOutcome::Committed)
        {
            const uint64_t ttl_ms = static_cast<uint64_t>(config.mount_lease_ttl_ms.count());
            const uint64_t deadline_boot_ms = result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
                ? std::numeric_limits<uint64_t>::max()
                : result.attempt_start_boot_ms + ttl_ms;
            restored = publishRenewedDeadline(deadline_boot_ms);
            /// A latched fence is armed by the first renewal whose own deadline leaves room for a ref append.
            if (mount_fence.lost.load(std::memory_order_acquire) && canArm(deadline_boot_ms))
                armFence(deadline_boot_ms, /*report_live=*/true);
        }
        else if (result.outcome == MountRenewOutcome::Terminal)
        {
            switch (caller)
            {
                case RenewCaller::Loop:
                    if (workers_stop_requested)
                    {
                        tripFenceWithoutOperationalLoss();
                        break;
                    }
                    tripMountLost();
                    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
                    if (!remountTerminal() && lossNeedsNewRequest())
                        ++remount_requested_generation;
                    break;
                case RenewCaller::Direct:
                    tripMountLost();
                    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
                    break;
                case RenewCaller::Startup:
                    tripFenceWithoutOperationalLoss();
                    break;
                case RenewCaller::Remount:
                    /// The reclaim latched the fence at its start and reports this failure itself.
                    if (workers_stop_requested)
                        tripFenceWithoutOperationalLoss();
                    break;
            }
        }
        driver_cv.notify_all();
    }

    if (restored)
        LOG_WARNING(getLogger("CasPool"),
            "CAS mount lease of '{}' was expired for {} ms; a renewal restored it and writes resume. "
            "Last failed renewal request: {}",
            server_root_id, restored->expired_ms, restored->last_failure);

    if (result.outcome == MountRenewOutcome::Committed)
    {
        std::optional<uint64_t> expired_ms;
        if (restored)
            expired_ms = restored->expired_ms;
        reportMountRenewCompletion(result, expired_ms);
        return;
    }
    if (result.outcome == MountRenewOutcome::NotAttempted)
    {
        reportMountRenewCompletion(result, std::nullopt);
        return;
    }

    if (!result.failure)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: terminal renewal has no failure");
    try
    {
        std::rethrow_exception(result.failure);
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::LOGICAL_ERROR)
            throw;
    }
    catch (...)
    {
    }

    reportMountRenewCompletion(result, std::nullopt);

    if (caller != RenewCaller::Loop)
        std::rethrow_exception(result.failure);
}

MountRenewResult CasMountRuntime::renewRenewerOnce(RenewCaller caller)
{
    MountLeaseRenewer * renewer = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        if (caller == RenewCaller::Direct && config.background_watermark)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS mount runtime: direct renewal is disabled when background ownership is configured");
        checkRenewerOwner();
        if (!mount_renewer)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal without a renewer");
        if (mount_renewer->state() != MountLeaseRenewerState::Active)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: renewal requires an Active renewer");
        renewer = mount_renewer.get();
    }
    /// Configuration is pointer/POD-only. A remount re-anchor retains its completed observation for the
    /// whole-chain finalizer to deliver after `remount_mutex` is released.
    configureMountRenewObservability(&server_root_id, &event_sink, caller == RenewCaller::Remount);
    /// The remount re-anchor runs before `armIfAdmissible`, with the fence still latched, so it renews on
    /// the renewer's open plane: admitted under the mount fence it could only ever give up.
    const MountRenewResult result = caller == RenewCaller::Remount
        ? renewer->renewForRemount(renewalEnvironment(caller))
        : renewer->renew(renewalEnvironment(caller));
    consumeRenewResult(result, caller);
    return result;
}

uint64_t CasMountRuntime::renewRenewerForStartupOnce()
{
    return renewRenewerOnce(RenewCaller::Startup).attempt_start_boot_ms;
}

uint64_t CasMountRuntime::renewRenewerForRemountOnce()
{
    return renewRenewerOnce(RenewCaller::Remount).attempt_start_boot_ms;
}

void CasMountRuntime::renewerReset()
{
    std::unique_ptr<MountLeaseRenewer> released;
    std::lock_guard lock(driver_mutex);
    checkRenewerOwner();
    std::swap(mount_renewer, released);
}

ThreadFromGlobalPool CasMountRuntime::makeWorker(std::function<void()> body)
{
    if (config.worker_factory)
        return config.worker_factory(std::move(body));
    return ThreadFromGlobalPool(std::move(body));
}

void CasMountRuntime::startBackgroundWorkers(std::chrono::milliseconds period)
{
    {
        std::lock_guard lock(driver_mutex);
        if (workers_started || renewal_worker.joinable())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: the lease thread cannot start in the current state");
        if (!mount_renewer || mount_renewer->state() != MountLeaseRenewerState::Active)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: the lease thread requires an Active renewer");
        workers_started = true;
        workers_stop_requested = false;
        lease_thread_id = {};
        renewal_period = period;
    }

    ThreadFromGlobalPool worker;
    try
    {
        worker = makeWorker([this] { renewalLoop(); });
    }
    catch (...)
    {
        /// No thread exists, so nothing is joined.
        {
            std::lock_guard lock(driver_mutex);
            workers_stop_requested = true;
            workers_started = false;
            lease_thread_id = {};
            driver_cv.notify_all();
        }
        tripFenceWithoutOperationalLoss();
        throw;
    }

    std::lock_guard lock(driver_mutex);
    renewal_worker = std::move(worker);
}

void CasMountRuntime::renewalLoop()
{
    setThreadName(ThreadName::CAS_LEASE_RENEWER);
    /// The tracker still counts this thread's allocations but never throws on it: a renewal failed by a
    /// memory limit costs the mount, and an exception outside the request ends this thread.
    LockMemoryExceptionInThread memory_exception_lock(VariableContext::Global);
    {
        std::lock_guard lock(driver_mutex);
        lease_thread_id = std::this_thread::get_id();
    }

    /// The wake condition of every wait in this loop. The reclaim backoff ignores requests: a newer one
    /// waits for the backoff too.
    const auto woken = [this](bool by_request)
    {
        const bool wake = workers_stop_requested
            || remountTerminal()
            || (by_request && remount_requested_generation > remount_handled_generation);
        if (!wake && config.lease_wait_predicate_false_hook_for_test)
            config.lease_wait_predicate_false_hook_for_test();
        return wake;
    };

    uint64_t backoff_ms = 1000;
    while (true)
    {
        if (config.renewal_before_driver_lock_hook_for_test)
            config.renewal_before_driver_lock_hook_for_test();

        bool reclaim = false;
        uint64_t snapshot = 0;
        {
            std::unique_lock lock(driver_mutex);
            if (workers_stop_requested || remountTerminal())
                return;
            if (remount_requested_generation > remount_handled_generation)
            {
                reclaim = true;
                snapshot = remount_requested_generation;
            }
            else if (!mount_renewer || mount_renewer->state() != MountLeaseRenewerState::Active)
            {
                driver_cv.wait(lock, [&] { return woken(/*by_request=*/true); });
                continue;
            }
            else
            {
                const uint64_t last_anchor = mount_renewer->lastCommittedAttemptStartBootMs();
                const uint64_t period_ms = static_cast<uint64_t>(std::max<int64_t>(0, renewal_period.count()));
                const uint64_t due = last_anchor > std::numeric_limits<uint64_t>::max() - period_ms
                    ? std::numeric_limits<uint64_t>::max()
                    : last_anchor + period_ms;
                const uint64_t now = bootMsNow();
                if (now < due)
                {
                    driver_cv.wait_for(lock, std::chrono::milliseconds(due - now), [&] { return woken(/*by_request=*/true); });
                    continue;
                }
            }
        }

        if (reclaim)
        {
            bool reclaimed = false;
            try
            {
                reclaimed = remount_attempt();
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasPool"), "CAS self-remount attempt failed");
            }

            std::unique_lock lock(driver_mutex);
            if (reclaimed)
            {
                /// `armIfAdmissible` already acknowledged the generation the attempt served. This keeps a
                /// callback that returns true without it from reclaiming the same generation again.
                remount_handled_generation = std::max(remount_handled_generation, snapshot);
                backoff_ms = 1000;
                continue;
            }
            driver_cv.wait_for(lock, std::chrono::milliseconds(backoff_ms), [&] { return woken(/*by_request=*/false); });
            backoff_ms = std::min<uint64_t>(backoff_ms * 2, 30000);
            continue;
        }

        if (config.renewal_admitted_hook_for_test)
            config.renewal_admitted_hook_for_test();
        try
        {
            (void)renewRenewerOnce(RenewCaller::Loop);
        }
        catch (...)
        {
            /// The loop's renewal does not propagate renewal failures, so anything arriving here is this
            /// loop's own state machine reporting that it was driven out of contract. A background loop
            /// must not take the process down, but it must not keep driving a state machine that just
            /// proved wrong either: every later renewal would be unaudited.
            ///
            /// Exiting is safe because write admission is bounded by the fence deadline, not by this
            /// thread's existence -- `mayMutate` requires `bootMsNow() < mount_fence.deadline_boot_ms`, so
            /// writes stop being admitted within one TTL whether or not anyone is renewing. Tripping the
            /// fence first brings that boundary forward instead of waiting for the TTL to lapse.
            tripMountLost();
            tryLogCurrentException(getLogger("CasPool"), "CAS mount-lease renewal loop");
            chassert(false);
            return;
        }
    }
}

void CasMountRuntime::stopBackgroundWorkers()
{
    ThreadFromGlobalPool * to_join = nullptr;
    {
        std::lock_guard lock(driver_mutex);
        if (!workers_started && !renewal_worker.joinable())
            return;
        workers_stop_requested = true;
        driver_cv.notify_all();
        to_join = &renewal_worker;
    }

    if (to_join->joinable())
        to_join->join();

    {
        std::lock_guard lock(driver_mutex);
        workers_started = false;
        lease_thread_id = {};
        driver_cv.notify_all();
    }
}

bool CasMountRuntime::isVanished() const
{
    const PoolLifecycle s = lifecycle();
    return s == PoolLifecycle::VanishedReplaced
        || s == PoolLifecycle::VanishedForgotten;
}

void CasMountRuntime::setLifecycleForTest(PoolLifecycle lc)
{
    /// Direct store, no precondition — the test harness pins an exact cell of the class × state table.
    /// A `Vanished*` value also latches `vanished_intent` so the forced terminal state matches what a
    /// natural `enterVanished` would leave behind (its truth semantics never depend on how it was reached).
    /// Stamp `since` to match a naturally-reached state (release-store before the state store below, so a
    /// snapshot reader that acquire-observes the forced state also observes the timestamp): 0 for `Live`,
    /// now for every non-`Live` value. Keeps the forced cell of the class × state table indistinguishable
    /// from a real transition for the introspection snapshot.
    lifecycle_since_wall_s.store(lc == PoolLifecycle::Live ? 0 : wallClockNowSeconds(), std::memory_order_release);
    pool_lifecycle.store(lc, std::memory_order_release);
    if (lc == PoolLifecycle::VanishedReplaced
        || lc == PoolLifecycle::VanishedForgotten)
    {
        vanished_intent.store(true, std::memory_order_release);
        /// Keep the terminal-state guard consistent with the forced state, so a later `enterVanished`
        /// (unusual, but not forbidden) is a clean no-op rather than re-storing / re-logging.
        terminal_state_published.store(true, std::memory_order_release);
    }
}

bool CasMountRuntime::noteLeaseLost()
{
    /// `Live -> TransientNotLive`, and nothing else. A compare-exchange FROM `Live` leaves every other
    /// state untouched, so a terminal state is never downgraded and a repeated call is a no-op. This is
    /// the only transition the runtime renewal consumer performs, and it needs no lock because of that discipline.
    PoolLifecycle expected = PoolLifecycle::Live;
    if (pool_lifecycle.compare_exchange_strong(
            expected, PoolLifecycle::TransientNotLive, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        /// The `since` the lifecycle snapshot reports for `not_live` — the wall-clock instant this became
        /// non-`Live`. Only the winning transition writes it (the guard above), so it is not re-stamped.
        lifecycle_since_wall_s.store(wallClockNowSeconds(), std::memory_order_release);
        /// This transition can be reached while the renewal driver or remount serializer lock is
        /// held. Trace-profile collection may allocate and enqueue a stack trace, so the lock-safe
        /// observability path must remain the direct atomic increment.
        ProfileEvents::incrementNoTrace(ProfileEvents::CASMountLeaseLost);
        return true;
    }
    return false;
}

void CasMountRuntime::noteRemounted()
{
    /// `TransientNotLive -> Live` on a successful reclaim. A compare-exchange FROM `TransientNotLive`
    /// never revives `IdentityLost` or a `Vanished` state ([D3]) and is a no-op if already `Live`.
    PoolLifecycle expected = PoolLifecycle::TransientNotLive;
    if (pool_lifecycle.compare_exchange_strong(
            expected, PoolLifecycle::Live, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        /// Back to `Live`: the lifecycle snapshot reports no `since` (0) for a live pool.
        lifecycle_since_wall_s.store(0, std::memory_order_release);
    }
}

std::unique_lock<std::mutex> CasMountRuntime::lockTerminalPublication()
{
    if (config.terminal_publication_waiting_for_driver_lock_hook_for_test)
        config.terminal_publication_waiting_for_driver_lock_hook_for_test();

    std::unique_lock lock(driver_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        if (config.terminal_publication_driver_lock_contended_hook_for_test)
            config.terminal_publication_driver_lock_contended_hook_for_test();
        lock.lock();
    }

    if (config.terminal_publication_driver_lock_acquired_hook_for_test)
        config.terminal_publication_driver_lock_acquired_hook_for_test();
    return lock;
}

void CasMountRuntime::enterIdentityLost()
{
    /// `TransientNotLive -> IdentityLost`, one way. The compare-exchange FROM `TransientNotLive` gives
    /// the brief's "from TransientNotLive only" precondition, idempotency (a second call finds the state
    /// already `IdentityLost` and its exchange fails), and safety against a concurrent renewer
    /// `noteLeaseLost` (which only ever moves `Live -> TransientNotLive`, never away from it). It does NOT
    /// set `vanished_intent` (that latch is reserved for the `Vanished*` idempotency/FORGET protocol);
    /// rev.8 makes `IdentityLost` a fail-loud TERMINAL state through `remountTerminal`, which folds it
    /// into the worker-exit boundary alongside `vanished_intent`, so the lease and GC threads exit
    /// rather than demote.
    /// `since` for the `identity_lost` snapshot row — the wall-clock instant the observer proved the
    /// sentinels gone. Stamped (release) BEFORE the CAS that publishes `IdentityLost`, so a reader that
    /// acquire-observes `IdentityLost` is guaranteed to observe this timestamp too (the winning CAS's
    /// release carries this prior store) — the same before-publish ordering `enterVanished` uses. Safe to
    /// stamp before the CAS here, unlike the lock-free `noteLeaseLost`: this runs only from
    /// `TransientNotLive` under `Pool::remount_mutex` (a `Vanished` pool bailed at the caller's
    /// `isVanished` gate and the caller guards `!= IdentityLost`), so the CAS wins deterministically and
    /// the stamp can never land on a state we did not transition.
    bool transitioned = false;
    {
        auto lock = lockTerminalPublication();
        lifecycle_since_wall_s.store(wallClockNowSeconds(), std::memory_order_release);

        PoolLifecycle expected = PoolLifecycle::TransientNotLive;
        transitioned = pool_lifecycle.compare_exchange_strong(
            expected, PoolLifecycle::IdentityLost, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    if (!transitioned)
        return;

    driver_cv.notify_all();

    ProfileEvents::increment(ProfileEvents::CASIdentityLost);
    LOG_WARNING(getLogger("CasPool"),
        "Content-addressed pool '{}' entered IdentityLost: the pool sentinels (_pool_meta and the owner "
        "anchor) are authoritatively absent (both KeyAbsent). This is a fail-loud TERMINAL state: "
        "store-class access now fails loud and this pool's remount + GC threads self-exit. "
        "Recover by restart or SYSTEM CAS FORGET — a matching-sentinel restore does NOT "
        "auto-revive this disk.",
        server_root_id);
}

void CasMountRuntime::enterVanished(PoolLifecycle which, const String & reason)
{
    /// Validate the target BEFORE mutating any state — `enterVanished` takes only the two `Vanished*`
    /// values (`VanishedReplaced`/`VanishedForgotten`); fail loud on a call-site bug rather than store a
    /// non-terminal value or mislabel it.
    const char * label = nullptr;
    switch (which)
    {
        case PoolLifecycle::VanishedReplaced:  label = "replaced"; break;
        case PoolLifecycle::VanishedForgotten: label = "forgotten"; break;
        case PoolLifecycle::Live:
        case PoolLifecycle::TransientNotLive:
        case PoolLifecycle::IdentityLost:
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CasMountRuntime::enterVanished called with a non-terminal lifecycle value");
    }

    if (terminal_state_published.load(std::memory_order_acquire))
        return;

    if (config.vanished_reason_prepare_hook_for_test)
        config.vanished_reason_prepare_hook_for_test();
    String prepared_reason = reason;
    const int64_t prepared_since_wall_s = wallClockNowSeconds();
    static_assert(std::is_nothrow_move_assignable_v<String>);

    bool transitioned = false;
    /// The guard is published LAST. Reason preparation above is the only potentially-throwing step;
    /// once this block begins, the statically-proven-noexcept move and atomic stores either publish
    /// one complete terminal transition or observe that an earlier transition already completed.
    {
        auto lock = lockTerminalPublication();
        if (!terminal_state_published.load(std::memory_order_acquire))
        {
            transitioned = true;

            /// Publish the terminal-intent latch (spec §3). For a natural transition this is the FIRST
            /// publish; for FORGET, `publishVanishedIntent` already set it at step 1. Either way it is
            /// published before the state store below and while holding the mutex used by every
            /// `driver_cv` terminal predicate.
            vanished_intent.store(true, std::memory_order_release);

            /// Record the reason BEFORE the state's release-store, so a reader that acquire-observes the
            /// terminal state (e.g. `Pool::throwIfLifecycleTerminal`) also observes this string. Written
            /// exactly once.
            vanished_reason = std::move(prepared_reason);

            /// The `since` the lifecycle snapshot reports for the `vanished` row — the wall-clock instant
            /// of the terminal transition. Written before the `pool_lifecycle` release-store below, same
            /// as the reason, so an acquire-observer of the terminal state also observes it.
            lifecycle_since_wall_s.store(prepared_since_wall_s, std::memory_order_release);

            /// The driver mutex serializes terminal transitions, and no non-terminal transition can move
            /// a `Vanished` state (their compare-exchanges are keyed on `Live`/`TransientNotLive`), so this
            /// value is absorbing.
            pool_lifecycle.store(which, std::memory_order_release);
            terminal_state_published.store(true, std::memory_order_release);
        }
    }

    driver_cv.notify_all();
    if (!transitioned)
        return;

    ProfileEvents::increment(ProfileEvents::CASDataRootVanished);
    LOG_WARNING(getLogger("CasPool"),
        "Content-addressed pool '{}' entered Vanished({}): {}. The disk stays registered but store-class "
        "access now fails loud with a typed error (truth); restart re-registers the name.",
        server_root_id, label, reason);
}

void CasMountRuntime::publishVanishedIntent()
{
    /// FORGET's first step: publish the terminal-intent latch WITHOUT settling the state. `scheduleRemount`
    /// and the lease loop consult `vanished_intent` at their step boundaries, so this stops new remount
    /// scheduling and makes the lease loop exit at its next step — bounding FORGET's join of the lease
    /// thread to one step + one backend timeout. The state store + WARN follow in `enterVanished`.
    /// Idempotent.
    {
        auto lock = lockTerminalPublication();
        vanished_intent.store(true, std::memory_order_release);
    }
    driver_cv.notify_all();
}

void CasMountRuntime::scheduleRemount()
{
    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(driver_mutex);
    if (workers_stop_requested || remountTerminal())
        return;
    ++remount_requested_generation;
    driver_cv.notify_all();
}

void CasMountRuntime::tripAndRequestRemount()
{
    schedule_remount_calls_for_test.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(driver_mutex);
    /// Atomics only.
    tripMountLost();
    if (workers_stop_requested || remountTerminal())
        return;
    if (lossNeedsNewRequest())
        ++remount_requested_generation;
    driver_cv.notify_all();
}

bool CasMountRuntime::scheduleRemountForTest()
{
    scheduleRemount();
    std::lock_guard lock(driver_mutex);
    return workers_started && remount_requested_generation > remount_handled_generation;
}

void CasMountRuntime::beginShutdownForTest()
{
    std::lock_guard lock(driver_mutex);
    workers_stop_requested = true;
    driver_cv.notify_all();
}

bool CasMountRuntime::workersRunningForTest() const
{
    std::lock_guard lock(driver_mutex);
    return workers_started && renewal_worker.joinable();
}

uint64_t CasMountRuntime::remountRequestedGenerationForTest() const
{
    std::lock_guard lock(driver_mutex);
    return remount_requested_generation;
}

void CasMountRuntime::finishTeardown(bool drained)
{
    stopBackgroundWorkers();

    if (!mount_renewer)
        return;
    if (drained && mount_renewer->state() == MountLeaseRenewerState::Active)
    {
        try
        {
            mount_renewer->release();
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CasPool"), "CAS mount-lease: release during Pool teardown failed");
        }
    }
    else if (!drained)
    {
        LOG_WARNING(
            getLogger("CasPool"),
            "CAS store shutdown with an unresolved ref-log PUT: skipping the clean-release marker; "
            "the next mount will treat this end as unclean");
    }
}
}
