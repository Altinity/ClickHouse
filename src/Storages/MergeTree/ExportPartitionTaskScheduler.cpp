#include <Storages/MergeTree/ExportPartitionTaskScheduler.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include "Common/ZooKeeper/Types.h"
#include "Storages/MergeTree/ExportPartitionUtils.h"


namespace DB
{

namespace
{
    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ExportReplicatedMergeTreePartitionManifest & manifest)
    {
        auto context_copy = Context::createCopy(context);
        context_copy->setSetting("output_format_parallel_formatting", manifest.parallel_formatting);
        context_copy->setSetting("output_format_parquet_parallel_encoding", manifest.parquet_parallel_encoding);
        context_copy->setSetting("max_threads", manifest.max_threads);
        return context_copy;
    }
}

ExportPartitionTaskScheduler::ExportPartitionTaskScheduler(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
{
}

void ExportPartitionTaskScheduler::run()
{
    std::lock_guard lock(storage.export_merge_tree_partition_mutex);

    auto zk = storage.getZooKeeper();

    // Iterate sorted by create_time
    for (const auto & entry : storage.export_merge_tree_partition_task_entries_by_create_time)
    {
        const auto & manifest = entry.manifest;
        const auto key = entry.getCompositeKey();
        const auto & database = storage.getContext()->resolveDatabase(manifest.destination_database);
        const auto & table = manifest.destination_table;

        const auto destination_storage_id = StorageID(QualifiedTableName {database, table});

        const auto destination_storage = DatabaseCatalog::instance().tryGetTable(destination_storage_id, storage.getContext());

        if (!destination_storage)
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to reconstruct destination storage: {}, skipping", destination_storage_id.getNameForLogs());
            continue;
        }

        std::string status;
        if (!zk->tryGet(fs::path(storage.zookeeper_path) / "exports" / key / "status", status))
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to get status, skipping");
            continue;
        }

        if (status != "PENDING")
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: Skipping... Status is not PENDING");
            continue;
        }

        std::vector<std::string> parts_in_processing_or_pending;
        
        if (Coordination::Error::ZOK != zk->tryGetChildren(fs::path(storage.zookeeper_path) / "exports" / key / "processing", parts_in_processing_or_pending))
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to get parts in processing or pending, skipping");
            continue;
        }

        if (parts_in_processing_or_pending.empty())
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: No parts in processing or pending, skipping");
            continue;
        }

        std::vector<std::string> locked_parts;

        if (Coordination::Error::ZOK != zk->tryGetChildren(fs::path(storage.zookeeper_path) / "exports" / key / "locks", locked_parts))
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to get locked parts, skipping");
            continue;
        }

        std::unordered_set<std::string> locked_parts_set(locked_parts.begin(), locked_parts.end());

        for (const auto & zk_part_name : parts_in_processing_or_pending)
        {
            if (locked_parts_set.contains(zk_part_name))
            {
                LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} is locked, skipping", zk_part_name);
                continue;
            }

            const auto part = storage.getPartIfExists(zk_part_name, {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated});
            if (!part)
            {
                LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} not found locally, skipping", zk_part_name);
                continue;
            }

            if (Coordination::Error::ZOK != zk->tryCreate(fs::path(storage.zookeeper_path) / "exports" / key / "locks" / zk_part_name, storage.replica_name, zkutil::CreateMode::Ephemeral))
            {
                LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to lock part {}, skipping", zk_part_name);
                continue;
            }

            try
            {
                storage.exportPartToTable(
                    part->name,
                    destination_storage_id,
                    manifest.transaction_id,
                    getContextCopyWithTaskSettings(storage.getContext(), manifest),
                    [this, key, zk_part_name, manifest, destination_storage]
                    (MergeTreePartExportManifest::CompletionCallbackResult result)
                    {
                        handlePartExportCompletion(key, zk_part_name, manifest, destination_storage, result);
                    });
            }
            catch (const Exception &)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
                zk->tryRemove(fs::path(storage.zookeeper_path) / "exports" / key / "locks" / zk_part_name);
                /// we should not increment retry_count because the node might just be full
            }
        }
    }

    /// maybe we failed to schedule or failed to export, need to retry eventually
    storage.export_merge_tree_partition_select_task->scheduleAfter(1000 * 5);
}

