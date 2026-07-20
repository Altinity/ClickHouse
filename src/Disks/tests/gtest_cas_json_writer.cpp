#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>

using namespace DB;
using namespace DB::Cas;

TEST(CasJsonWriter, KeyValueSequenceMatchesCanonicalShape)
{
    CasJsonWriter w;
    bool first = true;
    w.key("we", first);
    w.u64StringValue(7);
    w.key("mo", first);
    w.u64Number(3);
    w.key("ok", first);
    w.boolValue(true);
    w.key("o", "me", first);
    w.u64StringValue(1);
    w.closeObject(first);
    w.newline();
    EXPECT_EQ(std::move(w).take(), "{\"we\":\"7\",\"mo\":3,\"ok\":true,\"ome\":\"1\"}\n");
}

TEST(CasJsonWriter, EmptyObjectAndClear)
{
    CasJsonWriter w;
    bool first = true;
    w.closeObject(first);
    EXPECT_EQ(w.view(), "{}");
    w.clear();
    EXPECT_EQ(w.size(), 0u);
}

TEST(CasJsonWriter, Hex128MatchesU128ToHex)
{
    const UInt128 v = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
    CasJsonWriter w;
    w.hex128Value(v);
    EXPECT_EQ(std::move(w).take(), "\"" + u128ToHex(v) + "\"");
}

TEST(CasJsonWriter, U64Extremes)
{
    CasJsonWriter w;
    w.u64Number(0);
    w.appendChar(' ');
    w.u64Number(UINT64_MAX);
    EXPECT_EQ(std::move(w).take(), "0 18446744073709551615");
}
