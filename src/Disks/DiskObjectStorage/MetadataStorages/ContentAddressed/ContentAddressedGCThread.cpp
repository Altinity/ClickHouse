#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>

#include <Interpreters/Context.h>

#include <Core/BackgroundSchedulePool.h>
#include <Interpreters/StorageID.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <Poco/Util/AbstractConfiguration.h>

#include <chrono>

namespace DB
{

namespace
{

constexpr int64_t DEFAULT_GC_INTERVAL_SEC = 600;
constexpr int64_t DEFAULT_GC_GRACE_SEC = 3600;

/// Monotonic wall-clock-independent seconds. The grace timer in ContentAddressedGC measures the
/// duration an object has stayed unreferenced across sweeps; a steady clock makes that immune to
/// system clock jumps.
int64_t monotonicSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

ContentAddressedGCThread::ContentAddressedGCThread(
    std::string disk_name_,
    ContextPtr context,
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    LoggerPtr log_)
    : disk_name(std::move(disk_name_))
    , log(std::move(log_))
    , gc(std::move(object_storage_), std::move(key_prefix_))
    , interval_sec(DEFAULT_GC_INTERVAL_SEC)
    , grace_sec(DEFAULT_GC_GRACE_SEC)
{
    task = context->getSchedulePool().createTask(StorageID::createEmpty(), log->name(), [this]() { run(); });
    task->deactivate();
}

void ContentAddressedGCThread::run()
{
    LOG_TEST(log, "Starting content-addressed GC sweep");

    const int64_t grace = grace_sec.load();
    try
    {
        /// runSweepOnce is fail-close (deletes nothing if any step before removal throws), so a throw
        /// here leaves the pool intact; we only log and retry next round (fail-SAFE loop).
        auto stats = gc.runSweepOnce(monotonicSeconds(), grace);
        LOG_TEST(log, "Content-addressed GC sweep removed {} parts, {} blobs", stats.deleted_parts, stats.deleted_blobs);
    }
    catch (...)
    {
        tryLogCurrentException(log, "Content-addressed GC sweep failed; the pool is intact, will retry next round");
    }

    finished_rounds.fetch_add(1);
    finished_rounds.notify_all();

    const int64_t interval = interval_sec.load();
    task->scheduleAfter(interval * 1000);
}

void ContentAddressedGCThread::startup()
{
    LOG_INFO(log, "Starting content-addressed GC thread for disk {}", disk_name);
    task->activateAndSchedule();
}

void ContentAddressedGCThread::shutdown()
{
    LOG_INFO(log, "Shutting down content-addressed GC thread for disk {}", disk_name);
    task->deactivate();
}

void ContentAddressedGCThread::waitRound(int64_t expected_round)
{
    int64_t current_round = finished_rounds.load();
    while (current_round < expected_round)
    {
        finished_rounds.wait(current_round);
        current_round = finished_rounds.load();
    }
}

void ContentAddressedGCThread::triggerAndWait()
{
    int64_t expected_round = finished_rounds.load() + 1;
    task->activateAndSchedule();
    waitRound(expected_round);
}

void ContentAddressedGCThread::applyNewSettings(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix)
{
    interval_sec = config.getInt64(config_prefix + ".content_addressed_gc_interval_sec", DEFAULT_GC_INTERVAL_SEC);
    grace_sec = config.getInt64(config_prefix + ".content_addressed_gc_grace_sec", DEFAULT_GC_GRACE_SEC);
    LOG_INFO(log, "Applied content-addressed GC settings for disk {}: interval_sec={}, grace_sec={}", disk_name, interval_sec.load(), grace_sec.load());
}

}
