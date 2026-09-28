#pragma once

#include <Common/Logger.h>
#include <Common/MultiVersion.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/ExportFence.h>
#include <base/defines.h>

#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace DB
{

/// The `TTL ... EXPORT` state of a `ReplicatedMergeTree` table in Keeper, and the merge fence built from it.
///
/// Layout under `<zookeeper_path>`:
///  - `export_ttl/<destination_key>`: holds `ExportTTLDestination` of one destination;
///  - `export_ttl/<destination_key>/partitions/<partition_id>`: an `ExportTTLIndexEntry`;
///  - `export_ttl/scheduler_lock`: ephemeral, held by the replica that schedules TTL exports, whose name it holds;
///  - `export_fence`: bumped in every transaction that changes an index entry. A merge is assigned
///    with a check of the version its predicate read the index at, so no merge is assigned from a
///    stale view of the export states.
class ReplicatedExportTTLIndex
{
public:
    ReplicatedExportTTLIndex(String zookeeper_path_, LoggerPtr log_);

    String getFencePath() const;
    String getRootPath() const;
    String getSchedulerLockPath() const;
    String getDestinationPath(const String & destination_key) const;
    String getIndexEntryPath(const String & destination_key, const String & partition_id) const;

    /// Destination keys that have an index.
    std::vector<String> listDestinations(const zkutil::ZooKeeperPtr & zookeeper) const;

    /// Index entries of every partition exported to `destination_key`.
    std::map<String, ExportTTLVersionedEntry> readIndex(const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key) const;

    ExportTTLVersionedEntry readIndexEntry(
        const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key, const String & partition_id) const;

    /// Creates the nodes of `destination_key`, so entries can be created in a transaction.
    void ensureDestination(const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key, const String & description) const;

    /// Appends the ops that store `entry`, with a check of `version` (see `ExportTTLVersionedEntry`), and bump the fence.
    void appendUpdateEntryOps(
        Coordination::Requests & ops, const String & destination_key, const ExportTTLIndexEntry & entry, int32_t version) const;

    /// Removes the index of `destination_key` and bumps the fence. Not transactional: an interrupted
    /// removal leaves fewer entries, which only lifts more of the fence.
    void removeDestination(const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key) const;

    /// The whole index as of the current version of the fence node. It is read again only when that
    /// version changed, so while nothing changes this costs one `exists`.
    ExportTTLIndexSnapshotPtr getSnapshot(const zkutil::ZooKeeperPtr & zookeeper);

    /// The export states as of the returned version of the fence node.
    std::pair<ExportFencePtr, int32_t> getForMergeAssignment(const zkutil::ZooKeeperPtr & zookeeper);

    /// The export states read by the last `getSnapshot`, without reading Keeper.
    ExportFencePtr getLatest() const { return latest.get()->fence; }

private:
    const String zookeeper_path;
    const LoggerPtr log;

    /// Serializes reading the index again, which readers of `latest` do not wait for.
    std::mutex refresh_mutex;
    MultiVersion<ExportTTLIndexSnapshot> latest;

    int32_t readFenceVersion(const zkutil::ZooKeeperPtr & zookeeper) const;
};

using ReplicatedExportTTLIndexPtr = std::shared_ptr<ReplicatedExportTTLIndex>;

}
