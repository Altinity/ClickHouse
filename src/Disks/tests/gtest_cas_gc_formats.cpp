#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_root_shard.pb.h>
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
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');               /// not JSON (pure protobuf with CasHeader)
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
    /// Bad magic (wrong FormatId in CasHeader) => CORRUPTED_DATA.
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RetiredSet));   /// CART != CAGT
        hdr->set_compatibility_version(currentCompatibilityVersion());
        msg.set_snap_shards(1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcState));
        hdr->set_compatibility_version(G_BUILD + 1);
        msg.set_snap_shards(1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeGcState(bytes); });
    }
    /// snap_shards == 0 is an invariant violation => CORRUPTED_DATA (post-parse check).
    /// A GcStateProto with valid CasHeader but snap_shards=0 (proto3 default / explicitly zero).
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcState));
        hdr->set_compatibility_version(currentCompatibilityVersion());
        /// snap_shards defaults to 0 in proto3 — don't set it.
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
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
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   /// not JSON (pure protobuf with CasHeader)
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

/// ---------- validation throw-paths (CasHeader-based, pure protobuf) ----------

TEST(CasGcFormats, GcStateValidation)
{
    /// Empty / garbage / bad protobuf => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcState(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Wrong magic (CART magic in a CAGT-expecting decoder) => CORRUPTED_DATA.
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RetiredSet));   /// CART
        hdr->set_compatibility_version(currentCompatibilityVersion());
        msg.set_snap_shards(1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION.
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcState));
        hdr->set_compatibility_version(G_BUILD + 1);
        msg.set_snap_shards(1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeGcState(bytes); });
    }
}

TEST(CasGcFormats, RetiredSetValidation)
{
    /// Empty / garbage bytes => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRetiredSet(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRetiredSet(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Bad magic (wrong FormatId in CasHeader) => CORRUPTED_DATA.
    {
        Proto::RetiredSetProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcState));   /// CAGT != CART
        hdr->set_compatibility_version(currentCompatibilityVersion());
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    {
        Proto::RetiredSetProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RetiredSet));
        hdr->set_compatibility_version(G_BUILD + 1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeRetiredSet(bytes); });
    }
    /// Encoding is binary (pure protobuf with CasHeader), not JSON.
    {
        const String bytes = encodeRetiredSet(RetiredSet{});
        ASSERT_FALSE(bytes.empty());
        EXPECT_NE(bytes.front(), '{');
    }
    /// An entry whose kind is the proto3 default (0 / omitted) must be rejected.
    /// Build a valid RetiredSetProto with correct CasHeader but an entry with kind=0.
    {
        Proto::RetiredSetProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RetiredSet));
        hdr->set_compatibility_version(currentCompatibilityVersion());
        msg.add_entries();  /// default entry: kind=0 which is invalid
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRetiredSet(bytes); });
    }
}

TEST(CasGcFormats, OutcomeLogValidation)
{
    /// Empty / garbage bytes => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeOutcomeLog(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Bad magic (wrong FormatId in CasHeader) => CORRUPTED_DATA.
    {
        Proto::GcOutcomeLogProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::Blob));   /// CABL != CAGO
        hdr->set_compatibility_version(currentCompatibilityVersion());
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeOutcomeLog(bytes); });
    }
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    {
        Proto::GcOutcomeLogProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcOutcomes));
        hdr->set_compatibility_version(G_BUILD + 1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeOutcomeLog(bytes); });
    }
    /// Encoding is binary (pure protobuf with CasHeader), not JSON.
    {
        const String bytes = encodeOutcomeLog(OutcomeLog{});
        ASSERT_FALSE(bytes.empty());
        EXPECT_NE(bytes.front(), '{');
    }
    /// An entry with an invalid outcome value (0 is the proto3 default / unset) => CORRUPTED_DATA.
    {
        Proto::GcOutcomeLogProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcOutcomes));
        hdr->set_compatibility_version(currentCompatibilityVersion());
        msg.add_entries();  /// default entry: outcome=0 which is invalid
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeOutcomeLog(bytes); });
    }
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
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   /// not JSON (pure protobuf with CasHeader)
    const RootsRegistry d = decodeRootsRegistry(bytes);
    EXPECT_EQ(d.registry_version, 3u);
    EXPECT_EQ(d.fence_round, 2u);
    EXPECT_EQ(d.namespaces, registry.namespaces);
    EXPECT_EQ(encodeRootsRegistry(d), bytes);                    /// byte-stable re-encode

    /// Empty / garbage bytes => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootsRegistry(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Bad magic (wrong FormatId in CasHeader) => CORRUPTED_DATA.
    {
        Proto::RootsRegistryProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::Blob));   /// CABL != CARR
        hdr->set_compatibility_version(currentCompatibilityVersion());
        std::string bad_bytes; msg.SerializeToString(&bad_bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootsRegistry(bad_bytes); });
    }
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION (fail closed on the future).
    {
        Proto::RootsRegistryProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RootsRegistry));
        hdr->set_compatibility_version(G_BUILD + 1);
        std::string bad_bytes; msg.SerializeToString(&bad_bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeRootsRegistry(bad_bytes); });
    }
    /// codec-specific invariants: empty namespace entry fails closed (post-parse check).
    {
        RootsRegistry bad;
        bad.registry_version = 1;
        bad.namespaces.insert("");   /// empty namespace — insert bypasses post-parse guard
        /// encodeRootsRegistry will serialize the empty string, then decodeRootsRegistry must reject it.
        const String bad_bytes = encodeRootsRegistry(bad);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootsRegistry(bad_bytes); });
    }
    /// duplicate namespace entries fail closed (post-parse check).
    /// Craft a RootsRegistryProto with valid CasHeader + two duplicate namespaces.
    {
        Proto::RootsRegistryProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::RootsRegistry));
        hdr->set_compatibility_version(currentCompatibilityVersion());
        msg.add_namespaces("a");
        msg.add_namespaces("a");   /// duplicate
        std::string dup_bytes; msg.SerializeToString(&dup_bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootsRegistry(dup_bytes); });
    }
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
/// CasHeader round-trip goldens — confirm CasHeader is field 1 of each mutable object,
/// magic + writer_version + compatibility_version are all stamped correctly.
/// ===================================================================================

