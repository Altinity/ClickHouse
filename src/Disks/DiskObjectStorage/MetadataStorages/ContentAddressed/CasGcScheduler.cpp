#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>

namespace DB::ContentAddressed
{

CasGcScheduler::CasGcScheduler(Cas::StorePtr store_, std::chrono::seconds interval_, const String & log_name)
    : store(std::move(store_))
    , interval(interval_)
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
            tryLogCurrentException(log, "CA GC round failed (will retry next tick)");
        }
    }
}

}
