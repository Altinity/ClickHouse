#pragma once

#include <Storages/ExportReplicatedMergeTreePartitionManifest.h>

namespace DB
{
struct ExportReplicatedMergeTreePartitionTaskEntry
{
    ExportReplicatedMergeTreePartitionManifest manifest;

    std::size_t parts_to_do;
};
}