TEST(CasHeaderGolden, RootsRegistryCasHeaderRoundTrips)
{
    RootsRegistry registry;
    registry.registry_version = 5;
    registry.fence_round = 2;
    registry.namespaces = {"alpha", "beta", "gamma/sub"};
    const String bytes = encodeRootsRegistry(registry);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   // not JSON
    Proto::RootsRegistryProto msg;
    ASSERT_TRUE(msg.ParseFromString(bytes));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::RootsRegistry));
    EXPECT_EQ(msg.header().writer_version(), currentWriterVersion());
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
    const RootsRegistry d = decodeRootsRegistry(bytes);
    EXPECT_EQ(d.registry_version, 5u);
    EXPECT_EQ(d.fence_round, 2u);
    EXPECT_EQ(d.namespaces, registry.namespaces);
    EXPECT_EQ(encodeRootsRegistry(d), bytes);   // byte-stable
    /// Empty namespaces round-trips.
    RootsRegistry empty;
    empty.registry_version = 1;
    empty.fence_round = 0;
    EXPECT_TRUE(decodeRootsRegistry(encodeRootsRegistry(empty)).namespaces.empty());
}

TEST(CasHeaderGolden, GcOutcomesCasHeaderRoundTrips)
{
    OutcomeLog log;
    log.entries.push_back({ObjectKind::Tree, hexToU128("aa00000000000000000000000000000a"),
                           Token{"etag-1", TokenType::ETag}, OutcomeKind::Deleted});
    log.entries.push_back({ObjectKind::Blob, hexToU128("bb00000000000000000000000000000b"),
                           Token{"7", TokenType::Emulated}, OutcomeKind::Spared});
    const String bytes = encodeOutcomeLog(log);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   // not JSON
    Proto::GcOutcomeLogProto msg;
    ASSERT_TRUE(msg.ParseFromString(bytes));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::GcOutcomes));
    EXPECT_EQ(msg.header().writer_version(), currentWriterVersion());
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
    const OutcomeLog d = decodeOutcomeLog(bytes);
    ASSERT_EQ(d.entries.size(), 2u);
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Tree);
    EXPECT_EQ(d.entries[0].hash, hexToU128("aa00000000000000000000000000000a"));
    EXPECT_EQ(d.entries[0].outcome, OutcomeKind::Deleted);
    EXPECT_EQ(d.entries[1].outcome, OutcomeKind::Spared);
    EXPECT_EQ(encodeOutcomeLog(d), bytes);   // byte-stable
    /// Empty log round-trips.
    EXPECT_TRUE(decodeOutcomeLog(encodeOutcomeLog(OutcomeLog{})).entries.empty());
}

TEST(CasHeaderGolden, PoolMetaCasHeaderRoundTrips)
{
    const PoolMeta pm{.pool_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                      .root_shards = 16, .blob_header_len = 96, .min_reader_generation = 0};
    const String bytes = encodePoolMeta(pm);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   // not JSON
    Proto::PoolMetaProto msg;
    ASSERT_TRUE(msg.ParseFromString(bytes));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::PoolMeta));
    EXPECT_EQ(msg.header().writer_version(), currentWriterVersion());
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
    /// Round-trips correctly.
    const PoolMeta d = decodePoolMeta(bytes);
    EXPECT_EQ(d.pool_id, pm.pool_id);
    EXPECT_EQ(d.root_shards, 16u);
    EXPECT_EQ(d.blob_header_len, 96u);
    EXPECT_EQ(d.min_reader_generation, 0u);
}

