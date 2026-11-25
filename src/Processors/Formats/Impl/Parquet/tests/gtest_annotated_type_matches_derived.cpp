#include <gtest/gtest.h>

#include <Common/tests/gtest_global_register.h>
#include <DataTypes/DataTypeFactory.h>
#include <Processors/Formats/Impl/Parquet/Write.h>

using namespace DB;
using namespace DB::Parquet;

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

}

TEST(AnnotatedTypeMatchesDerived, AcceptsWhatTheParquetWriterProduces)
{
    EXPECT_TRUE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, String)"));
    EXPECT_TRUE(matches(
        "SimpleAggregateFunction(anyLast, LowCardinality(Nullable(String)))", "Nullable(String)"));

    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Date)", "UInt16"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime)", "UInt32"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime64(0))", "DateTime64(3, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime64(7))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Time)", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Time64(3))", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Time64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Enum8('a' = 1))", "Int8"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "String"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "Int16"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, UInt256)", "FixedString(32)"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, FixedString(16))", "String"));
}

TEST(AnnotatedTypeMatchesDerived, RefusesAnAnnotationThatRetypesTheColumn)
{
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Int64"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "FixedString(16)"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Array(String)"));

    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "String"));
    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(Int64)"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, Int64)"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Array(Tuple(String, String))"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "String"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a String)"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a Int64, b String)"));

    EXPECT_FALSE(matches("SimpleAggregateFunction(sum, UInt64)", "Int64"));
    EXPECT_FALSE(matches("Array(SimpleAggregateFunction(sum, UInt64))", "Array(Int64)"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, Date)", "Int32"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, IPv4)", "Int32"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, Float64)", "Int64"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, String)", "FixedString(16)"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, Decimal(18, 4))", "Decimal(18, 2)"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(3)"));
    EXPECT_FALSE(matches("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(9)"));
}
