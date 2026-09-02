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

    void loadFromDisk();

    static String compositeKey(const String & partition_id, const String & destination_database, const String & destination_table);

private:
    StorageMergeTree & storage;

    struct TaskEntry
    {
        /// Pins the source parts so they are not physically removed before the export finishes,
        /// including already-exported parts that Iceberg commit still reads for partition values.
        /// Cleared by `setDescriptor` when the installed snapshot is terminal.
        std::vector<DataPartPtr> part_references;
        /// Parts currently scheduled on the background move executor (avoids double scheduling).
        std::unordered_set<String> in_flight_parts;
        /// True while `tryCommit` is in the destination-commit / persist window. Callers must not
        /// write this flag; overlapping tryCommit calls no-op when it is already set.
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

        const MergeTreePartitionExportTask & getDescriptor() const { return descriptor; }

        /// Install a (typically just-persisted) snapshot. A terminal status drops the source-part
        /// pins so outdated parts can be physically removed without a restart.
        void setDescriptor(MergeTreePartitionExportTask new_descriptor)
        {
            descriptor = std::move(new_descriptor);
            if (descriptor.status != MergeTreePartitionExportTask::Status::PENDING)
                part_references.clear();
        }

    private:
        MergeTreePartitionExportTask descriptor;
    };

    mutable std::mutex mutex;
    std::map<String, TaskEntry> tasks;

    /// Renders a task key for logs and error messages. Unlike `compositeKey` this is not injective,
    /// so it must never be used as an identity.
    static String describeKey(const MergeTreePartitionExportTask & descriptor);

    void scheduleOnePart(const String & transaction_id, const String & part_name);
    void handlePartCompletion(const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result);
    void tryCommit(const String & transaction_id);

    /// Caller must hold `mutex`. `tasks.end()` if no task has this transaction id.
    std::map<String, TaskEntry>::iterator findByTransactionId(const String & transaction_id);

    /// Wall-clock timeout pass. Transitions expired PENDING tasks to KILLED (unless a commit is
    /// already in flight) and cancels their in-flight parts. Returns true if any PENDING work
    /// remains, so the caller should keep polling.
    bool enforceTimeouts();

    /// Caller must hold `mutex`. Persist `KILLED` with a timeout reason. Returns false if the
    /// local write fails (descriptor is left unchanged so the next tick retries).
    bool tryPersistTimeoutKill(const String & composite_key, TaskEntry & entry, time_t now);

    /// Atomically write `descriptor_json` to the task's on-disk file (tmp + replace), named by the
    /// composite key so a force-replace naturally overwrites the previous record. Always invoked
    /// while holding the registry `mutex` (write-through), so writes are serialized by that lock.
    void persist(const String & composite_key, const String & descriptor_json);

    String getExportsRelativePath() const;
};

}
