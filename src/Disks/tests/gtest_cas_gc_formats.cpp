#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <limits>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

/// ---------- round trips ----------

TEST(CasGcFormats, GcStateV3RoundTrip)
{
    GcState s;
    s.round = 7;
    s.fence_seq = 3;
    s.snap_shards = 1;
    s.snap_generation = 12;
    s.lease.owner = hexToU128("00000000000000000000000000000005");
    s.lease.seq = 5;
    s.fence_version[7]["srv1/tbl/0"] = 4;
    s.fence_version[7]["srv1/tbl/1"] = 9;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.round, 7u);
    EXPECT_EQ(d.fence_seq, 3u);
    EXPECT_EQ(d.snap_shards, 1u);
    EXPECT_EQ(d.snap_generation, 12u);
    EXPECT_EQ(d.lease.owner, hexToU128("00000000000000000000000000000005"));
    EXPECT_EQ(d.lease.seq, 5u);
    EXPECT_EQ(d.fence_version.at(7).at("srv1/tbl/0"), 4u);
}

TEST(CasGcFormats, GcStateSnapPrunedThroughRoundTrip)
{
    GcState s;
    s.snap_shards = 2;
    s.snap_generation = 42;
    s.snap_pruned_through = 38;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.snap_pruned_through, 38u);
}

TEST(CasGcFormats, GcStateSnapPrunedThroughBackCompatDefaultsZero)
{
    /// An old gc/state written before B174 has no "snap_pruned_through" key — it must decode to 0,
    /// not throw on the strict unknown-key check (the key is optional on read).
    const String old_state =
        R"({"format":"cas_gc_state","version":3,"round":1,"fence_seq":0,"snap_shards":1,)"
        R"("snap_generation":5,"lease":{"owner":"00000000000000000000000000000000","seq":0},)"
        R"("fence_version":{}})";
    auto d = decodeGcState(old_state);
    EXPECT_EQ(d.snap_pruned_through, 0u);
    EXPECT_EQ(d.snap_generation, 5u);
}

TEST(CasGcFormats, GcHeartbeatRoundTrip)
{
    GcHeartbeat hb;
    hb.owner = hexToU128("0123456789abcdeffedcba9876543210");
    hb.hb_seq = 12345;
    GcHeartbeat d = decodeGcHeartbeat(encodeGcHeartbeat(hb));
    EXPECT_EQ(d.owner, hb.owner);
    EXPECT_EQ(d.hb_seq, 12345u);
    /// boundary owners + a wrong-size blob fail closed
    GcHeartbeat z;
    z.owner = hexToU128("ffffffffffffffffffffffffffffffff");
    z.hb_seq = 0;
    EXPECT_EQ(decodeGcHeartbeat(encodeGcHeartbeat(z)).owner, z.owner);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcHeartbeat(String("short")); });
}

TEST(CasGcFormats, GcStateV3DefaultsAndReadability)
{
    GcState s;
    EXPECT_EQ(s.snap_shards, 1u);                /// default 1 (the GC constant)
    auto bytes = encodeGcState(s);
    EXPECT_NE(bytes.find("\"format\":\"cas_gc_state\""), String::npos);
    EXPECT_NE(bytes.find("\"version\":3"), String::npos);
    auto d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 0u);
    EXPECT_TRUE(d.fence_version.empty());
    EXPECT_EQ(d.lease.owner, DB::UInt128{});
}

TEST(CasGcFormats, GcStateV3Validation)
{
    /// future version => UNKNOWN_FORMAT_VERSION; v1/v2 (old CAGS) => CORRUPTED_DATA (unreleased, no compat);
    /// unknown top-level key, unknown lease key, non-numeric fence_version round key => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":4,"round":0,"fence_seq":0,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{}})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":1,"round":1,"fence_seq":1})"); });
    /// v2 with old folded_cursor key => CORRUPTED_DATA (version mismatch)
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":2,"round":0,"fence_seq":0,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"folded_cursor":{},"fence_version":{}})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":0,"fence_seq":0,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0,"extra":1},"fence_version":{}})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":0,"fence_seq":0,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{"notanumber":{}}})"); });
    /// snap_shards == 0 is an invariant violation => CORRUPTED_DATA
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":0,"fence_seq":0,"snap_shards":0,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{}})"); });
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

