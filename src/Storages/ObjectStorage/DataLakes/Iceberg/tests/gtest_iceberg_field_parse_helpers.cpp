#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <base/unaligned.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeNullable.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergFieldParseHelpers.h>

using namespace DB;
using namespace DB::Iceberg;

namespace
{

std::string littleEndianInt64(Int64 value)
{
    std::string bytes(sizeof(Int64), '\0');
    unalignedStoreLittleEndian<Int64>(bytes.data(), value);
    return bytes;
}

Int64 decimal64Ticks(const Field & field)
{
    return static_cast<Int64>(field.safeGet<DecimalField<DateTime64>>().getValue());
}

}

TEST(IcebergFieldParseHelpers, MicrosecondDateTime64BoundsAreKept)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// 2024-02-20 10:00:00 UTC as microseconds — spec-correct Iceberg `timestamp`.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    auto lower = deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_02_20), type, true);
    auto upper = deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_02_20), type, false);
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(decimal64Ticks(*lower), us_2024_02_20);
    EXPECT_EQ(decimal64Ticks(*upper), us_2024_02_20);

    /// 1960-01-01 00:00:00 UTC as microseconds.
    constexpr Int64 us_1960_01_01 = -315'619'200'000'000LL;
    auto negative = deserializeFieldFromBinaryRepr(littleEndianInt64(us_1960_01_01), type, true);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(decimal64Ticks(*negative), us_1960_01_01);

    /// Spark `9999-12-31` sentinel is spec-correct microseconds, below `1e18`.
    constexpr Int64 spark_9999_us = 253'402'300'799'999'999LL;
    auto sentinel = deserializeFieldFromBinaryRepr(littleEndianInt64(spark_9999_us), type, false);
    ASSERT_TRUE(sentinel.has_value());
    EXPECT_EQ(decimal64Ticks(*sentinel), spark_9999_us);
}

