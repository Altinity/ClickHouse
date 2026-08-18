#include "config.h"
#if USE_AVRO

#include <cctype>
#include <limits>

#include <base/unaligned.h>
#include <Columns/IColumn.h>
#include <Common/Exception.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesDecimal.h>
#include <Parsers/Prometheus/parseTimeSeriesTypes.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergFieldParseHelpers.h>

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

namespace Iceberg
{

namespace
{

/// ~year 2286 as DateTime64(6) microseconds. Spec-correct Iceberg `timestamp` bounds
/// for realistic dates stay below this; nanosecond ticks for dates after ~1970-04
/// sit well above it (2024 is ~1.7e18).
constexpr Int64 microseconds_implausible_threshold = 10'000'000'000'000'000LL;
constexpr Int64 nanoseconds_per_microsecond = 1000;

bool magnitudeExceeds(Int64 value, Int64 threshold)
{
    return value > threshold || value < -threshold;
}

/// Convert ns ticks to us ticks without shrinking a [lower, upper] range.
Int64 nanosecondsToMicrosecondsForBound(Int64 nanoseconds, bool lower_bound)
{
    Int64 microseconds = nanoseconds / nanoseconds_per_microsecond;
    if (nanoseconds % nanoseconds_per_microsecond == 0)
        return microseconds;

    /// C++ integer division truncates toward zero.
    if (lower_bound)
    {
        if (nanoseconds < 0)
            --microseconds;
    }
    else if (nanoseconds > 0)
    {
        ++microseconds;
    }
    return microseconds;
}

/// Iceberg timestamps are 8-byte little-endian (spec Appendix D). `insertData`
/// memcpy's native DateTime64 ticks, which is correct when the bound scale
/// matches the column. Some writers store nanosecond stats on Iceberg
/// `timestamp` (`DateTime64(6)`); treating those as microseconds yields dates
/// around year 2299 and over-prunes. Rescale those bounds; if they are still
/// implausible, fail open (`nullopt`) so min/max pruning is skipped.
std::optional<Field> deserializeDateTime64FromBinaryRepr(const std::string & str, const IDataType & type, bool lower_bound)
{
    if (str.size() != sizeof(Int64))
        return std::nullopt;

    Int64 unscaled = unalignedLoadLittleEndian<Int64>(str.data());
    const UInt32 scale = getDecimalScale(type);

    if (scale == 6 && magnitudeExceeds(unscaled, microseconds_implausible_threshold))
    {
        unscaled = nanosecondsToMicrosecondsForBound(unscaled, lower_bound);
        if (magnitudeExceeds(unscaled, microseconds_implausible_threshold))
            return std::nullopt;
    }

    return DecimalField<DateTime64>(DateTime64(unscaled), scale);
}

}

Int64 fieldToInt64(const Field & value, std::string_view context, std::string_view arg_name)
{
    if (value.getType() == Field::Types::Int64)
        return value.safeGet<Int64>();
    if (value.getType() == Field::Types::UInt64)
    {
        UInt64 v = value.safeGet<UInt64>();
        if (v > static_cast<UInt64>(std::numeric_limits<Int64>::max()))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} '{}' is too large: {}", context, arg_name, v);
        return static_cast<Int64>(v);
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} expects '{}' to be an integer literal", context, arg_name);
}

bool fieldToBool(const Field & value, std::string_view context, std::string_view arg_name)
{
    if (value.getType() == Field::Types::Bool)
        return value.safeGet<bool>();
    if (value.getType() == Field::Types::UInt64)
        return value.safeGet<UInt64>() != 0;
    if (value.getType() == Field::Types::Int64)
        return value.safeGet<Int64>() != 0;
    if (value.getType() == Field::Types::String)
    {
        String lower = value.safeGet<String>();
        for (auto & ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        if (lower == "true")
            return true;
        if (lower == "false")
            return false;
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} expects '{}' to be a boolean or integer literal", context, arg_name);
}

Int64 fieldToPeriodMs(const Field & value, std::string_view context, std::string_view arg_name)
{
    if (value.getType() != Field::Types::String)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "{} expects '{}' to be a duration string like '3d', '12h', '30m', '15s' or '250ms'",
            context, arg_name);

    const String & input = value.safeGet<String>();
    if (input.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} '{}' cannot be empty", context, arg_name);

    Decimal64 parsed_duration_ms;
    try
    {
        /// Scale=3 means the decimal stores milliseconds in the integer payload.
        parsed_duration_ms = parseTimeSeriesDuration(input, /* duration_scale */ 3);
    }
    catch (const Exception &)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{}: invalid duration '{}' for '{}'", context, input, arg_name);
    }

    Int64 milliseconds = parsed_duration_ms.value;
    if (milliseconds < 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} expects '{}' to be non-negative", context, arg_name);

    return milliseconds;
}

