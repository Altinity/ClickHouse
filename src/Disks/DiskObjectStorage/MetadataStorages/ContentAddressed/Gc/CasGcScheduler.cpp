#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ProfileEventsScope.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include <Common/thread_local_rng.h>
#include <base/scope_guard.h>
#include <algorithm>
#include <optional>

namespace DB::Cas
{

namespace
{
    /// Non-zero events of a per-round snapshot, keyed by event name. The snapshot is already a
    /// delta when produced by a ProfileEventsScope (its counters start at zero on this thread).
    std::map<String, UInt64> snapshotToMap(const ProfileEvents::Counters::Snapshot & snap)
    {
        std::map<String, UInt64> out;
        for (ProfileEvents::Event e = ProfileEvents::Event(0); e < ProfileEvents::Counters::num_counters; ++e)
        {
            const auto value = snap[e];
            if (value != 0)
                out.emplace(String(ProfileEvents::getName(e)), static_cast<UInt64>(value));
        }
        return out;
    }
}

CasGcScheduler::CasGcScheduler(
    Cas::PoolPtr store_,
    std::chrono::seconds interval_,
    const String & log_name,
    String disk_name_,
    GcRoundLogger logger_)
    : store(std::move(store_))
    , interval(interval_)
    /// Keep the advisory pulse comfortably inside the follower's observation window. The lower
    /// bound prevents an unusually small configured interval from creating an excessively busy
    /// heartbeat worker.
    , hb_interval(std::max<std::chrono::milliseconds>(
          std::chrono::milliseconds(50),
          std::chrono::duration_cast<std::chrono::milliseconds>(interval_) / 4))
    , log(getLogger(log_name))
    /// gc_id uniqueness across instances is the Gc caller obligation - a random u128 per scheduler.
    , gc_id((static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng())
    , disk_name(std::move(disk_name_))
    , logger(std::move(logger_))
    /// Thread this scheduler's own disk/srid-scoped logger (built from `log_name` above) into the
    /// round engine, so `Gc`'s log lines carry the same scope as the round-outcome log records.
    , gc(store, gc_id, {}, {}, log)
{
}

CasGcScheduler::~CasGcScheduler()
{
    stop();
}

void CasGcScheduler::start()
{
    std::lock_guard lock(mutex);
    if (thread.joinable())
        return;
    stopping = false;
    thread = ThreadFromGlobalPool([this] { loop(); });
    hb_thread = ThreadFromGlobalPool([this] { heartbeatLoop(); });
}

void CasGcScheduler::stop()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable())
        thread.join();
    if (hb_thread.joinable())
        hb_thread.join();
}

void CasGcScheduler::onLeaseAcquired()
{
    i_am_leader.store(true, std::memory_order_relaxed);
    try
    {
        Cas::Gc::pulseHeartbeat(*store, gc_id);
    }
    catch (...)
    {
        /// Advisory, same as heartbeatLoop's own pulse: a lost pulse is harmless, the next
        /// one (from heartbeatLoop, hb_interval later) retries. Must never fail the round.
        tryLogCurrentException(log, "CA GC acquire-time heartbeat pulse failed (advisory; will retry)");
    }
}

