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

TEST(CasGcFormats, GcStateSnapPrunedThroughDefaultsZero)
{
    /// snap_pruned_through defaults to 0 when omitted (proto3 zero-value default).
    GcState s;
    s.snap_shards = 1;
    s.snap_generation = 5;
    s.round = 1;
    s.snap_pruned_through = 0;
    auto d = decodeGcState(encodeGcState(s));
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

TEST(CasGcFormats, GcStateV3DefaultsAndEncodingIsBinary)
{
    GcState s;
    EXPECT_EQ(s.snap_shards, 1u);                /// default 1 (the GC constant)
    auto bytes = encodeGcState(s);
    ASSERT_GE(bytes.size(), 8u);
    /// Protobuf framing: CAGT magic + u16 LE writer + u16 LE min_reader.
    EXPECT_EQ(bytes.substr(0, 4), "CAGT");
    EXPECT_NE(bytes.front(), '{');               /// not JSON
    auto d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 0u);
    EXPECT_TRUE(d.fence_version.empty());
    EXPECT_EQ(d.lease.owner, DB::UInt128{});
}

TEST(CasGcFormats, GcStateV3Validation)
{
    /// Empty / garbage => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Bad magic (not CAGT) => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("CAGS\x01\x00\x01\x00", 8)); });
    /// Future min_reader => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [] { decodeGcState(String("CAGT\x02\x00\x02\x00", 8)); });
    /// snap_shards == 0 is an invariant violation => CORRUPTED_DATA (post-parse check).
    /// GcStateProto wire format: field 3 (snap_shards) tag byte is 0x18, value byte 0x01 for snap_shards=1.
    /// An all-zero proto body with valid framing has snap_shards=0 (proto3 default) => CORRUPTED_DATA.
    {
        /// 8-byte CAGT framing + empty proto body = snap_shards defaults to 0 in proto3.
        const String framing_only("CAGT\x01\x00\x01\x00", 8);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(framing_only); });
    }
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
    /// Empty / garbage / bad magic => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("CART\x01\x00\x01\x00", 8)); });
    /// Future min_reader => UNKNOWN_FORMAT_VERSION.
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [] { decodeGcState(String("CAGT\x02\x00\x02\x00", 8)); });
}

TEST(CasGcFormats, RetiredSetValidation)
{
    /// Empty / garbage bytes => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRetiredSet(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRetiredSet(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Bad magic (not CART) => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRetiredSet(String("CAGT\x01\x00\x01\x00", 8)); });
    /// Future min_reader => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [] { decodeRetiredSet(String("CART\x02\x00\x02\x00", 8)); });
    /// Encoding is binary (framing magic CART), not JSON.
    {
        const String bytes = encodeRetiredSet(RetiredSet{});
        ASSERT_GE(bytes.size(), 8u);
        EXPECT_EQ(bytes.substr(0, 4), "CART");
        EXPECT_NE(bytes.front(), '{');
    }
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
/// R10 strict-JSON encoder goldens (remaining JSON codecs: heartbeat, roots-registry, outcome-log).
/// The watermark, pool-meta, and retired-set goldens were removed when those codecs moved to protobuf
/// (Plan 3c). Protobuf framing-header goldens for the new codecs are in CasPoolMeta/CasWatermark/
/// CasGcFormats round-trip tests above.
/// ===================================================================================

namespace
{
constexpr std::string_view GOLDEN_HEARTBEAT =
    R"({"format":"cas_heartbeat","version":1,"server_id":"000102030405060708090a0b0c0d0e0f","heartbeat_seq":42,"created_at_ms":1234567890123})";
constexpr std::string_view GOLDEN_ROOTS_REGISTRY =
    R"({"format":"cas_roots_registry","version":1,"registry_version":5,"fence_round":2,"namespaces":["alpha","beta","gamma/sub"]})";
constexpr std::string_view GOLDEN_ROOTS_REGISTRY_EMPTY =
    R"({"format":"cas_roots_registry","version":1,"registry_version":1,"fence_round":0,"namespaces":[]})";
constexpr std::string_view GOLDEN_OUTCOME_LOG =
    R"({"format":"cas_gc_outcomes","version":1,"entries":[{"kind":"tree","hash":"aa00000000000000000000000000000a","token":"etag-1","token_type":"etag","outcome":"deleted"},{"kind":"blob","hash":"bb00000000000000000000000000000b","token":"7","token_type":"emulated","outcome":"spared"}]})";
constexpr std::string_view GOLDEN_OUTCOME_LOG_EMPTY =
    R"({"format":"cas_gc_outcomes","version":1,"entries":[]})";
}

TEST(CasJsonGolden, Heartbeat)
{
    const Heartbeat hb{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                       .heartbeat_seq = 42, .created_at_ms = 1234567890123};
    EXPECT_EQ(encodeHeartbeat(hb), GOLDEN_HEARTBEAT);
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

/// ===================================================================================
/// Protobuf framing-header goldens for pool-meta, watermark, gc-state, retired-set (Plan 3c).
/// Pin the framing magic bytes and verify binary (not JSON) encoding for each new codec.
/// ===================================================================================

TEST(CasProtobufFramingGolden, PoolMetaFramingMagic)
{
    const PoolMeta pm{.pool_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                      .root_shards = 16, .blob_header_len = 96};
    const String bytes = encodePoolMeta(pm);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes.substr(0, 4), "CAPM");
    EXPECT_NE(bytes.front(), '{');   // not JSON
    /// Round-trips correctly.
    const PoolMeta d = decodePoolMeta(bytes);
    EXPECT_EQ(d.pool_id, pm.pool_id);
    EXPECT_EQ(d.root_shards, 16u);
    EXPECT_EQ(d.blob_header_len, 96u);
}

TEST(CasProtobufFramingGolden, WatermarkFramingMagicAndRetiredSentinel)
{
    /// Live watermark.
    {
        const ServerWatermark w{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                                .epoch = 7, .min_active = 42, .seq = 3};
        const String bytes = encodeServerWatermark(w);
        ASSERT_GE(bytes.size(), 8u);
        EXPECT_EQ(bytes.substr(0, 4), "CAWM");
        EXPECT_NE(bytes.front(), '{');
        const ServerWatermark d = decodeServerWatermark(bytes);
        EXPECT_EQ(d.server_id, w.server_id);
        EXPECT_EQ(d.epoch, 7u);
        EXPECT_EQ(d.min_active, 42u);
        EXPECT_EQ(d.seq, 3u);
    }
    /// Retired sentinel (UINT64_MAX) round-trips.
    {
        const ServerWatermark w{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                                .epoch = 7, .min_active = std::numeric_limits<uint64_t>::max(), .seq = 9};
        const String bytes = encodeServerWatermark(w);
        const ServerWatermark d = decodeServerWatermark(bytes);
        EXPECT_EQ(d.min_active, std::numeric_limits<uint64_t>::max());
        EXPECT_EQ(d.seq, 9u);
    }
}

TEST(CasProtobufFramingGolden, GcStateFramingMagic)
{
    GcState s;
    s.round = 7;
    s.fence_seq = 3;
    s.snap_shards = 2;
    s.snap_generation = 12;
    const String bytes = encodeGcState(s);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes.substr(0, 4), "CAGT");
    EXPECT_NE(bytes.front(), '{');
    const GcState d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 7u);
    EXPECT_EQ(d.snap_shards, 2u);
}

