#include <Storages/MergeTree/exportMTPartToParquet.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Storages/MergeTree/MergeTreeSequentialSource.h>
#include <Processors/Formats/Impl/ParquetBlockOutputFormat.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Executors/PushingPipelineExecutor.h>
#include <Formats/FormatFactory.h>


namespace DB
{

void exportMTPartToParquet(const MergeTreeData & data, const MergeTreeData::DataPartPtr & data_part, ContextPtr context)
{
    auto metadata_snapshot = data.getInMemoryMetadataPtr();
    Names columns_to_read = metadata_snapshot->getColumns().getNamesOfPhysical();
    StorageSnapshotPtr storage_snapshot = data.getStorageSnapshot(metadata_snapshot, context);

    MergeTreeData::IMutationsSnapshot::Params params
    {
        .metadata_version = metadata_snapshot->getMetadataVersion(),
        .min_part_metadata_version = data_part->getMetadataVersion(),
    };

    auto mutations_snapshot = data.getMutationsSnapshot(params);

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
        data,
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
    auto header_block = pipeline.getHeader();

    auto out_file_name = data_part->name + ".parquet";

    auto out_file = std::make_shared<WriteBufferFromFile>(out_file_name);
    auto parquet_output = FormatFactory::instance().getOutputFormat("Parquet", *out_file, header_block, context);
    PullingPipelineExecutor executor(pipeline);

    Block block;
    while (executor.pull(block))
    {
        parquet_output->write(block);
    }

    parquet_output->finalize();

    out_file->finalize();
}

}
