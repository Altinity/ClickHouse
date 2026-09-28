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

ReplicatedExportTTLIndex::ReplicatedExportTTLIndex(String zookeeper_path_, LoggerPtr log_)
    : zookeeper_path(std::move(zookeeper_path_))
    , log(std::move(log_))
    , latest(ExportTTLIndexSnapshot::build(/* version */ -1, {}))
{
}

String ReplicatedExportTTLIndex::getFencePath() const
{
    return fs::path(zookeeper_path) / "export_fence";
}

String ReplicatedExportTTLIndex::getRootPath() const
{
    return fs::path(zookeeper_path) / "export_ttl";
}

String ReplicatedExportTTLIndex::getSchedulerLockPath() const
{
    return fs::path(getRootPath()) / "scheduler_lock";
}

String ReplicatedExportTTLIndex::getDestinationPath(const String & destination_key) const
{
    return fs::path(getRootPath()) / destination_key;
}

String ReplicatedExportTTLIndex::getIndexEntryPath(const String & destination_key, const String & partition_id) const
{
    return fs::path(getDestinationPath(destination_key)) / "partitions" / partition_id;
}

int32_t ReplicatedExportTTLIndex::readFenceVersion(const zkutil::ZooKeeperPtr & zookeeper) const
{
    const auto path = getFencePath();

    Coordination::Stat stat;
    if (!zookeeper->exists(path, &stat))
    {
        const auto code = zookeeper->tryCreate(path, "", zkutil::CreateMode::Persistent);
        if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
            throw zkutil::KeeperException::fromPath(code, path);
        zookeeper->exists(path, &stat);
    }
    return stat.version;
}

std::vector<String> ReplicatedExportTTLIndex::listDestinations(const zkutil::ZooKeeperPtr & zookeeper) const
{
    Strings children;
    const auto code = zookeeper->tryGetChildren(getRootPath(), children);
    if (code == Coordination::Error::ZNONODE)
        return {};
    if (code != Coordination::Error::ZOK)
        throw zkutil::KeeperException::fromPath(code, getRootPath());

    std::erase_if(children, [](const String & child) { return child == "scheduler_lock"; });
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

    ops.emplace_back(zkutil::makeSetRequest(getFencePath(), "", -1));
}

void ReplicatedExportTTLIndex::removeDestination(const zkutil::ZooKeeperPtr & zookeeper, const String & destination_key) const
{
    zookeeper->tryRemoveRecursive(getDestinationPath(destination_key));
    zookeeper->trySet(getFencePath(), "", -1);
    LOG_INFO(log, "Removed the TTL export index of destination {}", destination_key);
}

ExportTTLIndexSnapshotPtr ReplicatedExportTTLIndex::getSnapshot(const zkutil::ZooKeeperPtr & zookeeper)
{
    /// Read before the index, so the index is at least as new as the version it is cached for. Every
    /// change of the index bumps the version in the same transaction.
    const auto version = readFenceVersion(zookeeper);

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
    LOG_DEBUG(log, "Read the export index at version {} of the fence: {} partition(s) with exported or claimed parts",
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
