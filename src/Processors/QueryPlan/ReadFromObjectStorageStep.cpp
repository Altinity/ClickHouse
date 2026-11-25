#include <Processors/QueryPlan/ReadFromObjectStorageStep.h>
#include <Processors/QueryPlan/LazilyReadFromObjectStorage.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Core/Settings.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/QueryPlan/Serialization.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/Operators.h>
#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeConfiguration.h>
#include <Processors/QueryPlan/QueryPlanStepRegistry.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatParserSharedResources.h>
#include <IO/ReadBufferFromString.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/PreparedSets.h>
#include <Processors/QueryPlan/ReadNothingStep.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Common/SipHash.h>
#include <Processors/QueryPlan/IParameterLookup.h>
#include <Core/Block.h>
#include <Analyzer/TableExpressionModifiers.h>
#include <Planner/PlannerActionsVisitor.h>
#include <Interpreters/ActionsDAG.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Storages/VirtualColumnUtils.h>
#include <boost/algorithm/string/predicate.hpp>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3/Client.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int SUPPORT_IS_DISABLED;
    extern const int UNKNOWN_TABLE;
}

namespace Setting
{
    extern const SettingsBool parallelize_output_from_storages;
    extern const SettingsBool s3_validate_etag_on_read;
    extern const SettingsBool azure_validate_etag_on_read;
}


ReadFromObjectStorageStep::ReadFromObjectStorageStep(
    const StorageID & storage_id_,
    ObjectStoragePtr object_storage_,
    StorageObjectStorageConfigurationPtr configuration_,
    const Names & columns_to_read,
    const NamesAndTypesList & virtual_columns_,
    const SelectQueryInfo & query_info_,
    const StorageSnapshotPtr & storage_snapshot_,
    const std::optional<DB::FormatSettings> & format_settings_,
    bool distributed_processing_,
    ReadFromFormatInfo info_,
    bool need_only_count_,
    ContextPtr context_,
    size_t max_block_size_,
    size_t num_streams_)
    : SourceStepWithFilter(std::make_shared<const Block>(info_.source_header), columns_to_read, query_info_, storage_snapshot_, context_)
    , storage_id(storage_id_)
    , object_storage(object_storage_)
    , configuration(configuration_)
    , info(std::move(info_))
    , virtual_columns(virtual_columns_)
    , format_settings(format_settings_)
    , need_only_count(need_only_count_)
    , max_block_size(max_block_size_)
    , num_streams(num_streams_)
    , max_num_streams(num_streams_)
    , distributed_processing(distributed_processing_)
{
}

QueryPlanStepPtr ReadFromObjectStorageStep::clone() const
{
    return std::make_unique<ReadFromObjectStorageStep>(*this);
}

void ReadFromObjectStorageStep::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));
    if (!filter_actions_dag)
        return;

    if (boost::iequals(configuration->getFormat(), "Parquet") || boost::iequals(configuration->getFormat(), "ORC"))
        prepareEagerKeyConditionSets(
            filter_actions_dag,
            storage_snapshot, info.source_header,
            query_info.prewhere_info, query_info.row_level_filter, getContext());

    // It is important to build the inplace sets for the filter here, before reading data from object storage.
    // If we delay building these sets until later in the pipeline, the filter can be applied after the data
    // has already been read, potentially in parallel across many streams. This can significantly reduce the
    // effectiveness of an Iceberg partition pruning, as unnecessary data may be read. Additionally, building ordered sets
    // at this stage enables the KeyCondition class to apply more efficient optimizations than for unordered sets.
    /// Idempotent — sets already built above are skipped via !future_set->get() check.
    VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(*filter_actions_dag, getContext());
}

void ReadFromObjectStorageStep::updatePrewhereInfo(const PrewhereInfoPtr & prewhere_info_value)
{
    info = updateFormatPrewhereInfo(info, query_info.row_level_filter, prewhere_info_value);
    query_info.prewhere_info = prewhere_info_value;
    output_header = std::make_shared<const Block>(info.source_header);
}

namespace
{

/// Passes only the files of one bucket: the task of a bucket of a distributed read reads the files whose identifier hashes to it.
class BucketFilteringObjectIterator final : public IObjectIterator
{
public:
    BucketFilteringObjectIterator(ObjectIterator iterator_, size_t bucket_count_, size_t bucket_)
        : iterator(std::move(iterator_)), bucket_count(bucket_count_), bucket(bucket_)
    {
    }

