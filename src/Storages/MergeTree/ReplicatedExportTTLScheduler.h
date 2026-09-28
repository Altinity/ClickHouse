#pragma once

#include <Storages/MergeTree/ExportTTLScheduler.h>
#include <Common/ZooKeeper/ZooKeeper.h>

#include <mutex>

namespace DB
{

class StorageReplicatedMergeTree;

/// `ExportTTLScheduler` of a `ReplicatedMergeTree`: the index is in Keeper (see
/// `ReplicatedExportTTLIndex`), a group is claimed in the transaction that creates its task,
/// with a check that no merge was assigned meanwhile, and one replica schedules at a time.
class ReplicatedExportTTLScheduler final : public ExportTTLScheduler
{
public:
    explicit ReplicatedExportTTLScheduler(StorageReplicatedMergeTree & storage_);
    ~ReplicatedExportTTLScheduler() override;

    /// E.g. when the replica goes read-only, so another replica can take over.
    void releaseSchedulerLock();

protected:
    bool acquireSchedulerLock() override;
    bool isPaused() override;
    ExportTTLIndexSnapshotPtr getIndexSnapshot() override;
    bool identifiesDestinationByUUID() const override { return false; }
    String getReplicaName() const override;
    String getSchedulerReplica() override;
    TaskState getTaskState(const String & transaction_id) override;
    bool isCommitInProgress(const String & transaction_id) override;
    bool updateIndexEntry(const String & destination_key, const ExportTTLVersionedEntry & entry) override;
    bool startGroup(const GroupToStart & group, const ContextPtr & context) override;
    bool isPartBeingMerged(const MergeTreeDataPartPtr & part) override;
    void killTask(const String & transaction_id) override;
    void removeDestination(const String & destination_key) override;

private:
    StorageReplicatedMergeTree & replicated_storage;

    std::mutex lock_mutex;
    /// Keeps the session the lock was created in alive: the holder refers to it.
    zkutil::ZooKeeperPtr lock_zookeeper;
    zkutil::EphemeralNodeHolderPtr lock_holder;
};

}
