#include <Storages/ObjectStorage/MergeTree/ExportPartPlainMergeTreeTask.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <Interpreters/PartLog.h>
#include <Common/logger_useful.h>
#include <Storages/ObjectStorage/MergeTree/StorageObjectStorageMergeTreePartImporterSink.h>
#include <Storages/StorageMergeTree.h>
#include <Core/Settings.h>

namespace DB
{

namespace Setting
{
    extern const SettingsBool output_format_parallel_formatting;
}

ExportPartPlainMergeTreeTask::ExportPartPlainMergeTreeTask(
    StorageMergeTree & storage_,
    const DataPartPtr & part_to_export_,
    const StoragePtr & destination_storage_,
    ContextPtr context_,
    std::shared_ptr<MergeTreeExportManifest> manifest_,
    IExecutableTask::TaskResultCallback & task_result_callback_,
    size_t max_retries_)
    : storage(storage_)
    , part_to_export(part_to_export_)
    , destination_storage(destination_storage_)
    , context(std::move(context_))
    , manifest(std::move(manifest_))
    , task_result_callback(task_result_callback_)
    , max_retries(max_retries_)
{
    UInt64 transaction_id = std::stoull(manifest->transaction_id);
    priority.value = transaction_id;
}

StorageID ExportPartPlainMergeTreeTask::getStorageID() const
{
    return storage.getStorageID();
}

String ExportPartPlainMergeTreeTask::getQueryId() const
{
    return getStorageID().getShortName() + "::export_partition::" + manifest->transaction_id;
}

bool ExportPartPlainMergeTreeTask::executeStep()
{
    if (cancelled)
        return false;

    switch (state)
    {
        case State::NEED_PREPARE:
        {
            prepare();
            state = State::NEED_EXECUTE;
            return true;
        }
        case State::NEED_EXECUTE:
        {
            if (executeExport())
            {
                state = State::NEED_COMMIT;
            }
            else if (retry_count < max_retries)
            {
                retry_count++;
                LOG_INFO(getLogger("ExportPartPlainMergeTreeTask"),
                "Retrying export attempt {} for part {}",
                retry_count, part_to_export->name);
                state = State::NEED_EXECUTE;
            }
            else
            {
                state = State::FAILED;
            }

            return true;
        }
        case State::NEED_COMMIT:
        {
            if (commitExport())
            {
                state = State::SUCCESS;
            }
            else if (retry_count < max_retries)
            {
                retry_count++;
                LOG_INFO(getLogger("ExportPartPlainMergeTreeTask"),
                "Retrying export attempt {} for part {}",
                retry_count, part_to_export->name);
                state = State::NEED_COMMIT;
            }
            else
            {
                state = State::FAILED;
            }

            return true;
        }
        case State::FAILED:
        {
            std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);

            manifest->status = MergeTreeExportManifest::Status::failed;
            manifest->write();

            /// doesn't sound ideal, but it is actually ok to allow this partition to be re-exported as soon as a single part fails
            /// this is because the ongoing export will never commit, so it won't cause duplicates
            storage.already_exported_partition_ids.erase(manifest->partition_id);

            return false;
        }
        case State::SUCCESS:
        {
            return false;
        }
    }

    return false;
}


void ExportPartPlainMergeTreeTask::prepare()
{
    stopwatch_ptr = std::make_unique<Stopwatch>();
}

bool ExportPartPlainMergeTreeTask::executeExport()
{
    if (cancelled)
        return false;

    std::function<void(MergeTreePartImportStats)> part_log_wrapper = [this](MergeTreePartImportStats stats) {
        auto table_id = storage.getStorageID();

        UInt64 elapsed_ns = stopwatch_ptr->elapsedNanoseconds();

        storage.writePartLog(
            PartLogElement::Type::EXPORT_PART,
            stats.status,
            elapsed_ns,
            stats.part->name,
            stats.part,
            {stats.part},
            nullptr,
            nullptr);

        if (stats.status.code != 0)
        {
            LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"), "Error importing part {}: {}", stats.part->name, stats.status.message);
            return;
        }

        std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);

        storage.export_partition_transaction_id_to_manifest[manifest->transaction_id]->updateRemotePathAndWrite(
            stats.part->name, 
            stats.file_path);
    };

    try
    {
        auto context_copy = Context::createCopy(context);

        /// Manually disable parallelism because the idea is to control parallelism with tasks, not with formatting
        context_copy->setSetting("output_format_parallel_formatting", false);
        context_copy->setSetting("max_threads", 1);

        destination_storage->importMergeTreePart(
            storage,
            part_to_export,
            context_copy,
            part_log_wrapper);

        return true;
    }
    catch (...)
    {
        LOG_ERROR(getLogger("ExportPartPlainMergeTreeTask"), "Failed to export part {}", part_to_export->name);
        
        return false;
    }
}

bool ExportPartPlainMergeTreeTask::commitExport()
{
    std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);

    if (manifest->exportedPaths().size() == manifest->items.size())
    {
        destination_storage->commitExportPartitionTransaction(
            manifest->transaction_id,
            manifest->partition_id,
            manifest->exportedPaths(),
            context);
        manifest->status = MergeTreeExportManifest::Status::completed;
        manifest->write();
        storage.export_partition_transaction_id_to_manifest.erase(manifest->transaction_id);
        LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"),
        "Successfully committed export transaction {} for partition {}",
        manifest->transaction_id, manifest->partition_id);
    }

    LOG_INFO(getLogger("ExportPartPlainMergeTreeTask"), "Not all parts have been exported yet for transaction id {}, not comitting for this part", manifest->transaction_id);

    return true;
}

void ExportPartPlainMergeTreeTask::onCompleted()
{
    bool success = (state == State::SUCCESS);
    task_result_callback(success);
}

void ExportPartPlainMergeTreeTask::cancel() noexcept
{
    cancelled = true;
}

}
