#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
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

TEST(CasGcFormats, HeartbeatRoundTrip)
{
    Heartbeat hb{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                 .heartbeat_seq = 42,
                 .created_at_ms = 1234567890123};
    auto bytes = encodeHeartbeat(hb);
    auto d = decodeHeartbeat(bytes);
    EXPECT_EQ(d.server_id, hexToU128("000102030405060708090a0b0c0d0e0f"));
    EXPECT_EQ(d.heartbeat_seq, 42u);
    EXPECT_EQ(d.created_at_ms, 1234567890123u);
}

/// ---------- validation throw-paths (strict JSON) ----------

namespace
{

/// Shared corruption matrix for a JSON codec: the same fail-closed contract applies to every
/// non-hashed metadata object, so each codec's validation test just supplies the decode function and
/// a known-good document to mutate.
template <typename Decode>
void expectStrictJsonContract(Decode && decode, const String & expected_format)
{
    /// Malformed JSON.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(String("{not json")); });
    /// Trailing junk after an otherwise-valid document (the JSON analogue of the binary codecs'
    /// requireNoTrailingBytes guard — a half-written / spliced object must not silently decode).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":1}trailing)"); });
    /// Top-level value not an object.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(String("[]")); });
    /// Wrong format value.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":"cas_wrong","version":1})"); });
    /// Missing the format key entirely.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(R"({"version":1})"); });
    /// version as a string (wrong type).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":"1"})"); });
    /// Future version => NOT_IMPLEMENTED (fail closed on the future, never corruption).
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":2})"); });
}

}

TEST(CasGcFormats, GcStateValidation)
{
    expectStrictJsonContract([](const String & s) { return decodeGcState(s); }, "cas_gc_state");

    /// Readability pin: the encoded document carries the compact format marker.
    EXPECT_TRUE(encodeGcState(GcState{}).contains(R"("format":"cas_gc_state")"));

    /// Missing a required field.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeGcState(R"({"format":"cas_gc_state","version":1,"round":7})"); });
    /// Wrong type for a numeric field.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeGcState(R"({"format":"cas_gc_state","version":1,"round":"7","fence_seq":3})"); });
    /// Unknown extra key.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeGcState(R"({"format":"cas_gc_state","version":1,"round":7,"fence_seq":3,"x":1})"); });
}

TEST(CasGcFormats, RetiredSetValidation)
{
    expectStrictJsonContract([](const String & s) { return decodeRetiredSet(s); }, "cas_retired_set");

    EXPECT_TRUE(encodeRetiredSet(RetiredSet{}).contains(R"("format":"cas_retired_set")"));

    const String entry = R"({"kind":"blob","hash":"000102030405060708090a0b0c0d0e0f","token":"e","token_type":"etag","size":1})";

    /// Missing a required field inside an entry.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[{"kind":"blob"}]})"); });
    /// Unknown extra key inside an entry.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)"
            + String(R"({"kind":"blob","hash":"000102030405060708090a0b0c0d0e0f","token":"e","token_type":"etag","size":1,"x":1})") + "]}");
    });
    /// Wrong type: size as a string.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)"
            + String(R"({"kind":"blob","hash":"000102030405060708090a0b0c0d0e0f","token":"e","token_type":"etag","size":"1"})") + "]}");
    });
    /// Bad enum: unknown kind.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)"
            + String(R"({"kind":"banana","hash":"000102030405060708090a0b0c0d0e0f","token":"e","token_type":"etag","size":1})") + "]}");
    });
    /// Bad enum: unknown token_type.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)"
            + String(R"({"kind":"blob","hash":"000102030405060708090a0b0c0d0e0f","token":"e","token_type":"weird","size":1})") + "]}");
    });
    /// Bad hash hex.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)"
            + String(R"({"kind":"blob","hash":"nothex","token":"e","token_type":"etag","size":1})") + "]}");
    });
    /// Unknown extra key at the top level.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[],"x":1})"); });

    /// Sanity: the canonical entry round-trips through decode without throwing.
    EXPECT_NO_THROW(decodeRetiredSet(R"({"format":"cas_retired_set","version":1,"entries":[)" + entry + "]}"));
}

TEST(CasGcFormats, HeartbeatValidation)
{
    expectStrictJsonContract([](const String & s) { return decodeHeartbeat(s); }, "cas_heartbeat");

    EXPECT_TRUE(encodeHeartbeat(Heartbeat{}).contains(R"("format":"cas_heartbeat")"));

    /// Missing a required field.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeHeartbeat(R"({"format":"cas_heartbeat","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","heartbeat_seq":7})"); });
    /// Wrong type for a numeric field.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeHeartbeat(R"({"format":"cas_heartbeat","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","heartbeat_seq":"7","created_at_ms":9})"); });
    /// Bad hash hex for server_id.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeHeartbeat(R"({"format":"cas_heartbeat","version":1,"server_id":"nothex","heartbeat_seq":7,"created_at_ms":9})"); });
    /// Unknown extra key.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeHeartbeat(R"({"format":"cas_heartbeat","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","heartbeat_seq":7,"created_at_ms":9,"x":1})"); });
}
