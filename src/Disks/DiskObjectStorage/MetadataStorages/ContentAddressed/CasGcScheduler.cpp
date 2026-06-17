#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ProfileEventsScope.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <algorithm>
#include <optional>

namespace DB::ContentAddressed
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
    Cas::StorePtr store_,
    std::chrono::seconds interval_,
    const String & log_name,
    String disk_name_,
    GcRoundLogger logger_)
    : store(std::move(store_))
    , interval(interval_)
    /// B160: heartbeat cadence H = interval/4 (>= 50ms), comfortably below the follower's observation
    /// window W (= interval), so a live leader's pulse always advances within W.
    , hb_interval(std::max<std::chrono::milliseconds>(
          std::chrono::milliseconds(50),
          std::chrono::duration_cast<std::chrono::milliseconds>(interval_) / 4))
    , log(getLogger(log_name))
    /// gc_id uniqueness across instances is the Gc caller obligation - a random u128 per scheduler.
    , gc_id((static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng())
    , disk_name(std::move(disk_name_))
    , logger(std::move(logger_))
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
    hb_thread = ThreadFromGlobalPool([this] { heartbeatLoop(); });   /// B160
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
    if (hb_thread.joinable())   /// B160
        hb_thread.join();
}

Cas::RoundReport CasGcScheduler::runRoundLogged(Cas::Gc & gc, GcRoundLogRecord::Trigger trigger)
{
    using Rec = GcRoundLogRecord;
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
        const Cas::RoundReport rep = gc.runRegularRound();
        fin.outcome = rep.acquired_lease ? Rec::Outcome::Success : Rec::Outcome::NotALeader;
        fin.round = rep.round;
        fin.candidates_marked = rep.candidates;
        fin.objects_deleted = rep.deleted;
        fin.objects_absent = rep.absent;
        fin.objects_replaced = rep.replaced;
        fin.objects_spared = rep.spared;
        fin.children_cascaded = rep.cascaded;
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
    Cas::Gc gc(store, gc_id);
    return runRoundLogged(gc, trigger);
}

void CasGcScheduler::loop()
{
    /// One Gc instance for the thread's lifetime: the lease's observation-window steal protocol
    /// REQUIRES a stable observer (it compares the lease across consecutive runRegularRound calls
    /// of the same instance).
    Cas::Gc gc(store, gc_id);
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
            /// runRoundLogged emits the Start + Finish table rows (incl. the per-round
            /// ProfileEvents delta) and rethrows on a round exception (after an Aborted Finish).
            const Cas::RoundReport report = runRoundLogged(gc, GcRoundLogRecord::Trigger::Scheduled);
            i_am_leader.store(report.acquired_lease, std::memory_order_relaxed);   /// B160: gate the heartbeat
            if (report.acquired_lease)
            {
                consecutive_backoffs = 0;
                LOG_DEBUG(log, "CA GC round {}: candidates={} deleted={} absent={} replaced={} spared={} cascaded={}",
                    report.round, report.candidates, report.deleted, report.absent,
                    report.replaced, report.spared, report.cascaded);
            }
            else
            {
                /// NEVER silent: a follower backing off is the normal multi-mounter state, but a
                /// pool where THIS scheduler never leads must be observable (the lease layer is
                /// outside the TLA+ model's liveness - a misuse that starves rounds, like the
                /// retro's fresh-gc_id-per-call test-hook bug, would otherwise log nothing).
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
            i_am_leader.store(false, std::memory_order_relaxed);   /// B160: a failed round => assume not leading
            tryLogCurrentException(log, "CA GC round failed (will retry next tick)");
        }
    }
}

void CasGcScheduler::heartbeatLoop()
{
    /// B160: while this node holds the lease, bump gc/hb every hb_interval (H <= W) so a follower
    /// never falsely steals from us while a long round freezes our lease.seq. Independent of round
    /// progress; advisory (a missed/lost pulse is harmless). Shares the stop signal with loop().
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

}
