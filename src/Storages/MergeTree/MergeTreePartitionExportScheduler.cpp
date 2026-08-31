#include <Storages/MergeTree/MergeTreePartitionExportScheduler.h>
#include <Storages/StorageMergeTree.h>
#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/CancellationCode.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/SipHash.h>
#include <Common/logger_useful.h>
#include <Disks/IDisk.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadBufferFromFileBase.h>
#include <Core/Defines.h>
#include <base/types.h>
#include <algorithm>
#include <limits>

#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int EXPORT_PARTITION_ALREADY_EXPORTED;
    extern const int CORRUPTED_DATA;
    extern const int QUERY_WAS_CANCELLED;
    extern const int UNKNOWN_TABLE;
}

MergeTreePartitionExportScheduler::MergeTreePartitionExportScheduler(StorageMergeTree & storage_)
    : storage(storage_)
{
}

String MergeTreePartitionExportScheduler::compositeKey(
    const String & partition_id, const String & destination_database, const String & destination_table)
{
    const QualifiedTableName qualified_table_name{destination_database, destination_table};
    return partition_id + "_" + qualified_table_name.getFullName();
}

String MergeTreePartitionExportScheduler::getExportsRelativePath() const
{
    return fs::path(storage.getRelativeDataPath()) / "partition_exports";
}

void MergeTreePartitionExportScheduler::addTask(
    MergeTreePartitionExportTask descriptor, std::vector<DataPartPtr> part_references, bool force)
{
    const auto composite_key = compositeKey(descriptor.partition_id, descriptor.destination_database, descriptor.destination_table);
    const auto transaction_id = descriptor.transaction_id;

    String previous_transaction_id;

    {
        std::lock_guard lock(mutex);

        if (const auto it = tasks.find(composite_key); it != tasks.end())
        {
            if (!force)
                throw Exception(ErrorCodes::EXPORT_PARTITION_ALREADY_EXPORTED,
                    "Export with key {} already exported or it is being exported. "
                    "Set `export_merge_tree_partition_force_export` to overwrite it.",
                    composite_key);

            previous_transaction_id = it->second.getDescriptor().transaction_id;
        }

        TaskEntry entry;
        entry.part_references = std::move(part_references);
        entry.setDescriptor(std::move(descriptor));
        persist(composite_key, entry.getDescriptor().toJsonString());
        tasks.insert_or_assign(composite_key, std::move(entry));
    }

    if (!previous_transaction_id.empty())
    {
        LOG_INFO(storage.log, "ExportPartition: overwriting export with key {}", composite_key);
        storage.killExportPart(previous_transaction_id);
    }

    LOG_INFO(storage.log, "ExportPartition: scheduled export task {} (key {})", transaction_id, composite_key);
    storage.triggerPartitionExportTask();
}

CancellationCode MergeTreePartitionExportScheduler::kill(const String & transaction_id)
{
    /// set the status to killed
    {
        std::lock_guard lock(mutex);

        /// `tasks` is keyed by (partition, destination); KILL arrives with a transaction_id.
        TaskEntry * entry = nullptr;
        const String * composite_key = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.getDescriptor().transaction_id == transaction_id)
            {
                composite_key = &key;
                entry = &candidate;
                break;
            }

        if (!entry)
            return CancellationCode::NotFound;

        if (entry->getDescriptor().status != MergeTreePartitionExportTask::Status::PENDING)
            return CancellationCode::CancelCannotBeSent;

        if (entry->committing)
        {
            LOG_INFO(storage.log, "ExportPartition: commit in progress for {}, cannot cancel export partition task", transaction_id);
            return CancellationCode::CancelCannotBeSent;
        }

        auto updated = entry->getDescriptor();

        updated.status = MergeTreePartitionExportTask::Status::KILLED;
        persist(*composite_key, updated.toJsonString());
        entry->setDescriptor(std::move(updated));
    }

    /// cancel in-flight operations
    storage.killExportPart(transaction_id);

    return CancellationCode::CancelSent;
}

