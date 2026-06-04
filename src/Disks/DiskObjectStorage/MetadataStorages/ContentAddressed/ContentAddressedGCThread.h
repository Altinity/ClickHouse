#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>

#include <Core/BackgroundSchedulePoolTaskHolder.h>

#include <Interpreters/Context_fwd.h>

#include <Common/Logger.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
    /// `server_id_` is THIS mounter's id (the `ServerUUID`); it is the identity recorded in the
    /// per-pool GC-leader lock (`pool/gc.lock`) so a coordinated cross-mounter sweep can attribute and
    /// fence leadership. The background loop deletes ONLY while it holds that lock (see run).
    ContentAddressedGCThread(
        std::string disk_name_,
        ContextPtr context,
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        std::string server_id_,
        std::shared_ptr<std::mutex> gc_lock_,
        std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_,
        std::shared_ptr<ContentAddressed::InMemoryBlobRefIndex> blob_ref_index_,
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

    /// The object storage + key prefix + server id used to acquire/renew/release the per-pool GC-leader
    /// lock (`pool/gc.lock`). They mirror what the owned `gc` scans (single source of truth), kept here
    /// because the lock primitives (PoolCoordination) operate on the bucket directly, not through `gc`.
    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
    const std::string server_id;

    ContentAddressed::ContentAddressedGC gc;

    /// The GC-leader lock this thread currently holds across rounds while leading (nullopt when it does
    /// NOT lead). Only ever touched by the single schedule-pool task (run / shutdown), so no lock guards
    /// it. While set, each round renews it; if a successor stole leadership (renew fails), it is cleared
    /// and the round is skipped. Released best-effort on shutdown.
    std::optional<ContentAddressed::GcLock> held_lock;

    std::atomic<int64_t> finished_rounds{0};
    std::atomic<int64_t> interval_sec;
    std::atomic<int64_t> grace_sec;
    /// Opt-in gate for the recurring background sweep (default OFF). See startup.
    std::atomic<bool> background_enabled{false};

    BackgroundSchedulePoolTaskHolder task;
};

using ContentAddressedGCThreadPtr = std::shared_ptr<ContentAddressedGCThread>;

}