void ExportPartitionTaskScheduler::handlePartExportCompletion(
    const std::string & export_key,
    const std::string & part_name,
    const ExportReplicatedMergeTreePartitionManifest & manifest,
    const StoragePtr & destination_storage,
    const MergeTreePartExportManifest::CompletionCallbackResult & result)
{
    const auto export_path = fs::path(storage.zookeeper_path) / "exports" / export_key;
    const auto processing_parts_path = export_path / "processing";
    const auto processed_part_path = export_path / "processed" / part_name;
    const auto zk = storage.getZooKeeper();

    if (result.success)
    {
        handlePartExportSuccess(manifest, destination_storage, processing_parts_path, processed_part_path, part_name, export_path, zk, result.relative_path_in_destination_storage);
    }
    else
    {
        handlePartExportFailure(processing_parts_path, part_name, export_path, zk, result.exception, manifest.max_retries);
    }
}

void ExportPartitionTaskScheduler::handlePartExportSuccess(
    const ExportReplicatedMergeTreePartitionManifest & manifest,
    const StoragePtr & destination_storage,
    const std::filesystem::path & processing_parts_path,
    const std::filesystem::path & processed_part_path,
    const std::string & part_name,
    const std::filesystem::path & export_path,
    const zkutil::ZooKeeperPtr & zk,
    const String & relative_path_in_destination_storage
)
{
    LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} exported successfully", relative_path_in_destination_storage);

    Coordination::Stat locked_by_stat;
    std::string locked_by;

    if (!zk->tryGet(export_path / "locks" / part_name, locked_by, &locked_by_stat))
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} is not locked by any replica, will not commit or set it as completed", part_name);
        return;
    }

    /// Is this a good idea? what if the file we just pushed to s3 ends up triggering an exception in the replica that actually locks the part and it does not commit?
    /// I guess we should not throw if file already exists for export partition, hard coded.
    if (locked_by != storage.replica_name)
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} is locked by another replica, will not commit or set it as completed", part_name);
        return;
    }

    Coordination::Requests requests;

    if (zk->isFeatureEnabled(DB::KeeperFeatureFlag::REMOVE_RECURSIVE))
    {
        requests.emplace_back(zkutil::makeRemoveRecursiveRequest(*zk, processing_parts_path / part_name, -1));
    }
    else
    {
        // Remove children before parent (order matters for multi operations)
        // Maybe a ls + multi rm..
        requests.emplace_back(zkutil::makeRemoveRequest(processing_parts_path / part_name / "retry_count", -1));
        requests.emplace_back(zkutil::makeRemoveRequest(processing_parts_path / part_name / "status", -1));
        requests.emplace_back(zkutil::makeRemoveRequest(processing_parts_path / part_name, -1));
    }

    requests.emplace_back(zkutil::makeCreateRequest(processed_part_path, "", zkutil::CreateMode::Persistent));
    requests.emplace_back(zkutil::makeCreateRequest(processed_part_path / "path", relative_path_in_destination_storage, zkutil::CreateMode::Persistent));
    requests.emplace_back(zkutil::makeCreateRequest(processed_part_path / "status", "COMPLETED", zkutil::CreateMode::Persistent));
    requests.emplace_back(zkutil::makeCreateRequest(processed_part_path / "finished_by", storage.replica_name, zkutil::CreateMode::Persistent));
    requests.emplace_back(zkutil::makeRemoveRequest(export_path / "locks" / part_name, locked_by_stat.version));

    Coordination::Responses responses;
    if (Coordination::Error::ZOK != zk->tryMulti(requests, responses))
    {
        /// todo  arthur remember what to do here
        LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to update export path, skipping");
        return;
    }

    LOG_INFO(storage.log, "ExportPartition scheduler task: Marked part export {} as completed", part_name);
    
    Strings parts_in_processing_or_pending;
    if (Coordination::Error::ZOK != zk->tryGetChildren(export_path / "processing", parts_in_processing_or_pending))
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: Failed to get parts in processing or pending, will not try to commit export partition");
        return;
    }

    if (!parts_in_processing_or_pending.empty())
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: There are still parts in processing or pending, will not try to commit export partition");
        return;
    }

    LOG_INFO(storage.log, "ExportPartition scheduler task: All parts are processed, will try to commit export partition");

    ExportPartitionUtils::commit(manifest, destination_storage, zk, storage.log.load(), export_path, storage.getContext());
}

