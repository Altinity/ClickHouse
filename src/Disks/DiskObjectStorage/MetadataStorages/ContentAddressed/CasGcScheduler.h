#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h>
#include <Common/ThreadPool.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
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

    /// Per-disk GC health for system.content_addressed_mounts (B3): the process-global CurrentMetrics
    /// gauges were clobbered with >= 2 CAS disks. All fields snapshot THIS scheduler's own state;
    /// wedged_namespace_count is read live from the store's ref lanes.
    struct GcHealth
    {
        bool is_leader = false;
        bool ever_succeeded = false;
        Int64 pending_reclaim = 0;             /// cumulative condemned - executed deletes (this process)
        UInt64 last_success_age_seconds = 0;   /// seconds since the last led round (0 if never)
        UInt64 wedged_namespace_count = 0;
    };
    GcHealth gcHealth() const;

private:
    void loop();
    void heartbeatLoop();   /// B160: bump gc/hb while we hold the lease, on a fast cadence (H <= W)

    /// B160/P3-B1 acquire-time hook: fired synchronously the instant the lease is acquired/renewed,
    /// BEFORE the round's (potentially long) fold begins — marks us leader and fires the first
    /// advisory heartbeat pulse immediately, so neither a new automatic leader's nor a manually-
    /// acquired lease's first (possibly long) round runs unprotected. Shared by BOTH loop() and
    /// runOneRoundNow() so a manual `SYSTEM ... GC` acquisition is heartbeat-protected exactly like an
    /// automatic one.
    void onLeaseAcquired();

    /// Run one round through the full logging path (Start record, ProfileEventsScope, Finish
    /// record). Used by BOTH loop() and runOneRoundNow. Logging is best-effort - the logger sink
    /// never throws into the round. Rethrows a round exception (after emitting an Aborted Finish).
    /// `allow_steal` is forwarded to `Cas::Gc::runRegularRound` verbatim (see its doc comment).
    Cas::RoundReport runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                     std::function<void()> on_lease_acquired = {}, bool allow_steal = true);

    const Cas::StorePtr store;
    const std::chrono::seconds interval;
    const std::chrono::milliseconds hb_interval;   /// B160: heartbeat cadence H = interval/4
    const LoggerPtr log;
    const UInt128 gc_id;
    const String disk_name;
    const GcRoundLogger logger;

    /// One persistent Gc for BOTH loop() and runOneRoundNow: the lease's observation-window steal
    /// protocol REQUIRES a stable observer (it compares the lease across consecutive runRegularRound
    /// calls of the same instance). A throwaway per call could never recover a dead-incumbent lease.
    Cas::Gc gc;
    /// Serializes the manual round against the background round so the two never touch the single
    /// (not-thread-safe) `gc` concurrently. Distinct from `mutex`: the loop releases `mutex` before
    /// the round so stop()/heartbeatLoop are not blocked, so the round cannot hold `mutex`.
    std::mutex gc_round_mutex;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    ThreadFromGlobalPool thread;
    std::atomic<bool> i_am_leader{false};   /// B160: set by the round thread, read by the heartbeat thread
    ThreadFromGlobalPool hb_thread;

    std::atomic<Int64> pending_reclaim{0};     /// B3: cumulative condemned - redeleted while leading
    std::atomic<UInt64> last_success_ms{0};    /// B3: steady-clock ms of the last led round; 0 = never
};

}
