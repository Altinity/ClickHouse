#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

/// ---------- round trips ----------

TEST(CasGcFormats, GcStateRoundTrip)
{
    GcState s{.round = 7, .fence_seq = 3};
    auto bytes = encodeGcState(s);
    auto d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 7u);
    EXPECT_EQ(d.fence_seq, 3u);
}

TEST(CasGcFormats, RetiredSetRoundTrip)
{
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f"),
                          Token{"etag-1", TokenType::ETag}, 1234});
    rs.entries.push_back({ObjectKind::Tree, hexToU128("ffffffffffffffffffffffffffffffff"),
                          Token{"42", TokenType::Emulated}, 0});
    auto bytes = encodeRetiredSet(rs);
    auto d = decodeRetiredSet(bytes);
    ASSERT_EQ(d.entries.size(), 2u);
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Blob);
    EXPECT_EQ(d.entries[0].hash, hexToU128("000102030405060708090a0b0c0d0e0f"));
    EXPECT_EQ(d.entries[0].token.value, "etag-1");
    EXPECT_EQ(d.entries[0].token.type, TokenType::ETag);
    EXPECT_EQ(d.entries[0].size, 1234u);
    EXPECT_EQ(d.entries[1].kind, ObjectKind::Tree);
    EXPECT_EQ(d.entries[1].hash, hexToU128("ffffffffffffffffffffffffffffffff"));
    EXPECT_EQ(d.entries[1].token.value, "42");
    EXPECT_EQ(d.entries[1].token.type, TokenType::Emulated);
    EXPECT_EQ(d.entries[1].size, 0u);
}

TEST(CasGcFormats, EmptyRetiredSetRoundTrips)
{
    auto bytes = encodeRetiredSet(RetiredSet{});
    auto d = decodeRetiredSet(bytes);
    EXPECT_TRUE(d.entries.empty());
}

/// ---------- validation throw-paths ----------

TEST(CasGcFormats, Validation)
{
    auto bytes = encodeGcState(GcState{});
    bytes[0] = 'X';
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    auto good = encodeGcState(GcState{});
    good[4] = 2;   /// future version
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { decodeGcState(good); });
    auto rs = encodeRetiredSet(RetiredSet{});
    rs.push_back('\0');   /// trailing garbage
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(rs); });
}

TEST(CasGcFormats, MoreValidation)
{
    /// One-entry retired set used as the base for the corruption rows below.
    const auto makeOneEntrySet = []
    {
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Pack, hexToU128("000102030405060708090a0b0c0d0e0f"),
                              Token{"e", TokenType::ETag}, 1});
        return encodeRetiredSet(rs);
    };

    /// Truncated retired set (chop the last byte of a non-empty encode).
    {
        auto bytes = makeOneEntrySet();
        bytes.pop_back();
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }

    /// Invalid kind byte. Layout: "CART"(4) version(1) reserved(3) entry_count(8) -> kind at 16.
    {
        auto bytes = makeOneEntrySet();
        bytes[16] = 99;
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }

    /// Invalid token_type byte: kind(1) + hash(16) after offset 16 -> token_type at 33.
    {
        auto bytes = makeOneEntrySet();
        bytes[33] = 0;
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }

    /// Bad magic / future version on the retired set.
    {
        auto bytes = makeOneEntrySet();
        bytes[0] = 'X';
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }
    {
        auto bytes = makeOneEntrySet();
        bytes[4] = 2;
        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { decodeRetiredSet(bytes); });
    }

    /// Nonzero reserved bytes (offset 5..7 in both headers).
    {
        auto bytes = makeOneEntrySet();
        bytes[6] = 1;
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }
    {
        auto bytes = encodeGcState(GcState{});
        bytes[5] = 1;
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }

    /// Encoding a token longer than the u16 token_len field must fail closed (exception),
    /// never silently truncate the length while appending the full token value.
    {
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f"),
                              Token{String(70000, 't'), TokenType::ETag}, 1});
        expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { encodeRetiredSet(rs); });
    }

    /// Truncated and trailing-garbage gc/state.
    {
        auto bytes = encodeGcState(GcState{.round = 1, .fence_seq = 2});
        bytes.pop_back();
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }
    {
        auto bytes = encodeGcState(GcState{.round = 1, .fence_seq = 2});
        bytes.push_back('\0');
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }
}