TEST(CasGcFormats, OutcomeLogRoundTrip)
{
    OutcomeLog log;
    log.entries.push_back({ObjectKind::Tree, hexToU128("aa00000000000000000000000000000a"),
                           Token{"etag-1", TokenType::ETag}, OutcomeKind::Deleted});
    log.entries.push_back({ObjectKind::Blob, hexToU128("bb00000000000000000000000000000b"),
                           Token{"7", TokenType::Emulated}, OutcomeKind::Spared});
    log.entries.push_back({ObjectKind::Blob, hexToU128("cc00000000000000000000000000000c"),
                           Token{"8", TokenType::Emulated}, OutcomeKind::Replaced});
    log.entries.push_back({ObjectKind::Tree, hexToU128("dd00000000000000000000000000000d"),
                           Token{"9", TokenType::Emulated}, OutcomeKind::Absent});
    auto bytes = encodeOutcomeLog(log);
    EXPECT_NE(bytes.find("\"format\":\"cas_gc_outcomes\""), String::npos);
    auto d = decodeOutcomeLog(bytes);
    ASSERT_EQ(d.entries.size(), 4u);
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Tree);
    EXPECT_EQ(d.entries[0].hash, hexToU128("aa00000000000000000000000000000a"));
    EXPECT_EQ(d.entries[0].outcome, OutcomeKind::Deleted);
    EXPECT_EQ(d.entries[1].outcome, OutcomeKind::Spared);
    EXPECT_EQ(d.entries[2].outcome, OutcomeKind::Replaced);
    EXPECT_EQ(d.entries[3].outcome, OutcomeKind::Absent);
    EXPECT_EQ(d.entries[0].token.value, "etag-1");
    EXPECT_EQ(d.entries[0].token.type, TokenType::ETag);
    EXPECT_EQ(d.entries[3].token.value, "9");
    EXPECT_EQ(encodeOutcomeLog(d), bytes);                       /// byte-stable
}

TEST(CasGcFormats, EmptyOutcomeLogRoundTrips)
{
    auto bytes = encodeOutcomeLog(OutcomeLog{});
    auto d = decodeOutcomeLog(bytes);
    EXPECT_TRUE(d.entries.empty());
}

/// ---------- validation throw-paths (strict JSON) ----------

namespace
{

/// Shared corruption matrix for a JSON codec: the same fail-closed contract applies to every
/// non-hashed metadata object, so each codec's validation test just supplies the decode function and
/// a known-good document to mutate.
template <typename Decode>
void expectStrictJsonContract(Decode && decode, const String & expected_format, uint64_t current_version = 1)
{
    const String version_str = std::to_string(current_version);
    /// Malformed JSON.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(String("{not json")); });
    /// Trailing junk after an otherwise-valid document (the JSON analogue of the binary codecs'
    /// requireNoTrailingBytes guard — a half-written / spliced object must not silently decode).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":)" + version_str + "}trailing"); });
    /// Top-level value not an object.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(String("[]")); });
    /// Wrong format value.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":"cas_wrong","version":)" + version_str + "}"); });
    /// Missing the format key entirely.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(R"({"version":)" + version_str + "}"); });
    /// version as a string (wrong type).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":")" + version_str + R"("})"); });
    /// Future version => UNKNOWN_FORMAT_VERSION (fail closed on the future, never corruption).
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decode(R"({"format":")" + expected_format + R"(","version":)" + std::to_string(current_version + 1) + "}"); });
}

}

