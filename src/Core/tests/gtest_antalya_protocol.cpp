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
        "(antalya:1)",                      /// no base name, so no leading space either
        "ClickHouse server(antalya:1)",     /// missing the separating space
        "ClickHouse server (antalya:1",     /// unterminated
        "ClickHouse server (antalya:)",     /// no digits
        "ClickHouse server (antalya:0)",    /// zero is not a valid version
        "ClickHouse server (antalya:01)",   /// leading zero is not canonical
        "ClickHouse server (antalya:1234567890)",  /// 10 digits, above the cap
        "ClickHouse server (antalya:1) v2",        /// marker must be the suffix
        "ClickHouse server (ANTALYA:1)",    /// case sensitive
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
    /// The scan is anchored at the end, so a doubled marker yields the last value.
    EXPECT_EQ(parseMarker("ClickHouse server (antalya:99) (antalya:1)"), 1u);
}

TEST(AntalyaProtocol, StripMarkerRemovesOnlyTheMarker)
{
    String marked = "ClickHouse server (antalya:42)";
    EXPECT_EQ(stripMarker(marked), 42u);
    EXPECT_EQ(marked, "ClickHouse server");

    /// The widest marker the grammar allows, to pin the digit arithmetic at both ends.
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
