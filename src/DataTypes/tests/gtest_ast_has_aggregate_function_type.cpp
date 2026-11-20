#include <gtest/gtest.h>

#include <Core/Defines.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <Parsers/ParserDataType.h>
#include <Parsers/parseQuery.h>

using namespace DB;

namespace
{

bool hasState(const String & type_name)
{
    ParserDataType parser;
    ASTPtr ast = parseQuery(
        parser, type_name.data(), type_name.data() + type_name.size(), "data type",
        /*max_query_size=*/ 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);
    return astHasAggregateFunctionType(ast);
}

}

TEST(AstHasAggregateFunctionType, FindsOnlyAggregateFunctionTypeNames)
{
    for (const auto * name : {
             "AggregateFunction(uniq, UInt64)",
             "AggregateFunction(0, sumMap, Array(UInt8), Array(UInt32))",
             "Array(AggregateFunction(no_such_aggregate_function, UInt64))",
             "Map(String, AggregateFunction(uniq, UInt64))",
             "Tuple(a UInt64, b Array(AggregateFunction(uniq, UInt64)))"})
        EXPECT_TRUE(hasState(name)) << name;

    for (const auto * name : {
             "UInt64",
             "SimpleAggregateFunction(sum, UInt64)",
             "Array(SimpleAggregateFunction(max, String))",
             "Tuple(a UInt64, b Array(Nullable(String)))",
             "Enum8('AggregateFunction' = 1)",
             "Tuple(`AggregateFunction` String)"})
        EXPECT_FALSE(hasState(name)) << name;
}
