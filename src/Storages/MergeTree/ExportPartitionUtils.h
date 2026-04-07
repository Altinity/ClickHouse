#pragma once

#include <vector>
#include <string>
#include <Core/Field.h>
#include <Common/Logger.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include "Storages/IStorage.h"

namespace DB
{

class MergeTreeData;
struct ExportReplicatedMergeTreePartitionManifest;

namespace ExportPartitionUtils
{
    std::vector<std::string> getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path);

    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ExportReplicatedMergeTreePartitionManifest & manifest);

    /// Returns the partition key values for the given partition_id by reading from
    /// the first active local part. Throws LOGICAL_ERROR if no such part is found.
    ///
    /// Edge case: if the partition was dropped after export started, or this replica
    /// has not yet received any part for this partition (extreme replication lag on a
    /// recovery path), no active part will be found and the commit will fail. The task
    /// will be retried on the next poll cycle or picked up by a different replica.
    std::vector<Field> getPartitionValuesForIcebergCommit(
        MergeTreeData & storage, const String & partition_id);

    void commit(
        const ExportReplicatedMergeTreePartitionManifest & manifest,
        const StoragePtr & destination_storage,
        const zkutil::ZooKeeperPtr & zk,
        const LoggerPtr & log,
        const std::string & entry_path,
        const ContextPtr & context,
        MergeTreeData & source_storage
    );
}

}
