#include "config.h"
#if USE_AVRO

#include <cctype>
#include <limits>

#include <base/arithmeticOverflow.h>
#include <base/unaligned.h>
#include <Columns/IColumn.h>
#include <Core/DecimalFunctions.h>
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

/// 1e16 µs is ~year 2286. That is not an Iceberg `timestamp` max: Spark sentinels
/// such as `9999-12-31` (`~2.53e17` µs) and ClickHouse `DateTime64` max
/// `2299-12-31` (`~1.04e16` µs) are spec-correct microseconds above this.
/// 1e18 ns is ~year 2001; customer 2026 ns bounds are `~1.76e18`.
constexpr Int64 microseconds_ambiguous_threshold = 10'000'000'000'000'000LL;
constexpr Int64 nanoseconds_lower_threshold = 1'000'000'000'000'000'000LL;
constexpr Int64 nanoseconds_per_microsecond = 1000;
/// 2024-01-01 00:00:00 UTC as microseconds. Below this, `|ticks|` is also a
/// plausible near-epoch nanosecond count (1.704e15 ns is ~19.7 days). Fail-open
/// that overlap so a 1-day-after-epoch ns min/max is not read as year ~1972.
/// Ticks from this instant through 1e16 stay microseconds so 2024+ Iceberg
/// `timestamp` pruning is unchanged. Residual: 19.7..115 days as ns still collide
/// with 2024..2286 as µs.
constexpr Int64 min_kept_microseconds = 1'704'067'200'000'000LL;

bool magnitudeExceeds(Int64 value, Int64 threshold)
{
    return value > threshold || value < -threshold;
}

bool magnitudeBelow(Int64 value, Int64 threshold)
{
    return value < threshold && value > -threshold;
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

/// Iceberg timestamps are 8-byte little-endian (spec Appendix D).
/// Some writers store nanosecond stats on Iceberg `timestamp` (`DateTime64(6)`).
/// Convert only in a true nanosecond band (`|ticks| > 1e18`). Between 1e16 and
/// 1e18 the value may be far-future microseconds (do not convert; fail open so
/// min/max pruning is skipped). Below `min_kept_microseconds`, ticks may be
/// near-epoch nanoseconds (also fail open). `[min_kept_microseconds, 1e16]` is
/// kept as microseconds.
std::optional<Field> deserializeDateTime64FromBinaryRepr(const std::string & str, const IDataType & type, bool lower_bound)
{
    if (str.size() != sizeof(Int64))
        return std::nullopt;

    Int64 unscaled = unalignedLoadLittleEndian<Int64>(str.data());
    const UInt32 scale = getDecimalScale(type);

    if (scale == 6)
    {
        if (magnitudeBelow(unscaled, min_kept_microseconds))
            return std::nullopt;

        if (magnitudeExceeds(unscaled, microseconds_ambiguous_threshold))
        {
            if (!magnitudeExceeds(unscaled, nanoseconds_lower_threshold))
                return std::nullopt;

            unscaled = nanosecondsToMicrosecondsForBound(unscaled, lower_bound);
        }
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

namespace
{
    /// Iceberg store decimal values as unscaled value with two's-complement big-endian binary
    /// using the minimum number of bytes for the value
    /// Our decimal binary representation is little endian
    /// so we cannot reuse our default code for parsing it.
    ///
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
    /// `compensate_rounding` widens the bound as described above; pass false to read the value exactly
    /// as the manifest declares it.
    template <typename DecimalType>
    std::optional<Field> deserializeDecimalBound(const std::string & str, UInt32 scale, bool lower_bound, bool compensate_rounding = true)
    {
        using NativeType = typename DecimalType::NativeType;
        using UnsignedType = make_unsigned_t<NativeType>;

        if (str.size() > sizeof(NativeType))
            return std::nullopt;

        /// Accumulate into the unsigned counterpart, pre-filled with the sign bits,
        /// so that the sign extension comes out of the shifts themselves.
        UnsignedType unscaled = (str[0] & 0x80) ? ~UnsignedType(0) : UnsignedType(0);
        for (const auto byte : str)
            unscaled = (unscaled << 8) | static_cast<UInt8>(byte);

        NativeType unscaled_value = static_cast<NativeType>(unscaled);

        if (compensate_rounding && scale)
        {
            /// Every value in the file fits the precision of the type it is read as, so the largest
            /// magnitude that precision allows is on the outer side of all of them: it stands in for a
            /// widened bound that leaves the type, and it is the tightest bound that does. Dropping the
            /// bound instead would cost min/max pruning for the whole column.
            const NativeType limit
                = DecimalUtils::scaleMultiplier<NativeType>(DecimalUtils::max_precision<DecimalType>) - NativeType(1);

            NativeType scaler = lower_bound ? -10 : 10;
            for (UInt32 i = 1; i < scale; ++i)
                scaler *= 10;

            /// Widening can leave the type: a `Decimal(38, 38)` bound of magnitude above roughly 0.7
            /// needs almost `2 * 10^38` while `Int128` holds `1.7 * 10^38`.
            NativeType widened_value;
            if (common::addOverflow(unscaled_value, scaler, widened_value))
                widened_value = lower_bound ? -limit : limit;

            /// A bound is stored as raw bytes that are never checked against a precision, so the
            /// rounding the Iceberg writers apply can put it one unit outside the type: a file whose
            /// extreme value is `0.99` carries `1.0` (`10^38` unscaled) at scale 38. Saturating such a
            /// bound keeps it outside every value in the file, where rejecting it loses the column's
            /// pruning. Saturation is monotonic, so it cannot invert a pair that was not inverted.
            ///
            /// Only the outer side saturates: a lower bound above the largest value the type holds (or an
            /// upper bound below the smallest) is on the inner side of every value in the file, and
            /// clamping it would collapse the range to a single point that excludes the data. Such a
            /// bound is not a usable range border.
            if (lower_bound)
            {
                if (widened_value > limit)
                    return std::nullopt;
                if (widened_value < -limit)
                    widened_value = -limit;
            }
            else
            {
                if (widened_value < -limit)
                    return std::nullopt;
                if (widened_value > limit)
                    widened_value = limit;
            }

            unscaled_value = widened_value;
        }

        return DecimalField<DecimalType>(unscaled_value, scale);
    }

}

std::optional<Field>
deserializeFieldFromBinaryRepr(const std::string & str, DataTypePtr expected_type, bool lower_bound, bool compensate_rounding)
{
    auto non_nullable_type = removeNullable(expected_type);
    if (WhichDataType(non_nullable_type).isDateTime64())
        return deserializeDateTime64FromBinaryRepr(str, *non_nullable_type, lower_bound);

    auto column = non_nullable_type->createColumn();
    if (WhichDataType(non_nullable_type).isDecimal())
    {
        if (str.empty())
            return std::nullopt;

        const UInt32 scale = getDecimalScale(*non_nullable_type);
        if (checkDecimal<Decimal32>(*non_nullable_type))
            return deserializeDecimalBound<Decimal32>(str, scale, lower_bound, compensate_rounding);
        if (checkDecimal<Decimal64>(*non_nullable_type))
            return deserializeDecimalBound<Decimal64>(str, scale, lower_bound, compensate_rounding);
        if (checkDecimal<Decimal128>(*non_nullable_type))
            return deserializeDecimalBound<Decimal128>(str, scale, lower_bound, compensate_rounding);
        if (checkDecimal<Decimal256>(*non_nullable_type))
            return deserializeDecimalBound<Decimal256>(str, scale, lower_bound, compensate_rounding);
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
