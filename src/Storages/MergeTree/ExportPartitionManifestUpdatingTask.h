#pragma once

#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>
#include <Storages/System/StorageSystemReplicatedPartitionExports.h>
#include <Storages/ExportReplicatedMergeTreePartitionTaskEntry.h>
namespace DB
{

class StorageReplicatedMergeTree;
struct ExportReplicatedMergeTreePartitionManifest;

class ExportPartitionManifestUpdatingTask
{
public:
    ExportPartitionManifestUpdatingTask(StorageReplicatedMergeTree & storage);

    void poll();

    void handleStatusChanges();

    void addStatusChange(const std::string & key);

    /// Returns a snapshot of every replicated partition export task tracked by this
    /// replica's in-memory mirror. No ZooKeeper traffic; safe to call from query threads.
    std::vector<ReplicatedPartitionExportInfo> getPartitionExportsInfo() const;

private:
    StorageReplicatedMergeTree & storage;

    void addTask(
        const ExportReplicatedMergeTreePartitionManifest & metadata,
        ExportReplicatedMergeTreePartitionTaskEntry::Status status,
        std::map<String, LastExceptionEntry> last_exception_per_replica,
        const std::string & key,
        auto & entries_by_key
    );

    void removeStaleEntries(
        const std::unordered_set<std::string> & zk_children,
        auto & entries_by_key
    );

    /// Republish export_read_model as a fresh immutable copy (part_references stripped). Called
    /// under M_task at the end of each poll() / handleStatusChanges() batch.
    void publishReadModel();

    std::mutex status_changes_mutex;
    std::queue<std::string> status_changes;

    /// M_task: serializes poll() and handleStatusChanges() and is the sole guard of the
    /// writer-private authoritative container. Held across ZooKeeper I/O; no reader takes it
    /// (readers use export_read_model). Lock ordering: this -> export_manifests_mutex.
    std::mutex background_task_serialization_mutex;
};

}
