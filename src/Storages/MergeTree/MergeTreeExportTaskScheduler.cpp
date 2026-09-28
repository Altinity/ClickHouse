#include <Storages/MergeTree/MergeTreeExportTaskScheduler.h>
#include <Storages/StorageMergeTree.h>
#include <Storages/MergeTree/ExportTaskUtils.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/CancellationCode.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/escapeForFileName.h>
#include <Common/logger_useful.h>
#include <Common/quoteString.h>
#include <Disks/IDisk.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadBufferFromFileBase.h>
#include <Core/Defines.h>
#include <base/scope_guard.h>
#include <base/types.h>
#include <algorithm>
#include <limits>
#include <unordered_set>

#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int QUERY_WAS_CANCELLED;
    extern const int UNKNOWN_TABLE;
    extern const int UNKNOWN_EXCEPTION;
    extern const int LOGICAL_ERROR;
}

MergeTreeExportTaskScheduler::MergeTreeExportTaskScheduler(StorageMergeTree & storage_)
    : storage(storage_)
{
}

String MergeTreeExportTaskScheduler::getExportsRelativePath() const
{
    return fs::path(storage.getRelativeDataPath()) / "exports";
}

String MergeTreeExportTaskScheduler::descriptorRelativePath(const String & transaction_id) const
{
    return fs::path(getExportsRelativePath()) / (escapeForFileName(transaction_id) + ".json");
}

void MergeTreeExportTaskScheduler::addTask(MergeTreeExportTask descriptor, std::vector<DataPartPtr> part_references)
{
    const auto transaction_id = descriptor.transaction_id;
    const auto destination = fmt::format("{}.{}", backQuoteIfNeed(descriptor.destination_database), backQuoteIfNeed(descriptor.destination_table));

    {
        std::lock_guard lock(mutex);

        TaskEntry entry;
        entry.part_references = std::move(part_references);
        entry.setDescriptor(std::move(descriptor));
        persist(transaction_id, entry.getDescriptor().toJsonString());
        if (!tasks.emplace(transaction_id, std::move(entry)).second)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Export task {} already exists", transaction_id);
    }

    LOG_INFO(storage.log, "Export task: scheduled export task {} to {}", transaction_id, destination);
    storage.triggerExportTaskScheduling();
}

void MergeTreeExportTaskScheduler::wakeUpExportTTLIfFinished(const TaskEntry & entry)
{
    const auto & descriptor = entry.getDescriptor();
    if (descriptor.source == ExportTaskSource::ttl && descriptor.status != MergeTreeExportTask::Status::PENDING)
        storage.wakeUpExportTTL();
}

std::optional<MergeTreeExportTask> MergeTreeExportTaskScheduler::getTask(const String & transaction_id) const
{
    std::lock_guard lock(mutex);
    const auto it = tasks.find(transaction_id);
    if (it == tasks.end())
        return std::nullopt;
    return it->second.getDescriptor();
}

CancellationCode MergeTreeExportTaskScheduler::kill(const String & transaction_id)
{
    /// set the status to killed
    {
        std::lock_guard lock(mutex);

        auto it = tasks.find(transaction_id);
        if (it == tasks.end())
            return CancellationCode::NotFound;

        auto & entry = it->second;

        if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING)
        {
            LOG_INFO(storage.log, "Export task: export with the transaction id {} is not pending, cannot cancel it", transaction_id);
            return CancellationCode::CancelCannotBeSent;
        }


        if (entry.committing)
        {
            LOG_INFO(storage.log, "Export task: commit in progress for {}, cannot cancel export partition task", transaction_id);
            return CancellationCode::CancelCannotBeSent;
        }

        auto updated = entry.getDescriptor();

        updated.status = MergeTreeExportTask::Status::KILLED;
        persist(it->first, updated.toJsonString());
        entry.setDescriptor(std::move(updated));
        wakeUpExportTTLIfFinished(entry);
    }

    /// cancel in-flight operations
    storage.killExportPart(transaction_id);

    return CancellationCode::CancelSent;
}