    ObjectInfoPtr next(size_t id) override
    {
        while (auto object_info = iterator->next(id))
        {
            const String identifier = object_info->getIdentifier(/*include_file_bucket_info=*/false);
            if (sipHash64(identifier.data(), identifier.size()) % bucket_count == bucket)
                return object_info;
        }
        return nullptr;
    }

    size_t estimatedKeysCount() override { return std::max<size_t>(iterator->estimatedKeysCount() / bucket_count, 1); }
    std::optional<UInt64> getSnapshotVersion() const override { return iterator->getSnapshotVersion(); }
    void setEmitProfileEvents(bool value) override
    {
        IObjectIterator::setEmitProfileEvents(value);
        iterator->setEmitProfileEvents(value);
    }
    void setTasksGoToOtherReplicas(bool value) override
    {
        IObjectIterator::setTasksGoToOtherReplicas(value);
        iterator->setTasksGoToOtherReplicas(value);
    }

private:
    ObjectIterator iterator;
    size_t bucket_count;
    size_t bucket;
};

}

void ReadFromObjectStorageStep::applyPendingPrewhereFilter()
{
    if (!prewhere_pruning_filter_pending)
        return;
    prewhere_pruning_filter_pending = false;

    if (!query_info.prewhere_info)
        return;

    const auto & prewhere = *query_info.prewhere_info;
    if (const auto * node = prewhere.prewhere_actions.tryFindInOutputs(prewhere.prewhere_column_name))
        applyFilters(ActionDAGNodes{{node}});
}

void ReadFromObjectStorageStep::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings & settings)
{
    applyPendingPrewhereFilter();
    createIterator();

    if (distributed_read_bucket_count > 0 && !distributed_read_iterator_applied)
    {
        if (!settings.parameter_lookup)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "A distributed read of {} has no task parameters", storage_id.getNameForLogs());
        const UInt64 bucket = settings.parameter_lookup->getParameter("bucket_id").safeGet<String>().empty()
            ? 0
            : parse<UInt64>(settings.parameter_lookup->getParameter("bucket_id").safeGet<String>());
        if (bucket >= distributed_read_bucket_count)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Bucket {} of a distributed read is out of {} buckets", bucket, distributed_read_bucket_count);
        iterator_wrapper = std::make_shared<BucketFilteringObjectIterator>(iterator_wrapper, distributed_read_bucket_count, bucket);
        distributed_read_iterator_applied = true;
    }

    Pipes pipes;
    auto context = getContext();
    size_t estimated_keys_count = iterator_wrapper->estimatedKeysCount();

    if (estimated_keys_count > 1)
        num_streams = std::min(num_streams, estimated_keys_count);
    else
    {
        /// The amount of keys (zero) was probably underestimated.
        /// We will keep one stream for this particular case.
        num_streams = 1;
    }

    // here create for node -> query -> level thread pool
    auto parser_shared_resources = std::make_shared<FormatParserSharedResources>(context->getSettingsRef(), num_streams);

    /// Iceberg reads the row lineage columns from the data file.
    auto format_filter_info = std::make_shared<FormatFilterInfo>(
        info.getFormatFilter(filter_actions_dag, /*keep_row_lineage_columns=*/ configuration->isIcebergConfiguration()),
        context,
        configuration->getColumnMapperForCurrentSchema(storage_snapshot->metadata, context),
        query_info.row_level_filter,
        query_info.prewhere_info);

    for (size_t i = 0; i < num_streams; ++i)
    {
        auto source = std::make_shared<StorageObjectStorageSource>(
            storage_id,
            getName(),
            object_storage,
            configuration,
            storage_snapshot,
            info,
            format_settings,
            context,
            max_block_size,
            iterator_wrapper,
            parser_shared_resources,
            format_filter_info,
            need_only_count,
            lazy_row_index_registry);

        pipes.emplace_back(std::move(source));
    }
    auto pipe = Pipe::unitePipes(std::move(pipes));
    if (pipe.empty())
        pipe = Pipe(std::make_shared<NullSource>(std::make_shared<const Block>(info.source_header)));

    size_t output_ports = pipe.numOutputPorts();
    const bool parallelize_output = context->getSettingsRef()[Setting::parallelize_output_from_storages];
    if (parallelize_output
        && FormatFactory::instance().checkParallelizeOutputAfterReading(configuration->getFormat(), context)
        && output_ports > 0 && output_ports < max_num_streams)
        pipe.resize(max_num_streams);

    for (const auto & processor : pipe.getProcessors())
        processors.emplace_back(processor);

    pipeline.init(std::move(pipe));
}

