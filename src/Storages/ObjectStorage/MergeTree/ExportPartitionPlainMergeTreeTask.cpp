#include <Storages/ObjectStorage/MergeTree/ExportPartitionPlainMergeTreeTask.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <Interpreters/PartLog.h>
#include <Common/logger_useful.h>
#include <Storages/ObjectStorage/MergeTree/StorageObjectStorageMergeTreePartImporterSink.h>
#include <Storages/StorageMergeTree.h>

namespace DB
{

ExportPartitionPlainMergeTreeTask::ExportPartitionPlainMergeTreeTask(
    StorageMergeTree & storage_,
    const std::shared_ptr<CurrentlyExportingPartsTagger> & exports_tagger_,
    const StoragePtr & destination_storage_,
    ContextPtr context_,
    std::shared_ptr<MergeTreeExportManifest> manifest_,
    IExecutableTask::TaskResultCallback & task_result_callback_,
    size_t max_retries_)
    : storage(storage_)
    , exports_tagger(exports_tagger_)
    , destination_storage(destination_storage_)
    , context(std::move(context_))
    , manifest(std::move(manifest_))
    , task_result_callback(task_result_callback_)
    , max_retries(max_retries_)
{
    UInt64 transaction_id = std::stoull(manifest->transaction_id);
    priority.value = transaction_id;
}

StorageID ExportPartitionPlainMergeTreeTask::getStorageID() const
{
    return storage.getStorageID();
}

String ExportPartitionPlainMergeTreeTask::getQueryId() const
{
    return getStorageID().getShortName() + "::export_partition::" + manifest->transaction_id;
}

bool ExportPartitionPlainMergeTreeTask::executeStep()
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
            executeExport();
            state = State::NEED_ANALYZE;
            return true;
        }
        case State::NEED_ANALYZE:
        {
            if (exportedAllIndividualParts())
            {
                state = State::NEED_COMMIT;
                return true;
            }
            else
            {
                if (max_retries > retry_count)
                {
                    LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"),
                        "Retrying export attempt {} for partition {}",
                        retry_count, manifest->partition_id);
                    state = State::NEED_EXECUTE;
                    retry_count++;
                    return true;
                }

                /// do we need to update the state here?
                return false;
            }
        }
        case State::NEED_COMMIT:
        {
            if (commitExport())
            {
                state = State::SUCCESS;
            }
            else
            {
                state = State::NEED_EXECUTE;
                retry_count++;
                LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"),
                "Retrying export attempt {} for partition {}",
                retry_count, manifest->partition_id);
            }

            return true;
        }
        case State::SUCCESS:
        {
            return false;
        }
    }

    return false;
}

void ExportPartitionPlainMergeTreeTask::prepare()
{
    stopwatch_ptr = std::make_unique<Stopwatch>();
}

void ExportPartitionPlainMergeTreeTask::executeExport()
{
    if (cancelled)
        return;

    try
    {
        // Build a vector of parts that have not been exported yet (i.e., not present in manifest->exportedPaths)
        std::vector<DataPartPtr> parts_to_export;
        const auto & items = manifest->items;
        for (const auto & part : exports_tagger->parts_to_export)
        {
            if (std::find_if(items.begin(), items.end(), [&part](const auto & item) {
                return item.part_name == part->name && item.remote_path.empty();
            }) != items.end())
                parts_to_export.push_back(part);
        }

        std::function<void(MergeTreePartImportStats)> part_log_wrapper = [this](MergeTreePartImportStats stats) {

            std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);
            auto table_id = storage.getStorageID();
    
            if (stats.status.code != 0)
            {
                LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"), "Error importing part {}: {}", stats.part->name, stats.status.message);
                return;
            }
    
            storage.export_partition_transaction_id_to_manifest[manifest->transaction_id]->updateRemotePathAndWrite(
                stats.part->name, 
                stats.file_path);

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
        };

        destination_storage->importMergeTreePartition(
            storage,
            parts_to_export,
            context,
            part_log_wrapper);
    }
    catch (...)
    {
        LOG_ERROR(getLogger("ExportMergeTreePartitionToObjectStorageTask"),
            "Export attempt failed completely: {}", getCurrentExceptionMessage(true));
        
        throw;
    }
}

bool ExportPartitionPlainMergeTreeTask::commitExport()
{
    std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);

    destination_storage->commitExportPartitionTransaction(
        manifest->transaction_id,
        manifest->partition_id,
        manifest->exportedPaths(),
        context);
    
    manifest->completed = true;
    manifest->write();

    storage.export_partition_transaction_id_to_manifest.erase(manifest->transaction_id);
    
    LOG_INFO(getLogger("ExportMergeTreePartitionToObjectStorageTask"),
        "Successfully committed export transaction {} for partition {}",
        manifest->transaction_id, manifest->partition_id);

    return true;
}

void ExportPartitionPlainMergeTreeTask::onCompleted()
{
    std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);
    task_result_callback(manifest->completed);
}

void ExportPartitionPlainMergeTreeTask::cancel() noexcept
{
    cancelled = true;
}

bool ExportPartitionPlainMergeTreeTask::exportedAllIndividualParts() const
{
    std::lock_guard lock(storage.export_partition_transaction_id_to_manifest_mutex);
    return manifest->exportedPaths().size() == manifest->items.size();
}

}
