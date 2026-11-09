#include "config.h"

#if USE_AVRO

#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestListPruning.h>

#include <Common/FieldAccurateComparison.h>
#include <Common/logger_useful.h>
#include <DataTypes/DataTypeNullable.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergFieldParseHelpers.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>

using namespace DB;

namespace DB::Iceberg
{

namespace
{

/// Read an Iceberg single-value-serialized integer bound as the signed number it is.
/// Iceberg stores bounds as little-endian with the minimum byte count for the value's type
/// (1/2/4/8 bytes). A pre-1970 day/month/year/hour bound is a negative int32, so reading it
/// as unsigned yields a huge number and inverts the lower/upper pair.
std::optional<Int64> decodeSignedInteger(const String & bytes)
{
    if (bytes.empty() || bytes.size() > sizeof(Int64))
        return {};

    UInt64 value = (bytes.back() & 0x80) ? ~UInt64(0) : UInt64(0);
    for (size_t i = bytes.size(); i > 0; --i)
        value = (value << 8) | static_cast<UInt8>(bytes[i - 1]);

    return static_cast<Int64>(value);
}

/// Read an unsigned key column's bound as the signed number Iceberg wrote. A negative value
/// (pre-1970) or a value that overflows the type's bit width means the bound carries no usable
/// information for the unsigned key space, so the field is left unconstrained.
std::optional<Field> deserializeUnsignedBound(const String & bytes, const IDataType & type)
{
    const auto value = decodeSignedInteger(bytes);
    if (!value.has_value() || *value < 0)
        return {};

    const size_t type_bits = 8 * type.getSizeOfValueInMemory();
    if (type_bits < 8 * sizeof(UInt64) && (static_cast<UInt64>(*value) >> type_bits) != 0)
        return {};

    return Field(static_cast<UInt64>(*value));
}

/// Decode the lower/upper bounds of a single partition field summary. Returns nullopt when the
/// summary carries no usable bounds (missing, null/nan, pre-1970 for unsigned types, or inverted).
/// `DateTime64` uses `deserializeDateTime64MinMaxRanges`, which returns no range for an inverted pair.
std::optional<std::pair<Field, Field>> boundsOfPartitionFieldSummary(
    const PartitionFieldSummary & summary, const DataTypePtr & type, Int32 partition_spec_id, size_t field_index)
{
    if (summary.contains_null || summary.contains_nan || !summary.lower_bound.has_value() || summary.lower_bound->empty()
        || !summary.upper_bound.has_value() || summary.upper_bound->empty())
        return {};

    const auto non_nullable_type = removeNullable(type);

    if (WhichDataType(non_nullable_type).isDateTime64())
    {
        const auto timestamp_ranges = deserializeDateTime64MinMaxRanges(*summary.lower_bound, *summary.upper_bound, type);
        if (timestamp_ranges.size() != 1)
        {
            /// Eight-byte bounds that produce no range are inverted.
            if (timestamp_ranges.empty() && summary.lower_bound->size() == sizeof(Int64) && summary.upper_bound->size() == sizeof(Int64))
            {
                LOG_WARNING(
                    getLogger("ManifestListPruner"),
                    "Manifest list declares a lower bound above the upper bound for field {} of partition spec {}; "
                    "skipping partition pruning for this field",
                    field_index,
                    partition_spec_id);
            }
            return {};
        }

        return std::pair{timestamp_ranges[0].lower, timestamp_ranges[0].upper};
    }

    std::optional<Field> lower;
    std::optional<Field> upper;
    if (WhichDataType(non_nullable_type).isUInt())
    {
        lower = deserializeUnsignedBound(*summary.lower_bound, *non_nullable_type);
        upper = deserializeUnsignedBound(*summary.upper_bound, *non_nullable_type);
    }
    else
    {
        lower = deserializeFieldFromBinaryRepr(*summary.lower_bound, type, true);
        upper = deserializeFieldFromBinaryRepr(*summary.upper_bound, type, false);
    }

    if (!lower.has_value() || !upper.has_value())
        return {};

    if (accurateLess(*upper, *lower))
    {
        LOG_WARNING(
            getLogger("ManifestListPruner"),
            "Manifest list declares a lower bound above the upper bound for field {} of partition spec {}; "
            "skipping partition pruning for this field",
            field_index,
            partition_spec_id);
        return {};
    }

    return std::pair{std::move(*lower), std::move(*upper)};
}

}

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

        auto bounds = boundsOfPartitionFieldSummary(
            partition_summaries[spec_index], partition_key.data_types.at(i), partition_spec_id, spec_index);

        left_keys[i] = bounds.has_value() ? FieldRef(bounds->first) : FieldRef(NEGATIVE_INFINITY);
        right_keys[i] = bounds.has_value() ? FieldRef(bounds->second) : FieldRef(POSITIVE_INFINITY);
    }

    return !spec_condition.condition.mayBeTrueInRange(
        positions.size(), left_keys.data(), right_keys.data(), partition_key.data_types);
}

}

#endif
