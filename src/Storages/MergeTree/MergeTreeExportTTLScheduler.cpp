#include <Storages/MergeTree/MergeTreeExportTTLScheduler.h>

#include <Core/Defines.h>
#include <Disks/IDisk.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Storages/MergeTree/ExportTaskUtils.h>
#include <Storages/MergeTree/MergeTreeExportTaskScheduler.h>
#include <Storages/StorageMergeTree.h>
#include <Common/ProfileEvents.h>
#include <Common/escapeForFileName.h>
#include <Common/logger_useful.h>

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ExportTTLIndexSnapshotRefreshes;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

MergeTreeExportTTLIndex::MergeTreeExportTTLIndex(StorageMergeTree & storage_)
    : storage(storage_)
    , snapshot(ExportTTLIndexSnapshot::build(/* version */ -1, {}))
{
}

String MergeTreeExportTTLIndex::getRootPath() const
{
    return fs::path(storage.getRelativeDataPath()) / "export_ttl";
}

String MergeTreeExportTTLIndex::getDestinationPath(const String & destination_key) const
{
    return fs::path(getRootPath()) / destination_key;
}

String MergeTreeExportTTLIndex::getEntryPath(const String & destination_key, const String & partition_id) const
{
    return fs::path(getDestinationPath(destination_key)) / "partitions" / (escapeForFileName(partition_id) + ".json");
}

void MergeTreeExportTTLIndex::load()
{
    auto disk = storage.getDisks().front();
    const auto root_path = getRootPath();

    std::lock_guard lock(mutex);
    entries.clear();

    if (disk->existsDirectory(root_path))
    {
        for (auto destination_it = disk->iterateDirectory(root_path); destination_it->isValid(); destination_it->next())
        {
            const auto destination_key = destination_it->name();
            auto & by_partition = entries[destination_key];

            const auto partitions_path = fs::path(root_path) / destination_key / "partitions";
            if (!disk->existsDirectory(partitions_path))
                continue;

            for (auto it = disk->iterateDirectory(partitions_path); it->isValid(); it->next())
            {
                const auto file_name = it->name();
                /// Skip temporary files left by an interrupted write.
                if (!file_name.ends_with(".json"))
                    continue;

                const auto partition_id = unescapeForFileName(file_name.substr(0, file_name.size() - strlen(".json")));
                const auto path = fs::path(partitions_path) / file_name;

                /// Losing an entry would export its parts again, so an unreadable one fails the table load.
                String content;
                try
                {
                    auto in = disk->readFile(path, getReadSettings());
                    readStringUntilEOF(content, *in);
                    by_partition[partition_id] = ExportTTLVersionedEntry{
                        .entry = ExportTTLIndexEntry::fromJSONString(partition_id, content),
                        .version = 0,
                    };
                }
                catch (Exception & e)
                {
                    e.addMessage("While loading the TTL export index entry {}", path.string());
                    throw;
                }
                catch (...)
                {
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Cannot load the TTL export index entry {}: {}",
                        path.string(), getCurrentExceptionMessage(false));
                }
            }
        }
    }

    publishSnapshot();
}

bool MergeTreeExportTTLIndex::update(const String & destination_key, const ExportTTLVersionedEntry & versioned)
{
    std::lock_guard lock(mutex);

    auto & by_partition = entries[destination_key];
    const auto it = by_partition.find(versioned.entry.partition_id);
    const int32_t stored_version = it == by_partition.end() ? -1 : it->second.version;
    if (stored_version != versioned.version)
        return false;

    auto disk = storage.getDisks().front();
    const auto path = getEntryPath(destination_key, versioned.entry.partition_id);
    const auto tmp_path = path + ".tmp";
    disk->createDirectories(fs::path(path).parent_path());

    {
        auto out = disk->writeFile(tmp_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, storage.getContext()->getWriteSettings());
        writeString(versioned.entry.toJSONString(), *out);
        out->finalize();
        /// The entry must survive a crash before the task it claims for is created or completed.
        out->sync();
    }
    disk->replaceFile(tmp_path, path);

    by_partition[versioned.entry.partition_id] = ExportTTLVersionedEntry{.entry = versioned.entry, .version = stored_version + 1};
    publishSnapshot();
    return true;
}

