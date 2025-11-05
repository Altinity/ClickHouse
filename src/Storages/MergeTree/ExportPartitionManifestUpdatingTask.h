#pragma once

#include <string>
#include <unordered_set>
namespace DB
{

class StorageReplicatedMergeTree;
struct ExportReplicatedMergeTreePartitionManifest;

class ExportPartitionManifestUpdatingTask
{
public:
    ExportPartitionManifestUpdatingTask(StorageReplicatedMergeTree & storage);

    void run();

private:
    StorageReplicatedMergeTree & storage;

    void addTask(
        const ExportReplicatedMergeTreePartitionManifest & metadata,
        const std::string & key,
        auto & entries_by_key
    );

    void removeStaleEntries(
        const std::unordered_set<std::string> & zk_children,
        auto & entries_by_key
    );
};

}
