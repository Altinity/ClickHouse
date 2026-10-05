#include <gtest/gtest.h>

#include <Common/tests/gtest_global_register.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeFactory.h>

using namespace DB;

namespace
{

DataTypePtr typeFromString(const String & name)
{
    tryRegisterAggregateFunctions();
    return DataTypeFactory::instance().get(name);
}

bool matches(const String & annotated, const String & derived)
{
    return annotatedTypeMatchesDerived(typeFromString(annotated), typeFromString(derived));
}

bool matchesStrictly(const String & annotated, const String & derived)
{
    return annotatedTypeMatchesDerived(typeFromString(annotated), typeFromString(derived), /*strict=*/ true);
}

}

TEST(AnnotatedTypeMatchesDerived, AggregateFunctionNeedsAStringPosition)
{
    EXPECT_TRUE(matches("AggregateFunction(uniq, UInt64)", "String"));
    EXPECT_TRUE(matches("AggregateFunction(uniq, UInt64)", "Nullable(String)"));

    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Int64"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "FixedString(16)"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Array(String)"));

    EXPECT_TRUE(matchesStrictly("AggregateFunction(uniq, UInt64)", "String"));
    EXPECT_FALSE(matchesStrictly("AggregateFunction(uniq, UInt64)", "Int64"));
}

TEST(AnnotatedTypeMatchesDerived, ContainersAreMatchedElementwise)
{
    EXPECT_TRUE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(String)"));
    EXPECT_TRUE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(Nullable(String))"));
    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(Int64)"));

    EXPECT_TRUE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, String)"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, Int64)"));

    EXPECT_TRUE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a String, b Int64)"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a Int64, b String)"));
    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "String"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Array(Tuple(String, String))"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "String"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a String)"));

    EXPECT_TRUE(matches("SimpleAggregateFunction(sum, UInt64)", "Int64"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Nullable(String))", "Nullable(String)"));
    EXPECT_TRUE(matches("Array(SimpleAggregateFunction(sum, UInt64))", "Array(Int64)"));
}

TEST(AnnotatedTypeMatchesDerived, StrictModeRefusesAnAnnotationThatRetypesTheColumn)
{
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(9))", "Int64"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "Int64"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime64(9))", "Int64"));

    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "Int32"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv4)", "Int32"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Float64)", "Int64"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, String)", "FixedString(16)"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Decimal(18, 4))", "Decimal(18, 2)"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(3)"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(9)"));

    EXPECT_FALSE(matchesStrictly(
        "Tuple(a AggregateFunction(uniq, UInt64), b DateTime64(9))", "Tuple(a String, b Int64)"));
    EXPECT_FALSE(matchesStrictly("Array(SimpleAggregateFunction(sum, UInt64))", "Array(Int64)"));
    EXPECT_FALSE(matchesStrictly(
        "Map(String, SimpleAggregateFunction(anyLast, DateTime64(9)))", "Map(String, Int64)"));
}

TEST(AnnotatedTypeMatchesDerived, StrictModeAcceptsWhatTheParquetWriterProduces)
{
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "UInt64"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, String)", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, UUID)", "UUID"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Decimal(18, 4))", "Decimal(18, 4)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, FixedString(16))", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date32)", "Date32"));

    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "Nullable(UInt64)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Nullable(String))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Nullable(String))", "Nullable(String)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Array(UInt64))", "Array(Nullable(UInt64))"));
    EXPECT_TRUE(matchesStrictly(
        "SimpleAggregateFunction(anyLast, Map(String, UInt64))", "Map(String, Nullable(UInt64))"));
    EXPECT_TRUE(matchesStrictly(
        "Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a Nullable(String), b Nullable(UInt64))"));

    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, LowCardinality(String))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, LowCardinality(String))", "Nullable(String)"));
    EXPECT_TRUE(matchesStrictly(
        "SimpleAggregateFunction(anyLast, LowCardinality(Nullable(String)))", "Nullable(String)"));

    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "Date32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "UInt16"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime)", "DateTime64(3, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime)", "UInt32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(0))", "DateTime64(3, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(7))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time)", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time64(3))", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum8('a' = 1))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum8('a' = 1))", "Int8"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "Int16"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv4)", "UInt32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv6)", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Int128)", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, UInt256)", "FixedString(32)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, FixedString(16))", "String"));
}
