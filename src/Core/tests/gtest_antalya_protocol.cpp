#include <gtest/gtest.h>

#include <Core/AntalyaProtocol.h>

using namespace DB;
using namespace DB::AntalyaProtocol;


TEST(AntalyaProtocol, AppendMarkerSpellsTheWireForm)
{
    EXPECT_EQ(
        appendMarker("ClickHouse server"),
        "ClickHouse server (antalya:" + std::to_string(DBMS_ANTALYA_PROTOCOL_VERSION) + ")");
}

TEST(AntalyaProtocol, AppendThenParseRoundTrips)
{
    EXPECT_EQ(parseMarker(appendMarker("ClickHouse server")), static_cast<UInt64>(DBMS_ANTALYA_PROTOCOL_VERSION));
}

TEST(AntalyaProtocol, ParseRejectsNonCanonicalMarkers)
{
    const std::string_view rejected[] = {
        "",
        "ClickHouse server",
        "ClickHouse client",
        "(antalya:1)",
        "ClickHouse server(antalya:1)",
        "ClickHouse server (antalya:1",
        "ClickHouse server (antalya:)",
        "ClickHouse server (antalya:0)",
        "ClickHouse server (antalya:01)",
        "ClickHouse server (antalya:1234567890)",
        "ClickHouse server (antalya:1) v2",
        "ClickHouse server (ANTALYA:1)",
        "ClickHouse server (antalya:1x)",
        "ClickHouse server (antalya:1 )",
        "antalya:1)",
        ")",
    };

    for (const auto & name : rejected)
        EXPECT_EQ(parseMarker(name), 0u) << "should not have parsed: " << name;
}

TEST(AntalyaProtocol, ParseTakesTheTrailingMarkerWhenRepeated)
{
    EXPECT_EQ(parseMarker("ClickHouse server (antalya:99) (antalya:1)"), 1u);
}

TEST(AntalyaProtocol, StripMarkerRemovesOnlyTheMarker)
{
    String marked = "ClickHouse server (antalya:42)";
    EXPECT_EQ(stripMarker(marked), 42u);
    EXPECT_EQ(marked, "ClickHouse server");

    String widest = "ClickHouse (antalya:999999999)";
    EXPECT_EQ(stripMarker(widest), 999999999u);
    EXPECT_EQ(widest, "ClickHouse");

    String plain = "ClickHouse server";
    EXPECT_EQ(stripMarker(plain), 0u);
    EXPECT_EQ(plain, "ClickHouse server");

    String malformed = "ClickHouse server (antalya:01)";
    EXPECT_EQ(stripMarker(malformed), 0u);
    EXPECT_EQ(malformed, "ClickHouse server (antalya:01)");
}

TEST(AntalyaProtocol, NegotiateCapsToOurVersion)
{
    EXPECT_EQ(negotiate(0), 0u);
    EXPECT_EQ(negotiate(DBMS_ANTALYA_PROTOCOL_VERSION), static_cast<UInt64>(DBMS_ANTALYA_PROTOCOL_VERSION));
    EXPECT_EQ(negotiate(DBMS_ANTALYA_PROTOCOL_VERSION + 1), static_cast<UInt64>(DBMS_ANTALYA_PROTOCOL_VERSION));
    EXPECT_EQ(negotiate(999999999), static_cast<UInt64>(DBMS_ANTALYA_PROTOCOL_VERSION));
}
