#pragma once

#include <Storages/MergeTree/IExportTTLIndex.h>
#include <Common/MultiVersion.h>

#include <map>
#include <mutex>

namespace DB
{

class StorageMergeTree;

/// The export index of the `EXPORT` TTL of a plain `MergeTree`, in the data directory of the table:
/// `export_ttl/<destination_key>/partitions/<partition_id>.json`, each file written atomically.
/// Versions are kept in memory only, for the compare-and-set of the scheduler. There is one
/// scheduler, so its lock is always held.
class MergeTreeExportTTLIndex final : public IExportTTLIndex
{
public:
    explicit MergeTreeExportTTLIndex(StorageMergeTree & storage_);

    void load();

    ExportTTLIndexSnapshotPtr getSnapshot() override { return snapshot.get(); }
    ExportTTLIndexSnapshotPtr getLatest() const override { return snapshot.get(); }
    bool updateEntry(const String & destination_key, const ExportTTLVersionedEntry & versioned) override;
    void removeDestination(const String & destination_key) override;
    bool tryAcquireSchedulerLock() override { return true; }
    void releaseSchedulerLock() override {}
    String getSchedulerReplica() const override { return {}; }

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

}
