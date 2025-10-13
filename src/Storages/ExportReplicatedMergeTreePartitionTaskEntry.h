#pragma once

#include <Storages/ExportReplicatedMergeTreePartitionManifest.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>

namespace DB
{
struct ExportReplicatedMergeTreePartitionTaskEntry
{
    using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;
    ExportReplicatedMergeTreePartitionManifest manifest;

    std::size_t parts_to_do;
    /// References to the parts that should be exported
    /// This is used to prevent the parts from being deleted before finishing the export operation
    /// It does not mean this replica will export all the parts
    /// There is also a chance this replica does not contain a given part and it is totally ok.
    std::vector<DataPartPtr> part_references;
};

}
