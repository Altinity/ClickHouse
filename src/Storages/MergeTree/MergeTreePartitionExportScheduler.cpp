#include <Storages/MergeTree/MergeTreePartitionExportScheduler.h>
#include <Storages/StorageMergeTree.h>
#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/CancellationCode.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/logger_useful.h>
#include <Disks/IDisk.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadBufferFromFileBase.h>
#include <Core/Defines.h>
#include <base/types.h>

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

void MergeTreePartitionExportScheduler::throwIfAlreadyExported(const String & composite_key, bool force) const
{
    if (force)
        return;

    std::lock_guard lock(mutex);
    if (tasks.contains(composite_key))
        throw Exception(ErrorCodes::EXPORT_PARTITION_ALREADY_EXPORTED,
            "Export with key {} already exported or it is being exported. "
            "Set `export_merge_tree_partition_force_export` to overwrite it.",
            composite_key);
}

void MergeTreePartitionExportScheduler::addTask(
    MergeTreePartitionExportTask descriptor, std::vector<DataPartPtr> part_references, bool force)
{
    const auto composite_key = compositeKey(descriptor.partition_id, descriptor.destination_database, descriptor.destination_table);
    const auto transaction_id = descriptor.transaction_id;
    String descriptor_json;

    String previous_transaction_id;
    bool replaced = false;

    {
        std::lock_guard lock(mutex);

        if (const auto it = tasks.find(composite_key); it != tasks.end())
        {
            if (!force)
                throw Exception(ErrorCodes::EXPORT_PARTITION_ALREADY_EXPORTED,
                    "Export with key {} already exported or it is being exported. "
                    "Set `export_merge_tree_partition_force_export` to overwrite it.",
                    composite_key);

            /// Overwrite: erase the previous task atomically here; its in-flight part exports and
            /// its on-disk file are cleaned up below, outside the registry lock (see note).
            previous_transaction_id = it->second.descriptor.transaction_id;
            replaced = true;
            tasks.erase(it);
        }

        TaskEntry entry;
        entry.descriptor = std::move(descriptor);
        entry.part_references = std::move(part_references);
        descriptor_json = entry.descriptor.toJsonString();
        tasks.emplace(composite_key, std::move(entry));
    }

    /// killExportPart takes export_manifests_mutex, and a part-export completion callback runs while
    /// holding export_manifests_mutex and then takes our registry mutex. Calling killExportPart while
    /// holding the registry mutex would therefore be an AB-BA lock inversion, so we do it here after
    /// releasing the lock. The previous task's entry is already gone, so its late completion callbacks
    /// find nothing and no-op.
    if (replaced)
    {
        LOG_INFO(storage.log, "ExportPartition: overwriting export with key {}", composite_key);
        storage.killExportPart(previous_transaction_id);
        removeTaskFile(previous_transaction_id);
    }

    /// Persist before announcing success so a crash right after this call leaves a resumable task.
    persist(transaction_id, descriptor_json);

    LOG_INFO(storage.log, "ExportPartition: scheduled export task {} (key {})", transaction_id, composite_key);
    storage.triggerPartitionExportTask();
}

