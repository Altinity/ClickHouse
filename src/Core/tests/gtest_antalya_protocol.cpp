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

TEST(AntalyaProtocol, RejectsInvalidMarkers)
{
    const String rejected[] = {
        "",
        "ClickHouse server",
        "ClickHouse server (antalya:1",
        "ClickHouse server (antalya:0)",
        "ClickHouse server (antalya:01)",
        "ClickHouse server (antalya:1234567890)",
        "ClickHouse server (antalya:1x)",
    };

    for (auto name : rejected)
        EXPECT_EQ(stripMarker(name), 0u) << "should not have parsed: " << name;
}

TEST(AntalyaProtocol, StripsMarkerAndCapsVersion)
{
    String marked = "ClickHouse server (antalya:999999999)";
    EXPECT_EQ(stripMarker(marked), static_cast<UInt64>(DBMS_ANTALYA_PROTOCOL_VERSION));
    EXPECT_EQ(marked, "ClickHouse server");
}