TEST(CasGcFormats, GcStateValidation)
{
    expectStrictJsonContract([](const String & s) { return decodeGcState(s); }, "cas_gc_state", /*current_version*/ 3);

    /// Readability pin: the encoded document carries the compact format marker.
    EXPECT_TRUE(encodeGcState(GcState{}).contains(R"("format":"cas_gc_state")"));

    /// Missing a required field (no snap_generation).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":7,"fence_seq":3,"snap_shards":1,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{}})"); });
    /// Wrong type for a numeric field.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":"7","fence_seq":3,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{}})"); });
    /// Old v2 document with folded_cursor key => CORRUPTED_DATA (version mismatch, not unknown key).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":2,"round":7,"fence_seq":3,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"folded_cursor":{},"fence_version":{}})"); });
    /// Unknown extra key in a v3 document.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":7,"fence_seq":3,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{},"x":1})"); });
    /// Non-canonical fence_version round key ("07" parses to 7 but re-encodes as "7" — aliasing).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":7,"fence_seq":3,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{"07":{}}})"); });
    /// fence_version round value not an object.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(
        R"({"format":"cas_gc_state","version":3,"round":7,"fence_seq":3,"snap_shards":1,"snap_generation":0,"lease":{"owner":"00000000000000000000000000000000","seq":0},"fence_version":{"7":4}})"); });
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

TEST(CasGcFormats, OutcomeLogValidation)
{
    expectStrictJsonContract([](const String & s) { return decodeOutcomeLog(s); }, "cas_gc_outcomes");

    EXPECT_TRUE(encodeOutcomeLog(OutcomeLog{}).contains(R"("format":"cas_gc_outcomes")"));

    const String entry = R"({"kind":"tree","hash":"aa00000000000000000000000000000a","token":"etag-1","token_type":"etag","outcome":"deleted"})";

    /// Bad enum: unknown outcome.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"x","token_type":"etag","outcome":"bogus"}]})"); });
    /// Bad enum: unknown kind.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"banana","hash":"aa00000000000000000000000000000a","token":"x","token_type":"etag","outcome":"deleted"}]})"); });
    /// Bad enum: unknown token_type.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"x","token_type":"weird","outcome":"deleted"}]})"); });
    /// Unknown extra key inside an entry.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"x","token_type":"etag","outcome":"deleted","x":1}]})"); });
    /// Missing a required field inside an entry (no token_type).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"x","outcome":"deleted"}]})"); });
    /// Bad hash hex.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(
        R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"nothex","token":"x","token_type":"etag","outcome":"deleted"}]})"); });
    /// Unknown extra key at the top level.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { decodeOutcomeLog(R"({"format":"cas_gc_outcomes","version":1,"entries":[],"x":1})"); });

    /// Sanity: the canonical entry round-trips through decode without throwing.
    EXPECT_NO_THROW(decodeOutcomeLog(R"({"format":"cas_gc_outcomes","version":1,"entries":[)" + entry + "]}"));
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

TEST(CasGcFormats, RootsRegistryRoundTripAndValidation)
{
    /// The namespace registry (spec section 4, decision 2026-06-12): the authoritative namespace
    /// universe, CAS-appended by W-REGISTER and fenced by GC like a shard.
    RootsRegistry registry;
    registry.registry_version = 3;
    registry.fence_round = 2;
    registry.namespaces = {"srv1/tbl", "srv2/tbl2"};
    const String bytes = encodeRootsRegistry(registry);
    EXPECT_NE(bytes.find("\"format\":\"cas_roots_registry\""), String::npos);
    const RootsRegistry d = decodeRootsRegistry(bytes);
    EXPECT_EQ(d.registry_version, 3u);
    EXPECT_EQ(d.fence_round, 2u);
    EXPECT_EQ(d.namespaces, registry.namespaces);
    EXPECT_EQ(encodeRootsRegistry(d), bytes);                    /// byte-stable re-encode

    expectStrictJsonContract([](const String & s) { return decodeRootsRegistry(s); }, "cas_roots_registry");

    /// codec-specific rows: non-string / empty / duplicate namespace entries fail closed
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(
        R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":[7]})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(
        R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":[""]})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(
        R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":["a","a"]})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(
        R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":["a"],"extra":1})"); });
}

