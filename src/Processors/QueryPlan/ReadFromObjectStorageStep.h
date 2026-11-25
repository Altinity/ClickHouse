#pragma once

#include <Processors/QueryPlan/SourceStepWithFilter.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Storages/StorageInMemoryMetadata.h>

namespace DB
{

class LazilyReadFromObjectStorage;
struct LazyObjectStorageFileRegistry;
using LazyObjectStorageFileRegistryPtr = std::shared_ptr<LazyObjectStorageFileRegistry>;

class ReadFromObjectStorageStep : public SourceStepWithFilter
{
public:
    ReadFromObjectStorageStep(
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
        size_t num_streams_);

    static constexpr auto STEP_NAME = "ReadFromObjectStorage";

    std::string getName() const override { return STEP_NAME; }

    StorageMetadataPtr getStorageMetadata() const { return storage_snapshot->metadata; }

    void applyFilters(ActionDAGNodes added_filter_nodes) override;
    void updatePrewhereInfo(const PrewhereInfoPtr & prewhere_info_value) override;
    bool canUpdatePrewhereInfoMultipleTimes() const override { return false; }

    void initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &) override;
    QueryPlanStepPtr clone() const override;
    /// In a distributed query plan this step is executed in a distributed manner: the files are split into `bucket_count`
    /// buckets by the hash of their identifier, and the task of a bucket reads only the files of its bucket
    /// (the `bucket_id` task parameter says which).
    void setDistributedRead(size_t bucket_count);
    Strings getShardsForDistributedRead() const;

    /// The row count of the table when the storage knows it without reading the data.
    std::optional<UInt64> totalRows() const;

    bool isSerializable() const override { return true; }
    void serialize(Serialization & ctx) const override;
    static std::unique_ptr<IQueryPlanStep> deserialize(Deserialization & ctx);

    bool requestReadingInOrder() const;

    // The name of the returned type is misleading, this order has nothing in common with the corresponding SELECT query
    // and is taken from the storage metadata.
    InputOrderInfoPtr getDataOrder() const;

    /// Lazy materialization support (see optimizeLazyMaterialization2).
    bool canUseLazyMaterialization() const;

    /// Reduces the set of columns this step reads to `required_names` (plus the columns the
    /// PREWHERE / row-level filter needs, virtual columns and hive partition columns), makes the
    /// step append a `__global_row_index` column to the output, and returns a step that lazily
    /// reads the removed columns. Returns nullptr if there is nothing to defer.
    std::unique_ptr<LazilyReadFromObjectStorage> keepOnlyRequiredColumnsAndCreateLazyReadStep(const NameSet & required_names);

    LazyObjectStorageFileRegistryPtr getLazyRowIndexRegistry() const { return lazy_row_index_registry; }

private:
    StorageID storage_id;
    ObjectStoragePtr object_storage;
    StorageObjectStorageConfigurationPtr configuration;
    std::shared_ptr<IObjectIterator> iterator_wrapper;

    /// Lazy materialization: set iff keepOnlyRequiredColumnsAndCreateLazyReadStep was called.
    LazyObjectStorageFileRegistryPtr lazy_row_index_registry;

    ReadFromFormatInfo info;
    const NamesAndTypesList virtual_columns;
    const std::optional<DB::FormatSettings> format_settings;
    const bool need_only_count;
    const size_t max_block_size;
    size_t num_streams;
    const size_t max_num_streams;
    const bool distributed_processing;
    /// This is set when this step is part of a distributed query plan and it will be executed in a distributed manner.
    /// "bucket_id" task parameter will be used to determine what part of the data to read.
    size_t distributed_read_bucket_count = 0;
    /// The iterator of a distributed read is wrapped to pass only the files of the task's bucket, once.
    bool distributed_read_iterator_applied = false;
    /// A deserialized step prunes the files by the filter of its PREWHERE, like the initiator did.
    /// The sets of an `IN` in that filter are placeholders until the whole plan is deserialized
    /// (`QueryPlan::makeSets`), so the filter is applied when the pipeline is built, not on deserialization.
    bool prewhere_pruning_filter_pending = false;

    void applyPendingPrewhereFilter();
    void createIterator();
};

}