std::vector<Int64> fieldToInt64Array(const Field & value, std::string_view context, std::string_view arg_name)
{
    if (value.getType() != Field::Types::Array)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} expects '{}' to be an array literal", context, arg_name);

    const auto & src = value.safeGet<Array>();
    std::vector<Int64> result;
    result.reserve(src.size());
    for (const auto & elem : src)
        result.push_back(fieldToInt64(elem, context, arg_name));
    return result;
}

std::optional<Field> deserializeFieldFromBinaryRepr(const std::string & str, DataTypePtr expected_type, bool lower_bound)
{
    auto non_nullable_type = removeNullable(expected_type);
    if (WhichDataType(non_nullable_type).isDateTime64())
        return deserializeDateTime64FromBinaryRepr(str, *non_nullable_type, lower_bound);

    auto column = non_nullable_type->createColumn();
    if (WhichDataType(non_nullable_type).isDecimal())
    {
        /// Iceberg store decimal values as unscaled value with two's-complement big-endian binary
        /// using the minimum number of bytes for the value
        /// Our decimal binary representation is little endian
        /// so we cannot reuse our default code for parsing it.
        int64_t unscaled_value = 0;

        // Convert from big-endian to signed int
        for (const auto byte : str)
            unscaled_value = (unscaled_value << 8) | static_cast<uint8_t>(byte);

        /// Add sign
        if (str[0] & 0x80)
        {
            int64_t sign_extension = -1;
            sign_extension <<= (str.size() * 8);
            unscaled_value |= sign_extension;
        }

        /// NOTE: It's very weird, but Decimal values for lower bound and upper bound
        /// are stored rounded, without fractional part. What is more strange
        /// the integer part is rounded mathematically correctly according to fractional part.
        /// Example: 17.22 -> 17, 8888.999 -> 8889, 1423.77 -> 1424.
        /// I've checked two implementations: Spark and Amazon Athena and both of them
        /// do this.
        ///
        /// The problem is -- we cannot use rounded values for lower bounds and upper bounds.
        /// Example: upper_bound(x) = 17.22, but it's rounded 17.00, now condition WHERE x >= 17.21 will
        /// check rounded value and say: "Oh largest value is 17, so values bigger than 17.21 cannot be in this file,
        /// let's skip it". But it will produce incorrect result since actual value (17.22 >= 17.21) is stored in this file.
        ///
        /// To handle this issue we subtract 1 from the integral part for lower_bound and add 1 to integral
        /// part of upper_bound. This produces: 17.22 -> [16.0, 18.0]. So this is more rough boundary,
        /// but at least it doesn't lead to incorrect results.
        if (int32_t scale = getDecimalScale(*non_nullable_type))
        {
            int64_t scaler = lower_bound ? -10 : 10;
            while (--scale)
                scaler *= 10;

            unscaled_value += scaler;
        }

        if (const auto * decimal_type = checkDecimal<Decimal32>(*non_nullable_type))
        {
            DecimalField<Decimal32> result(static_cast<Int32>(unscaled_value), decimal_type->getScale());
            return result;
        }
        if (const auto * decimal_type = checkDecimal<Decimal64>(*non_nullable_type))
        {
            DecimalField<Decimal64> result(unscaled_value, decimal_type->getScale());
            return result;
        }

        return std::nullopt;
    }

    if (non_nullable_type->getTypeId() == TypeIndex::Variant)
        return std::nullopt;

    /// For all other types except decimal binary representation
    /// matches our internal representation
    column->insertData(str.data(), str.length());
    Field result;
    column->get(0, result);
    return result;
}

}
}

#endif
