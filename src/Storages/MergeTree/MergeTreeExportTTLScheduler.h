#pragma once

#include <Storages/MergeTree/ExportTTLScheduler.h>
#include <Common/MultiVersion.h>

#include <map>
#include <mutex>

namespace DB
{

class StorageMergeTree;

/// The export index of the `EXPORT` TTL of a plain `MergeTree`, in the data directory of the table:
/// `export_ttl/<destination_key>/partitions/<partition_id>.json`, each file written atomically.
/// Versions are kept in memory only, for the compare-and-set of the scheduler.
class MergeTreeExportTTLIndex
{
public:
    explicit MergeTreeExportTTLIndex(StorageMergeTree & storage_);

    void load();

    /// Stores `entry` if the stored version is still `entry.version`. Returns false otherwise.
    bool update(const String & destination_key, const ExportTTLVersionedEntry & entry);

    void removeDestination(const String & destination_key);

    ExportTTLIndexSnapshotPtr getSnapshot() const { return snapshot.get(); }
    ExportFencePtr getFence() const { return snapshot.get()->fence; }

    /// Highest block number in any entry, 0 if there is none.
    Int64 maxBlock() const;

private:
    StorageMergeTree & storage;

    mutable std::mutex mutex;
    /// By destination key, then by partition id.
    std::map<String, std::map<String, ExportTTLVersionedEntry>> entries;
    MultiVersion<ExportTTLIndexSnapshot> snapshot;

    String getRootPath() const;
    String getDestinationPath(const String & destination_key) const;
    String getEntryPath(const String & destination_key, const String & partition_id) const;

    /// Caller must hold `mutex`.
    void publishSnapshot();
};

/// `ExportTTLScheduler` of a plain `MergeTree`. A group is claimed in the index before its task is
/// created, under the lock that merge selection holds, so no merge of its parts can be selected
/// meanwhile. A crash in between leaves a claim without a task, which the next tick retries.
class MergeTreeExportTTLScheduler final : public ExportTTLScheduler
{
public:
    MergeTreeExportTTLScheduler(StorageMergeTree & storage_, MergeTreeExportTTLIndex & index_);

protected:
    bool acquireSchedulerLock() override { return true; }
    bool isPaused() override;
    ExportTTLIndexSnapshotPtr getIndexSnapshot() override { return index.getSnapshot(); }
    bool identifiesDestinationByUUID() const override { return true; }
    /// A single replica keeps its state in memory.
    std::optional<ExportTTLSchedulerState> readSchedulerState(const String &) override { return std::nullopt; }
    void writeSchedulerState(const String &, const ExportTTLSchedulerState &) override {}
    String getReplicaName() const override { return {}; }
    TaskState getTaskState(const String & transaction_id) override;
    bool updateIndexEntry(const String & destination_key, const ExportTTLVersionedEntry & entry) override { return index.update(destination_key, entry); }
    bool startGroup(const GroupToStart & group, const ContextPtr & context) override;
    bool isPartBeingMerged(const MergeTreeDataPartPtr & part) override;
    void killTask(const String & transaction_id) override;
    void removeDestination(const String & destination_key) override { index.removeDestination(destination_key); }

private:
    StorageMergeTree & plain_storage;
    MergeTreeExportTTLIndex & index;
};

}
