#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <unordered_set>

using namespace DB::Cas;

TEST(CasIds, StrongTypingAndContainers)
{
    /// `BlobId` (a bare-hex identity) was deleted in the mixed-algo-pools refactor -- a blob's
    /// identity is now ONLY the `BlobRef` pair (`CasBlobRef.h`). `TreeId` is unrelated and stays,
    /// exercising the SAME strong-typed-string macro (`CAS_STRONG_STRING`).
    TreeId t1{"00ff"}, t2{"00ff"}, t3{"0100"};
    EXPECT_EQ(t1, t2);
    EXPECT_NE(t1, t3);
    std::unordered_set<TreeId> s{t1, t3};
    EXPECT_EQ(s.size(), 2u);
    // TreeId and RootNamespace must not be interchangeable: the next line must NOT compile if uncommented.
    // RootNamespace ns = t1;
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
