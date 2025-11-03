#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/logger_useful.h>
#include "Storages/ExportReplicatedMergeTreePartitionManifest.h"
#include "Storages/StorageReplicatedMergeTree.h"
#include <filesystem>

namespace DB
{

namespace fs = std::filesystem;

namespace ExportPartitionUtils
{
    std::vector<std::string> getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path)
    {
        std::vector<std::string> exported_paths;

        LOG_INFO(log, "ExportPartition: Getting exported paths for {}", export_path);

        std::vector<std::string> parts_children;
        if (Coordination::Error::ZOK != zk->tryGetChildren(fs::path(export_path) / "processed", parts_children))
        {
            /// todo arthur do something here
            LOG_INFO(log, "ExportPartition: Failed to get parts children, exiting");
            return exported_paths;
        }

        for (const auto & part_child : parts_children)
        {
            std::string path_in_destination_storage;

            if (zk->tryGet(fs::path(export_path) / "parts" / part_child / "path", path_in_destination_storage))
            {
                LOG_INFO(log, "ExportPartition: Failed to get path in destination storage for part {}, skipping", part_child);
                continue;
            }

            exported_paths.push_back(path_in_destination_storage);
        }

        return exported_paths;
    }

    void commit(
        const ExportReplicatedMergeTreePartitionManifest & manifest,
        const StoragePtr & destination_storage,
        const zkutil::ZooKeeperPtr & zk,
        const LoggerPtr & log,
        const std::string & entry_path,
        const ContextPtr & context
    )
    {
        const auto exported_paths = ExportPartitionUtils::getExportedPaths(log, zk, entry_path);
    
        if (exported_paths.size() == manifest.parts.size())
        {
            LOG_INFO(log, "ExportPartition: Exported paths size matches parts size, commit the export");
            destination_storage->commitExportPartitionTransaction(manifest.transaction_id, manifest.partition_id, exported_paths, context);
    
            LOG_INFO(log, "ExportPartition: Committed export, mark as completed");
            if (Coordination::Error::ZOK == zk->trySet(fs::path(entry_path) / "status", "COMPLETED", -1))
            {
                LOG_INFO(log, "ExportPartition: Marked export as completed");
            }
            else
            {
                LOG_INFO(log, "ExportPartition: Failed to mark export as completed, will not try to fix it");
            }
        }
        else
        {
            LOG_INFO(log, "ExportPartition: Skipping {}: exported paths size does not match parts size, this is a BUG", entry_path);   
        }
    }
    
}

}