void ExportPartitionTaskScheduler::handlePartExportFailure(
    const std::filesystem::path & processing_parts_path,
    const std::string & part_name,
    const std::filesystem::path & export_path,
    const zkutil::ZooKeeperPtr & zk,
    const String & exception,
    size_t max_retries
)
{
    Coordination::Stat locked_by_stat;
    std::string locked_by;

    if (!zk->tryGet(export_path / "locks" / part_name, locked_by, &locked_by_stat))
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} is not locked by any replica, will not increment error counts", part_name);
        return;
    }

    if (locked_by != storage.replica_name)
    {
        LOG_INFO(storage.log, "ExportPartition scheduler task: Part {} is locked by another replica, will not increment error counts", part_name);
        return;
    }

    Coordination::Requests ops;

    const auto processing_part_path = processing_parts_path / part_name;
    std::string retry_count_string;
    if (zk->tryGet(processing_part_path / "retry_count", retry_count_string))
    {
        std::size_t retry_count = std::stoull(retry_count_string.c_str()) + 1;

        ops.emplace_back(zkutil::makeSetRequest(processing_part_path / "retry_count", std::to_string(retry_count), -1));
        ops.emplace_back(zkutil::makeRemoveRequest(export_path / "locks" / part_name, locked_by_stat.version));

        if (retry_count >= max_retries)
        {
            /// just set status in processing_part_path and finished_by
            ops.emplace_back(zkutil::makeSetRequest(processing_part_path / "status", "FAILED", -1));
            ops.emplace_back(zkutil::makeCreateRequest(processing_part_path / "finished_by", storage.replica_name, zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeSetRequest(export_path / "status", "FAILED", -1));

            LOG_INFO(storage.log, "ExportPartition scheduler task: Retry count limit exceeded for part {}, will try to fail the entire task", part_name);
        }

        std::size_t num_exceptions = 0;
    
        const auto exceptions_per_replica_path = export_path / "exceptions_per_replica" / storage.replica_name;
        const auto count_path = exceptions_per_replica_path / "count";
        const auto last_exception_path = exceptions_per_replica_path / "last_exception";

        if (zk->exists(exceptions_per_replica_path))
        {
            std::string num_exceptions_string;
            zk->tryGet(count_path, num_exceptions_string);
            num_exceptions = std::stoull(num_exceptions_string.c_str());

            ops.emplace_back(zkutil::makeSetRequest(last_exception_path / "part", part_name, -1));
            ops.emplace_back(zkutil::makeSetRequest(last_exception_path / "exception", exception, -1));
        }
        else
        {
            ops.emplace_back(zkutil::makeCreateRequest(exceptions_per_replica_path, "", zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(count_path, "0", zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(last_exception_path, "", zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(last_exception_path / "part", part_name, zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(last_exception_path / "exception", exception, zkutil::CreateMode::Persistent));
        }

        num_exceptions++;
        ops.emplace_back(zkutil::makeSetRequest(count_path, std::to_string(num_exceptions), -1));

        Coordination::Responses responses;
        if (Coordination::Error::ZOK != zk->tryMulti(ops, responses))
        {
            LOG_INFO(storage.log, "ExportPartition scheduler task: All failure mechanism failed, will not try to update it");
            return;
        }
    }
}

}
