#pragma once

#include <map>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreePartExportManifest.h>
#include <Storages/MergeTree/MergeTreePartitionExportTask.h>

namespace DB
{

class StorageMergeTree;

/// Read-model row for `system.partition_exports`. Populated from the scheduler's in-memory registry
/// (no disk I/O). Mirrors the useful subset of the replicated info, without the replica-specific
/// columns that make no sense for a single-node table.
struct PartitionExportInfo
{
    String source_database;
    String source_table;
    String destination_database;
    String destination_table;
    time_t create_time = 0;
    String partition_id;
    String transaction_id;
    String query_id;
    std::vector<String> parts;
    size_t parts_count = 0;
    size_t parts_to_do = 0;
    String status;
    String last_exception_message;
    String last_exception_part;
    time_t last_exception_time = 0;
    size_t exception_count = 0;
};

/// Local (ZooKeeper-free) coordinator for `EXPORT PARTITION` on a plain `MergeTree` table.
///
/// A single node owns the whole task, so there are no locks, no cross-replica scheduling, and no
/// races: the registry below is the live source of truth, backed by one JSON descriptor file per
/// task on disk (so tasks resume after a restart). The registry is guarded by a plain mutex; all
/// slow I/O (disk persistence, the object-storage / Iceberg commit, and scheduling part exports)
/// is performed outside the lock.
class MergeTreePartitionExportScheduler
{
public:
    explicit MergeTreePartitionExportScheduler(StorageMergeTree & storage_);

    using DataPartPtr = MergeTreePartExportManifest::DataPartPtr;

    /// Registers and persists a new task, then triggers the scheduler. Throws
    /// EXPORT_PARTITION_ALREADY_EXPORTED if a task with the same (partition, destination) key
    /// already exists and `force` is false; otherwise the previous task (and its file) is replaced.
    void addTask(MergeTreePartitionExportTask descriptor, std::vector<DataPartPtr> part_references, bool force);

    /// Best-effort early duplicate check used before doing the heavy request-time validation.
    /// The authoritative atomic check happens inside addTask.
    void throwIfAlreadyExported(const String & composite_key, bool force) const;

    /// Cancels a PENDING task: flips its status to KILLED, persists, and cancels any in-flight
    /// part-export tasks for the transaction. Returns a CancellationCode for the KILL query.
    CancellationCode kill(const String & transaction_id);

    /// Snapshot of every tracked task for system.partition_exports. Briefly locks the registry.
    std::vector<PartitionExportInfo> getInfo() const;

    /// Scheduler tick: schedule pending parts of PENDING tasks and commit tasks whose parts are all
    /// exported. Invoked from the storage's schedule-pool task. Returns true if at least one task is
    /// still PENDING (so the caller should keep polling); false when there is no work left, letting
    /// the task go idle until the next addTask/startup trigger.
    bool run();

    /// Reloads persisted descriptors from disk and re-pins the parts of PENDING tasks. Called once
    /// during table startup, before background merges can remove parts.
    void loadFromDisk();

    static String compositeKey(const String & partition_id, const String & destination_database, const String & destination_table);

private:
    StorageMergeTree & storage;

    struct TaskEntry
    {
        MergeTreePartitionExportTask descriptor;
        /// Pins the source parts so they are not physically removed before the export finishes.
        std::vector<DataPartPtr> part_references;
        /// Parts currently scheduled on the background move executor (avoids double scheduling).
        std::unordered_set<String> in_flight_parts;
        /// True while a commit attempt is in progress, so run() and completion callbacks do not
        /// drive commitExportPartitionTransaction concurrently for the same task.
        bool committing = false;
    };

    mutable std::mutex mutex;
    std::map<String, TaskEntry> tasks;

    /// Serializes on-disk descriptor writes so two concurrent completion callbacks cannot interleave.
    std::mutex persist_mutex;

    void scheduleOnePart(const String & transaction_id, const String & part_name);
    void handlePartCompletion(const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result);
    void tryCommit(const String & transaction_id);

    /// Serialize `descriptor` to its on-disk file (atomic tmp + replace). No registry lock is held.
    void persist(const String & transaction_id, const String & descriptor_json);
    void removeTaskFile(const String & transaction_id);

    String getExportsRelativePath() const;
};

}