TEST(IcebergFieldParseHelpers, AmbiguousTimestampBoundsAreTheOpenMiddleBand)
{
    EXPECT_FALSE(isAmbiguousIcebergTimestampBound(iceberg_timestamp_microseconds_only_threshold - 1));
    EXPECT_TRUE(isAmbiguousIcebergTimestampBound(iceberg_timestamp_microseconds_only_threshold));
    EXPECT_TRUE(isAmbiguousIcebergTimestampBound(200'000'000'000'000'000LL));
    EXPECT_TRUE(isAmbiguousIcebergTimestampBound(-200'000'000'000'000'000LL));
    EXPECT_TRUE(isAmbiguousIcebergTimestampBound(iceberg_timestamp_nanoseconds_lower_threshold - 1));
    EXPECT_FALSE(isAmbiguousIcebergTimestampBound(iceberg_timestamp_nanoseconds_lower_threshold));
}

TEST(IcebergFieldParseHelpers, TimestampBoundsUseBothRangesBetween1e16And1e18)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    constexpr Int64 middle = 200'000'000'000'000'000LL;
    const auto bytes = littleEndianInt64(middle);
    auto ranges = deserializeDateTime64MinMaxRanges(bytes, bytes, type);
    ASSERT_EQ(ranges.size(), 2);
    EXPECT_EQ(decimal64Ticks(ranges[0].lower), middle);
    EXPECT_EQ(decimal64Ticks(ranges[0].upper), middle);
    EXPECT_EQ(decimal64Ticks(ranges[1].lower), middle / 1000);
    EXPECT_EQ(decimal64Ticks(ranges[1].upper), middle / 1000);

    /// Optional Iceberg `timestamp` is `Nullable(DateTime64(6))` and uses the same two ranges.
    auto nullable_ranges = deserializeDateTime64MinMaxRanges(bytes, bytes, makeNullable(type));
    ASSERT_EQ(nullable_ranges.size(), 2);
    EXPECT_EQ(decimal64Ticks(nullable_ranges[0].lower), middle);
    EXPECT_EQ(decimal64Ticks(nullable_ranges[1].lower), middle / 1000);

    /// `2024-02-20` is below `1e16`, so it has one microsecond range.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    const auto small = littleEndianInt64(us_2024_02_20);
    auto microseconds = deserializeDateTime64MinMaxRanges(small, small, type);
    ASSERT_EQ(microseconds.size(), 1);
    EXPECT_EQ(decimal64Ticks(microseconds[0].lower), us_2024_02_20);

    /// A small lower bound keeps the Spark sentinel as microseconds only.
    constexpr Int64 spark_9999_us = 253'402'300'799'999'999LL;
    auto sentinel = deserializeDateTime64MinMaxRanges(small, littleEndianInt64(spark_9999_us), type);
    ASSERT_EQ(sentinel.size(), 1);
    EXPECT_EQ(decimal64Ticks(sentinel[0].lower), us_2024_02_20);
    EXPECT_EQ(decimal64Ticks(sentinel[0].upper), spark_9999_us);
}

TEST(IcebergFieldParseHelpers, BoundsBelow1e16StayMicroseconds)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// `86400000000000` is one day in nanoseconds (`1970-01-02`) and 1000 days in
    /// microseconds (`1972-09-27`). It is below `1e16`, so Iceberg `timestamp` keeps
    /// the integer and does not add a nanosecond range. The constant-column shortcut
    /// may use this point: it is not the ambiguous middle band.
    constexpr Int64 ticks = 86'400'000'000'000LL;
    EXPECT_FALSE(isAmbiguousIcebergTimestampBound(ticks));
    EXPECT_LT(ticks, iceberg_timestamp_microseconds_only_threshold);

    const auto bytes = littleEndianInt64(ticks);
    auto lower = deserializeFieldFromBinaryRepr(bytes, type, true);
    auto upper = deserializeFieldFromBinaryRepr(bytes, type, false);
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(decimal64Ticks(*lower), ticks);
    EXPECT_EQ(decimal64Ticks(*upper), ticks);

    auto ranges = deserializeDateTime64MinMaxRanges(bytes, bytes, type);
    ASSERT_EQ(ranges.size(), 1);
    EXPECT_EQ(decimal64Ticks(ranges[0].lower), ticks);
    EXPECT_EQ(decimal64Ticks(ranges[0].upper), ticks);
}

TEST(IcebergFieldParseHelpers, BoundsThatDifferBy1000ShareOnePrecision)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// `1e15` microseconds and `1e18` nanoseconds are the same instant. The lower bound is
    /// below `1e16`, so both stay microseconds and the constant-column shortcut sees a range.
    constexpr Int64 lower_us = 1'000'000'000'000'000LL;
    constexpr Int64 upper_times_1000 = 1'000'000'000'000'000'000LL;
    auto ranges = deserializeDateTime64MinMaxRanges(littleEndianInt64(lower_us), littleEndianInt64(upper_times_1000), type);
    ASSERT_EQ(ranges.size(), 1);
    EXPECT_EQ(decimal64Ticks(ranges[0].lower), lower_us);
    EXPECT_EQ(decimal64Ticks(ranges[0].upper), upper_times_1000);
    EXPECT_NE(decimal64Ticks(ranges[0].lower), decimal64Ticks(ranges[0].upper));

    /// The same gap before the epoch. `|upper| < 1e16`, so the lower bound is not divided.
    constexpr Int64 negative_lower = -1'000'000'000'000'000'000LL;
    constexpr Int64 negative_upper = -1'000'000'000'000'000LL;
    auto negative = deserializeDateTime64MinMaxRanges(
        littleEndianInt64(negative_lower), littleEndianInt64(negative_upper), type);
    ASSERT_EQ(negative.size(), 1);
    EXPECT_EQ(decimal64Ticks(negative[0].lower), negative_lower);
    EXPECT_EQ(decimal64Ticks(negative[0].upper), negative_upper);

    /// Neither bound is below `1e16`, and one is at `1e18`, so both are nanoseconds.
    constexpr Int64 middle = iceberg_timestamp_microseconds_only_threshold;
    auto nanoseconds = deserializeDateTime64MinMaxRanges(
        littleEndianInt64(middle), littleEndianInt64(upper_times_1000), type);
    ASSERT_EQ(nanoseconds.size(), 1);
    EXPECT_EQ(decimal64Ticks(nanoseconds[0].lower), middle / 1000);
    EXPECT_EQ(decimal64Ticks(nanoseconds[0].upper), upper_times_1000 / 1000);
}

TEST(IcebergFieldParseHelpers, NanosecondBoundsAbove1e18OnTimestampAreConverted)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// #2240: `2026-08-11 19:02:06.576815622` stored as nanoseconds on `timestamp`.
    constexpr Int64 ns_2026_08_11 = 1'786'474'926'576'815'622LL;
    auto lower = deserializeFieldFromBinaryRepr(littleEndianInt64(ns_2026_08_11), type, true);
    auto upper = deserializeFieldFromBinaryRepr(littleEndianInt64(ns_2026_08_11), type, false);
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    /// Division truncates toward zero; the upper bound widens by one microsecond.
    EXPECT_EQ(decimal64Ticks(*lower), 1'786'474'926'576'815LL);
    EXPECT_EQ(decimal64Ticks(*upper), 1'786'474'926'576'816LL);

    /// `1e18` itself is nanoseconds. The pair has only the converted range.
    constexpr Int64 exactly_1e18 = 1'000'000'000'000'000'000LL;
    auto edge = deserializeFieldFromBinaryRepr(littleEndianInt64(exactly_1e18), type, true);
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(decimal64Ticks(*edge), exactly_1e18 / 1000);
    const auto edge_bytes = littleEndianInt64(exactly_1e18);
    auto edge_ranges = deserializeDateTime64MinMaxRanges(edge_bytes, edge_bytes, type);
    ASSERT_EQ(edge_ranges.size(), 1);
    EXPECT_EQ(decimal64Ticks(edge_ranges[0].lower), exactly_1e18 / 1000);
}

TEST(IcebergFieldParseHelpers, DateTime64BoundsAreStoredLittleEndian)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// 2024-02-20 10:00:00 UTC. The written bytes must be little-endian, then read back unchanged.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    const auto bytes = serializeDateTime64IcebergBound(us_2024_02_20);
    EXPECT_EQ(bytes, littleEndianInt64(us_2024_02_20));
    auto field = deserializeFieldFromBinaryRepr(bytes, type, true);
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(decimal64Ticks(*field), us_2024_02_20);

    constexpr Int64 us_1960_01_01 = -315'619'200'000'000LL;
    const auto negative_bytes = serializeDateTime64IcebergBound(us_1960_01_01);
    EXPECT_EQ(negative_bytes, littleEndianInt64(us_1960_01_01));
    auto negative = deserializeFieldFromBinaryRepr(negative_bytes, type, false);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(decimal64Ticks(*negative), us_1960_01_01);
}

TEST(IcebergFieldParseHelpers, NanosecondDateTime64BoundsAreKept)
{
    auto type = std::make_shared<DataTypeDateTime64>(9);
    constexpr Int64 ns_2024_01_01 = 1'704'067'200'123'456'789LL;
    auto field = deserializeFieldFromBinaryRepr(littleEndianInt64(ns_2024_01_01), type, true);
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(decimal64Ticks(*field), ns_2024_01_01);
}

#endif
