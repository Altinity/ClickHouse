#pragma once

#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreePartExportManifest.h>
#include <Storages/MergeTree/MergeTreeExportTask.h>
#include <Storages/MergeTree/ExportTaskInfo.h>

namespace DB
{

class StorageMergeTree;


class MergeTreeExportTaskScheduler
{
public:
    explicit MergeTreeExportTaskScheduler(StorageMergeTree & storage_);

    using DataPartPtr = MergeTreePartExportManifest::DataPartPtr;

    /// Registers and persists a new task, then triggers the scheduler.
    void addTask(MergeTreeExportTask descriptor, std::vector<DataPartPtr> part_references);

    CancellationCode kill(const String & transaction_id);

    /// A copy of the descriptor of the task, or nothing if there is no such task.
    std::optional<MergeTreeExportTask> getTask(const String & transaction_id) const;

    std::vector<ExportTaskInfo> getInfo() const;

    /// Scheduler tick: schedule pending parts of PENDING tasks and commit tasks whose parts are all
    /// exported. Invoked from the storage's schedule-pool task. Returns true if at least one task is
    /// still PENDING (so the caller should keep polling); false when there is no work left, letting
    /// the task go idle until the next addTask/startup trigger.
    bool run();

    void load();

private:
    StorageMergeTree & storage;

    struct TaskEntry
    {
        /// Pins the source parts so they are not physically removed
        std::vector<DataPartPtr> part_references;
        /// Parts currently scheduled on the background move executor (avoids double scheduling).
        std::unordered_set<String> in_flight_parts;
        /// True while `tryCommit` is in the destination-commit / persist window. Callers must not
        /// write this flag; overlapping tryCommit calls no-op when it is already set.
        bool committing = false;
        /// In-memory per-part retry back-off, keyed by part name. Not persisted: after restart the
        /// first retry is immediate, then back-off resumes from subsequent failures.
        struct PartBackoff
        {
            size_t attempts = 0;
            time_t next_retry_time = 0;
        };
        std::unordered_map<String, PartBackoff> part_backoff;

        const MergeTreeExportTask & getDescriptor() const { return descriptor; }

        /// Install a (typically just-persisted) snapshot. A terminal status drops the source-part
        /// pins so outdated parts can be physically removed without a restart.
        void setDescriptor(MergeTreeExportTask new_descriptor)
        {
            descriptor = std::move(new_descriptor);
            if (descriptor.status != MergeTreeExportTask::Status::PENDING)
                part_references.clear();
        }

    private:
        MergeTreeExportTask descriptor;
    };

    mutable std::mutex mutex;
    /// By transaction id.
    std::map<String, TaskEntry> tasks;

    /// The `EXPORT` TTL records a finished task of its own as exported, or retries it, right away.
    void wakeUpExportTTLIfFinished(const TaskEntry & entry);

    void scheduleOnePart(const String & transaction_id, const String & part_name);
    void handlePartCompletion(const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result);
    void tryCommit(const String & transaction_id);


    /// Wall-clock timeout pass. Transitions expired PENDING tasks to KILLED (unless a commit is
    /// already in flight) and cancels their in-flight parts. Returns true if any PENDING work
    /// remains, so the caller should keep polling.
    bool enforceTimeouts();

    /// Caller must hold `mutex`. Persist `KILLED` with a timeout reason. Returns false if the
    /// local write fails (descriptor is left unchanged so the next tick retries).
    bool tryPersistTimeoutKill(const String & transaction_id, TaskEntry & entry, time_t now);

    /// Atomically write `descriptor_json` to the task's on-disk file (tmp + replace). Always invoked
    /// while holding the registry `mutex` (write-through), so writes are serialized by that lock.
    void persist(const String & transaction_id, const String & descriptor_json);

    /// Relative path of the descriptor file of the task, `exports/<transaction_id>.json`.
    String descriptorRelativePath(const String & transaction_id) const;

    String getExportsRelativePath() const;
};

}
