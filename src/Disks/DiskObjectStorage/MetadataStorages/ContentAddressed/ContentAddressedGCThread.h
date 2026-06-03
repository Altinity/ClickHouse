#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>

#include <Core/BackgroundSchedulePoolTaskHolder.h>

#include <Interpreters/Context_fwd.h>

#include <Common/Logger.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace Poco::Util
{
    class AbstractConfiguration;
}

namespace DB
{

/// Background driver that runs the content-addressed pool garbage collector on a per-disk schedule.
/// Mirrors BlobKillerThread: it owns a BackgroundSchedulePoolTaskHolder obtained from the global
/// schedule pool and the lifecycle is driven by the disk (startup activates and schedules the task,
/// shutdown deactivates it). Each round calls ContentAddressedGC::runSweepOnce inside a catch-all so
/// the loop is fail-SAFE: a sweep exception is logged and the loop retries next round, and because
/// runSweepOnce is itself fail-close it deletes nothing when it throws (the pool stays intact).
class ContentAddressedGCThread
{
public:
    /// `gc_lock_` is the per-pool in-process GC mutex shared with the transaction commit path (B49);
    /// it is forwarded to the owned ContentAddressedGC so the sweep and commits mutually exclude.
    /// `in_flight_pinned_blobs_` is the per-pool set of blob keys staged by uncommitted transactions
    /// (B52), shared by reference so the sweep treats them as reachable (guarded by `gc_lock_`).
    ContentAddressedGCThread(
        std::string disk_name_,
        ContextPtr context,
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        std::shared_ptr<std::mutex> gc_lock_,
        std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_,
        LoggerPtr log_);

    /// Background deletion is OPT-IN. startup activates and schedules the recurring sweep ONLY when
    /// the disk config sets `content_addressed_gc_enabled=true` (see setEnabled / applyNewSettings);
    /// it is a no-op otherwise. Unattended background deletion is unsafe until pool-ownership is
    /// enforced (Phase 5 `_pool_meta`) by a single coordinator, so it is disabled by default.
    /// triggerAndWait (manual / test one-shot) runs a sweep regardless of this flag.
    void startup();
    void shutdown();

    /// Synchronous one-shot for tests: schedule immediately and wait for the round counter to advance
    /// (mirrors BlobKillerThread::triggerAndWait). Uses the scheduler + round counter, never a sleep.
    void triggerAndWait();

    void applyNewSettings(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix);

private:
    void run();
    void waitRound(int64_t expected_round);

    const std::string disk_name;
    const LoggerPtr log;

    ContentAddressed::ContentAddressedGC gc;

    std::atomic<int64_t> finished_rounds{0};
    std::atomic<int64_t> interval_sec;
    std::atomic<int64_t> grace_sec;
    /// Opt-in gate for the recurring background sweep (default OFF). See startup.
    std::atomic<bool> background_enabled{false};

    BackgroundSchedulePoolTaskHolder task;
};

using ContentAddressedGCThreadPtr = std::shared_ptr<ContentAddressedGCThread>;

}