/// Verify that `encodeGcSnap` produces zstd-compressed output (version 3, codec byte 1) and that
/// the round-trip is byte-stable.  The snap is populated with 2000 repetitive root edges so that
/// the compressor has obvious redundancy to exploit — the encoded blob must be materially smaller
/// than the uncompressed field footprint, proving compression engaged.
TEST(CasGcSnapCodec, ZstdRoundTripAndShrinks)
{
    GcSnap snap;
    snap.snap_shard = 0;
    snap.generation = 7;
    /// 2000 root edges, each pointing to a distinct tree hash derived from i.
    /// Root edges: (root_shard="srv1/tbl/0", part_name="part_<i>") -> tree_i.
    /// part_name shares the same long prefix — lots of repetition for the compressor.
    for (uint64_t i = 0; i < 2000; ++i)
    {
        const UInt128 tree = (static_cast<UInt128>(i + 1) << 64) | (i + 7);
        snap.addRootEdge("srv1/tbl/0", "part_" + std::to_string(i), tree);
    }

    const String encoded = encodeGcSnap(snap);
    const GcSnap decoded = decodeGcSnap(encoded);

    /// Canonical, byte-stable re-encode: decodeGcSnap(encodeGcSnap(snap)) re-encodes identically.
    EXPECT_EQ(encodeGcSnap(decoded), encoded);

    /// Each root edge raw (uncompressed) costs ~40 bytes (1 kind byte + 2 len shorts + shard string
    /// "srv1/tbl/0" ~10 bytes + part string ~12 bytes avg + 1 target-kind + 16 hash bytes).
    /// 2000 edges * 40 bytes = ~80 KiB uncompressed field footprint; zstd on repetitive data
    /// collapses this to a few KiB.
    GTEST_LOG_(INFO) << "gc/snap zstd-encoded size for 2000 root edges: " << encoded.size() << " bytes"
                     << " (uncompressed footprint estimate: " << 2000u * 40u << " bytes)";
    EXPECT_LT(encoded.size(), 2000u * 40u);
}

/// ===================================================================================
/// R10 strict-JSON encoder goldens. These pin the EXACT bytes a fixed instance of each strict-JSON
/// metadata encoder emits, so routing the encoders through `JsonObjectWriter` cannot move a single
/// byte. The literals were captured by running each encoder BEFORE the R10 refactor.
/// ===================================================================================

namespace
{
constexpr std::string_view GOLDEN_WATERMARK_LIVE =
    R"({"format":"cas_server_watermark","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","epoch":7,"min_active":42,"seq":3})";
constexpr std::string_view GOLDEN_WATERMARK_RETIRED =
    R"({"format":"cas_server_watermark","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","epoch":7,"min_active":"retired","seq":9})";
constexpr std::string_view GOLDEN_HEARTBEAT =
    R"({"format":"cas_heartbeat","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","heartbeat_seq":42,"created_at_ms":1234567890123})";
constexpr std::string_view GOLDEN_POOL_META =
    R"({"format":"cas_pool_meta","version":1,"pool_id":"000102030405060708090a0b0c0d0e0f","root_shards":16,"blob_header_len":96})";
constexpr std::string_view GOLDEN_ROOTS_REGISTRY =
    R"({"format":"cas_roots_registry","version":1,"registry_version":5,"fence_round":2,"namespaces":["alpha","beta","gamma/sub"]})";
constexpr std::string_view GOLDEN_ROOTS_REGISTRY_EMPTY =
    R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":[]})";
constexpr std::string_view GOLDEN_RETIRED_SET =
    R"({"format":"cas_retired_set","version":1,"entries":[{"kind":"blob","hash":"000102030405060708090a0b0c0d0e0f","token":"etag-1","token_type":"etag","size":1234},{"kind":"tree","hash":"ffffffffffffffffffffffffffffffff","token":"42","token_type":"emulated","size":0}]})";
constexpr std::string_view GOLDEN_RETIRED_SET_EMPTY =
    R"({"format":"cas_retired_set","version":1,"entries":[]})";
constexpr std::string_view GOLDEN_OUTCOME_LOG =
    R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"etag-1","token_type":"etag","outcome":"deleted"},{"kind":"blob","hash":"bb00000000000000000000000000000b","token":"7","token_type":"emulated","outcome":"spared"}]})";