std::vector<PartitionExportInfo> MergeTreePartitionExportScheduler::getInfo() const
{
    std::vector<PartitionExportInfo> result;
    std::lock_guard lock(mutex);
    result.reserve(tasks.size());
    for (const auto & [key, entry] : tasks)
    {
        const auto & descriptor = entry.getDescriptor();
        PartitionExportInfo info;
        info.source_database = descriptor.source_database;
        info.source_table = descriptor.source_table;
        info.destination_database = descriptor.destination_database;
        info.destination_table = descriptor.destination_table;
        info.create_time = descriptor.create_time;
        info.partition_id = descriptor.partition_id;
        info.transaction_id = descriptor.transaction_id;
        info.query_id = descriptor.query_id;
        info.parts = descriptor.partNames();
        info.parts_count = descriptor.partsCount();
        info.parts_to_do = descriptor.partsToDo();
        info.status = String(magic_enum::enum_name(descriptor.status));
        info.last_exception_message = descriptor.last_exception.message;
        info.last_exception_part = descriptor.last_exception.part;
        info.last_exception_time = descriptor.last_exception.time;
        info.exception_count = descriptor.last_exception.count;
        result.push_back(std::move(info));
    }
    return result;
}

namespace
{
    bool isTimedOut(const MergeTreePartitionExportTask & descriptor, time_t now)
    {
        return descriptor.task_timeout_seconds > 0
            && descriptor.create_time + static_cast<time_t>(descriptor.task_timeout_seconds) < now;
    }
}

bool MergeTreePartitionExportScheduler::tryPersistTimeoutKill(const String & composite_key, TaskEntry & entry, time_t now)
{
    const auto transaction_id = entry.getDescriptor().transaction_id;
    const auto timeout_seconds = entry.getDescriptor().task_timeout_seconds;

    auto updated = entry.getDescriptor();
    updated.status = MergeTreePartitionExportTask::Status::KILLED;
    updated.last_exception.message = fmt::format(
        "Export partition task timed out: exceeded export_merge_tree_partition_task_timeout_seconds={} (created at {}, now {})",
        timeout_seconds, entry.getDescriptor().create_time, now);
    updated.last_exception.part = "";
    updated.last_exception.time = now;
    updated.last_exception.count += 1;

    try
    {
        persist(composite_key, updated.toJsonString());
    }
    catch (...)
    {
        tryLogCurrentException(storage.log, "ExportPartition: failed to persist timeout kill, will retry");
        return false;
    }

    entry.setDescriptor(std::move(updated));
    LOG_WARNING(storage.log,
        "ExportPartition: task {} exceeded task_timeout_seconds={}s, transitioned PENDING -> KILLED",
        transaction_id, timeout_seconds);
    return true;
}

bool MergeTreePartitionExportScheduler::enforceTimeouts()
{
    std::vector<String> timed_out_transactions;
    bool any_pending = false;

    {
        std::lock_guard lock(mutex);
        const auto now = time(nullptr);
        for (auto & [key, entry] : tasks)
        {
            if (entry.getDescriptor().status != MergeTreePartitionExportTask::Status::PENDING)
                continue;

            if (!isTimedOut(entry.getDescriptor(), now))
            {
                any_pending = true;
                continue;
            }

            /// A commit already in flight may have landed on the destination; do not overwrite
            /// it with KILLED (same as a user KILL that sees `committing`).
            if (entry.committing || !tryPersistTimeoutKill(key, entry, now))
            {
                any_pending = true;
                continue;
            }

            timed_out_transactions.push_back(entry.getDescriptor().transaction_id);
        }
    }

    for (const auto & transaction_id : timed_out_transactions)
        storage.killExportPart(transaction_id);

    return any_pending;
}

