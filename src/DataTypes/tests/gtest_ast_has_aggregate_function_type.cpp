#include <gtest/gtest.h>

#include <Core/Defines.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <Parsers/ParserDataType.h>
#include <Parsers/parseQuery.h>

using namespace DB;

/// `astHasAggregateFunctionType` decides whether a type name recorded next to the data denotes an
/// aggregate state, from the name alone: resolving it into a type is exactly what the gate around it
/// is there to prevent, so nothing here goes through `DataTypeFactory`.

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

TEST(AstHasAggregateFunctionType, AStateIsFoundWhereverItIsNested)
{
    EXPECT_TRUE(hasState("AggregateFunction(uniq, UInt64)"));
    EXPECT_TRUE(hasState("AggregateFunction(0, sumMap, Array(UInt8), Array(UInt32))"));
    EXPECT_TRUE(hasState("AggregateFunction(quantiles(0.5, 0.9), UInt64)"));
    EXPECT_TRUE(hasState("Array(AggregateFunction(uniq, UInt64))"));
    EXPECT_TRUE(hasState("Map(String, AggregateFunction(uniq, UInt64))"));
    EXPECT_TRUE(hasState("Tuple(a UInt64, b Array(AggregateFunction(uniq, UInt64)))"));
}

TEST(AstHasAggregateFunctionType, SimpleAggregateFunctionIsNotAState)
{
    /// It is an ordinary value of its storage type, deserialized by that type, so it is not gated.
    EXPECT_FALSE(hasState("SimpleAggregateFunction(sum, UInt64)"));
    EXPECT_FALSE(hasState("Array(SimpleAggregateFunction(max, String))"));
    EXPECT_FALSE(hasState("Map(String, SimpleAggregateFunction(any, Nullable(UInt64)))"));
}

TEST(AstHasAggregateFunctionType, OrdinaryTypesAreNotStates)
{
    EXPECT_FALSE(hasState("UInt64"));
    EXPECT_FALSE(hasState("String"));
    EXPECT_FALSE(hasState("Tuple(a UInt64, b Array(Nullable(String)))"));

    /// Only a type name counts, not a name that merely spells the same in another position.
    EXPECT_FALSE(hasState("Enum8('AggregateFunction' = 1)"));
    EXPECT_FALSE(hasState("Tuple(`AggregateFunction` String)"));
}

TEST(AstHasAggregateFunctionType, TheNameIsAnsweredWithoutResolvingIt)
{
    /// A file can name an aggregate function this build does not have - precisely a name to refuse
    /// rather than look up.
    EXPECT_TRUE(hasState("AggregateFunction(no_such_aggregate_function, UInt64)"));
    EXPECT_TRUE(hasState("Array(AggregateFunction(no_such_aggregate_function, UInt64))"));
}
