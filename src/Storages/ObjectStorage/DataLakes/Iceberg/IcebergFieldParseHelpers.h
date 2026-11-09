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
/// `compensate_rounding` widens a decimal bound by one integral unit; pass false to read the value
/// exactly as the manifest declares it.
std::optional<Field>
deserializeFieldFromBinaryRepr(const std::string & str, DataTypePtr expected_type, bool lower_bound, bool compensate_rounding = true);

/// Appendix D stores `timestamp`, `timestamptz`, `timestamp_ns`, and `timestamptz_ns` as an 8-byte little-endian `long`.
std::string serializeDateTime64IcebergBound(Int64 ticks);

/// One Iceberg timestamp lower/upper pair, in the unit of the column type.
struct DateTime64BoundRange
{
    Field lower;
    Field upper;
};

/// Min/max used for file pruning. `DateTime64(6)` is `timestamp` / `timestamptz` (microseconds) and
/// `DateTime64(9)` is `timestamp_ns` / `timestamptz_ns` (nanoseconds). Bounds are read in that unit.
/// Returns one range, or none when the pair is inverted or not 8 bytes.
std::vector<DateTime64BoundRange> deserializeDateTime64MinMaxRanges(
    const std::string & lower_str,
    const std::string & upper_str,
    DataTypePtr type);

}

#endif
