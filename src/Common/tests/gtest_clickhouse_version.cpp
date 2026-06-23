#include <Common/ClickHouseVersion.h>
#include <Common/Exception.h>

#include <gtest/gtest.h>

using namespace DB;

TEST(ClickHouseVersion, ParseNumeric)
{
    EXPECT_EQ(ClickHouseVersion("26.4").toString(), "26.4");
    EXPECT_EQ(ClickHouseVersion("18.12.17").toString(), "18.12.17");
    EXPECT_EQ(ClickHouseVersion("26").toString(), "26");
    EXPECT_EQ(ClickHouseVersion("26.1.3.20001").toString(), "26.1.3.20001");
}

TEST(ClickHouseVersion, ParseSuffixed)
{
    /// A single terminal non-numeric suffix is allowed after numeric components.
    EXPECT_EQ(ClickHouseVersion("26.1.3.20001.altinityantalya").toString(), "26.1.3.20001.altinityantalya");
    EXPECT_EQ(ClickHouseVersion("25.8.16.20001.altinityantalya").toString(), "25.8.16.20001.altinityantalya");
}

TEST(ClickHouseVersion, RejectMalformed)
{
    /// Empty component in the middle.
    EXPECT_THROW(ClickHouseVersion("26..1"), Exception);
    /// Trailing dot (empty terminal component).
    EXPECT_THROW(ClickHouseVersion("26.1."), Exception);
    /// Non-numeric component that is not the terminal one.
    EXPECT_THROW(ClickHouseVersion("26.x.1"), Exception);
    /// A suffix split by a dot is two non-numeric tokens, so the first is intermediate.
    EXPECT_THROW(ClickHouseVersion("26.1.altinity.antalya"), Exception);
    /// Leading dot (no numeric component before the suffix).
    EXPECT_THROW(ClickHouseVersion(".1"), Exception);
    /// No numeric component at all.
    EXPECT_THROW(ClickHouseVersion("altinityantalya"), Exception);
    EXPECT_THROW(ClickHouseVersion(""), Exception);
}

TEST(ClickHouseVersion, Ordering)
{
    EXPECT_LT(ClickHouseVersion("26.3"), ClickHouseVersion("26.4"));
    EXPECT_GT(ClickHouseVersion("26.4"), ClickHouseVersion("26.3"));
    EXPECT_TRUE((ClickHouseVersion("26.4") <=> ClickHouseVersion("26.4")) == std::strong_ordering::equal);
    /// A pure numeric version sorts before its suffixed sibling (empty suffix < non-empty suffix).
    EXPECT_LT(ClickHouseVersion("26.1.3.20001"), ClickHouseVersion("26.1.3.20001.altinityantalya"));
}
