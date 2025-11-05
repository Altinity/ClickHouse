#include <Storages/MergeTree/ExportPartitionManifestUpdatingTask.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/ExportReplicatedMergeTreePartitionTaskEntry.h>
#include "Storages/MergeTree/ExportPartitionUtils.h"
#include <Common/ZooKeeper/Types.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Interpreters/DatabaseCatalog.h>

namespace DB
{

/// v2 of my initial design
/*
table_path/
    exports/
        <partition_id+destination_storage_id>/
            metadata.json -> {tid, partition_id, destination_id, create_time, ttl}
            parts/
                processing/ <-- not started, in progress
                    part_1.json -> {retry_count, max_retry_count, path_in_destination}
                    ...
                    part_n.json
                processed/
                    part_1.json -> {retry_count, max_retry_count, path_in_destination}
                    ...
                    part_n.json
            locks
                part_1 -> r1
                part_n -> rN
    cleanup_lock <--- ephemeral

    One of the ideas behind this design is to reduce the number of required CAS loops.
    It should work as follows:

    upon request, the structure should be created in zk in case it does not exist.

    once the task is published in zk, replicas are notified there is a new task and will fetch it.

    once they have it loaded locally, eventually the scheduler thread will run and try to lock individual parts in that task to export.

    the lock process is kind of the following:

    try to create an ephemeral node with the aprt name under the `locks` path. If it succeeded, the part is locked and the task will be scheduled within that replica.

    if it fails, it means the part is already locked by another replica. Try the next part.

    Once it completes, moves the part structure that lives under processing to processed with status either of failed or succeeded. If it failed, it'll also fail the entire task.

    Also, once it completes a local part, after moving it to processed (a transaction). It tries to read `processing` to check if it is empty.

    If it is empty, it means all parts have been exported and it is time to commit the export. Note that this is not transactional with the previous operation of moving the part to processed.

    So it means there is a chance the last part will be exported, but the server might die right before checking processing path and will never commit. For this, the cleanup thread also helps

    This is the overall idea, but please read the code to get a better understanding
*/

ExportPartitionManifestUpdatingTask::ExportPartitionManifestUpdatingTask(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
{
}

void ExportPartitionManifestUpdatingTask::run()
{
    std::lock_guard lock(storage.export_merge_tree_partition_mutex);

    auto zk = storage.getZooKeeper();
    
    const std::string exports_path = fs::path(storage.zookeeper_path) / "exports";
    const std::string cleanup_lock_path = fs::path(storage.zookeeper_path) / "exports_cleanup_lock";

    bool cleanup_lock_acquired = zk->tryCreate(cleanup_lock_path, "", ::zkutil::CreateMode::Ephemeral) == Coordination::Error::ZOK;

    if (cleanup_lock_acquired)
    {
        LOG_INFO(storage.log, "ExportPartition: Cleanup lock acquired, will remove stale entries");
    }

    Coordination::Stat stat;
    const auto children = zk->getChildrenWatch(exports_path, &stat, storage.export_merge_tree_partition_watch_callback);
    const std::unordered_set<std::string> zk_children(children.begin(), children.end());

    const auto now = time(nullptr);

    /// Load new entries
    /// If we have the cleanup lock, also remove stale entries from zk and local
    /// Upload dangling commit files if any
    for (const auto & key : zk_children)
    {
        const std::string entry_path = fs::path(exports_path) / key;

        std::string metadata_json;
        if (!zk->tryGet(fs::path(entry_path) / "metadata.json", metadata_json))
        {
            LOG_INFO(storage.log, "ExportPartition: Skipping {}: missing metadata.json", key);
            continue;
        }

        const auto metadata = ExportReplicatedMergeTreePartitionManifest::fromJsonString(metadata_json);

        auto & entries_by_key = storage.export_merge_tree_partition_task_entries_by_key;
        const auto local_entry = entries_by_key.find(key);

        /// If the zk entry has been replaced with export_merge_tree_partition_force_export, checking only for the export key is not enough
        /// we need to make sure it is the same transaction id. If it is not, it needs to be replaced.
        bool has_local_entry_and_is_up_to_date = local_entry != entries_by_key.end()
            && local_entry->manifest.transaction_id == metadata.transaction_id;

        /// If the entry is up to date and we don't have the cleanup lock, early exit, nothing to be done.
        if (!cleanup_lock_acquired && has_local_entry_and_is_up_to_date)
            continue;

        std::string status;
        if (!zk->tryGet(fs::path(entry_path) / "status", status))
        {
            LOG_INFO(storage.log, "ExportPartition: Skipping {}: missing status", key);
            continue;
        }

        bool is_not_pending = status != "PENDING";

        if (cleanup_lock_acquired)
        {
            bool has_expired = metadata.create_time < now - 180;

            if (has_expired && is_not_pending)
            {
                zk->tryRemoveRecursive(fs::path(entry_path));
                auto it = entries_by_key.find(key);
                if (it != entries_by_key.end())
                    entries_by_key.erase(it);
                LOG_INFO(storage.log, "ExportPartition: Removed {}: expired", key);
                continue;
            }
        }

        if (is_not_pending)
        {
            LOG_INFO(storage.log, "ExportPartition: Skipping {}: status is not PENDING", key);
            continue;
        }


        if (cleanup_lock_acquired)
        {
            std::vector<std::string> parts_in_processing_or_pending;
            if (Coordination::Error::ZOK != zk->tryGetChildren(fs::path(entry_path) / "processing", parts_in_processing_or_pending))
            {
                LOG_INFO(storage.log, "ExportPartition: Failed to get parts in processing or pending, skipping");
                continue;
            }
    
            if (parts_in_processing_or_pending.empty())
            {
                LOG_INFO(storage.log, "ExportPartition: Cleanup found PENDING for {} with all parts exported, try to fix it by committing the export", entry_path);

                const auto destination_storage_id = StorageID(QualifiedTableName {metadata.destination_database, metadata.destination_table});
                const auto destination_storage = DatabaseCatalog::instance().tryGetTable(destination_storage_id, storage.getContext());
                if (!destination_storage)
                {
                    LOG_INFO(storage.log, "ExportPartition: Failed to reconstruct destination storage: {}, skipping", destination_storage_id.getNameForLogs());
                    continue;
                }

                /// it sounds like a replica exported the last part, but was not able to commit the export. Try to fix it
                ExportPartitionUtils::commit(metadata, destination_storage, zk, storage.log.load(), entry_path, storage.getContext());
            }
        }

        if (has_local_entry_and_is_up_to_date)
        {
            LOG_INFO(storage.log, "ExportPartition: Skipping {}: already exists", key);
            continue;
        }

        std::vector<DataPartPtr> part_references;

        for (const auto & part_name : metadata.parts)
        {
            if (const auto part = storage.getPartIfExists(part_name, {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated}))
            {
                part_references.push_back(part);
            }
        }

        /// Insert or update entry. The multi_index container automatically maintains both indexes.
        auto entry = ExportReplicatedMergeTreePartitionTaskEntry {metadata, std::move(part_references)};
        auto it = entries_by_key.find(key);
        if (it != entries_by_key.end())
            entries_by_key.replace(it, entry);
        else
            entries_by_key.insert(entry);
    }

    /// Remove entries that were deleted by someone else
    auto & entries_by_key = storage.export_merge_tree_partition_task_entries_by_key;
    for (auto it = entries_by_key.begin(); it != entries_by_key.end();)
    {
        const auto & key = it->getCompositeKey();
        if (zk_children.contains(key))
        {
            ++it;
            continue;
        }

        const auto & transaction_id = it->manifest.transaction_id;
        LOG_INFO(storage.log, "ExportPartition: Export task {} was deleted, calling killExportPartition for transaction {}", key, transaction_id);
        
        try
        {
            storage.killExportPart(transaction_id);
        }
        catch (...)
        {
            tryLogCurrentException(storage.log, __PRETTY_FUNCTION__);
        }

        it = entries_by_key.erase(it);
    }

    if (cleanup_lock_acquired)
    {
        zk->tryRemove(cleanup_lock_path);
    }

    storage.export_merge_tree_partition_select_task->schedule();
}

}
