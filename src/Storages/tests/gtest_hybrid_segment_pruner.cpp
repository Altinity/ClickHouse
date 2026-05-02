#include <gtest/gtest.h>

#include <Storages/HybridSegmentPruner.h>

#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypesNumber.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>

using namespace DB;

namespace
{

ASTPtr parseExpression(const std::string & text)
{
    ParserExpression parser;
    return parseQuery(parser, text, 4096, 1000, 1000000);
}

NamesAndTypesList hybridColumnsForTests()
{
    return {
        {"ts", std::make_shared<DataTypeDateTime>()},
        {"date", std::make_shared<DataTypeDate>()},
        {"customerid", std::make_shared<DataTypeUInt64>()},
        {"x", std::make_shared<DataTypeInt64>()},
        {"y", std::make_shared<DataTypeInt64>()},
    };
}

class HybridSegmentPrunerTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        tryRegisterFunctions();
    }
};

}

TEST_F(HybridSegmentPrunerTest, RangeContradictionPrunes)
{
    /// `ts > '2025-10-01' AND ts <= '2025-09-01'` → unsat → can prune.
    auto where = parseExpression("ts > '2025-10-01'");
    auto seg = parseExpression("ts <= '2025-09-01'");
    EXPECT_TRUE(canPruneHybridSegment(
        /*prewhere*/ nullptr, where, seg, hybridColumnsForTests(), getContext().context));
}

TEST_F(HybridSegmentPrunerTest, OverlappingRangeKeeps)
{
    /// `ts > '2025-10-01' AND ts > '2025-08-01'` → satisfiable → keep.
    auto where = parseExpression("ts > '2025-10-01'");
    auto seg = parseExpression("ts > '2025-08-01'");
    EXPECT_FALSE(canPruneHybridSegment(
        /*prewhere*/ nullptr, where, seg, hybridColumnsForTests(), getContext().context));
}

TEST_F(HybridSegmentPrunerTest, BoundedDnfWithConstantFolding)
{
    /// `(date = yesterday() AND customerid IN (2, 3)) OR (date = today() AND customerid IN (2, 3))`
    /// combined with `date < '2015-01-01'` is unsat in every DNF branch.
    auto where = parseExpression(
        "(date = yesterday() AND customerid IN (2, 3)) OR (date = today() AND customerid IN (2, 3))");
    auto seg = parseExpression("date < '2015-01-01'");
    EXPECT_TRUE(canPruneHybridSegment(
        /*prewhere*/ nullptr, where, seg, hybridColumnsForTests(), getContext().context));
}

TEST_F(HybridSegmentPrunerTest, OrAlternativeNotMandatoryConstraint)
{
    /// `(x < 0 OR y = 1) AND x > 5`: the `x < 0` branch is unsat,
    /// but the `y = 1` branch is satisfiable → keep.
    auto where = parseExpression("(x < 0 OR y = 1) AND x > 5");
    EXPECT_FALSE(canPruneHybridSegment(
        /*prewhere*/ nullptr, where, /*segment*/ nullptr, hybridColumnsForTests(), getContext().context));
}

TEST_F(HybridSegmentPrunerTest, UnsupportedAtomInOrKeeps)
{
    /// An OR with an unsupported atom (e.g. `length(...)`) cannot be pruned.
    auto where = parseExpression("(length(toString(x)) > 10 OR x = 1) AND x = 2");
    EXPECT_FALSE(canPruneHybridSegment(
        /*prewhere*/ nullptr, where, /*segment*/ nullptr, hybridColumnsForTests(), getContext().context));
}