TEST(CasProtobufFramingGolden, RetiredSetFramingMagic)
{
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f"),
                          Token{"etag-1", TokenType::ETag}, 1234});
    rs.entries.push_back({ObjectKind::Tree, hexToU128("ffffffffffffffffffffffffffffffff"),
                          Token{"42", TokenType::Emulated}, 0});
    const String bytes = encodeRetiredSet(rs);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes.substr(0, 4), "CART");
    EXPECT_NE(bytes.front(), '{');
    const RetiredSet d = decodeRetiredSet(bytes);
    ASSERT_EQ(d.entries.size(), 2u);
    /// Order after sort (Blob < Tree by kind).
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Blob);
    EXPECT_EQ(d.entries[0].token.value, "etag-1");
    EXPECT_EQ(d.entries[1].kind, ObjectKind::Tree);
    EXPECT_EQ(d.entries[1].token.value, "42");
}

TEST(CasProtobufFramingGolden, PoolMetaFailClosedOnGarbage)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [] { decodePoolMeta(String("CAPM\x02\x00\x02\x00", 8)); });
}

TEST(CasProtobufFramingGolden, WatermarkFailClosedOnGarbage)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeServerWatermark(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeServerWatermark(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [] { decodeServerWatermark(String("CAWM\x02\x00\x02\x00", 8)); });
}

TEST(CasPoolMeta, ConstantInvariantsPostParse)
{
    /// `encodePoolMeta` serializes whatever is in the struct without validation, so we can build
    /// structs with invalid constants and verify that `decodePoolMeta` rejects them as CORRUPTED_DATA.

    /// root_shards == 0 => CORRUPTED_DATA.
    {
        PoolMeta bad;
        bad.pool_id = UInt128(1);
        bad.root_shards = 0;
        bad.blob_header_len = 96;
        const String bytes = encodePoolMeta(bad);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodePoolMeta(bytes); });
    }
    /// blob_header_len not 8-aligned => CORRUPTED_DATA.
    {
        PoolMeta bad;
        bad.pool_id = UInt128(1);
        bad.root_shards = 1;
        bad.blob_header_len = 97;   /// not 8-aligned
        const String bytes = encodePoolMeta(bad);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodePoolMeta(bytes); });
    }
    /// blob_header_len < 96 => CORRUPTED_DATA.
    {
        PoolMeta bad;
        bad.pool_id = UInt128(1);
        bad.root_shards = 1;
        bad.blob_header_len = 88;   /// < 96
        const String bytes = encodePoolMeta(bad);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodePoolMeta(bytes); });
    }
    /// blob_header_len > 16384 => CORRUPTED_DATA.
    {
        PoolMeta bad;
        bad.pool_id = UInt128(1);
        bad.root_shards = 1;
        bad.blob_header_len = 32768;   /// > 16384
        const String bytes = encodePoolMeta(bad);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodePoolMeta(bytes); });
    }
}