void ReadFromObjectStorageStep::createIterator()
{
    if (iterator_wrapper)
        return;

    const ActionsDAG::Node * predicate = nullptr;
    if (filter_actions_dag)
        predicate = filter_actions_dag->getOutputs().at(0);

    auto context = getContext();

    iterator_wrapper = StorageObjectStorageSource::createFileIterator(
        configuration, configuration->getQuerySettings(context), object_storage, storage_snapshot->metadata, distributed_processing,
        context, predicate, filter_actions_dag.get(), virtual_columns, info.hive_partition_columns_to_read_from_file_path, nullptr, context->getFileProgressCallback(),
        /*ignore_archive_globs=*/ false, /*skip_object_metadata=*/ false, /*with_tags=*/ info.requested_virtual_columns.contains("_tags"));
}

static InputOrderInfoPtr convertSortingKeyToInputOrder(const KeyDescription & key_description)
{
    SortDescription sort_description_for_merging;
    for (size_t i = 0; i < key_description.column_names.size(); ++i)
        sort_description_for_merging.push_back(
            SortColumnDescription(key_description.column_names[i], (!key_description.reverse_flags.empty() && key_description.reverse_flags[i]) ? -1 : 1));
    return std::make_shared<const InputOrderInfo>(sort_description_for_merging, sort_description_for_merging.size(), 1, 0);
}

bool ReadFromObjectStorageStep::canUseLazyMaterialization() const
{
    if (need_only_count)
        return false;

    /// The global row index requires per-row file row numbers (ChunkInfoRowNumbers), and the lazy
    /// branch requires reading an explicit set of rows (FormatFilterInfo::rows_to_read).
    /// Only the Parquet reader supports both.
    if (!boost::iequals(configuration->format, "Parquet"))
        return false;

    /// Data lakes can have per-file formats, deletes, and schema evolution; the configuration
    /// proves against the concrete data snapshot that every file can take the lazy path.
    if (!configuration->supportsLazyMaterialization(storage_snapshot->metadata, getContext()))
        return false;

    /// The lazy pass rereads the surviving files and must prove it sees the same generation of
    /// each object (see `LazyRowsObjectIterator::validateObjectGeneration`). A backend whose
    /// metadata may carry no comparable token at all (no `ETag`, unknown size and modification
    /// time — e.g. a web origin) would fail close on that reread even without any concurrent
    /// overwrite, so keep it on the single-pass plan.
    if (!object_storage->supportsObjectGenerationComparison())
        return false;

    /// Even when the two generations are comparable, on most backends the second pass opens an
    /// unconditional read: `HDFSObjectStorage` and the local disk ignore `StoredObject::etag`, so a concurrent in-place overwrite between the metadata probe and the
    /// read could still stitch together rows of two versions of the file. The reread is only
    /// generation-safe when either:
    ///   - the data files are immutable by the format's contract — a data lake never overwrites a
    ///     data file in place, so a file identified by path is always the same generation; or
    ///   - the backend pins the actual read to the captured generation — S3 with
    ///     `s3_validate_etag_on_read` issues the GET with an `If-Match` on the captured ETag and
    ///     rejects a response whose ETag drifted from it (see `ReadBufferFromS3`), which is atomic
    ///     with respect to an overwrite; Azure with `azure_validate_etag_on_read` does the same
    ///     (see `ReadBufferFromAzureBlobStorage`).
    /// The pin only takes effect when the captured metadata actually carries a non-empty `ETag`
    /// (see `createReadBuffer`), and `GCS` accessed through the S3 API is documented to legitimately
    /// return objects without one — so a `GCS`-provider client is not pinned even with the setting
    /// on. Any other provider that unexpectedly yields an empty `ETag` at read time fails close in
    /// `LazyRowsObjectIterator::validateObjectGeneration` instead of degrading to an unpinned read.
    /// For a mutable file on a backend that cannot pin the read, keep the single-pass plan.
    bool reread_is_generation_pinned = false;
#if USE_AWS_S3
    if (object_storage->getType() == ObjectStorageType::S3
        && getContext()->getSettingsRef()[Setting::s3_validate_etag_on_read])
    {
        const auto s3_client = object_storage->tryGetS3StorageClient();
        reread_is_generation_pinned = s3_client && s3_client->getProviderType() != S3::ProviderType::GCS;
    }
#endif
    if (object_storage->getType() == ObjectStorageType::Azure
        && getContext()->getSettingsRef()[Setting::azure_validate_etag_on_read])
        reread_is_generation_pinned = true;
    if (!configuration->dataFilesAreImmutable() && !reread_is_generation_pinned)
        return false;

    /// The transformed plan is not serializable.
    if (distributed_read_bucket_count)
        return false;

    return true;
}