constexpr std::string_view GOLDEN_OUTCOME_LOG_EMPTY =
    R"({"format":"cas_gc_outcomes","version":1,"entries":[]})";
}

TEST(CasJsonGolden, ServerWatermarkLive)
{
    const ServerWatermark w{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                            .epoch = 7, .min_active = 42, .seq = 3};
    EXPECT_EQ(encodeServerWatermark(w), GOLDEN_WATERMARK_LIVE);
}

TEST(CasJsonGolden, ServerWatermarkRetired)
{
    const ServerWatermark w{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                            .epoch = 7, .min_active = std::numeric_limits<uint64_t>::max(), .seq = 9};
    EXPECT_EQ(encodeServerWatermark(w), GOLDEN_WATERMARK_RETIRED);
}

TEST(CasJsonGolden, Heartbeat)
{
    const Heartbeat hb{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                       .heartbeat_seq = 42, .created_at_ms = 1234567890123};
    EXPECT_EQ(encodeHeartbeat(hb), GOLDEN_HEARTBEAT);
}

TEST(CasJsonGolden, PoolMeta)
{
    const PoolMeta pm{.pool_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                      .root_shards = 16, .blob_header_len = 96};
    EXPECT_EQ(encodePoolMeta(pm), GOLDEN_POOL_META);
}

TEST(CasJsonGolden, RootsRegistry)
{
    RootsRegistry registry;
    registry.registry_version = 5;
    registry.fence_round = 2;
    registry.namespaces = {"alpha", "beta", "gamma/sub"};
    EXPECT_EQ(encodeRootsRegistry(registry), GOLDEN_ROOTS_REGISTRY);
}

TEST(CasJsonGolden, RootsRegistryEmptyNamespaces)
{
    RootsRegistry registry;
    registry.registry_version = 1;
    registry.fence_round = 0;
    EXPECT_EQ(encodeRootsRegistry(registry), GOLDEN_ROOTS_REGISTRY_EMPTY);
}

TEST(CasJsonGolden, RetiredSet)
{
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f"),
                          Token{"etag-1", TokenType::ETag}, 1234});
    rs.entries.push_back({ObjectKind::Tree, hexToU128("ffffffffffffffffffffffffffffffff"),
                          Token{"42", TokenType::Emulated}, 0});
    EXPECT_EQ(encodeRetiredSet(rs), GOLDEN_RETIRED_SET);
}

TEST(CasJsonGolden, RetiredSetEmpty)
{
    EXPECT_EQ(encodeRetiredSet(RetiredSet{}), GOLDEN_RETIRED_SET_EMPTY);
}

TEST(CasJsonGolden, OutcomeLog)
{
    OutcomeLog log;
    log.entries.push_back({ObjectKind::Tree, hexToU128("aa00000000000000000000000000000a"),
                           Token{"etag-1", TokenType::ETag}, OutcomeKind::Deleted});
    log.entries.push_back({ObjectKind::Blob, hexToU128("bb00000000000000000000000000000b"),
                           Token{"7", TokenType::Emulated}, OutcomeKind::Spared});
    EXPECT_EQ(encodeOutcomeLog(log), GOLDEN_OUTCOME_LOG);
}

TEST(CasJsonGolden, OutcomeLogEmpty)
{
    EXPECT_EQ(encodeOutcomeLog(OutcomeLog{}), GOLDEN_OUTCOME_LOG_EMPTY);
}
