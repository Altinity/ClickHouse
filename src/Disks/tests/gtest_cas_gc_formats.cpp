#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_format.pb.h>
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
namespace Proto = ::clickhouse::cas::format;

/// ---------- round trips ----------

TEST(CasGcFormats, GcStateV3RoundTrip)
{
    GcState s;
    s.round = 7;
    s.fence_seq = 3;
    s.gc_shards = 1;
    s.snap_generation = 12;
    s.lease.owner = hexToU128("00000000000000000000000000000005");
    s.lease.seq = 5;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.round, 7u);
    EXPECT_EQ(d.fence_seq, 3u);
    EXPECT_EQ(d.gc_shards, 1u);
    EXPECT_EQ(d.snap_generation, 12u);
    EXPECT_EQ(d.lease.owner, hexToU128("00000000000000000000000000000005"));
    EXPECT_EQ(d.lease.seq, 5u);
}

TEST(CasGcFormats, GcStateSnapPrunedThroughRoundTrip)
{
    GcState s;
    s.gc_shards = 2;
    s.snap_generation = 42;
    s.snap_pruned_through = 38;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.snap_pruned_through, 38u);
}

TEST(CasGcFormats, GcStateSnapPrunedThroughDefaultsZero)
{
    /// snap_pruned_through defaults to 0 when omitted (proto3 zero-value default).
    GcState s;
    s.gc_shards = 1;
    s.snap_generation = 5;
    s.round = 1;
    s.snap_pruned_through = 0;
    auto d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.snap_pruned_through, 0u);
    EXPECT_EQ(d.snap_generation, 5u);
}

TEST(CasGcFormats, SnapAttemptRoundTrips)
{
    DB::Cas::GcState s;
    s.round = 7;
    s.snap_generation = 4;
    s.snap_attempt = 42;
    const String bytes = DB::Cas::encodeGcState(s);
    const DB::Cas::GcState back = DB::Cas::decodeGcState(bytes);
    EXPECT_EQ(back.snap_attempt, 42u);
    EXPECT_EQ(back.snap_generation, 4u);
}

TEST(CasGcFormats, SnapAttemptDefaultsZero)
{
    DB::Cas::GcState s;
    EXPECT_EQ(DB::Cas::decodeGcState(DB::Cas::encodeGcState(s)).snap_attempt, 0u);
}

TEST(CasGcFormats, ManifestSweepCursorRoundTrips)
{
    DB::Cas::GcState s;
    s.gc_shards = 1;
    s.manifest_sweep_cursor = "p/cas/manifests/server/store/abc/table@cas@/writer/42/aa/id.proto";
    const String bytes = DB::Cas::encodeGcState(s);
    const DB::Cas::GcState back = DB::Cas::decodeGcState(bytes);
    EXPECT_EQ(back.manifest_sweep_cursor, s.manifest_sweep_cursor);
}

TEST(CasGcFormats, ManifestSweepCursorDefaultsEmpty)
{
    DB::Cas::GcState s;
    EXPECT_TRUE(DB::Cas::decodeGcState(DB::Cas::encodeGcState(s)).manifest_sweep_cursor.empty());
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
    EXPECT_EQ(s.gc_shards, 1u);                  /// default 1 (the GC constant)
    auto bytes = encodeGcState(s);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');               /// not JSON (pure protobuf with CasHeader)
    auto d = decodeGcState(bytes);
    EXPECT_EQ(d.round, 0u);
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
        hdr->set_magic(magicFor(FormatId::PoolMeta));   /// CAPM != CAGT
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
    /// gc_shards == 0 is an invariant violation => CORRUPTED_DATA (post-parse check).
    /// A GcStateProto with valid CasHeader but snap_shards=0 (proto3 default / explicitly zero).
    {
        Proto::GcStateProto msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::GcState));
        hdr->set_compatibility_version(currentCompatibilityVersion());
        /// gc_shards (proto field snap_shards) defaults to 0 in proto3 — don't set it.
        std::string bytes; msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcState(bytes); });
    }
}

/// ---------- retired-in-snapshot T4: fold-seal condemned_summary ----------