void MergeTreeExportTTLIndex::removeDestination(const String & destination_key)
{
    std::lock_guard lock(mutex);

    auto disk = storage.getDisks().front();
    const auto path = getDestinationPath(destination_key);
    if (disk->existsDirectory(path))
        disk->removeRecursive(path);

    entries.erase(destination_key);
    publishSnapshot();

    LOG_INFO(storage.log, "Removed the TTL export index of destination {}", destination_key);
}

Int64 MergeTreeExportTTLIndex::maxBlock() const
{
    std::lock_guard lock(mutex);
    Int64 result = 0;
    for (const auto & [_, by_partition] : entries)
        for (const auto & partition_entry : by_partition)
            result = std::max(result, partition_entry.second.entry.maxBlock());
    return result;
}

void MergeTreeExportTTLIndex::publishSnapshot()
{
    ProfileEvents::increment(ProfileEvents::ExportTTLIndexSnapshotRefreshes);
    snapshot.set(ExportTTLIndexSnapshot::build(/* version */ -1, entries));
}

MergeTreeExportTTLScheduler::MergeTreeExportTTLScheduler(StorageMergeTree & storage_, MergeTreeExportTTLIndex & index_)
    : ExportTTLScheduler(storage_)
    , plain_storage(storage_)
    , index(index_)
{
}

bool MergeTreeExportTTLScheduler::isPaused()
{
    return plain_storage.parts_mover.moves_blocker.isCancelled();
}

ExportTTLScheduler::TaskState MergeTreeExportTTLScheduler::getTaskState(const String & transaction_id)
{
    TaskState state;
    const auto task = plain_storage.export_task_scheduler->getTask(transaction_id);
    if (!task)
        return state;

    switch (task->status)
    {
        case MergeTreeExportTask::Status::PENDING: state.status = TaskStatus::PENDING; break;
        case MergeTreeExportTask::Status::COMPLETED: state.status = TaskStatus::COMPLETED; break;
        case MergeTreeExportTask::Status::FAILED: state.status = TaskStatus::FAILED; break;
        case MergeTreeExportTask::Status::KILLED: state.status = TaskStatus::KILLED; break;
    }
    state.reached_commit = task->allPartsDone();
    state.retry_of = task->retry_of;
    return state;
}

bool MergeTreeExportTTLScheduler::startGroup(const GroupToStart & group, const ContextPtr & context)
{
    const auto parts_with_rows = group.partsWithRows();
    std::optional<MergeTreeExportTask> descriptor;
    if (!parts_with_rows.empty())
    {
        const auto source_metadata = plain_storage.getInMemoryMetadataPtr(context, false);
        const auto destination_metadata = group.destination->getInMemoryMetadataPtr(context, false);
        ExportTaskUtils::verifyExportSchemaCastable(source_metadata, destination_metadata, group.destination->getStorageID(), context);

        MergeTreeData::DataPartsVector parts(parts_with_rows.begin(), parts_with_rows.end());
        descriptor = plain_storage.buildExportTask(
            group.destination->getStorageID(), group.destination, source_metadata, destination_metadata, parts, group.partition_id, context);
        descriptor->transaction_id = group.transaction_id;
        descriptor->source = ExportTaskSource::ttl;
        descriptor->retry_of = group.retry_of;
    }

    {
        /// Merge selection holds it while it checks the fence and tags the parts it merges.
        std::lock_guard lock(plain_storage.currently_processing_in_background_mutex);
        for (const auto & part : group.parts)
        {
            if (part->getState() != MergeTreeDataPartState::Active || plain_storage.currently_merging_mutating_parts.contains(part->info))
                return false;
        }

        if (!index.update(group.destination_key, group.entry))
            return false;
    }

    if (descriptor)
        plain_storage.export_task_scheduler->addTask(std::move(*descriptor), parts_with_rows);
    return true;
}

bool MergeTreeExportTTLScheduler::isPartBeingMerged(const MergeTreeDataPartPtr & part)
{
    std::lock_guard lock(plain_storage.currently_processing_in_background_mutex);
    return plain_storage.currently_merging_mutating_parts.contains(part->info);
}

void MergeTreeExportTTLScheduler::killTask(const String & transaction_id)
{
    plain_storage.export_task_scheduler->kill(transaction_id);
}

}