std::vector<ExportTaskInfo> MergeTreeExportTaskScheduler::getInfo() const
{
    std::vector<ExportTaskInfo> result;
    std::lock_guard lock(mutex);
    result.reserve(tasks.size());
    for (const auto & [key, entry] : tasks)
    {
        const auto & descriptor = entry.getDescriptor();
        ExportTaskInfo info;
        info.destination_database = descriptor.destination_database;
        info.destination_table = descriptor.destination_table;
        info.create_time = descriptor.create_time;
        info.partition_id = descriptor.parts.empty()
            ? ""
            : ExportTaskUtils::getPartitionIdOfParts(descriptor.partNames(), storage.format_version);
        info.source = String(magic_enum::enum_name(descriptor.source));
        info.retry_of = descriptor.retry_of;
        info.transaction_id = descriptor.transaction_id;
        info.query_id = descriptor.query_id;
        info.parts = descriptor.partNames();
        info.parts_count = descriptor.partsCount();
        info.parts_to_do = descriptor.partsToDo();
        info.status = String(magic_enum::enum_name(descriptor.status));

        /// A single node exports on its own, so there is at most one "per-replica" entry and it
        /// carries an empty replica name.
        if (descriptor.last_exception.count > 0)
        {
            info.last_exception_per_replica.push_back(
                {/*replica*/ "", descriptor.last_exception.message, descriptor.last_exception.part,
                 descriptor.last_exception.time, descriptor.last_exception.count});
        }
        info.exception_count = descriptor.last_exception.count;

        for (const auto & part : descriptor.parts)
            if (!part.paths_in_destination.empty())
                info.destination_file_paths_per_part.emplace(part.part_name, part.paths_in_destination);

        if (descriptor.commit_info)
        {
            info.committed_metadata_file = descriptor.commit_info->iceberg_metadata_file;
            info.committed_manifest_list = descriptor.commit_info->iceberg_manifest_list;
            info.committed_manifest_file = descriptor.commit_info->iceberg_manifest_file;
            info.committed_marker_file = descriptor.commit_info->commit_marker_file;
        }

        info.backoff_per_part.reserve(entry.part_backoff.size());
        for (const auto & [part_name, backoff] : entry.part_backoff)
            info.backoff_per_part.push_back({part_name, backoff.attempts, backoff.next_retry_time});

        result.push_back(std::move(info));
    }
    return result;
}

bool MergeTreeExportTaskScheduler::tryPersistTimeoutKill(const String & transaction_id_key, TaskEntry & entry, time_t now)
{
    const auto transaction_id = entry.getDescriptor().transaction_id;
    const auto timeout_seconds = entry.getDescriptor().task_timeout_seconds;

    auto updated = entry.getDescriptor();
    updated.status = MergeTreeExportTask::Status::KILLED;
    updated.last_exception.message = fmt::format(
        "Export partition task timed out: exceeded export_merge_tree_task_timeout_seconds={} (created at {}, now {})",
        timeout_seconds, entry.getDescriptor().create_time, now);
    updated.last_exception.part = "";
    updated.last_exception.time = now;
    updated.last_exception.count += 1;

    try
    {
        persist(transaction_id_key, updated.toJsonString());
    }
    catch (...)
    {
        tryLogCurrentException(storage.log, "Export task: failed to persist timeout kill, will retry");
        return false;
    }

    entry.setDescriptor(std::move(updated));
    wakeUpExportTTLIfFinished(entry);
    LOG_WARNING(storage.log,
        "Export task: task {} exceeded task_timeout_seconds={}s, transitioned PENDING -> KILLED",
        transaction_id, timeout_seconds);
    return true;
}

