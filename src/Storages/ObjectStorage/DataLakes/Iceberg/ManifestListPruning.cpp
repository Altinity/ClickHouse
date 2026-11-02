#include "config.h"

#if USE_AVRO

#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestListPruning.h>

#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergFieldParseHelpers.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>

using namespace DB;

namespace DB::Iceberg
{

ManifestListPruner::ManifestListPruner(
    const IcebergSchemaProcessor & schema_processor_,
    Int32 current_schema_id_,
    Int32 partition_schema_id_,
    const Poco::JSON::Array::Ptr & partition_specs,
    const DB::ActionsDAG * filter_dag,
    DB::ContextPtr context)
{
    if (filter_dag == nullptr || !partition_specs || partition_specs->size() == 0)
        return;

    std::vector<Int32> used_columns_in_filter;
    std::unordered_map<Int32, DB::NameAndTypePair> row_lineage_columns_in_filter;
    auto transformed_dag = renameFilterDagColumnsToFieldIds(
        schema_processor_, current_schema_id_, partition_schema_id_, filter_dag, used_columns_in_filter, row_lineage_columns_in_filter);

    for (UInt32 i = 0; i < partition_specs->size(); ++i)
    {
        auto spec = partition_specs->getObject(i);
        if (!spec->has(f_spec_id) || !spec->has(f_fields))
            continue;

        auto fields = spec->get(f_fields).extract<Poco::JSON::Array::Ptr>();
        if (!fields || fields->size() == 0)
            continue;

        auto partition_key = buildPartitionKeyFromSpec(fields, partition_schema_id_, schema_processor_, context);
        /// Some spec fields may be dropped from the partition key (multi-arg V3 transforms, unknown
        /// transforms, or columns missing from the schema). `partition_key_spec_positions` records which
        /// spec fields survived, so the summaries are projected down to the key in `canBePruned`.
        if (!partition_key.key_description.has_value()
            || partition_key.key_description->data_types.size() != partition_key.partition_key_spec_positions.size())
            continue;

        ActionsDAGWithInversionPushDown inverted_dag(transformed_dag->getOutputs().front(), context, /* boolean_context */ true);
        DB::KeyCondition condition(
            inverted_dag, context, partition_key.key_description->column_names, partition_key.key_description->expression);
        conditions_by_spec_id.emplace(
            spec->getValue<Int32>(f_spec_id),
            SpecCondition{*partition_key.key_description, std::move(condition), std::move(partition_key.partition_key_spec_positions)});
    }
}

bool ManifestListPruner::canBePruned(Int32 partition_spec_id, const PartitionFieldSummaries & partition_summaries) const
{
    if (partition_summaries.empty())
        return false;

    auto condition_it = conditions_by_spec_id.find(partition_spec_id);
    if (condition_it == conditions_by_spec_id.end())
        return false;

    const auto & spec_condition = condition_it->second;
    const auto & positions = spec_condition.spec_positions;
    const auto & partition_key = spec_condition.partition_key;

    /// Project the full per-spec summaries down to the fields that survived into the partition key.
    /// Spec fields dropped from the key (multi-arg V3 transforms, unknown transforms) are unconstrained
    /// in the condition, so they are simply not projected.
    if (positions.size() != partition_key.data_types.size())
        return false;

    std::vector<FieldRef> left_keys(positions.size());
    std::vector<FieldRef> right_keys(positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const size_t spec_index = positions[i];
        if (spec_index >= partition_summaries.size())
            return false;
        const auto & summary = partition_summaries[spec_index];
        const auto & type = partition_key.data_types.at(i);

        std::optional<Field> lower;
        std::optional<Field> upper;
        if (!summary.contains_nan && summary.lower_bound.has_value() && !summary.lower_bound->empty()
            && summary.upper_bound.has_value() && !summary.upper_bound->empty())
        {
            lower = deserializeFieldFromBinaryRepr(*summary.lower_bound, type, true);
            upper = deserializeFieldFromBinaryRepr(*summary.upper_bound, type, false);
        }

        left_keys[i] = (lower.has_value() && !summary.contains_null) ? FieldRef(*lower) : FieldRef(NEGATIVE_INFINITY);
        right_keys[i] = (upper.has_value() && !summary.contains_null) ? FieldRef(*upper) : FieldRef(POSITIVE_INFINITY);
    }

    return !spec_condition.condition.mayBeTrueInRange(
        positions.size(), left_keys.data(), right_keys.data(), partition_key.data_types);
}

}

#endif