bool MergeTreePartitionExportScheduler::run()
{
    /// Timeouts first, before the "cannot make progress this tick" early returns, so a wedged
    /// task (no move executors, memory pressure, ...) still expires.
    if (!enforceTimeouts())
        return false;

    /// There is pending work. Even if we cannot make progress this tick (no move executors, moves
    /// stopped, or memory pressure), keep the scheduler awake so it retries on the next tick.
    const auto available_move_executors = storage.background_moves_assignee.getAvailableMoveExecutors();
    if (available_move_executors == 0)
        return true;

    if (storage.parts_mover.moves_blocker.isCancelled())
        return true;

    /// Respect the background memory soft-limit like the per-part export path does.
    if (!canEnqueueBackgroundTask())
        return true;

    std::vector<std::pair<String, String>> parts_to_schedule;  /// (transaction_id, part_name)
    std::vector<String> tasks_to_commit;

    {
        std::lock_guard lock(mutex);
        const auto now = time(nullptr);
        size_t scheduled = 0;
        for (auto & [key, entry] : tasks)
        {
            const auto & descriptor = entry.getDescriptor();
            if (descriptor.status != MergeTreePartitionExportTask::Status::PENDING)
                continue;

            if (descriptor.allPartsDone())
            {
                /// All parts exported: commit (or retry a previously-failed commit). Guard with the
                /// committing flag so a concurrent completion callback does not commit in parallel.
                if (!entry.committing)
                {
                    entry.committing = true;
                    tasks_to_commit.push_back(descriptor.transaction_id);
                }
                continue;
            }

            for (const auto & part : descriptor.parts)
            {
                if (scheduled >= available_move_executors)
                    break;
                if (part.done || entry.in_flight_parts.contains(part.part_name))
                    continue;
                if (const auto backoff_it = entry.part_backoff.find(part.part_name);
                    backoff_it != entry.part_backoff.end() && now < backoff_it->second.next_retry_time)
                    continue;

                entry.in_flight_parts.insert(part.part_name);
                parts_to_schedule.emplace_back(descriptor.transaction_id, part.part_name);
                ++scheduled;
            }

            if (scheduled >= available_move_executors)
                break;
        }
    }

    for (const auto & [transaction_id, part_name] : parts_to_schedule)
        scheduleOnePart(transaction_id, part_name);

    for (const auto & transaction_id : tasks_to_commit)
        tryCommit(transaction_id);

    /// A task was PENDING this tick (either scheduled/committed above, or waiting on in-flight parts
    /// or a retry). Keep polling until every task reaches a terminal state.
    return true;
}

void MergeTreePartitionExportScheduler::scheduleOnePart(const String & transaction_id, const String & part_name)
{
    MergeTreePartitionExportTask descriptor_copy;
    {
        std::lock_guard lock(mutex);
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.getDescriptor().transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;
        descriptor_copy = entry->getDescriptor();
    }

    const StorageID destination_storage_id{descriptor_copy.destination_database, descriptor_copy.destination_table};

    auto release_in_flight = [this, transaction_id, part_name]()
    {
        std::lock_guard lock(mutex);
        for (auto & [key, entry] : tasks)
            if (entry.getDescriptor().transaction_id == transaction_id)
            {
                entry.in_flight_parts.erase(part_name);
                break;
            }
    };

    try
    {
        auto context = ExportPartitionUtils::getContextCopyWithTaskSettings(storage.getContext(), descriptor_copy);

        LOG_INFO(storage.log, "ExportPartition: scheduling part export {} for task {}", part_name, transaction_id);

        storage.exportPartToTable(
            part_name,
            destination_storage_id,
            transaction_id,
            context,
            descriptor_copy.iceberg_metadata_json,
            /*allow_outdated_parts*/ true,
            [this, transaction_id, part_name](MergeTreePartExportManifest::CompletionCallbackResult result)
            {
                handlePartCompletion(transaction_id, part_name, result);
            });
    }
    catch (...)
    {
        tryLogCurrentException(storage.log, __PRETTY_FUNCTION__);
        /// Dispatch failed (e.g. destination missing, executor busy). Release the in-flight marker
        /// so the part becomes eligible again on the next tick.
        release_in_flight();
    }
}

