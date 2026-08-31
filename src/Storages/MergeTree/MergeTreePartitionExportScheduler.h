#pragma once

#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreePartExportManifest.h>
#include <Storages/MergeTree/MergeTreePartitionExportTask.h>

namespace DB
{

class StorageMergeTree;


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

class MergeTreePartitionExportScheduler
{
public:
    explicit MergeTreePartitionExportScheduler(StorageMergeTree & storage_);

    using DataPartPtr = MergeTreePartExportManifest::DataPartPtr;

    /// Registers and persists a new task, then triggers the scheduler. Throws
    /// EXPORT_PARTITION_ALREADY_EXPORTED if a task with the same (partition, destination) key
    /// already exists and `force` is false; otherwise the previous task (and its file) is replaced.
    void addTask(MergeTreePartitionExportTask descriptor, std::vector<DataPartPtr> part_references, bool force);

    CancellationCode kill(const String & transaction_id);

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
        /// In-memory per-part retry back-off. Keyed by part name so a force-replace of the same
        /// composite key does not inherit a prior instance's delay. Not persisted: after restart
        /// the first retry is immediate, then back-off resumes from subsequent failures.
        struct PartBackoff
        {
            size_t attempts = 0;
            time_t next_retry_time = 0;
        };
        std::unordered_map<String, PartBackoff> part_backoff;
    };

    mutable std::mutex mutex;
    std::map<String, TaskEntry> tasks;

    void scheduleOnePart(const String & transaction_id, const String & part_name);
    void handlePartCompletion(const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result);
    void tryCommit(const String & transaction_id);

    /// Atomically write `descriptor_json` to the task's on-disk file (tmp + replace), named by the
    /// composite key so a force-replace naturally overwrites the previous record. Always invoked
    /// while holding the registry `mutex` (write-through), so writes are serialized by that lock.
    void persist(const String & composite_key, const String & descriptor_json);

    String getExportsRelativePath() const;
};

}
