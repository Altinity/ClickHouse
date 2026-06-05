#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>

#include <Interpreters/Context.h>

#include <Core/BackgroundSchedulePool.h>
#include <Interpreters/StorageID.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <Poco/Util/AbstractConfiguration.h>

#include <algorithm>
#include <chrono>

namespace DB
{

namespace
{

constexpr int64_t DEFAULT_GC_INTERVAL_SEC = 600;
constexpr int64_t DEFAULT_GC_GRACE_SEC = 3600;

/// Wall-clock (Unix) seconds. The sweep's `now` is fed to BOTH the in-process grace timer AND the
/// cross-process leases (write-session `lease_deadline_unix`, the `gc.lock` lease). Those leases are
/// written by other processes/servers, so the clock MUST be a shared wall-clock domain — a per-process
/// steady_clock is not comparable across mounters (and its epoch ≈ uptime would make every wall-clock
/// `lease_deadline_unix` look perpetually live, so a crashed writer's session pins would never expire).
/// The grace timer is a duration comparison, so wall-clock is fine for it too; `first_unreachable` is
/// in-memory (reset on restart), so a clock jump only shifts one grace window, never corrupts state.
int64_t wallClockSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

ContentAddressedGCThread::ContentAddressedGCThread(
    std::string disk_name_,
    ContextPtr context,
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    std::string server_id_,
    std::shared_ptr<std::mutex> gc_lock_,
    std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_,
    std::shared_ptr<ContentAddressed::InMemoryBlobRefIndex> blob_ref_index_,
    LoggerPtr log_)
    : disk_name(std::move(disk_name_))
    , log(std::move(log_))
    , object_storage(object_storage_)
    , key_prefix(key_prefix_)
    , server_id(std::move(server_id_))
    , gc(object_storage_, key_prefix_, std::move(gc_lock_), std::move(in_flight_pinned_blobs_), std::move(blob_ref_index_))
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
    const int64_t interval = interval_sec.load();

    /// The GC-leader lease (`pool/gc.lock`). It must comfortably outlive one sweep PLUS one round so a
    /// leader that is mid-sweep when the next round is due is not declared dead by a peer. We size it as
    /// a few intervals (and at least one grace window): the lease is only a LIVENESS hint — the fence
    /// token re-checked at delete time (`runSweepOnce(..., held)`) is the real safety authority — so a
    /// modest, generous lease is fine. Clamp to >= 1 so an interval of 0 (tests) still yields a live lease.
    const uint64_t lease_seconds = static_cast<uint64_t>(std::max<int64_t>(1, std::max(grace, interval * 4)));

    /// One consistent WALL-CLOCK domain for BOTH the sweep timers and the cross-process lease
    /// comparisons (write-session pins + the gc.lock lease are written by other servers). Reusing it keeps the lease deadlines
    /// this thread writes and re-reads coherent across rounds (steady, immune to wall-clock jumps).
    const int64_t now = wallClockSeconds();
    const auto now_unix = static_cast<uint64_t>(std::max<int64_t>(0, now));

    try
    {
        /// Acquire-or-renew leadership for THIS round. We delete ONLY while we hold the lock: at most one
        /// mounter sweeps a shared pool at a time, and a paused leader is fenced (a successor's higher
        /// fence stops our delete in runSweepOnce). If we do NOT lead this round we skip the sweep
        /// entirely — never sweep un-coordinated.
        if (held_lock)
        {
            /// Renew our existing lease. If a successor stole leadership (a higher fence on disk), renew
            /// fails: drop our stale lock and skip this round (we are no longer the leader).
            if (!ContentAddressed::renewGcLock(*object_storage, key_prefix, *held_lock, lease_seconds, now_unix))
            {
                LOG_INFO(log, "Content-addressed GC: lost leadership for disk {} (a peer took a higher fence); skipping this round", disk_name);
                held_lock.reset();
            }
        }
        else
        {
            /// Try to take (or steal an expired) leadership.
            held_lock = ContentAddressed::tryAcquireGcLock(*object_storage, key_prefix, server_id, lease_seconds, now_unix);
        }

        if (held_lock)
        {
            /// runSweepOnce is fail-close (deletes nothing if any step before removal throws), so a throw
            /// here leaves the pool intact; we only log and retry next round (fail-SAFE loop). The held
            /// lock is forwarded so the fence-ownership guard re-confirms leadership before each delete.
            auto stats = gc.runSweepOnce(now, grace, held_lock);
            LOG_TEST(log, "Content-addressed GC sweep removed {} parts, {} blobs", stats.deleted_parts, stats.deleted_blobs);
        }
        else
        {
            LOG_TEST(log, "Content-addressed GC: another mounter leads for disk {}; skipping this round", disk_name);
        }
    }
    catch (...)
    {
        tryLogCurrentException(log, "Content-addressed GC sweep failed; the pool is intact, will retry next round");
    }

    finished_rounds.fetch_add(1);
    finished_rounds.notify_all();

    task->scheduleAfter(interval * 1000);
}

void ContentAddressedGCThread::startup()
{
    /// Background deletion is OPT-IN (default OFF). Unattended sweeping is unsafe until pool-ownership
    /// is enforced (Phase 5 `_pool_meta`) by a single coordinator, so we only activate the recurring
    /// task when the disk config explicitly set `content_addressed_gc_enabled=true`. triggerAndWait
    /// (manual / test one-shot) still runs regardless of this flag.
    if (!background_enabled.load())
    {
        LOG_INFO(log, "Background content-addressed GC is disabled for disk {} (set content_addressed_gc_enabled=true to enable)", disk_name);
        return;
    }
    LOG_INFO(log, "Starting content-addressed GC thread for disk {}", disk_name);
    task->activateAndSchedule();
}

void ContentAddressedGCThread::shutdown()
{
    LOG_INFO(log, "Shutting down content-addressed GC thread for disk {}", disk_name);
    /// deactivate joins any in-flight round, so after it returns no run() touches held_lock concurrently.
    task->deactivate();

    /// Release leadership best-effort so a peer can take over promptly instead of waiting out the lease
    /// (an expired lock is stolen anyway, so this is only a liveness courtesy). releaseGcLock is itself
    /// a fenced read-check (it never deletes a successor's lock) and a missing lock is a no-op.
    if (held_lock)
    {
        try
        {
            ContentAddressed::releaseGcLock(*object_storage, key_prefix, *held_lock);
        }
        catch (...)
        {
            tryLogCurrentException(log, "Content-addressed GC: best-effort GC-lock release on shutdown failed");
        }
        held_lock.reset();
    }
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
    /// Default OFF: the recurring background sweep only runs when explicitly opted in (see startup).
    background_enabled = config.getBool(config_prefix + ".content_addressed_gc_enabled", false);
    /// §9 orphan-drift bound (default 0 = never): how often a sweep round also runs the heavy reconciliation
    /// scan so abandoned uploads / externally-mutated objects with no `gc/log` delta are bounded. Pushed
    /// into the GC so its `reconciliationDue` cadence counter honours it.
    reconciliation_cadence_rounds = config.getInt64(config_prefix + ".content_addressed_gc_reconciliation_cadence_rounds", 0);
    gc.setReconciliationCadenceRounds(reconciliation_cadence_rounds.load());
    LOG_INFO(log, "Applied content-addressed GC settings for disk {}: enabled={}, interval_sec={}, grace_sec={}, reconciliation_cadence_rounds={}",
             disk_name, background_enabled.load(), interval_sec.load(), grace_sec.load(), reconciliation_cadence_rounds.load());
}

}
