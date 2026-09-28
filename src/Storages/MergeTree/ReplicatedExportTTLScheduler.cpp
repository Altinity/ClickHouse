#include <Storages/MergeTree/ReplicatedExportTTLScheduler.h>

#include <Storages/ExportReplicatedMergeTreeTaskManifest.h>
#include <Storages/ExportReplicatedMergeTreeTaskEntry.h>
#include <Storages/MergeTree/Compaction/MergePredicates/ReplicatedMergeTreeMergePredicate.h>
#include <Storages/MergeTree/ExportTaskUtils.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Common/ProfileEvents.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/logger_useful.h>
#include <base/defines.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ExportTaskZooKeeperRequests;
    extern const Event ExportTaskZooKeeperMulti;
}

namespace DB
{

ReplicatedExportTTLScheduler::ReplicatedExportTTLScheduler(StorageReplicatedMergeTree & storage_)
    : ExportTTLScheduler(storage_)
    , replicated_storage(storage_)
{
}

ReplicatedExportTTLScheduler::~ReplicatedExportTTLScheduler()
{
    releaseSchedulerLock();
}

void ReplicatedExportTTLScheduler::releaseSchedulerLock()
{
    std::lock_guard lock(lock_mutex);
    lock_holder.reset();
    lock_zookeeper.reset();
}

bool ReplicatedExportTTLScheduler::acquireSchedulerLock()
{
    auto zookeeper = replicated_storage.tryGetZooKeeper();
    if (!zookeeper || zookeeper->expired() || replicated_storage.is_readonly)
    {
        releaseSchedulerLock();
        return false;
    }

    std::lock_guard lock(lock_mutex);
    if (lock_holder && lock_zookeeper == zookeeper)
        return true;

    lock_holder.reset();
    lock_zookeeper.reset();

    const auto & fence = *replicated_storage.export_fence;
    const auto root_path = fence.getRootPath();
    if (const auto code = zookeeper->tryCreate(root_path, "", zkutil::CreateMode::Persistent);
        code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
        throw zkutil::KeeperException::fromPath(code, root_path);

    lock_holder = zkutil::EphemeralNodeHolder::tryCreate(fence.getSchedulerLockPath(), *zookeeper, replicated_storage.getReplicaName());
    if (!lock_holder)
        return false;

    lock_zookeeper = zookeeper;
    LOG_INFO(log, "This replica schedules the EXPORT TTL of the table");
    return true;
}

bool ReplicatedExportTTLScheduler::isPaused()
{
    return replicated_storage.parts_mover.moves_blocker.isCancelled();
}

ExportTTLIndexSnapshotPtr ReplicatedExportTTLScheduler::getIndexSnapshot()
{
    return replicated_storage.export_fence->getSnapshot(replicated_storage.getZooKeeper());
}

std::optional<ExportTTLSchedulerState> ReplicatedExportTTLScheduler::readSchedulerState(const String & destination_key)
{
    String data;
    if (!replicated_storage.getZooKeeper()->tryGet(replicated_storage.export_fence->getSchedulerStatePath(destination_key), data))
        return std::nullopt;
    return ExportTTLSchedulerState::fromJSONString(data);
}

void ReplicatedExportTTLScheduler::writeSchedulerState(const String & destination_key, const ExportTTLSchedulerState & state)
{
    const auto zookeeper = replicated_storage.getZooKeeper();
    const auto & fence = *replicated_storage.export_fence;
    const auto path = fence.getSchedulerStatePath(destination_key);
    const auto data = state.toJSONString();

    auto code = zookeeper->trySet(path, data);
    if (code == Coordination::Error::ZNONODE)
    {
        fence.ensureDestination(zookeeper, destination_key, destination_key);
        code = zookeeper->tryCreate(path, data, zkutil::CreateMode::Persistent);
        if (code == Coordination::Error::ZNODEEXISTS)
            code = zookeeper->trySet(path, data);
    }

    if (code != Coordination::Error::ZOK)
        throw zkutil::KeeperException::fromPath(code, path);
}

String ReplicatedExportTTLScheduler::getReplicaName() const
{
    return replicated_storage.getReplicaName();
}

ExportTTLScheduler::TaskState ReplicatedExportTTLScheduler::getTaskState(const String & transaction_id)
{
    using Status = ExportReplicatedMergeTreeTaskEntry::Status;
    const auto to_task_status = [](Status status) -> TaskStatus
    {
        switch (status)
        {
            case Status::PENDING: return TaskStatus::PENDING;
            case Status::COMPLETED: return TaskStatus::COMPLETED;
            case Status::FAILED: return TaskStatus::FAILED;
            case Status::KILLED: return TaskStatus::KILLED;
        }
        UNREACHABLE();
    };

    /// The in-memory mirror of the tasks may lag behind Keeper, which only delays a retry. A task it
    /// does not know yet, e.g. just created, is read from Keeper, so it is never taken for missing.
    if (const auto tasks = replicated_storage.export_partition_manifests.get())
    {
        const auto & by_transaction_id = tasks->get<ExportTaskEntryTagByTransactionId>();
        if (const auto it = by_transaction_id.find(transaction_id); it != by_transaction_id.end())
            return TaskState{.status = to_task_status(it->status), .retry_of = it->manifest.retry_of};
    }

    const auto zookeeper = replicated_storage.getZooKeeper();
    const fs::path task_path = fs::path(replicated_storage.zookeeper_path) / "exports" / transaction_id;
    const Strings paths{task_path / "status", task_path / "metadata.json"};

    auto responses = zookeeper->tryGet(paths);
    responses.waitForResponses();

    TaskState state;
    if (responses[0].error == Coordination::Error::ZNONODE)
        return state;
    if (responses[0].error != Coordination::Error::ZOK)
        throw zkutil::KeeperException::fromPath(responses[0].error, paths[0]);

    if (const auto status = magic_enum::enum_cast<Status>(responses[0].data))
    {
        state.status = to_task_status(*status);
    }
    else
    {
        /// Treated as failed: the recovery check still decides whether it committed.
        LOG_WARNING(log, "Export task {} has an unknown status {}", transaction_id, responses[0].data);
        state.status = TaskStatus::FAILED;
    }

    if (responses[1].error == Coordination::Error::ZOK)
        state.retry_of = ExportReplicatedMergeTreeTaskManifest::fromJsonString(responses[1].data).retry_of;
    else if (responses[1].error != Coordination::Error::ZNONODE)
        throw zkutil::KeeperException::fromPath(responses[1].error, paths[1]);

    return state;
}

bool ReplicatedExportTTLScheduler::updateIndexEntry(const String & destination_key, const ExportTTLVersionedEntry & entry)
{
    const auto zookeeper = replicated_storage.getZooKeeper();
    const auto & fence = *replicated_storage.export_fence;

    if (entry.version < 0)
        fence.ensureDestination(zookeeper, destination_key, destination_key);

    Coordination::Requests ops;
    fence.appendUpdateEntryOps(ops, destination_key, entry.entry, entry.version);

    Coordination::Responses responses;
    const auto code = zookeeper->tryMulti(ops, responses);
    if (code == Coordination::Error::ZOK)
        return true;
    if (code == Coordination::Error::ZBADVERSION || code == Coordination::Error::ZNODEEXISTS || code == Coordination::Error::ZNONODE)
        return false;
    zkutil::KeeperMultiException::check(code, ops, responses);
    return false;
}

bool ReplicatedExportTTLScheduler::startGroup(const GroupToStart & group, const ContextPtr & context)
{
    auto zookeeper = replicated_storage.getZooKeeperAndAssertNotReadonly();
    replicated_storage.checkAllReplicasSupportExportTTL(zookeeper);

    /// Its `/log` version is checked when the task is created, so no merge can be assigned between
    /// checking the parts below and claiming them.
    const auto merge_predicate = replicated_storage.queue.getMergePredicate(zookeeper, PartitionIdsHint{group.partition_id});
    for (const auto & part : group.parts)
    {
        const auto covering_part = merge_predicate->getCoveringVirtualPart(part->name);
        if (covering_part.empty())
            return false;

        const auto covering_info = MergeTreePartInfo::fromPartName(covering_part, replicated_storage.format_version);
        if (covering_info.min_block != part->info.min_block || covering_info.max_block != part->info.max_block)
            return false;
    }

    const auto source_metadata = replicated_storage.getInMemoryMetadataPtr(context, false);
    const auto destination_metadata = group.destination->getInMemoryMetadataPtr(context, false);
    ExportTaskUtils::verifyExportSchemaCastable(source_metadata, destination_metadata, group.destination->getStorageID(), context);

    MergeTreeData::DataPartsVector parts(group.parts.begin(), group.parts.end());
    auto manifest = replicated_storage.buildExportTaskManifest(
        group.destination->getStorageID(), group.destination, source_metadata, destination_metadata, parts, group.partition_id, context);
    manifest.transaction_id = group.transaction_id;
    manifest.source = ExportTaskSource::ttl;
    manifest.retry_of = group.retry_of;

    const auto & fence = *replicated_storage.export_fence;
    if (group.entry.version < 0)
        fence.ensureDestination(zookeeper, group.destination_key, group.destination->getStorageID().getNameForLogs());

    const auto task_path = fs::path(replicated_storage.zookeeper_path) / "exports" / group.transaction_id;

    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeCheckRequest(fs::path(replicated_storage.zookeeper_path) / "log", merge_predicate->getVersion()));
    ExportTaskUtils::appendCreateExportTaskOps(ops, task_path, manifest);
    fence.appendUpdateEntryOps(ops, group.destination_key, group.entry.entry, group.entry.version);

    ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
    ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperMulti);
    Coordination::Responses responses;
    const auto code = zookeeper->tryMulti(ops, responses);

    if (code == Coordination::Error::ZOK)
    {
        if (replicated_storage.export_task_updating_task)
            replicated_storage.export_task_updating_task->schedule();
        return true;
    }

    if (code == Coordination::Error::ZBADVERSION || code == Coordination::Error::ZNODEEXISTS)
        return false;

    zkutil::KeeperMultiException::check(code, ops, responses);
    return false;
}

bool ReplicatedExportTTLScheduler::isPartBeingMerged(const MergeTreeDataPartPtr & part)
{
    return replicated_storage.queue.isGoingToBeMergedWithOtherParts(part->info);
}

void ReplicatedExportTTLScheduler::killTask(const String & transaction_id)
{
    replicated_storage.killExportTask(transaction_id);
}

void ReplicatedExportTTLScheduler::removeDestination(const String & destination_key)
{
    replicated_storage.export_fence->removeDestination(replicated_storage.getZooKeeper(), destination_key);
}

}
