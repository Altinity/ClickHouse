#include <gtest/gtest.h>
#include <compare>
#include <Common/ClickHouseVersion.h>
#include <Common/Exception.h>

using namespace DB;

TEST(ClickHouseVersion, AcceptsValidFormats)
{
    EXPECT_EQ(ClickHouseVersion("26.3").toString(), "26.3");
    EXPECT_EQ(ClickHouseVersion("25.8").toString(), "25.8");
    EXPECT_EQ(ClickHouseVersion("18.12.17").toString(), "18.12.17");
    EXPECT_EQ(ClickHouseVersion("26.3.1.20001").toString(), "26.3.1.20001");
    EXPECT_EQ(ClickHouseVersion("26.1.3.20001.altinityantalya").toString(), "26.1.3.20001.altinityantalya");
    EXPECT_EQ(ClickHouseVersion("25.8.16.20001.altinityantalya").toString(), "25.8.16.20001.altinityantalya");
}

TEST(ClickHouseVersion, RejectsMalformedComponents)
{
    EXPECT_THROW(ClickHouseVersion("26..1"), Exception);  /// empty middle component
    EXPECT_THROW(ClickHouseVersion("26.1."), Exception);  /// trailing dot
    EXPECT_THROW(ClickHouseVersion("26.x.1"), Exception); /// non-numeric token that is not the last token
    EXPECT_THROW(ClickHouseVersion(".26"), Exception);    /// leading dot
    EXPECT_THROW(ClickHouseVersion(""), Exception);       /// empty string
    EXPECT_THROW(ClickHouseVersion("       "), Exception);  /// whitespace-only token
    EXPECT_THROW(ClickHouseVersion("v26.1"), Exception);  /// alphabetic prefix where a number is required
    EXPECT_THROW(ClickHouseVersion("."), Exception);      /// lone separator, no numeric component
    EXPECT_THROW(ClickHouseVersion("26.."), Exception);   /// two empty trailing tokens
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001."), Exception); /// trailing dot after a full version
}

TEST(ClickHouseVersion, RejectsInvalidSuffix)
{
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001.altinityantalya.1"), Exception);
    EXPECT_THROW(ClickHouseVersion("26.altinityantalya"), Exception);
    EXPECT_THROW(ClickHouseVersion("26.1.altinityantalya"), Exception);
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001.altinitystable"), Exception);
    EXPECT_THROW(ClickHouseVersion("altinityantalya"), Exception);
}

TEST(ClickHouseVersion, RejectsWrongGroupCount)
{
    EXPECT_THROW(ClickHouseVersion("26"), Exception);              /// 1 group
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001.1"), Exception);  /// 5 groups, no suffix
}

TEST(ClickHouseVersion, ComparesGroupVersion)
{
    EXPECT_TRUE(ClickHouseVersion("26.1") < ClickHouseVersion("26.1.0"));
}
