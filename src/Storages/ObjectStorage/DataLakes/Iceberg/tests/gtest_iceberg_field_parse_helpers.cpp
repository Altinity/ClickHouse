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

TEST(IcebergFieldParseHelpers, AmbiguousScaleDateTime64BoundsFailOpen)
{
    auto type = std::make_shared<DataTypeDateTime64>(6);
    /// 2024-01-01 UTC µs is also ~19.7 days as ns. No integer test splits those scales.
    constexpr Int64 us_2024_01_01 = 1'704'067'200'000'000LL;
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(us_2024_01_01), type, true).has_value());
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(-us_2024_01_01), type, true).has_value());

    /// 50 days after the epoch as ns (`4.32e15`) sits in the same band as 2024–2286 µs.
    constexpr Int64 fifty_days_ns = 4'320'000'000'000'000LL;
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(fifty_days_ns), type, true).has_value());
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(fifty_days_ns), type, false).has_value());

    /// Band upper edge: 1e16 ns is ~1970-04-26, 1e16 µs is ~year 2286.
    constexpr Int64 one_e16 = 10'000'000'000'000'000LL;
    EXPECT_FALSE(deserializeFieldFromBinaryRepr(littleEndianInt64(one_e16), type, true).has_value());
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
