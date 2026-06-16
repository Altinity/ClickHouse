#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <algorithm>

namespace DB::ContentAddressed
{

CasGcScheduler::CasGcScheduler(Cas::StorePtr store_, std::chrono::seconds interval_, const String & log_name)
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
    thread = std::thread([this] { loop(); });
    hb_thread = std::thread([this] { heartbeatLoop(); });   /// B160
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

void CasGcScheduler::runOneRoundNow()
{
    Cas::Gc gc(store, gc_id);
    const Cas::RoundReport report = gc.runRegularRound();
    LOG_DEBUG(log, "CA GC round {}: lease={} candidates={} deleted={} absent={} replaced={} spared={} cascaded={}",
        report.round, report.acquired_lease, report.candidates, report.deleted, report.absent,
        report.replaced, report.spared, report.cascaded);
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
            const Cas::RoundReport report = gc.runRegularRound();
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
