#include <Storages/MergeTree/ReplicatedExportTTLIndex.h>

#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/logger_useful.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ExportTTLIndexSnapshotRefreshes;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int NO_ZOOKEEPER;
}

ReplicatedExportTTLIndex::ReplicatedExportTTLIndex(
    String zookeeper_path_,
    String replica_name_,
    std::function<zkutil::ZooKeeperPtr()> get_zookeeper_,
    std::function<bool()> is_readonly_,
    LoggerPtr log_)
    : zookeeper_path(std::move(zookeeper_path_))
    , replica_name(std::move(replica_name_))
    , get_zookeeper(std::move(get_zookeeper_))
    , is_readonly(std::move(is_readonly_))
    , log(std::move(log_))
    , latest(ExportTTLIndexSnapshot::build(/* version */ -1, {}))
{
}

ReplicatedExportTTLIndex::~ReplicatedExportTTLIndex()
{
    releaseSchedulerLock();
}

zkutil::ZooKeeperPtr ReplicatedExportTTLIndex::getZooKeeper() const
{
    auto zookeeper = get_zookeeper();
    if (!zookeeper)
        throw Exception(ErrorCodes::NO_ZOOKEEPER, "Cannot get ZooKeeper");
    return zookeeper;
}

void ReplicatedExportTTLIndex::releaseSchedulerLock()
{
    std::lock_guard lock(lock_mutex);
    lock_holder.reset();
    lock_zookeeper.reset();
}

