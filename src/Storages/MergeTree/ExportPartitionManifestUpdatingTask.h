#pragma once

namespace DB
{

class StorageReplicatedMergeTree;

class ExportPartitionManifestUpdatingTask
{
public:
    ExportPartitionManifestUpdatingTask(StorageReplicatedMergeTree & storage);

    void run();

private:
    StorageReplicatedMergeTree & storage;
};

}
