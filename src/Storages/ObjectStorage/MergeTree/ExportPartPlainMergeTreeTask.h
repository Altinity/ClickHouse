#pragma once

#include <Storages/MergeTree/IExecutableTask.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreeExportManifest.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Interpreters/Context.h>
#include <Common/Stopwatch.h>
#include <Storages/MergeTree/MergeMutateSelectedEntry.h>

namespace DB
{

class MergeTreeData;

class ExportPartPlainMergeTreeTask : public IExecutableTask
{
public:
    ExportPartPlainMergeTreeTask(
        StorageMergeTree & storage_,
        const DataPartPtr & part_to_export_,
        const StoragePtr & destination_storage_,
        ContextPtr context_,
        std::shared_ptr<MergeTreeExportManifest> manifest_,
        IExecutableTask::TaskResultCallback & task_result_callback_,
        size_t max_retries_);

    void onCompleted() override;
    bool executeStep() override;
    void cancel() noexcept override;
    StorageID getStorageID() const override;
    Priority getPriority() const override { return priority; }
    String getQueryId() const override;

private:
    void prepare();
    bool executeExport();
    bool commitExport();
    bool exportedAllIndividualParts() const;

    enum class State : uint8_t
    {
        NEED_PREPARE,
        NEED_EXECUTE,
        NEED_COMMIT,
        FAILED,
        SUCCESS
    };

    State state{State::NEED_PREPARE};

    StorageMergeTree & storage;
    DataPartPtr part_to_export;
    StoragePtr destination_storage;
    ContextPtr context;
    std::shared_ptr<MergeTreeExportManifest> manifest;
    IExecutableTask::TaskResultCallback task_result_callback;

    size_t max_retries;
    size_t retry_count = 0;
    Priority priority;
    std::unique_ptr<Stopwatch> stopwatch_ptr;

    bool cancelled = false;
    std::exception_ptr current_exception;
};

using ExportPartPlainMergeTreeTaskPtr = std::shared_ptr<ExportPartPlainMergeTreeTask>;

}
