#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h>
#include <Common/ThreadPool.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace DB::ContentAddressed
{

/// The thin GC pacing thread (M-W T10; design section 4 "a GC scheduler thread calling
/// Gc::runRegularRound"). All protocol safety lives in Cas::Gc - the lease makes concurrent
/// schedulers on other mounters safe (work dedup, split-brain-safe), so this thread carries NO
/// coordination of its own: tick, run one round, log, sleep. Round errors are logged and
/// swallowed - every step of the round is idempotent and the next tick retries; LOGICAL_ERROR
/// class failures surface loudly in the log.
///
/// The pacing thread runs as a ThreadFromGlobalPool (NOT a raw std::thread): each round is wrapped
/// in a ProfileEventsScope for the per-round ProfileEvents delta, and ProfileEventsScope requires
/// an attached ThreadStatus (CurrentThread::get throws LOGICAL_ERROR on a bare thread).
/// ThreadFromGlobalPool attaches one; the Manual path runs on the already-attached query thread.
class CasGcScheduler
{
public:
    CasGcScheduler(
        Cas::StorePtr store_,
        std::chrono::seconds interval_,
        const String & log_name,
        String disk_name_,
        GcRoundLogger logger_ = {});
    ~CasGcScheduler();

    void start();
    void stop();

    /// Test/diagnostics hook: run ONE round synchronously on the caller's thread. Returns the round
    /// report so the SYSTEM command / tests can inspect it. Emits a Start + Finish record.
    Cas::RoundReport runOneRoundNow(GcRoundLogRecord::Trigger trigger = GcRoundLogRecord::Trigger::Manual);

private:
    void loop();
    void heartbeatLoop();   /// B160: bump gc/hb while we hold the lease, on a fast cadence (H <= W)

    /// Run one round through the full logging path (Start record, ProfileEventsScope, Finish
    /// record). Used by BOTH loop() and runOneRoundNow. Logging is best-effort - the logger sink
    /// never throws into the round. Rethrows a round exception (after emitting an Aborted Finish).
    Cas::RoundReport runRoundLogged(Cas::Gc & gc, GcRoundLogRecord::Trigger trigger);

    const Cas::StorePtr store;
    const std::chrono::seconds interval;
    const std::chrono::milliseconds hb_interval;   /// B160: heartbeat cadence H = interval/4
    const LoggerPtr log;
    const UInt128 gc_id;
    const String disk_name;
    const GcRoundLogger logger;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    ThreadFromGlobalPool thread;
    std::atomic<bool> i_am_leader{false};   /// B160: set by the round thread, read by the heartbeat thread
    ThreadFromGlobalPool hb_thread;
};

}