bool ReplicatedExportTTLIndex::tryAcquireSchedulerLock()
{
    auto zookeeper = get_zookeeper();
    if (!zookeeper || zookeeper->expired() || is_readonly())
    {
        releaseSchedulerLock();
        return false;
    }

    std::lock_guard lock(lock_mutex);
    if (lock_holder && lock_zookeeper == zookeeper)
        return true;

    lock_holder.reset();
    lock_zookeeper.reset();

    const auto root_path = getRootPath();
    if (const auto code = zookeeper->tryCreate(root_path, "", zkutil::CreateMode::Persistent);
        code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
        throw zkutil::KeeperException::fromPath(code, root_path);

    lock_holder = zkutil::EphemeralNodeHolder::tryCreate(getSchedulerLockPath(), *zookeeper, replica_name);
    if (!lock_holder)
        return false;

    lock_zookeeper = zookeeper;
    LOG_INFO(log, "This replica schedules the EXPORT TTL of the table");
    return true;
}

String ReplicatedExportTTLIndex::getSchedulerReplica() const
{
    {
        std::lock_guard lock(lock_mutex);
        if (lock_holder && !lock_zookeeper->expired())
            return replica_name;
    }

    std::lock_guard lock(scheduler_replica_mutex);
    return scheduler_replica;
}

void ReplicatedExportTTLIndex::refreshAndWatch(
    const zkutil::ZooKeeperPtr & zookeeper, const Coordination::WatchCallbackPtr & watch, bool has_export_ttl)
{
    if (!has_export_ttl)
    {
        Coordination::Stat stat;
        const int32_t version = zookeeper->exists(getVersionPath(), &stat) ? stat.version : -1;
        const auto snapshot = refresh(zookeeper, version);
        loaded = true;
        if (snapshot->entries.empty())
        {
            std::lock_guard lock(scheduler_replica_mutex);
            scheduler_replica.clear();
            return;
        }
    }

    /// The watches are set before the nodes are read, so a change after a read always calls this again.
    String replica;
    if (zookeeper->existsWatch(getSchedulerLockPath(), nullptr, watch))
        zookeeper->tryGet(getSchedulerLockPath(), replica);
    {
        std::lock_guard lock(scheduler_replica_mutex);
        scheduler_replica = std::move(replica);
    }

    /// Without the `version` node there is no index yet: it is created before the first entry.
    Coordination::Stat stat;
    const int32_t version = zookeeper->existsWatch(getVersionPath(), &stat, watch) ? stat.version : -1;
    refresh(zookeeper, version);
    loaded = true;
}

String ReplicatedExportTTLIndex::getRootPath() const
{
    return fs::path(zookeeper_path) / "export_ttl";
}

String ReplicatedExportTTLIndex::getVersionPath() const
{
    return fs::path(getRootPath()) / "version";
}

String ReplicatedExportTTLIndex::getSchedulerLockPath() const
{
    return fs::path(getRootPath()) / "scheduler_lock";
}

String ReplicatedExportTTLIndex::getDestinationsPath() const
{
    return fs::path(getRootPath()) / "destinations";
}

String ReplicatedExportTTLIndex::getDestinationPath(const String & destination_key) const
{
    return fs::path(getDestinationsPath()) / destination_key;
}

String ReplicatedExportTTLIndex::getIndexEntryPath(const String & destination_key, const String & partition_id) const
{
    return fs::path(getDestinationPath(destination_key)) / "partitions" / partition_id;
}

int32_t ReplicatedExportTTLIndex::readVersion(const zkutil::ZooKeeperPtr & zookeeper) const
{
    const auto path = getVersionPath();

    Coordination::Stat stat;
    if (!zookeeper->exists(path, &stat))
    {
        for (const auto & node : {getRootPath(), path})
        {
            const auto code = zookeeper->tryCreate(node, "", zkutil::CreateMode::Persistent);
            if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
                throw zkutil::KeeperException::fromPath(code, node);
        }
        zookeeper->exists(path, &stat);
    }
    return stat.version;
}

std::vector<String> ReplicatedExportTTLIndex::listDestinations(const zkutil::ZooKeeperPtr & zookeeper) const
{
    Strings children;
    const auto code = zookeeper->tryGetChildren(getDestinationsPath(), children);
    if (code == Coordination::Error::ZNONODE)
        return {};
    if (code != Coordination::Error::ZOK)
        throw zkutil::KeeperException::fromPath(code, getDestinationsPath());
    return children;
}

std::map<String, ExportTTLVersionedEntry> ReplicatedExportTTLIndex::readIndex(
    const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key) const
{
    const String partitions_path = fs::path(getDestinationPath(destination_key)) / "partitions";

    Strings partitions;
    const auto code = zookeeper->tryGetChildren(partitions_path, partitions);
    if (code == Coordination::Error::ZNONODE)
        return {};
    if (code != Coordination::Error::ZOK)
        throw zkutil::KeeperException::fromPath(code, partitions_path);

    Strings paths;
    paths.reserve(partitions.size());
    for (const auto & partition_id : partitions)
        paths.push_back(fs::path(partitions_path) / partition_id);

    auto responses = zookeeper->tryGet(paths);
    responses.waitForResponses();

    std::map<String, ExportTTLVersionedEntry> result;
    for (size_t i = 0; i < partitions.size(); ++i)
    {
        const auto & response = responses[i];
        if (response.error == Coordination::Error::ZNONODE)
            continue;
        if (response.error != Coordination::Error::ZOK)
            throw zkutil::KeeperException::fromPath(response.error, paths[i]);

        result[partitions[i]] = ExportTTLVersionedEntry{
            .entry = ExportTTLIndexEntry::fromJSONString(partitions[i], response.data),
            .version = response.stat.version,
        };
    }
    return result;
}

ExportTTLVersionedEntry ReplicatedExportTTLIndex::readIndexEntry(
    const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key, const String & partition_id) const
{
    String data;
    Coordination::Stat stat;
    if (!zookeeper->tryGet(getIndexEntryPath(destination_key, partition_id), data, &stat))
    {
        ExportTTLVersionedEntry missing;
        missing.entry.partition_id = partition_id;
        return missing;
    }
    return ExportTTLVersionedEntry{.entry = ExportTTLIndexEntry::fromJSONString(partition_id, data), .version = stat.version};
}

void ReplicatedExportTTLIndex::ensureDestination(
    const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key, const String & description) const
{
    const std::vector<std::pair<String, String>> nodes{
        {getRootPath(), ""},
        {getDestinationsPath(), ""},
        {getDestinationPath(destination_key), description},
        {fs::path(getDestinationPath(destination_key)) / "partitions", ""},
    };

    for (const auto & [path, data] : nodes)
    {
        const auto code = zookeeper->tryCreate(path, data, zkutil::CreateMode::Persistent);
        if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
            throw zkutil::KeeperException::fromPath(code, path);
    }
}

void ReplicatedExportTTLIndex::appendUpdateEntryOps(
    Coordination::Requests & ops, const String & destination_key, const ExportTTLIndexEntry & entry, int32_t version) const
{
    const auto path = getIndexEntryPath(destination_key, entry.partition_id);
    if (version < 0)
        ops.emplace_back(zkutil::makeCreateRequest(path, entry.toJSONString(), zkutil::CreateMode::Persistent));
    else
        ops.emplace_back(zkutil::makeSetRequest(path, entry.toJSONString(), version));

    ops.emplace_back(zkutil::makeSetRequest(getVersionPath(), "", -1));
}

bool ReplicatedExportTTLIndex::updateEntry(const String & destination_key, const ExportTTLVersionedEntry & versioned)
{
    const auto zookeeper = getZooKeeper();
    if (versioned.version < 0)
        ensureDestination(zookeeper, destination_key, destination_key);

    Coordination::Requests ops;
    appendUpdateEntryOps(ops, destination_key, versioned.entry, versioned.version);

    Coordination::Responses responses;
    const auto code = zookeeper->tryMulti(ops, responses);
    if (code == Coordination::Error::ZOK)
        return true;
    if (code == Coordination::Error::ZBADVERSION || code == Coordination::Error::ZNODEEXISTS || code == Coordination::Error::ZNONODE)
        return false;
    zkutil::KeeperMultiException::check(code, ops, responses);
    return false;
}

void ReplicatedExportTTLIndex::removeDestination(const String & destination_key)
{
    const auto zookeeper = getZooKeeper();
    zookeeper->tryRemoveRecursive(getDestinationPath(destination_key));
    zookeeper->trySet(getVersionPath(), "", -1);
    LOG_INFO(log, "Removed the TTL export index of destination {}", destination_key);
}

ExportTTLIndexSnapshotPtr ReplicatedExportTTLIndex::getSnapshot()
{
    return getSnapshot(getZooKeeper());
}

ExportTTLIndexSnapshotPtr ReplicatedExportTTLIndex::getSnapshot(const zkutil::ZooKeeperPtr & zookeeper)
{
    auto snapshot = refresh(zookeeper, readVersion(zookeeper));
    loaded = true;
    return snapshot;
}

ExportTTLIndexSnapshotPtr ReplicatedExportTTLIndex::refresh(const zkutil::ZooKeeperPtr & zookeeper, int32_t version)
{
    /// `version` is read before the index, so the index is at least as new as the version it is cached
    /// for. Every change of the index bumps the version in the same transaction.
    if (auto cached = latest.get(); cached->version == version)
        return cached;

    std::lock_guard lock(refresh_mutex);
    if (auto cached = latest.get(); cached->version == version)
        return cached;

    std::map<String, std::map<String, ExportTTLVersionedEntry>> entries;
    for (const auto & destination_key : listDestinations(zookeeper))
        entries[destination_key] = readIndex(zookeeper, destination_key);

    ProfileEvents::increment(ProfileEvents::ExportTTLIndexSnapshotRefreshes);
    auto snapshot = ExportTTLIndexSnapshot::build(version, std::move(entries));
    LOG_DEBUG(log, "Read the export index at version {}: {} partition(s) with exported or claimed parts",
        version, snapshot->fence->entries_by_partition.size());

    latest.set(std::move(snapshot));
    return latest.get();
}

std::pair<ExportFencePtr, int32_t> ReplicatedExportTTLIndex::getForMergeAssignment(const zkutil::ZooKeeperPtr & zookeeper)
{
    const auto snapshot = getSnapshot(zookeeper);
    return {snapshot->fence, snapshot->version};
}

}
