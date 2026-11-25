#include <gtest/gtest.h>

#include <Common/tests/gtest_global_register.h>
#include <DataTypes/DataTypeAggregateFunction.h>

using namespace DB;

/// With states disabled, a name with an `AggregateFunction` is not resolved, so its aggregate function is never looked up.
TEST(ResolveClickHouseTypeAnnotation, SkipsOnlyAggregateFunctionStatesWhenDisabled)
{
    tryRegisterAggregateFunctions();

    for (const auto * name : {
             "AggregateFunction(uniq, UInt64)",
             "AggregateFunction(0, sumMap, Array(UInt8), Array(UInt32))",
             "Array(AggregateFunction(no_such_aggregate_function, UInt64))",
             "Map(String, AggregateFunction(uniq, UInt64))",
             "Tuple(a UInt64, b Array(AggregateFunction(uniq, UInt64)))"})
        EXPECT_EQ(resolveClickHouseTypeAnnotation(name, /*allow_aggregate_function_states=*/ false), nullptr) << name;

    for (const auto * name : {
             "UInt64",
             "SimpleAggregateFunction(sum, UInt64)",
             "Array(SimpleAggregateFunction(max, String))",
             "Tuple(a UInt64, b Array(Nullable(String)))",
             "Enum8('AggregateFunction' = 1)",
             "Tuple(`AggregateFunction` String)"})
        EXPECT_NE(resolveClickHouseTypeAnnotation(name, /*allow_aggregate_function_states=*/ false), nullptr) << name;
}
