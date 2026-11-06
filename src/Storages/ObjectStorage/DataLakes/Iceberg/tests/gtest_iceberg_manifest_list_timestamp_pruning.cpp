#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <base/unaligned.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>
#include <Poco/JSON/Parser.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestListPruning.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>

using namespace DB;
using namespace DB::Iceberg;

namespace
{

ASTPtr parseExpression(const std::string & text)
{
    ParserExpression parser;
    return parseQuery(parser, text, 4096, 1000, 1000000);
}

std::string littleEndianInt64(Int64 value)
{
    std::string bytes(sizeof(Int64), '\0');
    unalignedStoreLittleEndian<Int64>(bytes.data(), value);
    return bytes;
}

/// Identity partition on Iceberg `timestamp` field id 1.
bool manifestCanBePruned(const std::string & where, Int64 lower_ticks, Int64 upper_ticks)
{
    auto context = getContext().context;
    IcebergSchemaProcessor schema_processor(context);
    Poco::JSON::Parser schema_parser;
    auto schema = schema_parser.parse(R"({
        "type": "struct",
        "schema-id": 0,
        "fields": [{"id": 1, "name": "ts", "required": true, "type": "timestamp"}]
    })").extract<Poco::JSON::Object::Ptr>();
    schema_processor.addIcebergTableSchema(schema, context);

    Poco::JSON::Parser specs_parser;
    auto specs = specs_parser.parse(R"([{
        "spec-id": 0,
        "fields": [{"source-id": 1, "transform": "identity", "name": "ts"}]
    }])").extract<Poco::JSON::Array::Ptr>();

    auto columns = schema_processor.getClickHouseTableSchemaById(0);
    auto where_ast = parseExpression(where);
    auto syntax_result = TreeRewriter(context).analyze(where_ast, *columns);
    auto dag = ExpressionAnalyzer(where_ast, syntax_result, context).getActionsDAG(true);

    ManifestListPruner pruner(schema_processor, /* current_schema_id */ 0, /* partition_schema_id */ 0, specs, &dag, context);

    PartitionFieldSummary summary;
    summary.lower_bound = littleEndianInt64(lower_ticks);
    summary.upper_bound = littleEndianInt64(upper_ticks);
    return pruner.canBePruned(/* partition_spec_id */ 0, {summary});
}

class ManifestListTimestampPruningTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        tryRegisterFunctions();
    }
};

}

TEST_F(ManifestListTimestampPruningTest, NanosecondsInTheMiddleBandDoNotPruneAMatchingPredicate)
{
    /// `1990-01-01` as nanoseconds is inside `[1e16, 1e18)`. Reading it as microseconds is year ~21700.
    constexpr Int64 ns_1990_01_01 = 631'152'000'000'000'000LL;
    EXPECT_FALSE(manifestCanBePruned(
        "ts = toDateTime64('1990-01-01 00:00:00', 6, 'UTC')", ns_1990_01_01, ns_1990_01_01));
}

TEST_F(ManifestListTimestampPruningTest, MicrosecondBoundsKeepTheirRange)
{
    /// `2024-02-20 10:00:00` UTC as microseconds, below `1e16`.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    EXPECT_FALSE(manifestCanBePruned(
        "ts = toDateTime64('2024-02-20 10:00:00', 6, 'UTC')", us_2024_02_20, us_2024_02_20));
    EXPECT_TRUE(manifestCanBePruned(
        "ts = toDateTime64('1990-01-01 00:00:00', 6, 'UTC')", us_2024_02_20, us_2024_02_20));
}

TEST_F(ManifestListTimestampPruningTest, SparkSentinelUpperBoundStaysMicroseconds)
{
    /// A normal lower bound keeps the `9999-12-31` sentinel as microseconds.
    constexpr Int64 us_2024_02_20 = 1'708'423'200'000'000LL;
    constexpr Int64 spark_9999_us = 253'402'300'799'999'999LL;
    EXPECT_FALSE(manifestCanBePruned(
        "ts = toDateTime64('2024-02-20 10:00:00', 6, 'UTC')", us_2024_02_20, spark_9999_us));
    EXPECT_TRUE(manifestCanBePruned(
        "ts = toDateTime64('1990-01-01 00:00:00', 6, 'UTC')", us_2024_02_20, spark_9999_us));
}

TEST_F(ManifestListTimestampPruningTest, NanosecondsAtOrAbove1e18AreConverted)
{
    /// `2026-08-11 19:02:06.576815622` stored as nanoseconds. The converted range is one microsecond wide.
    constexpr Int64 ns_2026_08_11 = 1'786'474'926'576'815'622LL;
    EXPECT_FALSE(manifestCanBePruned(
        "ts >= toDateTime64('2026-08-11 19:02:06', 6, 'UTC') AND ts < toDateTime64('2026-08-11 19:02:07', 6, 'UTC')",
        ns_2026_08_11,
        ns_2026_08_11));
    EXPECT_TRUE(manifestCanBePruned(
        "ts = toDateTime64('1990-01-01 00:00:00', 6, 'UTC')", ns_2026_08_11, ns_2026_08_11));
}

#endif
