#include <gtest/gtest.h>
#include <compare>
#include <Common/ClickHouseVersion.h>
#include <Common/Exception.h>

using namespace DB;

/// Canonical, well-formed versions must parse and round-trip through toString() unchanged.
TEST(ClickHouseVersion, ParsesValidVersions)
{
    EXPECT_EQ(ClickHouseVersion("26").toString(), "26");                     /// single numeric component
    EXPECT_EQ(ClickHouseVersion("26.1").toString(), "26.1");                 /// short two-component form
    EXPECT_EQ(ClickHouseVersion("25.3").toString(), "25.3");                 /// real-world `compatibility` value
    EXPECT_EQ(ClickHouseVersion("26.3.1.20001").toString(), "26.3.1.20001"); /// full upstream-shaped version
    EXPECT_EQ(ClickHouseVersion("26.1.3.20001.altinityantalya").toString(),
              "26.1.3.20001.altinityantalya");                              /// canonical Antalya build string
}

/// The suffix handling must stay generic: any single trailing non-numeric token is a valid
/// suffix, no matter its text or how many numeric components precede it.
TEST(ClickHouseVersion, ParsesSuffixedVariants)
{
    EXPECT_EQ(ClickHouseVersion("26.3.1.20001.altinitystable").toString(),
              "26.3.1.20001.altinitystable");                               /// different suffix text still accepted
    EXPECT_EQ(ClickHouseVersion("26.3.1.20001.altinityfips").toString(),
              "26.3.1.20001.altinityfips");                                 /// another suffix variant
    EXPECT_EQ(ClickHouseVersion("26.1.altinityantalya").toString(),
              "26.1.altinityantalya");                                      /// suffix after only two components (non-canonical, still valid)
    EXPECT_EQ(ClickHouseVersion("26.altinityantalya").toString(),
              "26.altinityantalya");                                        /// suffix after a single component
}

/// Malformed inputs must fail fast instead of silently parsing to a different version
TEST(ClickHouseVersion, RejectsMalformedVersions)
{
    EXPECT_THROW(ClickHouseVersion("26..1"), Exception);   /// empty middle component
    EXPECT_THROW(ClickHouseVersion("26.1."), Exception);   /// trailing dot -> empty trailing token
    EXPECT_THROW(ClickHouseVersion("26.x.1"), Exception);  /// non-numeric token that is not the last token

    EXPECT_THROW(ClickHouseVersion(""), Exception);              /// empty string
    EXPECT_THROW(ClickHouseVersion("."), Exception);            /// lone separator, no numeric component
    EXPECT_THROW(ClickHouseVersion(".26"), Exception);          /// leading dot -> empty first component
    EXPECT_THROW(ClickHouseVersion("26.."), Exception);         /// two empty trailing tokens
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001."), Exception);/// trailing dot after a full version

    EXPECT_THROW(ClickHouseVersion("altinityantalya"), Exception);          /// suffix with no numeric component at all
    EXPECT_THROW(ClickHouseVersion("26.altinityantalya.1"), Exception);     /// suffix is not the last token
    EXPECT_THROW(ClickHouseVersion("26.3.1.20001.altinityantalya.1"), Exception); /// junk token after an otherwise-valid suffix

    EXPECT_THROW(ClickHouseVersion("v26.1"), Exception);   /// alphabetic prefix where a number is required
}

/// Ordering compares numeric components first (element by element, shorter list is smaller),
/// then breaks ties on the suffix string lexicographically.
TEST(ClickHouseVersion, ComparesByComponentsThenSuffix)
{
    EXPECT_TRUE(ClickHouseVersion("26.1") < ClickHouseVersion("26.2"));
    EXPECT_TRUE(ClickHouseVersion("25.8") < ClickHouseVersion("26.1"));

    /// 26.1 < 26.1.0  (a trailing .0 makes it "greater" -- can be surprising, so pin it)
    EXPECT_TRUE(ClickHouseVersion("26.1") < ClickHouseVersion("26.1.0"));

    /// with equal components, no suffix sorts before any suffix ("" < "altinityantalya")
    EXPECT_TRUE(ClickHouseVersion("26.1.3.20001") < ClickHouseVersion("26.1.3.20001.altinityantalya"));

    /// with equal components, suffixes order lexicographically: "...antalya" < "...stable"
    EXPECT_TRUE(std::is_lt(ClickHouseVersion("26.3.1.20001.altinityantalya")
                       <=> ClickHouseVersion("26.3.1.20001.altinitystable")));

    /// identical strings compare equal
    EXPECT_TRUE(std::is_eq(ClickHouseVersion("26.1.3.20001.altinityantalya")
                       <=> ClickHouseVersion("26.1.3.20001.altinityantalya")));
}
