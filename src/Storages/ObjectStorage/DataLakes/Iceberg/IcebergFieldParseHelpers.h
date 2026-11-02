#pragma once

#include "config.h"

#if USE_AVRO

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Core/Field.h>
#include <Core/Types.h>
#include <DataTypes/IDataType.h>

namespace DB::Iceberg
{

/// Convert a Field to Int64, accepting Int64 and UInt64 (with overflow check).
Int64 fieldToInt64(const Field & value, std::string_view context, std::string_view arg_name);

/// Convert a Field to bool, accepting Bool, UInt64, Int64, and case-insensitive "true"/"false".
bool fieldToBool(const Field & value, std::string_view context, std::string_view arg_name);

/// Parse a Prometheus-style duration string (e.g. "3d", "1d12h30m", "500ms") into milliseconds.
Int64 fieldToPeriodMs(const Field & value, std::string_view context, std::string_view arg_name);

/// Convert a Field containing an Array to vector<Int64>, validating each element.
std::vector<Int64> fieldToInt64Array(const Field & value, std::string_view context, std::string_view arg_name);

/// Deserialize a single lower/upper bound value from Iceberg's binary representation.
/// See https://iceberg.apache.org/spec/#appendix-d-single-value-serialization
std::optional<Field> deserializeFieldFromBinaryRepr(const std::string & str, DataTypePtr expected_type, bool lower_bound);

/// One interpretation of an Iceberg `timestamp` lower/upper pair.
struct DateTime64BoundRange
{
    Field lower;
    Field upper;
};

/// `DateTime64(6)` min/max used for file pruning.
/// `|ticks| < 1e16` is microseconds. `|ticks| >= 1e18` is nanoseconds.
/// A value between them can be either, so both ranges are returned.
/// `DateTime64(9)` returns the raw nanosecond range.
std::vector<DateTime64BoundRange> deserializeDateTime64MinMaxRanges(
    const std::string & lower_str,
    const std::string & upper_str,
    DataTypePtr type);

}

#endif
