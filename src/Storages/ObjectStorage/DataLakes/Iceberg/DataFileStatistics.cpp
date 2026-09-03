#include <Storages/ObjectStorage/DataLakes/Iceberg/DataFileStatistics.h>

#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Columns/ColumnAggregateFunction.h>
#include <Columns/ColumnNullable.h>
#include <Columns/IColumn.h>

namespace DB
{

#if USE_AVRO

DataFileStatistics::DataFileStatistics(Poco::JSON::Array::Ptr schema_)
{
    field_ids.resize(schema_->size());
    for (UInt32 i = 0; i < schema_->size(); ++i)
    {
        auto field = schema_->getObject(i);
        size_t field_id = field->getValue<size_t>(Iceberg::f_id);
        field_ids[i] =  field_id;
    }
}

static Range getExtremeRangeFromColumn(const ColumnPtr & column)
{
    Field min_val;
    Field max_val;
    column->getExtremes(min_val, max_val, 0, column->size());
    return Range(min_val, true, max_val, true);
}

/// `ColumnAggregateFunction::getExtremes` reports a serialized state, and comparing two such
/// `Field`s throws. Iceberg records no bounds for aggregate states anyway.
static bool supportsExtremeRange(const IColumn & column)
{
    if (checkAndGetColumn<ColumnAggregateFunction>(&column))
        return false;

    bool result = true;
    column.forEachSubcolumnRecursively([&](const IColumn & subcolumn)
    {
        result &= checkAndGetColumn<ColumnAggregateFunction>(&subcolumn) == nullptr;
    });
    return result;
}

void DataFileStatistics::update(const Chunk & chunk)
{
    if (!chunk.hasRows())
        return;
    size_t num_columns = chunk.getNumColumns();
    if (column_sizes.empty())
    {
        column_sizes.resize(num_columns, 0);
        null_counts.resize(num_columns, 0);
        track_ranges.resize(num_columns);
        for (size_t i = 0; i < num_columns; ++i)
        {
            const auto & col = chunk.getColumns()[i];
            track_ranges[i] = supportsExtremeRange(*col);
            ranges.push_back(track_ranges[i] ? getExtremeRangeFromColumn(col) : Range::createWholeUniverse());
        }
    }

    chassert(ranges.size() == num_columns);

    for (size_t i = 0; i < num_columns; ++i)
    {
        const auto & col = chunk.getColumns()[i];
        column_sizes[i] += col->byteSize();
        if (const auto * nullable_col = checkAndGetColumn<ColumnNullable>(col.get()))
        {
            for (UInt8 v : nullable_col->getNullMapData())
                null_counts[i] += v;
        }
        if (track_ranges[i])
            ranges[i] = uniteRanges(ranges[i], getExtremeRangeFromColumn(col));
    }
}

void DataFileStatistics::merge(const DataFileStatistics & other)
{
    if (other.column_sizes.empty())
        return;

    if (column_sizes.empty())
    {
        column_sizes = other.column_sizes;
        null_counts = other.null_counts;
        ranges = other.ranges;
        track_ranges = other.track_ranges;
        return;
    }

    chassert(column_sizes.size() == other.column_sizes.size());
    for (size_t i = 0; i < column_sizes.size(); ++i)
    {
        column_sizes[i] += other.column_sizes[i];
        null_counts[i] += other.null_counts[i];
        if (track_ranges[i])
            ranges[i] = uniteRanges(ranges[i], other.ranges[i]);
    }
}

Range DataFileStatistics::uniteRanges(const Range & left, const Range & right)
{
    return Range(
        Range::less(left.left, right.left) ? left.left : right.left,
        true,
        Range::less(right.right, left.right) ? left.right : right.right,
        true);
}

std::vector<std::pair<size_t, size_t>> DataFileStatistics::getColumnSizes() const
{
    std::vector<std::pair<size_t, size_t>> result;
    for (size_t i = 0; i < column_sizes.size(); ++i)
    {
        result.push_back({field_ids[i], column_sizes[i]});
    }
    return result;
}

std::vector<std::pair<size_t, size_t>> DataFileStatistics::getNullCounts() const
{
    std::vector<std::pair<size_t, size_t>> result;
    for (size_t i = 0; i < null_counts.size(); ++i)
    {
        result.push_back({field_ids[i], null_counts[i]});
    }
    return result;
}


std::vector<std::pair<size_t, Field>> DataFileStatistics::getLowerBounds() const
{
    std::vector<std::pair<size_t, Field>> result;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        /// Untracked columns (aggregate states) carry a whole-universe range whose infinite bounds
        /// cannot be dumped. Emitting them would fail the all-or-nothing canWriteStatistics() check
        /// and drop bounds for every column in the file; omit them so the rest still prune.
        if (!track_ranges[i])
            continue;
        result.push_back({field_ids[i], ranges[i].left});
    }
    return result;
}

std::vector<std::pair<size_t, Field>> DataFileStatistics::getUpperBounds() const
{
    std::vector<std::pair<size_t, Field>> result;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        if (!track_ranges[i])
            continue;
        result.push_back({field_ids[i], ranges[i].right});
    }
    return result;
}

void IcebergStatisticsTransform::transform(Chunk & chunk)
{
    stats->update(chunk);
    cur_chunk = chunk.clone();
}


#endif

}