std::unique_ptr<LazilyReadFromObjectStorage> ReadFromObjectStorageStep::keepOnlyRequiredColumnsAndCreateLazyReadStep(const NameSet & required_names)
{
    /// `StorageObjectStorage::read` propagates a bare row policy (no PREWHERE) into
    /// `info.row_level_filter`, which the split pins to the main pass; keep this guard in case a
    /// caller constructs the step without that propagation, since the source would still evaluate
    /// the filter in the main pass via `FormatFilterInfo`.
    NameSet names_to_keep = required_names;
    if (!info.row_level_filter && query_info.row_level_filter)
        for (const auto & column : query_info.row_level_filter->actions.getRequiredColumns())
            names_to_keep.insert(column.name);

    auto lazy_info = splitLazilyReadColumnsFromFormatInfo(info, names_to_keep);
    if (!lazy_info)
        return {};

    output_header = std::make_shared<const Block>(info.source_header);

    NameSet lazy_names;
    for (const auto & column : lazy_info->source_header)
        lazy_names.insert(column.name);
    std::erase_if(required_source_columns, [&](const String & name) { return lazy_names.contains(name); });

    lazy_row_index_registry = std::make_shared<LazyObjectStorageFileRegistry>();

    auto lazy_source_header = std::make_shared<const Block>(lazy_info->source_header);
    auto lazy_step = std::make_unique<LazilyReadFromObjectStorage>(
        std::move(lazy_source_header),
        storage_id,
        object_storage,
        configuration,
        storage_snapshot,
        format_settings,
        std::move(*lazy_info),
        getContext(),
        max_block_size);

    return lazy_step;
}

bool ReadFromObjectStorageStep::requestReadingInOrder() const
{
    return configuration->isDataSortedBySortingKey(storage_snapshot->metadata, getContext());
}

InputOrderInfoPtr ReadFromObjectStorageStep::getDataOrder() const
{
    return convertSortingKeyToInputOrder(storage_snapshot->metadata->getSortingKey());
}


void ReadFromObjectStorageStep::setDistributedRead(size_t bucket_count)
{
    distributed_read_bucket_count = bucket_count;
}

Strings ReadFromObjectStorageStep::getShardsForDistributedRead() const
{
    Strings shards;
    shards.reserve(distributed_read_bucket_count);
    for (size_t bucket = 0; bucket < distributed_read_bucket_count; ++bucket)
        shards.push_back(fmt::format("bucket {} of {} of the files of {}", bucket, distributed_read_bucket_count, storage_id.getNameForLogs()));
    return shards;
}

std::optional<UInt64> ReadFromObjectStorageStep::totalRows() const
{
    auto storage = DatabaseCatalog::instance().tryGetTable(storage_id, getContext());
    if (!storage)
        return std::nullopt;
    return storage->totalRows(getContext());
}

void ReadFromObjectStorageStep::serialize(Serialization & ctx) const
{
    if (lazy_row_index_registry)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "make_distributed_plan does not support a read with lazy materialization");

    /// The worker looks the table up by its name, so every node of the plan must see the same catalog.
    writeStringBinary(storage_id.getDatabaseName(), ctx.out);
    writeStringBinary(storage_id.getTableName(), ctx.out);

    writeVarUInt(required_source_columns.size(), ctx.out);
    for (const auto & column : required_source_columns)
        writeStringBinary(column, ctx.out);

    writeVarUInt(max_block_size, ctx.out);
    writeVarUInt(max_num_streams, ctx.out);

    UInt8 flags = 0;
    if (query_info.row_level_filter != nullptr)
        flags |= 1;
    if (query_info.prewhere_info != nullptr)
        flags |= 2;
    if (need_only_count)
        flags |= 4;
    writeIntBinary(flags, ctx.out);

    if (query_info.row_level_filter)
        query_info.row_level_filter->serialize(ctx);
    if (query_info.prewhere_info)
        query_info.prewhere_info->serialize(ctx);

    writeVarUInt(distributed_read_bucket_count, ctx.out);
}

