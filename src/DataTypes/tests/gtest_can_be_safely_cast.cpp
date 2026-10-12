#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/Utils.h>

#include <gtest/gtest.h>

using namespace DB;

namespace
{

bool canCast(const std::string & from, const std::string & to)
{
    auto & factory = DataTypeFactory::instance();
    return canBeSafelyCast(factory.get(from), factory.get(to));
}

}

TEST(CanBeSafelyCast, UnsignedToWiderSigned)
{
    EXPECT_TRUE(canCast("UInt8", "Int16"));
    EXPECT_TRUE(canCast("UInt8", "Int32"));
    EXPECT_TRUE(canCast("UInt8", "Int64"));
    EXPECT_TRUE(canCast("UInt8", "Int128"));
    EXPECT_TRUE(canCast("UInt8", "Int256"));
    EXPECT_TRUE(canCast("UInt16", "Int32"));
    EXPECT_TRUE(canCast("UInt16", "Int64"));
    EXPECT_TRUE(canCast("UInt32", "Int64"));
    EXPECT_TRUE(canCast("UInt64", "Int128"));
    EXPECT_TRUE(canCast("UInt64", "Int256"));
    EXPECT_TRUE(canCast("UInt128", "Int256"));
}

TEST(CanBeSafelyCast, UnsignedToSignedOfSameOrSmallerSizeIsLossy)
{
    EXPECT_FALSE(canCast("UInt8", "Int8"));
    EXPECT_FALSE(canCast("UInt16", "Int8"));
    EXPECT_FALSE(canCast("UInt16", "Int16"));
    EXPECT_FALSE(canCast("UInt32", "Int16"));
    EXPECT_FALSE(canCast("UInt32", "Int32"));
    EXPECT_FALSE(canCast("UInt64", "Int32"));
    EXPECT_FALSE(canCast("UInt64", "Int64"));
    EXPECT_FALSE(canCast("UInt128", "Int64"));
    EXPECT_FALSE(canCast("UInt128", "Int128"));
    EXPECT_FALSE(canCast("UInt256", "Int128"));
    EXPECT_FALSE(canCast("UInt256", "Int256"));
}

TEST(CanBeSafelyCast, SignedToUnsignedIsLossy)
{
    EXPECT_FALSE(canCast("Int8", "UInt8"));
    EXPECT_FALSE(canCast("Int8", "UInt16"));
    EXPECT_FALSE(canCast("Int8", "UInt64"));
    EXPECT_FALSE(canCast("Int32", "UInt64"));
    EXPECT_FALSE(canCast("Int128", "UInt256"));
}

TEST(CanBeSafelyCast, SameSignednessWidening)
{
    EXPECT_TRUE(canCast("UInt8", "UInt8"));
    EXPECT_TRUE(canCast("UInt8", "UInt64"));
    EXPECT_FALSE(canCast("UInt64", "UInt8"));
    EXPECT_TRUE(canCast("Int8", "Int64"));
    EXPECT_FALSE(canCast("Int64", "Int8"));
}

TEST(CanBeSafelyCast, UnsignedToSignedThroughWrappers)
{
    EXPECT_TRUE(canCast("UInt8", "Nullable(Int16)"));
    EXPECT_TRUE(canCast("UInt8", "LowCardinality(Int16)"));
    EXPECT_TRUE(canCast("Nullable(UInt8)", "Nullable(Int16)"));
    EXPECT_TRUE(canCast("Array(UInt8)", "Array(Int16)"));
    EXPECT_TRUE(canCast("Tuple(UInt8, UInt32)", "Tuple(Int16, Int64)"));
    EXPECT_TRUE(canCast("Map(UInt8, UInt16)", "Map(Int16, Int32)"));

    EXPECT_FALSE(canCast("Array(UInt8)", "Array(Int8)"));
    EXPECT_FALSE(canCast("Tuple(UInt8, UInt32)", "Tuple(Int16, Int32)"));
}
