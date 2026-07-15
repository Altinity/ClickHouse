#include <Parsers/ASTSystemQuery.h>
#include <Parsers/ParserSystemQuery.h>
#include <Parsers/parseQuery.h>

#include <gtest/gtest.h>

using namespace DB;

/// Grammar-level coverage for `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` (design
/// 2026-07-13-cas-pool-member-decommission, Task 5) that does not need a running server: parses
/// straight through `ParserSystemQuery`, no `Context`/`Interpreter` involved.

TEST(ParserSystemQuery, ContentAddressedDropPoolMemberParsesAndRoundTrips)
{
    const String sql = "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'srv1' FROM DISK 'disk1'";
    ParserSystemQuery parser;
    ASTPtr ast = parseQuery(parser, sql, /*max_query_size=*/0, /*max_parser_depth=*/0, /*max_parser_backtracks=*/0);

    auto * query = ast->as<ASTSystemQuery>();
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(query->type, ASTSystemQuery::Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER);
    EXPECT_EQ(query->replica, "srv1");
    EXPECT_EQ(query->disk, "disk1");

    /// Round-trip: the auto-derived keyword sequence ("CONTENT ADDRESSED DROP POOL MEMBER", the
    /// magic_enum mechanism in ASTSystemQuery.cpp) plus the formatImpl branch reproduce the input.
    EXPECT_EQ(query->formatWithSecretsMultiLine(), sql);
}

TEST(ParserSystemQuery, ContentAddressedDropPoolMemberAcceptsOnCluster)
{
    const String sql = "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'srv1' FROM DISK 'disk1' ON CLUSTER 'my_cluster'";
    ParserSystemQuery parser;
    ASTPtr ast = parseQuery(parser, sql, /*max_query_size=*/0, /*max_parser_depth=*/0, /*max_parser_backtracks=*/0);

    auto * query = ast->as<ASTSystemQuery>();
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(query->type, ASTSystemQuery::Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER);
    EXPECT_EQ(query->replica, "srv1");
    EXPECT_EQ(query->disk, "disk1");
    EXPECT_EQ(query->cluster, "my_cluster");
}

TEST(ParserSystemQuery, ContentAddressedDropPoolMemberRequiresFromDisk)
{
    ParserSystemQuery parser;
    const String sql = "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'srv1'";
    EXPECT_THROW(parseQuery(parser, sql, /*max_query_size=*/0, /*max_parser_depth=*/0, /*max_parser_backtracks=*/0), DB::Exception);
}

TEST(ParserSystemQuery, ContentAddressedDropPoolMemberRequiresSrid)
{
    ParserSystemQuery parser;
    const String sql = "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER FROM DISK 'disk1'";
    EXPECT_THROW(parseQuery(parser, sql, /*max_query_size=*/0, /*max_parser_depth=*/0, /*max_parser_backtracks=*/0), DB::Exception);
}