bool MergeTreeExportTaskScheduler::enforceTimeouts()
{
    std::vector<String> timed_out_transactions;
    bool any_pending = false;

    {
        std::lock_guard lock(mutex);
        const auto now = time(nullptr);
        for (auto & [key, entry] : tasks)
        {
            if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING)
                continue;

            if (!ExportTaskUtils::isExportTaskTimedOut(
                    entry.getDescriptor().create_time, entry.getDescriptor().task_timeout_seconds, now))
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

bool MergeTreeExportTaskScheduler::run()
{
    /// Timeouts first, before the "cannot make progress this tick" early returns, so a wedged
    /// task (no move executors, memory pressure, ...) still expires.
    if (!enforceTimeouts())
        return false;

    /// There is pending work. Even if we cannot make progress this tick (no move executors, moves
    /// stopped, or memory pressure), keep the scheduler awake so it retries on the next tick.
    const auto available_move_executors = storage.background_moves_assignee.getAvailableMoveExecutors();
    if (available_move_executors == 0)
    {
        LOG_INFO(storage.log, "Export task: no move executors available, skipping run on plain");
        return true;
    }


    if (storage.parts_mover.moves_blocker.isCancelled())
    {
        LOG_INFO(storage.log, "Export task: moves cancelled, skipping run on plain");
        return true;
    }

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
            if (descriptor.status != MergeTreeExportTask::Status::PENDING)
                continue;

            if (descriptor.allPartsDone())
            {
                /// All parts exported: commit (or retry a previously-failed commit). tryCommit
                /// itself takes the committing lease; skip if a commit is already in flight.
                if (!entry.committing)
                    {
                        LOG_DEBUG(storage.log, "Export task: all parts exported for task {}, committing", descriptor.transaction_id);
                        tasks_to_commit.push_back(descriptor.transaction_id);
                    }
                continue;
            }

            for (const auto & part : descriptor.parts)
            {
                if (scheduled >= available_move_executors)
                {
                    LOG_DEBUG(storage.log, "Export task: no move executors available, skipping part export for task {}", descriptor.transaction_id);
                    break;
                }

                if (part.done || entry.in_flight_parts.contains(part.part_name))
                {
                    LOG_DEBUG(storage.log, "Export task: part {} already exported or in flight for task {}, skipping", part.part_name, descriptor.transaction_id);
                    continue;
                }

                if (const auto backoff_it = entry.part_backoff.find(part.part_name);
                    backoff_it != entry.part_backoff.end() && now < backoff_it->second.next_retry_time)
                {
                    LOG_DEBUG(storage.log, "Export task: part {} backoff time not reached for task {}, skipping", part.part_name, descriptor.transaction_id);
                    continue;
                }

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

void MergeTreeExportTaskScheduler::scheduleOnePart(const String & transaction_id, const String & part_name)
{
    MergeTreeExportTask descriptor_copy;
    {
        std::lock_guard lock(mutex);
        auto it = tasks.find(transaction_id);
        if (it == tasks.end())
            return;

        auto & entry = it->second;

        /// run() marked the part in flight and released the mutex, so a KILL from a query thread
        /// may have made the task terminal since then. Dispatching now would write destination
        /// objects for an export the user already cancelled.
        if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING)
        {
            entry.in_flight_parts.erase(part_name);
            return;
        }

        descriptor_copy = entry.getDescriptor();
    }

    const StorageID destination_storage_id{descriptor_copy.destination_database, descriptor_copy.destination_table};

    try
    {
        auto context = ExportTaskUtils::getContextCopyWithTaskSettings(storage.getContext(), descriptor_copy);

        LOG_INFO(storage.log, "Export task: scheduling part export {} for task {}", part_name, transaction_id);

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

        /// killExportPart can only cancel a task that is already registered in `export_manifests`.
        /// A KILL that ran between the status check above and this registration therefore found
        /// nothing to cancel. Its terminal status is durable by then, so re-read it now that the
        /// task is visible and cancel it ourselves if it lost that race.
        bool still_pending = false;
        {
            std::lock_guard lock(mutex);
            auto it = tasks.find(transaction_id);
            still_pending = it != tasks.end()
                && it->second.getDescriptor().status == MergeTreeExportTask::Status::PENDING;
        }

        if (!still_pending)
            storage.killExportPart(transaction_id);
    }
    catch (const Exception & e)
    {
        tryLogCurrentException(storage.log, __PRETTY_FUNCTION__);
        /// Dispatch failed before a move-executor task was queued (destination dropped, schema
        /// mismatch, executor busy, ...). Route through the same completion-failure transition as
        /// an async export error so last_exception is persisted and non-retryable faults become
        /// FAILED. handlePartCompletion also releases the in-flight marker.
        handlePartCompletion(
            transaction_id,
            part_name,
            MergeTreePartExportManifest::CompletionCallbackResult::createFailure(e));
    }
    catch (...)
    {
        tryLogCurrentException(storage.log, __PRETTY_FUNCTION__);
        handlePartCompletion(
            transaction_id,
            part_name,
            MergeTreePartExportManifest::CompletionCallbackResult::createFailure(
                Exception::createRuntime(
                    ErrorCodes::UNKNOWN_EXCEPTION,
                    getCurrentExceptionMessage(/*with_stacktrace=*/ false))));
    }
}

void MergeTreeExportTaskScheduler::handlePartCompletion(
    const String & transaction_id, const String & part_name, const MergeTreePartExportManifest::CompletionCallbackResult & result)
{
    bool ready_to_commit = false;

    {
        std::lock_guard lock(mutex);
        auto it = tasks.find(transaction_id);
        if (it == tasks.end())
        {
            LOG_DEBUG(storage.log, "Export task: task {} completed, but manifest not. The task was likely overwritten by a new task or this is a bug", transaction_id);
            return;
        }

        auto & entry = it->second;

        entry.in_flight_parts.erase(part_name);

        /// Task already terminal (KILLED / FAILED / COMPLETED): ignore late completions.
        if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING)
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
        auto updated = entry.getDescriptor();

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

            if (result.exception && ExportTaskUtils::isNonRetryablePlainExportError(result.exception->code()))
                updated.status = MergeTreeExportTask::Status::FAILED;
        }

        try
        {
            persist(it->first, updated.toJsonString());
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "Export task: failed to persist part-completion state, will retry");
            return;
        }

        entry.setDescriptor(std::move(updated));
        wakeUpExportTTLIfFinished(entry);

        if (result.success)
        {
            entry.part_backoff.erase(part_name);
            if (entry.getDescriptor().allPartsDone() && !entry.committing)
                ready_to_commit = true;
        }
        else if (entry.getDescriptor().status == MergeTreeExportTask::Status::FAILED)
        {
            LOG_WARNING(storage.log, "Export task: task {} failed on part {} with non-retryable error",
                transaction_id, part_name);
        }
        else
        {
            auto & backoff = entry.part_backoff[part_name];
            ++backoff.attempts;
            const auto backoff_seconds = ExportTaskUtils::computeRetryBackoffSeconds(
                backoff.attempts,
                entry.getDescriptor().retry_initial_backoff_seconds,
                entry.getDescriptor().retry_max_backoff_seconds);
            const auto now = time(nullptr);
            const size_t headroom = static_cast<size_t>(std::numeric_limits<time_t>::max() - now);
            backoff.next_retry_time = now + static_cast<time_t>(std::min(backoff_seconds, headroom));

            LOG_INFO(storage.log, "Export task: task {} part {} failed with retryable error, will retry at {}",
                transaction_id, part_name, backoff.next_retry_time);
        }
    }

    if (ready_to_commit)
        tryCommit(transaction_id);
}

