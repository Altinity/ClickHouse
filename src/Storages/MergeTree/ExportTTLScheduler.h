#pragma once

#include <Interpreters/Context_fwd.h>
#include <Interpreters/StorageID.h>
#include <Storages/ExportRetriedTask.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Common/CurrentMetrics.h>
#include <Common/Logger.h>
#include <base/types.h>

#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB
{

class MergeTreeData;

/// What `system.ttl_exports` shows about one partition of a table with a `TTL ... EXPORT` expression.
struct ExportTTLPartitionInfo
{
    String partition_id;
    String destination_database;
    String destination_table;
    size_t exported_parts = 0;
    size_t claimed_parts = 0;
    size_t eligible_parts = 0;
    size_t eligible_bytes = 0;
    /// Parts held back from the delete TTL because they are not exported yet.
    size_t parts_held_by_delete_gate = 0;
    /// When this replica first saw an eligible part that is not exported, 0 if there is none.
    time_t first_eligible_time = 0;
    /// When the next group can start at the latest, 0 if nothing waits.
    time_t next_group_time = 0;
    /// Transaction id of the task exporting the partition now, empty if there is none.
    String current_transaction_id;
    String last_error;
    /// The replica that schedules the TTL exports of the table, empty for a plain `MergeTree`.
    String scheduler_replica;
};

/// The background task of a table with a `TTL <expr> EXPORT TO TABLE <destination>` expression.
///
/// On every tick it ships groups of eligible parts (the maximum TTL value of their rows is due) to
/// the destination as export tasks of the table, one partition per group, and never exports a part
/// twice: what was exported is recorded in the export index (see `ExportTTLIndexEntry`), and the
/// merge fence keeps parts in different export states from merging.
///
/// Eligible parts of a partition are shipped once no new eligible part appeared for the batching
/// window, once the first of them waited for the maximum delay, or once they reach the size
/// threshold. A task that failed without committing keeps its parts claimed, and they are retried
/// first by the next group, which records in `retry_of` the failed tasks whose commit may still land.
///
/// Every replica observes the state of every partition, which `system.ttl_exports` shows, and
/// tracks the batching windows of the parts it has, so a replica that takes over the scheduling
/// continues them. Only the replica holding the scheduler lock acts: it resolves finished tasks and
/// starts groups.
///
/// Engines implement access to the index and to their export tasks.
class ExportTTLScheduler
{
public:
    explicit ExportTTLScheduler(MergeTreeData & storage_);
    virtual ~ExportTTLScheduler() = default;

    /// One tick. Returns the number of milliseconds until the next one.
    UInt64 run();

    std::vector<ExportTTLPartitionInfo> getInfo() const;

    /// Key in the export index of the current destination, empty if it is not known yet.
    String getDestinationKey() const;

protected:
    enum class TaskStatus : UInt8
    {
        PENDING,
        COMPLETED,
        FAILED,
        KILLED,
        /// No such task, e.g. a crash between claiming its parts and creating it.
        MISSING,
    };

    struct TaskState
    {
        TaskStatus status = TaskStatus::MISSING;
        /// Whether it exported all its parts, which it does before committing: a task that did not
        /// cannot have committed. Unknown counts as reached.
        bool reached_commit = true;
        ExportRetriedTasks retry_of;
    };

    struct GroupToStart
    {
        String transaction_id;
        String destination_key;
        StoragePtr destination;
        String partition_id;
        std::vector<MergeTreeDataPartPtr> parts;
        ExportRetriedTasks retry_of;
        /// The index entry with the claim of the group, to be stored with a check of its version.
        ExportTTLVersionedEntry entry;
    };

    /// Only one replica schedules, which keeps the replicas from conflicting on the index. The lock
    /// is kept until it is lost or the table shuts down. Correctness does not depend on it.
    virtual bool acquireSchedulerLock() = 0;

    /// E.g. `SYSTEM STOP MOVES`.
    virtual bool isPaused() = 0;

    virtual ExportTTLIndexSnapshotPtr getIndexSnapshot() = 0;

    /// Whether the key of the destination in the index contains its UUID, which tells apart a table
    /// that was dropped and created again. Replicas have different UUIDs for the same destination
    /// unless its database is `Replicated`, so they identify it by name only.
    virtual bool identifiesDestinationByUUID() const = 0;

    virtual String getReplicaName() const = 0;

    /// The replica holding the scheduler lock, empty if there is none.
    virtual String getSchedulerReplica() = 0;

    virtual TaskState getTaskState(const String & transaction_id) = 0;

    /// Whether a replica is in the commit of the task, e.g. one that started before the task failed.
    virtual bool isCommitInProgress(const String & transaction_id) = 0;

    /// Stores `entry` with a check of its version. Returns false on a conflict.
    virtual bool updateIndexEntry(const String & destination_key, const ExportTTLVersionedEntry & entry) = 0;

    /// Creates the export task of `group` and stores its index entry. Returns false on a conflict,
    /// e.g. a merge was assigned or the index changed meanwhile.
    virtual bool startGroup(const GroupToStart & group, const ContextPtr & context) = 0;

    /// Whether the part is a source of an assigned merge that changes its block range.
    virtual bool isPartBeingMerged(const MergeTreeDataPartPtr & part) = 0;

    virtual void killTask(const String & transaction_id) = 0;
    virtual void removeDestination(const String & destination_key) = 0;

    MergeTreeData & storage;
    const LoggerPtr log;

private:
    struct PartitionBatch
    {
        /// Block ranges of the eligible parts already seen, so a merge of seen parts is not new.
        std::vector<MergeTreePartInfo> seen_ranges;
        time_t first_eligible_time = 0;
        time_t last_new_part_time = 0;
    };

    /// What every replica can tell about a partition without acting on it.
    struct PartitionView
    {
        ExportTTLPartitionInfo info;
        PartitionBatch batch;
        std::vector<MergeTreeDataPartPtr> claimed_parts;
        /// Eligible parts that are neither exported nor claimed nor being merged, by block number.
        std::vector<MergeTreeDataPartPtr> shippable;
        bool batch_ready = false;
        /// When a part that is not due yet becomes eligible, 0 if there is none.
        time_t next_eligible_time = 0;
    };

    using TaskStates = std::unordered_map<String, TaskState>;

    mutable std::mutex mutex;
    /// By partition id, for the current destination.
    std::map<String, PartitionBatch> batches;
    std::map<String, String> last_errors;
    std::map<String, ExportTTLPartitionInfo> info_by_partition;
    String current_destination_key;
    String current_destination_error;

    /// False once there is neither an `EXPORT` TTL nor an index left, so ticks do no Keeper reads.
    bool may_have_index = true;

    CurrentMetrics::Increment parts_held_by_delete_gate;

    ContextPtr makeContext() const;

    /// Resolves the claim of the index entry unless its task is in flight: a task that completed, or
    /// that failed but committed to the destination, has its claim moved to the exported ranges.
    struct ResolvedClaim
    {
        /// The task in flight, or the failed task whose parts are still claimed; empty if there is none.
        String in_flight;
        String failed;
        bool changed = false;
    };
    ResolvedClaim resolveClaim(ExportTTLIndexEntry & entry, const StoragePtr & destination, TaskStates & task_states, const ContextPtr & context);

    /// The task `failed`, which holds the claim of `entry`, and the ones it retried whose commit may
    /// still land, for the `retry_of` of the group that retries its parts.
    ExportRetriedTasks collectRetriedTasks(
        const ExportTTLIndexEntry & entry,
        const String & failed,
        const StoragePtr & destination,
        time_t now,
        TaskStates & task_states,
        const ContextPtr & context);

    const TaskState & getCachedTaskState(TaskStates & task_states, const String & transaction_id);

    /// Kills the TTL tasks of a destination that is no longer the destination of the TTL, and
    /// removes its index once none of them holds a claim.
    void cleanupDestination(const String & destination_key, const std::map<String, ExportTTLVersionedEntry> & index);

    /// Read-only, except for the batching window of the partition, which it returns in the view.
    PartitionView observePartition(
        const ExportTTLIndexEntry & entry,
        const std::vector<MergeTreeDataPartPtr> & parts,
        const TTLDescriptions & export_ttls,
        time_t now,
        PartitionBatch batch,
        TaskStates & task_states);

    /// On the replica that schedules: resolves finished tasks and starts a group if one is due.
    /// Returns the index entry of the partition if it was changed.
    std::optional<ExportTTLIndexEntry> actOnPartition(
        const String & destination_key,
        const StoragePtr & destination,
        ExportTTLVersionedEntry versioned,
        PartitionView & view,
        time_t now,
        size_t & in_flight,
        TaskStates & task_states,
        const ContextPtr & context);
};

}