TEST(CasGcFormats, FoldSealCondemnedSummaryRoundTrips)
{
    /// A seal carrying a non-empty condemned_summary over 2 shards (one a zero entry) round-trips
    /// byte-for-byte and compares equal: decodeFoldSeal(encodeFoldSeal(s)) == s.
    CasFoldSeal s;
    s.generation = 9;
    s.parent_generation = 8;
    s.per_ns_shard["ns/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok"}, .folded_cursor = 3, .incarnation = {}};
    s.blob_target_runs.push_back(RunRef{.key = "gc/gen/9/blob_target/0/0", .checksum = UInt128(0x77),
                                        .shard = 0, .generation = 9});
    s.condemned_summary[0] = CondemnedSummary{.condemned_total = 3, .pending_total = 1,
                                              .oldest_nonpending_condemn_round = 5};
    s.condemned_summary[1] = CondemnedSummary{};   /// explicit zero entry (totality over gc_shards)

    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(s));
    EXPECT_EQ(out, s);
    ASSERT_EQ(out.condemned_summary.size(), 2u);
    EXPECT_EQ(out.condemned_summary.at(0).condemned_total, 3u);
    EXPECT_EQ(out.condemned_summary.at(0).pending_total, 1u);
    EXPECT_EQ(out.condemned_summary.at(0).oldest_nonpending_condemn_round, 5u);
    EXPECT_EQ(out.condemned_summary.at(1).condemned_total, 0u);
    EXPECT_EQ(out.condemned_summary.at(1).pending_total, 0u);
    EXPECT_EQ(out.condemned_summary.at(1).oldest_nonpending_condemn_round,
              std::numeric_limits<uint64_t>::max());   /// UINT64_MAX sentinel survives the round-trip

    /// Byte-deterministic: sorted-by-shard encoding is stable.
    EXPECT_EQ(encodeFoldSeal(s), encodeFoldSeal(s));

    /// Absent by default (proto3): a seal with no summary decodes to an empty map.
    EXPECT_TRUE(decodeFoldSeal(encodeFoldSeal(CasFoldSeal{})).condemned_summary.empty());
}

TEST(CasGcFormats, GcStateRejectsOldVersionFailClosed)
{
    /// Fail-closed on an object whose compatibility_version is beyond this build: the ONLY functional
    /// version guard in this proto codec. (retired_refs is an ADDITIVE proto field — same regime as
    /// snap_attempt (commit e9d898d5c44) — so there is no per-object "v3" integer to
    /// reject; the write-down-to-floor branch is deferred until a roster exists (CasFormat.h). The
    /// forward-incompatible body is what a mixed-version pool would produce and MUST fail closed.)
    Proto::GcStateProto msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::GcState));
    hdr->set_compatibility_version(G_BUILD + 1);
    msg.set_snap_shards(1);
    std::string bytes; msg.SerializeToString(&bytes);
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeGcState(bytes); });
}

TEST(CasGcFormats, OutcomeLogRoundTrip)
{
    OutcomeLog log;
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("aa00000000000000000000000000000a")),
                           Token{"etag-1", TokenType::ETag}, OutcomeKind::Deleted});
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("bb00000000000000000000000000000b")),
                           Token{"7", TokenType::Emulated}, OutcomeKind::Spared});
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("cc00000000000000000000000000000c")),
                           Token{"8", TokenType::Emulated}, OutcomeKind::Replaced});
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("dd00000000000000000000000000000d")),
                           Token{"9", TokenType::Emulated}, OutcomeKind::Absent});
    auto bytes = encodeOutcomeLog(log);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');   /// not JSON (pure protobuf with CasHeader)
    auto d = decodeOutcomeLog(bytes);
    ASSERT_EQ(d.entries.size(), 4u);
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Blob);
    EXPECT_EQ(d.entries[0].hash, BlobDigest::fromU128(hexToU128("aa00000000000000000000000000000a")));
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
        hdr->set_magic(magicFor(FormatId::PoolMeta));   /// CAPM != CAGT
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

/// ===================================================================================
/// CasHeader round-trip goldens — confirm CasHeader is field 1 of each mutable object,
/// magic + writer_version + compatibility_version are all stamped correctly.
/// ===================================================================================

TEST(CasHeaderGolden, GcOutcomesCasHeaderRoundTrips)
{
    OutcomeLog log;
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("aa00000000000000000000000000000a")),
                           Token{"etag-1", TokenType::ETag}, OutcomeKind::Deleted});
    log.entries.push_back({ObjectKind::Blob, BlobDigest::fromU128(hexToU128("bb00000000000000000000000000000b")),
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
    EXPECT_EQ(d.entries[0].kind, ObjectKind::Blob);
    EXPECT_EQ(d.entries[0].hash, BlobDigest::fromU128(hexToU128("aa00000000000000000000000000000a")));
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

TEST(CasHeaderGolden, GcStateCasHeaderRoundTrips)
{
    GcState s;
    s.round = 7;
    s.fence_seq = 3;
    s.gc_shards = 2;
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
    EXPECT_EQ(d.gc_shards, 2u);
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
