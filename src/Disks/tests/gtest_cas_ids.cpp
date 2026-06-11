#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <unordered_set>

using namespace DB::Cas;

TEST(CasIds, StrongTypingAndContainers)
{
    BlobId b1{"00ff"}, b2{"00ff"}, b3{"0100"};
    EXPECT_EQ(b1, b2);
    EXPECT_NE(b1, b3);
    std::unordered_set<BlobId> s{b1, b3};
    EXPECT_EQ(s.size(), 2u);
    // BlobId and TreeId must not be interchangeable: the next line must NOT compile if uncommented.
    // TreeId t = b1;
}

TEST(CasIds, HexU128RoundTrip)
{
    // UInt128 is a global typedef (wide::integer<128,unsigned>), not in DB:: namespace.
    const UInt128 v = (UInt128(0x0123456789abcdefULL) << 64) | 0xfedcba9876543210ULL;
    const auto hex = u128ToHex(v);
    EXPECT_EQ(hex.size(), 32u);
    EXPECT_EQ(hexToU128(hex), v);
    EXPECT_THROW(hexToU128("zz"), DB::Exception);          // not hex
    EXPECT_THROW(hexToU128("0123"), DB::Exception);        // wrong length
}

TEST(CasToken, Basics)
{
    Token a{"etag-1", TokenType::ETag}, b{"etag-1", TokenType::ETag}, c{"etag-2", TokenType::ETag};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_TRUE(Token{}.empty());
    EXPECT_FALSE(a.empty());
}
