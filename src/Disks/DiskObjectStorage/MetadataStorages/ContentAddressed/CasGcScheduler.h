#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace DB::ContentAddressed
{

/// The thin GC pacing thread (M-W T10; design section 4 "a GC scheduler thread calling
/// Gc::runRegularRound"). All protocol safety lives in Cas::Gc - the lease makes concurrent
/// schedulers on other mounters safe (work dedup, split-brain-safe), so this thread carries NO
/// coordination of its own: tick, run one round, log, sleep. Round errors are logged and
/// swallowed - every step of the round is idempotent and the next tick retries; LOGICAL_ERROR
/// class failures surface loudly in the log.
class CasGcScheduler
{
public:
    CasGcScheduler(Cas::StorePtr store_, std::chrono::seconds interval_, const String & log_name);
    ~CasGcScheduler();

    void start();
    void stop();

    /// Test/diagnostics hook: run ONE round synchronously on the caller's thread.
    void runOneRoundNow();

private:
    void loop();
    void heartbeatLoop();   /// B160: bump gc/hb while we hold the lease, on a fast cadence (H <= W)

    const Cas::StorePtr store;
    const std::chrono::seconds interval;
    const std::chrono::milliseconds hb_interval;   /// B160: heartbeat cadence H = interval/4
    const LoggerPtr log;
    const UInt128 gc_id;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread thread;
    std::atomic<bool> i_am_leader{false};   /// B160: set by the round thread, read by the heartbeat thread
    std::thread hb_thread;
};

}