TEST(CasHeaderGolden, WatermarkCasHeaderAndRetiredSentinel)
{
    /// Live watermark.
    {
        const ServerWatermark w{.server_id = hexToU128("000102030405060708090a0b0c0d0e0f"),
                                .epoch = 7, .min_active = 42, .seq = 3};
        const String bytes = encodeServerWatermark(w);
        ASSERT_FALSE(bytes.empty());
        EXPECT_NE(bytes.front(), '{');
        Proto::WatermarkProto msg;
        ASSERT_TRUE(msg.ParseFromString(bytes));
        EXPECT_EQ(msg.header().magic(), magicFor(FormatId::Watermark));
        EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
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

TEST(CasHeaderGolden, GcStateCasHeaderRoundTrips)
{
    GcState s;
    s.round = 7;
    s.fence_seq = 3;
    s.snap_shards = 2;
    s.snap_generation = 12;
    const String bytes = encodeGcState(s);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');
    Proto::GcStateProto msg;
    ASSERT_TRUE(msg.ParseFromString(bytes));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::GcState));
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
    const GcState d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 7u);
    EXPECT_EQ(d.snap_shards, 2u);
}

TEST(CasHeaderGolden, RetiredSetCasHeaderRoundTrips)
{
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f"),
                          Token{"etag-1", TokenType::ETag}, 1234});
    rs.entries.push_back({ObjectKind::Tree, hexToU128("ffffffffffffffffffffffffffffffff"),
                          Token{"42", TokenType::Emulated}, 0});
    const String bytes = encodeRetiredSet(rs);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');
    Proto::RetiredSetProto msg;
    ASSERT_TRUE(msg.ParseFromString(bytes));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::RetiredSet));
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
    const RetiredSet d = decodeRetiredSet(bytes);
    ASSERT_EQ(d.entries.size(), 2u);
    /// Order after sort (Blob < Tree by kind).
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Blob);
    EXPECT_EQ(d.entries[0].token.value, "etag-1");
    EXPECT_EQ(d.entries[1].kind, ObjectKind::Tree);
    EXPECT_EQ(d.entries[1].token.value, "42");
}

TEST(CasHeaderGolden, PoolMetaFailClosedOnGarbage)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION.
    {
        Proto::PoolMetaProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::PoolMeta));
        hdr->set_compatibility_version(G_BUILD + 1);
        msg.set_root_shards(1);
        msg.set_blob_header_len(96);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodePoolMeta(bytes); });
    }
}

TEST(CasHeaderGolden, WatermarkFailClosedOnGarbage)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeServerWatermark(String("")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeServerWatermark(String("\xff\xff\xff\xff\x00\x00\x00\x00", 8)); });
    /// Future compatibility_version => UNKNOWN_FORMAT_VERSION.
    {
        Proto::WatermarkProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::Watermark));
        hdr->set_compatibility_version(G_BUILD + 1);
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeServerWatermark(bytes); });
    }
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

/// ===================================================================================
/// Task 5: pool-meta min_reader_generation startup gate
/// ===================================================================================

TEST(CasPoolMeta, MinReaderGenerationGate)
{
    /// A pool-meta with min_reader_generation == G_BUILD opens fine.
    {
        PoolMeta pm;
        pm.pool_id = UInt128(42);
        pm.root_shards = 1;
        pm.blob_header_len = 96;
        pm.min_reader_generation = G_BUILD;   /// at-floor: OK
        const String bytes = encodePoolMeta(pm);
        const PoolMeta d = decodePoolMeta(bytes);
        EXPECT_EQ(d.min_reader_generation, G_BUILD);
    }
    /// A pool-meta with min_reader_generation == 0 (default) opens fine.
    {
        PoolMeta pm;
        pm.pool_id = UInt128(43);
        pm.root_shards = 1;
        pm.blob_header_len = 96;
        pm.min_reader_generation = 0;
        const String bytes = encodePoolMeta(pm);
        const PoolMeta d = decodePoolMeta(bytes);
        EXPECT_EQ(d.min_reader_generation, 0u);
    }
    /// A pool-meta with min_reader_generation > G_BUILD => UNKNOWN_FORMAT_VERSION.
    {
        PoolMeta pm;
        pm.pool_id = UInt128(44);
        pm.root_shards = 1;
        pm.blob_header_len = 96;
        pm.min_reader_generation = G_BUILD + 1;   /// above the floor: this build is too old
        const String bytes = encodePoolMeta(pm);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodePoolMeta(bytes); });
    }
}