void MergeTreeExportTaskScheduler::tryCommit(const String & transaction_id)
{
    MergeTreeExportTask descriptor_copy;
    /// This call is the sole writer of `committing`. Drop the lease on every exit after we claimed
    /// it, including exceptions that are not `DB::Exception` (otherwise the task is wedged until
    /// restart: timeout and KILL also refuse to act while the flag is set).
    bool claimed = false;
    SCOPE_EXIT(
    {
        if (!claimed)
            return;
        std::lock_guard lock(mutex);
        auto it = tasks.find(transaction_id);
        if (it != tasks.end())
            it->second.committing = false;
    });

    {
        std::lock_guard lock(mutex);
        auto it = tasks.find(transaction_id);
        if (it == tasks.end())
            return;

        auto & entry = it->second;
        if (entry.committing)
            return;
        if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING || !entry.getDescriptor().allPartsDone())
            return;

        entry.committing = true;
        claimed = true;
        descriptor_copy = entry.getDescriptor();
    }

    const StorageID destination_storage_id{descriptor_copy.destination_database, descriptor_copy.destination_table};

    bool success = false;
    std::optional<Exception> failure;
    IStorage::ExportCommitInfo destination_commit_info;
    try
    {
        auto exported_paths = descriptor_copy.collectExportedPaths();

        std::optional<ContextPtr> context;
        StoragePtr destination_storage;
        const auto get_destination = [&]
        {
            if (destination_storage)
                return;
            destination_storage = DatabaseCatalog::instance().tryGetTable(destination_storage_id, storage.getContext());
            if (!destination_storage)
                throw Exception(ErrorCodes::UNKNOWN_TABLE, "Destination table {} not found for export commit",
                    destination_storage_id.getNameForLogs());
            context = ExportTaskUtils::getContextCopyWithTaskSettings(storage.getContext(), descriptor_copy);
        };

        /// A task this one retries may have landed after it was considered failed. Its parts are
        /// then in the destination already, so only the files of the other parts are committed.
        if (!descriptor_copy.retry_of.empty())
        {
            get_destination();
            const auto committed_ranges = ExportTaskUtils::getRangesCommittedByRetriedTasks(
                descriptor_copy.retry_of,
                destination_storage,
                [this](const String & retried_transaction_id) -> std::optional<std::vector<String>>
                {
                    if (const auto task = getTask(retried_transaction_id))
                        return task->partNames();
                    return std::nullopt;
                },
                storage.format_version,
                *context);

            if (!committed_ranges.empty())
            {
                const auto parts_to_commit = ExportTaskUtils::getPartsNotCommitted(
                    descriptor_copy.partNames(), committed_ranges, storage.format_version);
                const std::unordered_set<String> parts_to_commit_set(parts_to_commit.begin(), parts_to_commit.end());

                exported_paths.clear();
                for (const auto & part : descriptor_copy.parts)
                    if (parts_to_commit_set.contains(part.part_name))
                        exported_paths.insert(exported_paths.end(), part.paths_in_destination.begin(), part.paths_in_destination.end());

                LOG_INFO(storage.log, "Export task: a task retried by {} committed some of its parts, committing the files of {} of {} parts",
                    transaction_id, parts_to_commit.size(), descriptor_copy.parts.size());
            }
        }

        /// Every part is done and its paths were recorded durably at completion, so an empty set
        /// here is not a lost-data symptom: an Iceberg destination writes no data file for a part
        /// with no surviving rows, so a partition that is entirely deleted produces nothing at all.
        /// There is no file to make visible, so there is nothing to commit.
        if (exported_paths.empty())
        {
            LOG_INFO(storage.log,
                "Export task: task {} has no destination files to commit", transaction_id);
        }
        else
        {
            get_destination();

            LOG_INFO(storage.log, "Export task: all parts exported for task {}, committing", transaction_id);

            destination_commit_info = ExportTaskUtils::commitExportOnDestination(
                descriptor_copy.transaction_id,
                ExportTaskUtils::getPartitionIdOfParts(descriptor_copy.partNames(), storage.format_version),
                descriptor_copy.iceberg_metadata_json,
                descriptor_copy.write_full_path_in_iceberg_metadata,
                descriptor_copy.iceberg_partition_timezone,
                exported_paths,
                descriptor_copy.partNames(),
                destination_storage,
                storage,
                *context);
        }

        success = true;
    }
    catch (const Exception & e)
    {
        failure = e;
        LOG_WARNING(storage.log, "Export task: commit for task {} failed: {}", transaction_id, e.message());
    }
    catch (...)
    {
        tryLogCurrentException(storage.log, "Export task: commit for task " + transaction_id + " failed");
        failure = Exception::createRuntime(ErrorCodes::UNKNOWN_EXCEPTION, getCurrentExceptionMessage(/*with_stacktrace=*/ false));
    }

    {
        std::lock_guard lock(mutex);
        auto it = tasks.find(transaction_id);
        if (it == tasks.end())
            return;

        auto & entry = it->second;

        /// A concurrent KILL may have won the race while we were committing. Its terminal state is
        /// already durable, so there is nothing to persist here. `SCOPE_EXIT` still drops the lease.
        if (entry.getDescriptor().status != MergeTreeExportTask::Status::PENDING)
            return;

        /// Persist-then-apply under the lock. Note the destination commit above already happened
        /// (effect-first): if this local write throws, we leave the task PENDING with all parts done
        /// so run() retries the commit -- idempotent thanks to the transaction id / commit file.
        auto updated = entry.getDescriptor();
        if (success)
        {
            updated.status = MergeTreeExportTask::Status::COMPLETED;
            updated.commit_info = ExportCommitInfoEntry{
                destination_commit_info.iceberg_metadata_file,
                destination_commit_info.iceberg_manifest_list,
                destination_commit_info.iceberg_manifest_file,
                destination_commit_info.commit_marker_file};
        }
        else
        {
            updated.last_exception.message = failure ? failure->message() : "Unknown commit failure";
            updated.last_exception.part = "";
            updated.last_exception.time = time(nullptr);
            updated.last_exception.count += 1;

            if (failure && ExportTaskUtils::isNonRetryablePlainExportError(failure->code()))
                updated.status = MergeTreeExportTask::Status::FAILED;
            /// Otherwise leave PENDING: run() will retry the commit on the next tick.
        }

        try
        {
            persist(transaction_id, updated.toJsonString());
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "Export task: failed to persist commit result, will retry");
            return;
        }

        entry.setDescriptor(std::move(updated));
        wakeUpExportTTLIfFinished(entry);
    }
}