void MergeTreePartitionExportScheduler::handlePartCompletion(
    const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result)
{
    bool ready_to_commit = false;

    {
        std::lock_guard lock(mutex);
        const String * composite_key = nullptr;
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.getDescriptor().transaction_id == transaction_id)
            {
                composite_key = &key;
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        entry->in_flight_parts.erase(part_name);

        /// Task already terminal (KILLED / FAILED / COMPLETED): ignore late completions.
        if (entry->getDescriptor().status != MergeTreePartitionExportTask::Status::PENDING)
            return;

        /// A cancelled export (KILL, or SYSTEM STOP MOVES) is not a real failure: leave the part
        /// pending. If it was a KILL the status is already handled above; otherwise the next tick
        /// retries it.
        if (!result.success && result.exception && result.exception->code() == ErrorCodes::QUERY_WAS_CANCELLED)
            return;

        /// Persist-then-apply under the lock: mutate a copy, write it durably, then swap it in. If
        /// the write throws we leave the in-memory descriptor unchanged (so the part is retried) and
        /// swallow the error -- a completion callback must not surface a local disk error as a part
        /// failure.
        auto updated = entry->getDescriptor();

        if (result.success)
        {
            if (auto * part = updated.findPart(part_name))
            {
                part->done = true;
                part->paths_in_destination = result.relative_paths_in_destination_storage;
            }
        }
        else
        {
            updated.last_exception.message = result.exception ? result.exception->message() : "Unknown export failure";
            updated.last_exception.part = part_name;
            updated.last_exception.time = time(nullptr);
            updated.last_exception.count += 1;

            if (result.exception && ExportPartitionUtils::isNonRetryableExportError(result.exception->code()))
                updated.status = MergeTreePartitionExportTask::Status::FAILED;
        }

        try
        {
            persist(*composite_key, updated.toJsonString());
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "ExportPartition: failed to persist part-completion state, will retry");
            return;
        }

        entry->setDescriptor(std::move(updated));

        if (result.success)
        {
            entry->part_backoff.erase(part_name);
            if (entry->getDescriptor().allPartsDone() && !entry->committing)
            {
                entry->committing = true;
                ready_to_commit = true;
            }
        }
        else if (entry->getDescriptor().status == MergeTreePartitionExportTask::Status::FAILED)
        {
            LOG_WARNING(storage.log, "ExportPartition: task {} failed on part {} with non-retryable error",
                transaction_id, part_name);
        }
        else
        {
            auto & backoff = entry->part_backoff[part_name];
            ++backoff.attempts;
            const auto backoff_seconds = ExportPartitionUtils::computeRetryBackoffSeconds(
                backoff.attempts,
                entry->getDescriptor().retry_initial_backoff_seconds,
                entry->getDescriptor().retry_max_backoff_seconds);
            const auto now = time(nullptr);
            const size_t headroom = static_cast<size_t>(std::numeric_limits<time_t>::max() - now);
            backoff.next_retry_time = now + static_cast<time_t>(std::min(backoff_seconds, headroom));

            LOG_INFO(storage.log, "ExportPartition: task {} part {} failed with retryable error, will retry at {}",
                transaction_id, part_name, backoff.next_retry_time);
        }
    }

    if (ready_to_commit)
        tryCommit(transaction_id);
}

void MergeTreePartitionExportScheduler::tryCommit(const String & transaction_id)
{
    MergeTreePartitionExportTask descriptor_copy;
    {
        std::lock_guard lock(mutex);
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.getDescriptor().transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        if (entry->getDescriptor().status != MergeTreePartitionExportTask::Status::PENDING || !entry->getDescriptor().allPartsDone())
        {
            entry->committing = false;
            return;
        }

        entry->committing = true;
        descriptor_copy = entry->getDescriptor();
    }

    const StorageID destination_storage_id{descriptor_copy.destination_database, descriptor_copy.destination_table};

    bool success = false;
    std::optional<Exception> failure;
    try
    {
        auto destination_storage = DatabaseCatalog::instance().tryGetTable(destination_storage_id, storage.getContext());
        if (!destination_storage)
            throw Exception(ErrorCodes::UNKNOWN_TABLE, "Destination table {} not found for export commit",
                destination_storage_id.getNameForLogs());

        const auto exported_paths = descriptor_copy.collectExportedPaths();
        if (exported_paths.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "No exported paths found for export {}, will not commit. This might be a bug", transaction_id);

        auto context = ExportPartitionUtils::getContextCopyWithTaskSettings(storage.getContext(), descriptor_copy);

        LOG_INFO(storage.log, "ExportPartition: all parts exported for task {}, committing", transaction_id);

        ExportPartitionUtils::commitExportedPaths(
            descriptor_copy.transaction_id,
            descriptor_copy.partition_id,
            descriptor_copy.iceberg_metadata_json,
            descriptor_copy.write_full_path_in_iceberg_metadata,
            descriptor_copy.iceberg_partition_timezone,
            exported_paths,
            descriptor_copy.partNames(),
            destination_storage,
            storage,
            context);

        success = true;
    }
    catch (const Exception & e)
    {
        failure = e;
        LOG_WARNING(storage.log, "ExportPartition: commit for task {} failed: {}", transaction_id, e.message());
    }

    {
        std::lock_guard lock(mutex);
        const String * composite_key = nullptr;
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.getDescriptor().transaction_id == transaction_id)
            {
                composite_key = &key;
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        entry->committing = false;

        /// A concurrent KILL may have won the race while we were committing. Its terminal state is
        /// already durable, so there is nothing to persist here.
        if (entry->getDescriptor().status != MergeTreePartitionExportTask::Status::PENDING)
            return;

        /// Persist-then-apply under the lock. Note the destination commit above already happened
        /// (effect-first): if this local write throws, we leave the task PENDING with all parts done
        /// so run() retries the commit -- idempotent thanks to the transaction id / commit file.
        auto updated = entry->getDescriptor();
        if (success)
        {
            updated.status = MergeTreePartitionExportTask::Status::COMPLETED;
        }
        else
        {
            updated.last_exception.message = failure ? failure->message() : "Unknown commit failure";
            updated.last_exception.part = "";
            updated.last_exception.time = time(nullptr);
            updated.last_exception.count += 1;

            if (failure && ExportPartitionUtils::isNonRetryableExportError(failure->code()))
                updated.status = MergeTreePartitionExportTask::Status::FAILED;
            /// Otherwise leave PENDING: run() will retry the commit on the next tick.
        }

        try
        {
            persist(*composite_key, updated.toJsonString());
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "ExportPartition: failed to persist commit result, will retry");
            return;
        }

        entry->setDescriptor(std::move(updated));
    }
}

