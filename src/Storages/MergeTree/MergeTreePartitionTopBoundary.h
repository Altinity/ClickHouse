#pragma once

#include <base/types.h>
#include <Common/IntervalKind.h>
#include <ctime>

namespace DB
{

struct KeyDescription;

/// Helpers for deriving a partition's "top boundary" (the supremum of the underlying date/time
/// values that fall into the partition) for the TTL EXPORT feature.
///
/// Supported PARTITION BY expressions (the curated whitelist):
///   - Identity Date / Date32 / DateTime / DateTime64 column.
///   - toYear / toYYYYMM / toYYYYMMDD / toMonday.
///   - toStartOfYear / toStartOfQuarter / toStartOfMonth / toStartOfWeek
///     / toStartOfDay / toStartOfHour / toStartOfMinute.
///
/// The boundary is returned as the last second of the partition's range, expressed in unix
/// seconds (server timezone). Adding the TTL interval to this boundary gives the earliest
/// time at which the partition becomes eligible for export.
namespace MergeTreePartitionTopBoundary
{
    /// Returns true iff the partition key is in the curated whitelist.
    bool isPartitionExpressionSupported(const KeyDescription & partition_key);

    /// Throws BAD_TTL_EXPRESSION if not supported, with an explanatory message.
    void checkPartitionExpressionSupported(const KeyDescription & partition_key);

    /// Compute the top boundary (inclusive) of the partition identified by `partition_id`.
    /// Returns the value as unix seconds. The partition expression must be supported.
    /// Throws on parse failure or unsupported partition expression.
    time_t computeTopBoundary(const KeyDescription & partition_key, const String & partition_id);

    /// Add the TTL interval to the top boundary. Honors variable-length kinds (Month/Quarter/Year)
    /// exactly via DateLUT arithmetic.
    time_t addInterval(time_t top_boundary, IntervalKind kind, Int64 count);

    /// Compare two partition_ids numerically (as if by their underlying value).
    /// Returns negative / zero / positive. The partition expression must be supported.
    int comparePartitionIds(const KeyDescription & partition_key, const String & a, const String & b);
}

}