CancellationCode MergeTreePartitionExportScheduler::kill(const String & transaction_id)
{
    String descriptor_json;
    {
        std::lock_guard lock(mutex);
        TaskEntry * target = nullptr;
        for (auto & [key, entry] : tasks)
        {
            if (entry.descriptor.transaction_id == transaction_id)
            {
                target = &entry;
                break;
            }
        }

        if (!target)
            return CancellationCode::NotFound;

        if (target->descriptor.status != MergeTreePartitionExportTask::Status::PENDING)
            return CancellationCode::CancelCannotBeSent;

        target->descriptor.status = MergeTreePartitionExportTask::Status::KILLED;
        descriptor_json = target->descriptor.toJsonString();
    }

    persist(transaction_id, descriptor_json);

    /// Cancel any in-flight part exports for this transaction. Their completion callbacks will see
    /// the KILLED status and skip further bookkeeping.
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
        const auto & descriptor = entry.descriptor;
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

void MergeTreePartitionExportScheduler::run()
{
    const auto available_move_executors = storage.background_moves_assignee.getAvailableMoveExecutors();
    if (available_move_executors == 0)
        return;

    if (storage.parts_mover.moves_blocker.isCancelled())
        return;

    /// Respect the background memory soft-limit like the per-part export path does.
    if (!canEnqueueBackgroundTask())
        return;

    std::vector<std::pair<String, String>> parts_to_schedule;  /// (transaction_id, part_name)
    std::vector<String> tasks_to_commit;

    {
        std::lock_guard lock(mutex);
        size_t scheduled = 0;
        for (auto & [key, entry] : tasks)
        {
            auto & descriptor = entry.descriptor;
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
}

void MergeTreePartitionExportScheduler::scheduleOnePart(const String & transaction_id, const String & part_name)
{
    MergeTreePartitionExportTask descriptor_copy;
    {
        std::lock_guard lock(mutex);
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.descriptor.transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;
        descriptor_copy = entry->descriptor;
    }

    const StorageID destination_storage_id{descriptor_copy.destination_database, descriptor_copy.destination_table};

    auto release_in_flight = [this, transaction_id, part_name]()
    {
        std::lock_guard lock(mutex);
        for (auto & [key, entry] : tasks)
            if (entry.descriptor.transaction_id == transaction_id)
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
    String descriptor_json;
    bool should_persist = false;

    {
        std::lock_guard lock(mutex);
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.descriptor.transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        entry->in_flight_parts.erase(part_name);

        auto & descriptor = entry->descriptor;

        /// Task already terminal (KILLED / FAILED / COMPLETED): ignore late completions.
        if (descriptor.status != MergeTreePartitionExportTask::Status::PENDING)
            return;

        if (result.success)
        {
            if (auto * part = descriptor.findPart(part_name))
            {
                part->done = true;
                part->paths_in_destination = result.relative_paths_in_destination_storage;
            }
            should_persist = true;

            if (descriptor.allPartsDone() && !entry->committing)
            {
                entry->committing = true;
                ready_to_commit = true;
            }
        }
        else
        {
            /// A cancelled export (KILL, or SYSTEM STOP MOVES) is not a real failure: leave the part
            /// pending. If it was a KILL the status is already handled above; otherwise the next tick
            /// retries it.
            if (result.exception && result.exception->code() == ErrorCodes::QUERY_WAS_CANCELLED)
                return;

            descriptor.last_exception.message = result.exception ? result.exception->message() : "Unknown export failure";
            descriptor.last_exception.part = part_name;
            descriptor.last_exception.time = time(nullptr);
            descriptor.last_exception.count += 1;
            should_persist = true;

            if (result.exception && ExportPartitionUtils::isNonRetryableExportError(result.exception->code()))
            {
                descriptor.status = MergeTreePartitionExportTask::Status::FAILED;
                LOG_WARNING(storage.log, "ExportPartition: task {} failed on part {} with non-retryable error (code {})",
                    transaction_id, part_name, result.exception->code());
            }
            else
            {
                LOG_INFO(storage.log, "ExportPartition: task {} part {} failed with retryable error, will retry",
                    transaction_id, part_name);
            }
        }

        if (should_persist)
            descriptor_json = descriptor.toJsonString();
    }

    if (should_persist)
        persist(transaction_id, descriptor_json);

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
            if (candidate.descriptor.transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        if (entry->descriptor.status != MergeTreePartitionExportTask::Status::PENDING || !entry->descriptor.allPartsDone())
        {
            entry->committing = false;
            return;
        }

        entry->committing = true;
        descriptor_copy = entry->descriptor;
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

    String descriptor_json;
    {
        std::lock_guard lock(mutex);
        TaskEntry * entry = nullptr;
        for (auto & [key, candidate] : tasks)
            if (candidate.descriptor.transaction_id == transaction_id)
            {
                entry = &candidate;
                break;
            }
        if (!entry)
            return;

        entry->committing = false;
        auto & descriptor = entry->descriptor;

        /// A concurrent KILL may have won the race while we were committing.
        if (descriptor.status != MergeTreePartitionExportTask::Status::PENDING)
        {
            descriptor_json = descriptor.toJsonString();
        }
        else if (success)
        {
            descriptor.status = MergeTreePartitionExportTask::Status::COMPLETED;
            descriptor_json = descriptor.toJsonString();
        }
        else
        {
            descriptor.last_exception.message = failure ? failure->message() : "Unknown commit failure";
            descriptor.last_exception.part = "";
            descriptor.last_exception.time = time(nullptr);
            descriptor.last_exception.count += 1;

            if (failure && ExportPartitionUtils::isNonRetryableExportError(failure->code()))
                descriptor.status = MergeTreePartitionExportTask::Status::FAILED;
            /// Otherwise leave PENDING: run() will retry the commit on the next tick.

            descriptor_json = descriptor.toJsonString();
        }
    }

    persist(transaction_id, descriptor_json);
}

void MergeTreePartitionExportScheduler::persist(const String & transaction_id, const String & descriptor_json)
{
    std::lock_guard file_lock(persist_mutex);

    auto disk = storage.getDisks().front();
    const auto directory = getExportsRelativePath();
    disk->createDirectories(directory);

    const auto final_path = fs::path(directory) / (transaction_id + ".json");
    const auto tmp_path = fs::path(directory) / (transaction_id + ".json.tmp");

    {
        auto out = disk->writeFile(tmp_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, storage.getContext()->getWriteSettings());
        writeString(descriptor_json, *out);
        out->finalize();
    }

    disk->replaceFile(tmp_path, final_path);
}

void MergeTreePartitionExportScheduler::removeTaskFile(const String & transaction_id)
{
    std::lock_guard file_lock(persist_mutex);
    auto disk = storage.getDisks().front();
    const auto final_path = fs::path(getExportsRelativePath()) / (transaction_id + ".json");
    disk->removeFileIfExists(final_path);
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

            entry.descriptor = std::move(descriptor);
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