void MergeTreePartitionExportScheduler::persist(const String & composite_key, const String & descriptor_json)
{
    /// Always called while holding the registry `mutex`, so writes are already serialized. The file
    /// is named by a 128-bit hash of the composite key (a fixed-length, filesystem-safe, opaque
    /// handle), so a force-replace overwrites the previous record in place. The authoritative key is
    /// recomputed from the JSON body in loadFromDisk, so the name only needs to be deterministic and
    /// collision-free -- sipHash128 (matching FileCacheKey) provides both.
    auto disk = storage.getDisks().front();
    const auto directory = getExportsRelativePath();
    disk->createDirectories(directory);

    const auto file_name = sipHash128String(composite_key) + ".json";
    const auto final_path = fs::path(directory) / file_name;
    const auto tmp_path = fs::path(directory) / (file_name + ".tmp");

    {
        auto out = disk->writeFile(tmp_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, storage.getContext()->getWriteSettings());
        writeString(descriptor_json, *out);
        out->finalize();
    }

    disk->replaceFile(tmp_path, final_path);
}

void MergeTreePartitionExportScheduler::loadFromDisk()
{
    auto disk = storage.getDisks().front();
    const auto directory = getExportsRelativePath();

    if (!disk->existsDirectory(directory))
        return;

    std::lock_guard lock(mutex);

    for (auto it = disk->iterateDirectory(directory); it->isValid(); it->next())
    {
        const auto file_name = it->name();
        /// Skip stale temporary files left by an interrupted write.
        if (!endsWith(file_name, ".json"))
            continue;

        try
        {
            auto buf = disk->readFile(fs::path(directory) / file_name, getReadSettings());
            String content;
            readStringUntilEOF(content, *buf);

            auto descriptor = MergeTreePartitionExportTask::fromJsonString(content);
            const auto composite_key = compositeKey(descriptor.partition_id, descriptor.destination_database, descriptor.destination_table);

            TaskEntry entry;

            /// Re-pin the not-yet-exported parts of a resumable task so background merges do not
            /// remove them before the export finishes.
            if (descriptor.status == MergeTreePartitionExportTask::Status::PENDING)
            {
                for (const auto & part : descriptor.parts)
                {
                    if (part.done)
                        continue;
                    if (auto data_part = storage.getPartIfExists(
                            part.part_name, {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated}))
                        entry.part_references.push_back(data_part);
                }
            }

            entry.setDescriptor(std::move(descriptor));
            tasks.emplace(composite_key, std::move(entry));

            LOG_INFO(storage.log, "ExportPartition: loaded export task from disk (key {})", composite_key);
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "Failed to load partition export descriptor " + file_name);
        }
    }
}

}
