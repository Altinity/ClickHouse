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

    std::mutex status_changes_mutex;
    std::queue<std::string> status_changes;

    /// Serializes the full bodies of poll() and handleStatusChanges() against each other.
    /// Held across ZooKeeper I/O so those two tasks never overlap; the mirror lock
    /// (StorageReplicatedMergeTree::export_merge_tree_partition_mutex) is then taken only
    /// briefly under this, for the in-memory container mutations. This is what lets the
    /// system.replicated_partition_exports reader (which takes the mirror lock shared and
    /// briefly) avoid waiting behind slow-network ZooKeeper round-trips.
    /// Lock ordering: this -> export_merge_tree_partition_mutex -> export_manifests_mutex.
    std::mutex background_task_serialization_mutex;
};

}
