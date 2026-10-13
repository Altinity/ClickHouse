#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <base/unaligned.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeDateTime64.h>
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

TEST(IcebergFieldParseHelpers, NearEpochNanosecondDateTime64BoundsFailOpen)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// One day after the epoch as nanoseconds: kept as µs this is ~1972, not 1970-01-02.
    constexpr Int64 one_day_ns = 86'400'000'000'000LL;
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(one_day_ns), type, true).has_value());
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(one_day_ns), type, false).has_value());
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(-one_day_ns), type, true).has_value());
}

TEST(IcebergFieldParseHelpers, ModernMicrosecondDateTime64BoundsAreKept)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// 2024-01-01 00:00:00 UTC is the first kept microsecond bound (`min_kept_microseconds`).
    constexpr Int64 us_2024_01_01 = 1'704'067'200'000'000LL;
    auto at_threshold = deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_01_01), type, true);
    ASSERT_TRUE(at_threshold.has_value());
    EXPECT_EQ(decimal64Ticks(*at_threshold), us_2024_01_01);
    auto negative_threshold = deserializeFieldFromBinaryRepr(littleEndianInt64(-us_2024_01_01), type, true);
    ASSERT_TRUE(negative_threshold.has_value());
    EXPECT_EQ(decimal64Ticks(*negative_threshold), -us_2024_01_01);
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_01_01 - 1), type, true).has_value());

    /// 2024-02-20 10:00:00 UTC as microseconds — spec-correct Iceberg `timestamp`.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    auto lower = deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_02_20), type, true);
    auto upper = deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_02_20), type, false);
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(decimal64Ticks(*lower), us_2024_02_20);
    EXPECT_EQ(decimal64Ticks(*upper), us_2024_02_20);
}

TEST(IcebergFieldParseHelpers, ProvenNanosecondDateTime64BoundsAreConverted)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    constexpr Int64 ns_2024_01_01 = 1'704'067'200'123'456'789LL;
    auto field = deserializeFieldFromBinaryRepr(littleEndianInt64(ns_2024_01_01), type, true);
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(decimal64Ticks(*field), ns_2024_01_01 / 1000);
}

TEST(IcebergFieldParseHelpers, FarFutureMicrosecondDateTime64BoundsFailOpen)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// Spark 9999-12-31 sentinel sits in (1e16, 1e18].
    constexpr Int64 spark_9999_us = 253'402'300'799'999'999LL;
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(spark_9999_us), type, false).has_value());
}

#endif
