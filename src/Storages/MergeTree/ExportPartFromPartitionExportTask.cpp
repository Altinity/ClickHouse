#include <Storages/MergeTree/ExportPartFromPartitionExportTask.h>
#include <Storages/ExportReplicatedMergeTreePartitionManifest.h>
#include <Storages/ExportReplicatedMergeTreePartitionTaskEntry.h>
#include <Common/ProfileEvents.h>
#include <Common/ZooKeeper/Types.h>

namespace ProfileEvents
{
    extern const Event ExportPartitionZooKeeperRequests;
    extern const Event ExportPartitionZooKeeperGet;
    extern const Event ExportPartitionZooKeeperGetChildren;
    extern const Event ExportPartitionZooKeeperCreate;
    extern const Event ExportPartitionZooKeeperMulti;
}
namespace DB
{

ExportPartFromPartitionExportTask::ExportPartFromPartitionExportTask(
    StorageReplicatedMergeTree & storage_,
    const std::string & key_,
    const MergeTreePartExportManifest & manifest_,
    size_t max_retries_)
    : storage(storage_),
    key(key_),
    manifest(manifest_),
    max_retries(max_retries_)
{
    export_part_task = std::make_shared<ExportPartTask>(storage, manifest);
}

bool ExportPartFromPartitionExportTask::executeStep()
{
    const auto zk = storage.getZooKeeper();
    const auto part_name = manifest.data_part->name;
    const auto processing_part_path = fs::path(storage.zookeeper_path) / "exports" / key / "processing" / part_name;

    LOG_INFO(storage.log, "ExportPartFromPartitionExportTask: Attempting to lock and increment retry count for part: {}", part_name);

    Coordination::Stat processing_part_stat;
    std::string processing_part_string;

    ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
    ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGet);
    if (!zk->tryGet(processing_part_path, processing_part_string, &processing_part_stat))
    {
        LOG_INFO(storage.log, "ExportPartFromPartitionExportTask: Failed to get processing part for {}, skipping", part_name);
        return false;
    }

    auto processing_part_entry = ExportReplicatedMergeTreePartitionProcessingPartEntry::fromJsonString(processing_part_string);

    /// If retry count already exceeds limit, mark as failed (recovery for stale state).
    if (processing_part_entry.retry_count > max_retries)
    {
        LOG_INFO(
            storage.log,
            "ExportPartFromPartitionExportTask: Retry count limit exceeded for part {} and it is not marked as failed. Trying to mark it as failed",
            part_name);

        processing_part_entry.status = ExportReplicatedMergeTreePartitionProcessingPartEntry::Status::FAILED;
        processing_part_entry.finished_by = "unknown";

        const auto export_path = fs::path(storage.zookeeper_path) / "exports" / key;
        zk->trySet(processing_part_path, processing_part_entry.toJsonString(), processing_part_stat.version);
        zk->trySet(export_path / "status", String(magic_enum::enum_name(ExportReplicatedMergeTreePartitionTaskEntry::Status::FAILED)).data(), -1);
        return false;
    }

    processing_part_entry.retry_count++;

    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeSetRequest(processing_part_path.string(), processing_part_entry.toJsonString(), processing_part_stat.version));
    ops.emplace_back(zkutil::makeCreateRequest((fs::path(storage.zookeeper_path) / "exports" / key / "locks" / part_name).string(), storage.replica_name, zkutil::CreateMode::Ephemeral));

    ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
    ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperMulti);

    Coordination::Responses responses;
    if (Coordination::Error::ZOK != zk->tryMulti(ops, responses))
    {
        LOG_INFO(storage.log, "ExportPartFromPartitionExportTask: Failed to either lock or increment retry count for part {}, skipping", part_name);
        return false;
    }

    LOG_INFO(storage.log, "ExportPartFromPartitionExportTask: Locked part: {}", part_name);
    export_part_task->executeStep();
    return false;
}

void ExportPartFromPartitionExportTask::cancel() noexcept
{
    export_part_task->cancel();
}

void ExportPartFromPartitionExportTask::onCompleted()
{
    export_part_task->onCompleted();
}

StorageID ExportPartFromPartitionExportTask::getStorageID() const
{
    return export_part_task->getStorageID();
}

Priority ExportPartFromPartitionExportTask::getPriority() const
{
    return export_part_task->getPriority();
}

String ExportPartFromPartitionExportTask::getQueryId() const
{
    return export_part_task->getQueryId();
}
}