Cas::RoundReport CasGcScheduler::runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                                 std::function<void()> on_lease_acquired, bool allow_steal)
{
    using Rec = GcRoundLogRecord;

    /// Mark a round in flight for the whole body (success AND exception paths) so the `Vanished(erased)`
    /// erasure proof's `gc_quiescent_fn` never samples an empty prefix while a round could still land a
    /// durable `gc/state`/heartbeat write. See `isQuiescent`.
    round_in_flight.store(true, std::memory_order_release);
    SCOPE_EXIT({ round_in_flight.store(false, std::memory_order_release); });

    /// Best-effort: the table row must never break GC. A throwing sink is swallowed.
    auto emit = [&](const Rec & r)
    {
        if (logger)
        {
            try
            {
                logger(r);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    };

    Rec start;
    start.event_type = Rec::EventType::Start;
    start.trigger = trigger;
    start.disk_name = disk_name;
    start.srid = store->poolConfig().server_root_id;
    start.gc_id = Cas::u128ToHex(gc_id);
    emit(start);

    const auto t0 = std::chrono::steady_clock::now();
    /// ProfileEventsScope requires an attached ThreadStatus (CurrentThread::get throws otherwise).
    /// On the server it always is: the Scheduled path runs on a ThreadFromGlobalPool, the Manual
    /// path on the query thread. In unit tests calling runOneRoundNow on a bare gtest thread there
    /// is none — skip the per-round delta there rather than fail the round.
    std::optional<ProfileEventsScope> profile_scope;
    if (CurrentThread::isInitialized())
        profile_scope.emplace();

    auto collect_profile_events = [&]() -> std::map<String, UInt64>
    {
        if (!profile_scope)
            return {};
        return snapshotToMap(*profile_scope->getSnapshot());
    };

    Rec fin = start;
    fin.event_type = Rec::EventType::Finish;
    try
    {
        const Cas::RoundReport rep = round_gc.runRegularRound(std::move(on_lease_acquired), allow_steal);
        if (rep.acquired_lease)
        {
            /// Keep health state per scheduler. Process-global gauges cannot distinguish multiple
            /// content-addressed disks in one server process.
            pending_reclaim.fetch_add(
                static_cast<Int64>(rep.condemned) - static_cast<Int64>(rep.redeleted),
                std::memory_order_relaxed);
            last_success_ms.store(
                static_cast<UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
                std::memory_order_relaxed);
        }
        fin.outcome = rep.acquired_lease ? Rec::Outcome::Success : Rec::Outcome::NotALeader;
        fin.round = rep.round;
        fin.candidates_marked = rep.candidates;
        fin.objects_deleted = rep.deleted;
        fin.objects_absent = rep.absent;
        fin.objects_replaced = rep.replaced;
        fin.objects_spared = rep.spared;
        fin.manifests_deleted = rep.manifests_deleted;
        fin.entries_condemned = rep.condemned;
        fin.entries_graduated = rep.graduated;
        fin.entries_redeleted = rep.redeleted;
        fin.fence_outs = rep.fence_outs;
        fin.anomalies = rep.anomalies.size();
        fin.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        fin.profile_events = collect_profile_events();
        emit(fin);
        return rep;
    }
    catch (...)
    {
        fin.outcome = Rec::Outcome::Failed;
        fin.error = getCurrentExceptionMessage(false);
        fin.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        fin.profile_events = collect_profile_events();
        emit(fin);
        throw;
    }
}

Cas::RoundReport CasGcScheduler::runOneRoundNow(GcRoundLogRecord::Trigger trigger)
{
    std::lock_guard round_lock(gc_round_mutex);
    /// allow_steal=false: a manual round may acquire a FREE lease or renew ITS OWN, but must never
    /// steal a live incumbent — see Cas::Gc::runRegularRound's doc comment. Dead-incumbent recovery
    /// stays the loop's job (bounded ~2*interval; loop() below passes the default allow_steal=true).
    const Cas::RoundReport report = runRoundLogged(gc, trigger, [this] { onLeaseAcquired(); }, /*allow_steal=*/false);
    i_am_leader.store(report.acquired_lease, std::memory_order_relaxed);
    return report;
}

void CasGcScheduler::loop()
{
    setThreadName(ThreadName::CAS_GC_SCHEDULER);
    size_t consecutive_backoffs = 0;
    while (true)
    {
        {
            std::unique_lock lock(mutex);
            if (wake.wait_for(lock, interval, [this] { return stopping; }))
                return;
        }
        try
        {
            /// LOW/benign: if stop() flips `stopping` while we're blocked here (a concurrent manual
            /// round holds gc_round_mutex), we still run one more Scheduled round once it unblocks,
            /// before the next wait_for() observes `stopping` - an accepted extra round, not a
            /// correctness issue.
            std::lock_guard round_lock(gc_round_mutex);

            /// runRoundLogged emits the Start + Finish table rows (incl. the per-round
            /// ProfileEvents delta) and rethrows on a round exception (after an Aborted Finish).
            /// on_lease_acquired (onLeaseAcquired, shared with runOneRoundNow) fires the instant the
            /// lease is (re)acquired, before the fold runs - a new leader's first round is otherwise
            /// unprotected (i_am_leader would only flip below, AFTER the whole round returns), so a
            /// follower observing the frozen (owner, seq) across two of its own ticks would steal
            /// deterministically once that first round outlasts them. allow_steal defaults to true here
            /// (the loop is the ONLY caller allowed to execute the steal CAS).
            const Cas::RoundReport report = runRoundLogged(gc, GcRoundLogRecord::Trigger::Scheduled, [this] { onLeaseAcquired(); });
            i_am_leader.store(report.acquired_lease, std::memory_order_relaxed);
            if (report.acquired_lease)
            {
                consecutive_backoffs = 0;
                LOG_DEBUG(log, "CA GC round {}: candidates={} deleted={} absent={} replaced={} spared={} manifests_deleted={}",
                    report.round, report.candidates, report.deleted, report.absent,
                    report.replaced, report.spared, report.manifests_deleted);
            }
            else
            {
                /// NEVER silent: a follower backing off is the normal multi-mounter state, but a
                /// pool where this scheduler never leads must be observable. The lease layer
                /// handles safety, while these messages expose a liveness problem such as every
                /// round using a new scheduler identity and never accumulating the observations
                /// needed for dead-incumbent recovery.
                ++consecutive_backoffs;
                if (consecutive_backoffs % 10 == 0)
                    LOG_INFO(log, "CA GC: lease held by another mounter for {} consecutive ticks "
                        "(normal for a follower; investigate if no mounter is reclaiming)", consecutive_backoffs);
                else
                    LOG_TRACE(log, "CA GC: lease held by another mounter (tick {})", consecutive_backoffs);
            }
        }
        catch (...)
        {
            /// Idempotent round - the next tick retries; failures must never kill the pacing thread.
            /// runRoundLogged already emitted the Aborted Finish row before rethrowing.
            i_am_leader.store(false, std::memory_order_relaxed);
            tryLogCurrentException(log, "CA GC round failed (will retry next tick)");
        }
    }
}

void CasGcScheduler::heartbeatLoop()
{
    setThreadName(ThreadName::CAS_GC_HEARTBEAT);
    /// Advance the advisory heartbeat independently of round progress. A long round updates the
    /// durable lease only when it completes, so without these pulses a follower could mistake a
    /// live leader for a dead one during the observation window. A missed pulse is harmless because
    /// the heartbeat is advisory and the next cadence retries it.
    while (true)
    {
        {
            std::unique_lock lock(mutex);
            if (wake.wait_for(lock, hb_interval, [this] { return stopping; }))
                return;
        }
        if (!i_am_leader.load(std::memory_order_relaxed))
            continue;
        try
        {
            Cas::Gc::pulseHeartbeat(*store, gc_id);
        }
        catch (...)
        {
            tryLogCurrentException(log, "CA GC heartbeat pulse failed (advisory; will retry)");
        }
    }
}

CasGcScheduler::GcHealth CasGcScheduler::gcHealth() const
{
    GcHealth h;
    h.is_leader = i_am_leader.load(std::memory_order_relaxed);
    h.pending_reclaim = pending_reclaim.load(std::memory_order_relaxed);
    const UInt64 last_ms = last_success_ms.load(std::memory_order_relaxed);
    h.ever_succeeded = last_ms != 0;
    if (last_ms != 0)
    {
        const UInt64 now_ms = static_cast<UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        h.last_success_age_seconds = now_ms > last_ms ? (now_ms - last_ms) / 1000 : 0;
    }
    h.wedged_namespace_count = store->wedgedRefLaneCount();
    return h;
}

}
