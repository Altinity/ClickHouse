#pragma once

#include <Common/Logger.h>
#include <Common/MultiVersion.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/ExportFence.h>
#include <Storages/MergeTree/IExportTTLIndex.h>
#include <base/defines.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace DB
{

/// The `TTL ... EXPORT` state of a `ReplicatedMergeTree` table in Keeper, and the merge fence built from it.
///
/// Layout under `<zookeeper_path>/export_ttl`:
///  - `version`: bumped in every transaction that changes an index entry. Replicas cache the index
///    for its version, and a merge is assigned with a check of the version its predicate read the
///    index at, so no merge is assigned from a stale view of the export states;
///  - `scheduler_lock`: ephemeral, held by the replica that schedules TTL exports, whose name it holds;
///  - `destinations/<destination_key>`: holds the name of one destination;
///  - `destinations/<destination_key>/partitions/<partition_id>`: an `ExportTTLIndexEntry`.
///
/// Every replica of a table with an `EXPORT` TTL, or with an index left to clean up, watches `version`
/// and `scheduler_lock`, so that its copy of the index and of the lock holder follow Keeper, and
/// `system.ttl_exports` reads neither from Keeper.
class ReplicatedExportTTLIndex final : public IExportTTLIndex
{
public:
    /// `get_zookeeper` returns the current session of the table, or nullptr if there is none. A
    /// replica does not schedule while `is_readonly`.
    ReplicatedExportTTLIndex(
        String zookeeper_path_,
        String replica_name_,
        std::function<zkutil::ZooKeeperPtr()> get_zookeeper_,
        std::function<bool()> is_readonly_,
        LoggerPtr log_);
    ~ReplicatedExportTTLIndex() override;

    /// Reads the index again only when the version of the `version` node changed, so while nothing
    /// changes this costs one `exists`.
    ExportTTLIndexSnapshotPtr getSnapshot() override;
    ExportTTLIndexSnapshotPtr getLatest() const override { return loaded ? latest.get() : nullptr; }
    bool updateEntry(const String & destination_key, const ExportTTLVersionedEntry & versioned) override;
    /// Not transactional: an interrupted removal leaves fewer entries, which only lifts more of the fence.
    void removeDestination(const String & destination_key) override;
    /// Needs a session that is not expired, and holds the lock while that session lives and the
    /// replica is not read-only.
    bool tryAcquireSchedulerLock() override;
    /// Also e.g. when the replica goes read-only, so another replica can take over.
    void releaseSchedulerLock() override;
    String getSchedulerReplica() const override;

    /// Reads the index again if it changed, and the holder of the scheduler lock, with `watch` on the
    /// nodes they are read from, so that it is called again when they change. Creates no node.
    /// Without `has_export_ttl`, an empty index is read without watches: the table has nothing to
    /// follow until an `EXPORT` TTL is added, which calls this again.
    void refreshAndWatch(const zkutil::ZooKeeperPtr & zookeeper, const Coordination::WatchCallbackPtr & watch, bool has_export_ttl);

    String getRootPath() const;
    String getVersionPath() const;
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

    /// Appends the ops that store `entry`, with a check of `version` (see `ExportTTLVersionedEntry`), and bump the version of the index.
    void appendUpdateEntryOps(
        Coordination::Requests & ops, const String & destination_key, const ExportTTLIndexEntry & entry, int32_t version) const;

    ExportTTLIndexSnapshotPtr getSnapshot(const zkutil::ZooKeeperPtr & zookeeper);

    /// The export states as of the returned version of the `version` node.
    std::pair<ExportFencePtr, int32_t> getForMergeAssignment(const zkutil::ZooKeeperPtr & zookeeper);

private:
    const String zookeeper_path;
    const String replica_name;
    const std::function<zkutil::ZooKeeperPtr()> get_zookeeper;
    const std::function<bool()> is_readonly;
    const LoggerPtr log;

    /// Serializes reading the index again, which readers of `latest` do not wait for.
    std::mutex refresh_mutex;
    MultiVersion<ExportTTLIndexSnapshot> latest;
    /// Whether `latest` was read from Keeper at least once.
    std::atomic<bool> loaded = false;

    mutable std::mutex scheduler_replica_mutex;
    /// The holder of the scheduler lock as of the last `refreshAndWatch`.
    String scheduler_replica;

    mutable std::mutex lock_mutex;
    /// Keeps the session the lock was created in alive: the holder refers to it.
    zkutil::ZooKeeperPtr lock_zookeeper;
    zkutil::EphemeralNodeHolderPtr lock_holder;

    String getDestinationsPath() const;
    int32_t readVersion(const zkutil::ZooKeeperPtr & zookeeper) const;
    zkutil::ZooKeeperPtr getZooKeeper() const;

    /// The index as of `version` of the `version` node, read again unless it is cached for it.
    ExportTTLIndexSnapshotPtr refresh(const zkutil::ZooKeeperPtr & zookeeper, int32_t version);
};

using ReplicatedExportTTLIndexPtr = std::shared_ptr<ReplicatedExportTTLIndex>;

}
