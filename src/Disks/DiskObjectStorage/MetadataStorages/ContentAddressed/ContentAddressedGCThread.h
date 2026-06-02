#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>

#include <Core/BackgroundSchedulePoolTaskHolder.h>

#include <Interpreters/Context_fwd.h>

#include <Common/Logger.h>

#include <atomic>
#include <cstdint>
#include <memory>
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
    ContentAddressedGCThread(
        std::string disk_name_,
        ContextPtr context,
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        LoggerPtr log_);

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

    BackgroundSchedulePoolTaskHolder task;
};

using ContentAddressedGCThreadPtr = std::shared_ptr<ContentAddressedGCThread>;

}
