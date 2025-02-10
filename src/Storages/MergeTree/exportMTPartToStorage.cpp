#include <Processors/Executors/CompletedPipelineExecutor.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Formats/Impl/ParquetBlockOutputFormat.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/Sinks/EmptySink.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/MergeTree/MergeTreeSequentialSource.h>
#include <Storages/MergeTree/exportMTPartToStorage.h>


namespace DB
{

void exportMTPartToStorage(const MergeTreeData & source_data, const MergeTreeData::DataPartPtr & data_part, SinkToStoragePtr dst_storage_sink, ContextPtr context)
{
    auto metadata_snapshot = source_data.getInMemoryMetadataPtr();
    Names columns_to_read = metadata_snapshot->getColumns().getNamesOfPhysical();
    StorageSnapshotPtr storage_snapshot = source_data.getStorageSnapshot(metadata_snapshot, context);

    MergeTreeData::IMutationsSnapshot::Params params
    {
        .metadata_version = metadata_snapshot->getMetadataVersion(),
        .min_part_metadata_version = data_part->getMetadataVersion(),
    };

    auto mutations_snapshot = source_data.getMutationsSnapshot(params);

    auto alter_conversions = MergeTreeData::getAlterConversionsForPart(
        data_part,
        mutations_snapshot,
        metadata_snapshot,
        context);

    QueryPlan plan;

    // todoa arthur
    MergeTreeSequentialSourceType read_type = MergeTreeSequentialSourceType::Merge;

    bool apply_deleted_mask = true;
    bool read_with_direct_io = false;
    bool prefetch = false;

    createReadFromPartStep(
        read_type,
        plan,
        source_data,
        storage_snapshot,
        data_part,
        alter_conversions,
        columns_to_read,
        nullptr,
        apply_deleted_mask,
        std::nullopt,
        read_with_direct_io,
        prefetch,
        context,
        getLogger("abcde"));

    auto pipeline_settings = BuildQueryPipelineSettings::fromContext(context);
    auto optimization_settings = QueryPlanOptimizationSettings::fromContext(context);
    auto builder = plan.buildQueryPipeline(optimization_settings, pipeline_settings);

    QueryPipeline pipeline = QueryPipelineBuilder::getPipeline(std::move(*builder));

    pipeline.complete(std::move(dst_storage_sink));

    CompletedPipelineExecutor executor(pipeline);
    executor.execute();
}

}