void MergeTreeExportTaskScheduler::persist(const String & transaction_id, const String & descriptor_json)
{
    /// Always called while holding the registry `mutex`, so writes are already serialized.
    auto disk = storage.getDisks().front();
    disk->createDirectories(getExportsRelativePath());

    const auto final_path = descriptorRelativePath(transaction_id);
    const auto tmp_path = final_path + ".tmp";

    {
        auto out = disk->writeFile(tmp_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, storage.getContext()->getWriteSettings());
        writeString(descriptor_json, *out);
        out->finalize();
    }

    disk->replaceFile(tmp_path, final_path);
}

void MergeTreeExportTaskScheduler::load()
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

            auto descriptor = MergeTreeExportTask::fromJsonString(content);
            const auto transaction_id = descriptor.transaction_id;

            TaskEntry entry;

            /// Re-pin every source part of a resumable task, including already-exported ones.
            /// Unfinished parts still need to be read; Iceberg commit also derives partition
            /// values from the original parts, so they must survive until COMPLETED/FAILED/KILLED.
            if (descriptor.status == MergeTreeExportTask::Status::PENDING)
            {
                for (const auto & part : descriptor.parts)
                {
                    if (auto data_part = storage.getPartIfExists(
                            part.part_name, {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated}))
                        entry.part_references.push_back(data_part);
                }
            }

            entry.setDescriptor(std::move(descriptor));

            /// The file is named by the transaction id, so a duplicate means the directory was tampered with.
            if (!tasks.emplace(transaction_id, std::move(entry)).second)
            {
                LOG_ERROR(storage.log, "Export task: ignoring {}, another record already describes task {}",
                    file_name, transaction_id);
                continue;
            }

            LOG_INFO(storage.log, "Export task: loaded export task {} from disk", transaction_id);
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, "Failed to load partition export descriptor " + file_name);
        }
    }
}

}