std::unique_ptr<IQueryPlanStep> ReadFromObjectStorageStep::deserialize(Deserialization & ctx)
{
    String database_name;
    String table_name;
    readStringBinary(database_name, ctx.in);
    readStringBinary(table_name, ctx.in);

    size_t num_columns = 0;
    readVarUInt(num_columns, ctx.in);
    Names column_names;
    column_names.reserve(num_columns);
    for (size_t i = 0; i < num_columns; ++i)
    {
        String column_name;
        readStringBinary(column_name, ctx.in);
        column_names.push_back(std::move(column_name));
    }

    UInt64 max_block_size = 0;
    readVarUInt(max_block_size, ctx.in);
    size_t num_streams = 0;
    readVarUInt(num_streams, ctx.in);

    UInt8 flags = 0;
    readIntBinary(flags, ctx.in);

    SelectQueryInfo query_info;
    if (flags & 1)
        query_info.row_level_filter = std::make_shared<FilterDAGInfo>(FilterDAGInfo::deserialize(ctx));
    if (flags & 2)
        query_info.prewhere_info = std::make_shared<PrewhereInfo>(PrewhereInfo::deserialize(ctx));
    query_info.optimize_trivial_count = flags & 4;
    /// There is no query on a worker: without the modifiers `SelectQueryInfo::isFinal` reads the missing AST.
    query_info.table_expression_modifiers = TableExpressionModifiers(/*has_final_=*/ false, /*sample_size_ratio_=*/ {}, /*sample_offset_ratio_=*/ {});

    size_t distributed_read_bucket_count = 0;
    readVarUInt(distributed_read_bucket_count, ctx.in);

    /// The plan is only being drained off the buffer and will be discarded.
    if (ctx.skipping)
        return std::make_unique<ReadNothingStep>(ctx.output_header);

    StorageID table_id(database_name, table_name);
    auto storage_ptr = DatabaseCatalog::instance().getTable(table_id, ctx.context);
    const auto metadata_snapshot = storage_ptr->getInMemoryMetadataPtr(ctx.context, false);
    auto storage_snapshot = storage_ptr->getStorageSnapshot(metadata_snapshot, ctx.context);

    QueryPlan plan;
    storage_ptr->read(
        plan, column_names, storage_snapshot, query_info, Context::createCopy(ctx.context), QueryProcessingStage::FetchColumns, max_block_size, num_streams);
    if (!plan.isInitialized() || !plan.getRootNode()->children.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Reading from {} is expected to produce one step", table_id.getNameForLogs());

    QueryPlanStepPtr step = std::move(plan.getRootNode()->step);
    auto * read_step = typeid_cast<ReadFromObjectStorageStep *>(step.get());
    if (!read_step)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Reading from {} is expected to produce ReadFromObjectStorage", table_id.getNameForLogs());

    /// The files are pruned by the filter: the one the prewhere carries is the one the initiator pruned with.
    /// It is applied when the pipeline is built: the sets of an `IN` in it are not resolved before the plan is deserialized.
    read_step->prewhere_pruning_filter_pending = query_info.prewhere_info != nullptr;

    if (distributed_read_bucket_count)
        read_step->setDistributedRead(distributed_read_bucket_count);

    assertBlocksHaveEqualStructure(*read_step->getOutputHeader(), *ctx.output_header, "ReadFromObjectStorage deserialization");

    ctx.storage_holders.push_back(storage_ptr);
    return step;
}

void registerReadFromObjectStorageStep(QueryPlanStepRegistry & registry);
void registerReadFromObjectStorageStep(QueryPlanStepRegistry & registry)
{
    registry.registerStep(ReadFromObjectStorageStep::STEP_NAME, ReadFromObjectStorageStep::deserialize);
}

}
