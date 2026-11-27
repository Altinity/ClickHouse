#include <gtest/gtest.h>

#include <Formats/ClickHouseTypeAnnotationUtils.h>

using namespace DB;

/// Aggregate functions are not registered: the check must not look up the aggregate function named by the type.
TEST(TypeNameHasAggregateFunction, DetectsOnlyAggregateFunctionStates)
{
    for (const auto * name : {
             "AggregateFunction(uniq, UInt64)",
             "AggregateFunction(0, sumMap, Array(UInt8), Array(UInt32))",
             "Array(AggregateFunction(no_such_aggregate_function, UInt64))",
             "Map(String, AggregateFunction(uniq, UInt64))",
             "Tuple(a UInt64, b Array(AggregateFunction(uniq, UInt64)))"})
        EXPECT_TRUE(typeNameHasAggregateFunction(name)) << name;

    for (const auto * name : {
             "UInt64",
             "SimpleAggregateFunction(sum, UInt64)",
             "Array(SimpleAggregateFunction(max, String))",
             "Tuple(a UInt64, b Array(Nullable(String)))",
             "Enum8('AggregateFunction' = 1)",
             "Tuple(`AggregateFunction` String)"})
        EXPECT_FALSE(typeNameHasAggregateFunction(name)) << name;
}
