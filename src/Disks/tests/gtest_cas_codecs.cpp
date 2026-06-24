#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

namespace
{

EnvelopeHeader makeBlobHeader()
{
    EnvelopeHeader h;
    h.kind = ObjectKind::Blob;
    h.hash_algo = 1;
    h.logical_size = 1000;
    h.logical_hash = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
    h.domain_id = UInt128(0x42);
    h.incarnation_tag = UInt128(0x99);
    h.build_id = UInt128(0x7);
    return h;
}

}

/// ---------- round trip ----------

TEST(CasEnvelope, RoundTripWithProvenanceAndIntendedRef)
{
    EnvelopeHeader h = makeBlobHeader();
    Provenance p;
    p.created_at_ms = 1717000000000ULL;
    p.creator_server_id = (UInt128(0xaabbccddULL) << 64) | UInt128(0x11223344ULL);
    p.ch_version = 24006001;
    p.op = ProvenanceOp::Merge;
    h.provenance = p;
    h.intended_ref = String("srv1/tbl-uuid/all_1_1_0");

    String bytes = encodeEnvelopeHeader(h);
    EXPECT_GE(h.header_len, 94u);
    EXPECT_EQ(bytes.size(), h.header_len);

    const uint64_t object_size = h.header_len + h.logical_size;
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, object_size, ObjectKind::Blob);

    EXPECT_EQ(d.kind, ObjectKind::Blob);
    EXPECT_EQ(d.hash_algo, 1u);
    EXPECT_EQ(d.logical_size, 1000u);
    EXPECT_EQ(d.logical_hash, h.logical_hash);
    EXPECT_EQ(d.domain_id, h.domain_id);
    EXPECT_EQ(d.incarnation_tag, h.incarnation_tag);
    EXPECT_EQ(d.build_id, h.build_id);
    EXPECT_EQ(d.header_len, h.header_len);

    ASSERT_TRUE(d.provenance.has_value());
    EXPECT_EQ(d.provenance->created_at_ms, p.created_at_ms);
    EXPECT_EQ(d.provenance->creator_server_id, p.creator_server_id);
    EXPECT_EQ(d.provenance->ch_version, p.ch_version);
    EXPECT_EQ(d.provenance->op, ProvenanceOp::Merge);

    ASSERT_TRUE(d.intended_ref.has_value());
    EXPECT_EQ(*d.intended_ref, "srv1/tbl-uuid/all_1_1_0");
}

TEST(CasEnvelope, RoundTripNoExtensions)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// 94-byte core + 2 zero alignment bytes = 96 on disk.
    EXPECT_EQ(h.header_len, 96u);

    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob);
    EXPECT_FALSE(d.provenance.has_value());
    EXPECT_FALSE(d.intended_ref.has_value());
}

TEST(CasEnvelope, PayloadOffsetHelper)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob);
    EXPECT_EQ(payloadOffset(d), static_cast<uint64_t>(d.header_len));
}

/// ---------- validation throw-paths ----------

TEST(CasEnvelope, FutureMinReaderVersionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// min_reader_version is at [6,8) LE — patch to 2 to drive the gateOnRead fail-closed path.
    bytes[6] = 2; bytes[7] = 0;
    expectThrowsCode(
        DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob); });
}

TEST(CasEnvelope, WrongKindThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Tree), DB::Exception);
}

TEST(CasEnvelope, BadHeaderLenThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// header_len = 90 (< 94) at offset 10; the header hash would mismatch too, but range check fires first.
    bytes[10] = 90;
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, SizeMismatchThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size + 1, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, CorruptedHeaderFieldFailsHeaderHash)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[24] ^= 0xff;  /// flip a byte inside logical_hash [22,38) -> header hash mismatch
    expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob); });
}

TEST(CasEnvelope, CorruptedStoredHeaderHashThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[92] ^= 0xff;  /// flip a byte inside the stored header_hash itself ([86,94))
    expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob); });
}

TEST(CasEnvelope, CriticalUnknownExtensionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    h.flags_has_critical_extension = true;
    h.unknown_critical_tlv = true;  /// emit an unknown TLV type with the critical flag set
    String bytes = encodeEnvelopeHeader(h);
    expectThrowsCode(
        DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob); });
}

/// ===================================================================================
/// CasTreeCodec
/// ===================================================================================

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <algorithm>
#include <random>

namespace
{

TreeEntry inlineEntry(const String & name, const String & bytes)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Inline;
    e.file_hash = UInt128(0);
    e.file_size = bytes.size();
    e.inline_bytes = bytes;
    return e;
}

TreeEntry blobEntry(const String & name, UInt128 hash, uint64_t size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Blob;
    e.file_hash = hash;
    e.file_size = size;
    return e;
}

TreeEntry subtreeEntry(const String & name, UInt128 child_tree, uint64_t tree_size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Subtree;
    e.file_hash = child_tree;
    e.file_size = tree_size;
    return e;
}

std::vector<TreeEntry> sampleEntries()
{
    return {
        inlineEntry("primary.idx", "0123456789"),
        blobEntry("data.bin", (UInt128(0xaaaa) << 64) | UInt128(0xbbbb), 4096),
        subtreeEntry("nested", (UInt128(0x1) << 64) | UInt128(0x2), 200),
    };
}

}

TEST(CasTreeCodec, RoundTripAllPlacements)
{
    auto entries = sampleEntries();
    const String encoded = encodeTree(entries);
    const auto decoded = decodeTree(encoded);

    ASSERT_EQ(decoded.size(), 3u);

    /// decoded is sorted by name byte-wise
    std::vector<String> names;
    for (const auto & e : decoded)
        names.push_back(e.name);
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));

    for (const auto & e : decoded)
    {
        if (e.name == "primary.idx")
        {
            EXPECT_EQ(e.placement, Placement::Inline);
            EXPECT_EQ(e.inline_bytes, "0123456789");
            EXPECT_EQ(e.file_size, 10u);
        }
        else if (e.name == "data.bin")
        {
            EXPECT_EQ(e.placement, Placement::Blob);
            EXPECT_EQ(e.file_hash, (UInt128(0xaaaa) << 64) | UInt128(0xbbbb));
            EXPECT_EQ(e.file_size, 4096u);
        }
        else if (e.name == "nested")
        {
            EXPECT_EQ(e.placement, Placement::Subtree);
            EXPECT_EQ(e.file_hash, (UInt128(0x1) << 64) | UInt128(0x2));
            EXPECT_EQ(e.file_size, 200u);
        }
        else
            FAIL() << "unexpected entry name " << e.name;
    }
}

TEST(CasTreeCodec, DeterministicAcrossInputOrder)
{
    auto a = sampleEntries();
    auto b = sampleEntries();
    std::mt19937 rng(12345);
    std::shuffle(b.begin(), b.end(), rng);

    const String ea = encodeTree(a);
    const String eb = encodeTree(b);
    EXPECT_EQ(ea, eb);   /// shuffled input -> byte-identical output
}

TEST(CasTreeCodec, DuplicateNameThrows)
{
    std::vector<TreeEntry> entries = {
        inlineEntry("dup", "a"),
        inlineEntry("dup", "b"),
    };
    EXPECT_THROW(encodeTree(entries), DB::Exception);
}

TEST(CasTreeCodec, UnknownPlacementOnDecodeThrows)
{
    auto entries = sampleEntries();
    String encoded = encodeTree(entries);
    /// Corrupt the placement byte of the first entry. New payload layout: count(4) then per entry:
    /// name_len(2) name placement(1) ...
    /// First entry name is the smallest: "data.bin" (8 chars).
    /// count(4) + name_len(2) + name(8) = 14; placement byte at 14.
    encoded[14] = 99;
    EXPECT_THROW(decodeTree(encoded), DB::Exception);
}

TEST(CasTreeCodec, TruncatedBufferThrows)
{
    auto entries = sampleEntries();
    String encoded = encodeTree(entries);
    encoded.resize(encoded.size() - 3);
    EXPECT_THROW(decodeTree(encoded), DB::Exception);
}

TEST(CasTreeCodec, HugeInlineLengthThrowsCorruptedData)
{
    /// A corrupted data_length field must be rejected by the inline-slice bounds check in `decodeTree`
    /// before any allocation: the decoder checks (offset+length) against the data section size and throws
    /// CORRUPTED_DATA rather than silently allocating gigabytes.
    std::vector<TreeEntry> entries = {inlineEntry("f", "hello")};
    String encoded = encodeTree(entries);

    /// New catalog-first layout (see `encodeTree`): count(4) then for the single entry:
    /// name_len(2) name(1, "f") placement(1) file_hash(16) file_size(8) data_offset(8) data_length(8).
    /// data_length sits at offset 4 + 2 + 1 + 1 + 16 + 8 + 8 = 40.
    constexpr size_t len_offset = 4 + 2 + 1 + 1 + 16 + 8 + 8;
    ASSERT_LE(len_offset + 8, encoded.size());
    /// Sanity: the bytes at the computed offset hold the known length of "hello" (5, LE u64).
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset]), 5u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 1]), 0u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 2]), 0u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 3]), 0u);

    /// Patch the length to ~4 GiB (0xFFFFFFF0 little-endian u64).
    encoded[len_offset] = static_cast<char>(0xF0);
    encoded[len_offset + 1] = static_cast<char>(0xFF);
    encoded[len_offset + 2] = static_cast<char>(0xFF);
    encoded[len_offset + 3] = static_cast<char>(0xFF);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeTree(encoded); });
}

TEST(CasTreeCodec, CorruptedCountThrows)
{
    /// The new payload starts with a u32 entry_count (no magic). A junk count of 0xFFFFFFFF far exceeds
    /// what the buffer could possibly encode; the decoder's count-vs-buffer guard rejects it as
    /// CORRUPTED_DATA before any reserve, so a junk count can't trigger a huge allocation.
    auto entries = sampleEntries();
    String encoded = encodeTree(entries);
    encoded[0] = encoded[1] = encoded[2] = encoded[3] = static_cast<char>(0xFF);   /// entry_count = 0xFFFFFFFF
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeTree(encoded); });
}

TEST(CasTreeCodec, EmptyTreeRoundTrips)
{
    std::vector<TreeEntry> empty;
    const String encoded = encodeTree(empty);
    const auto decoded = decodeTree(encoded);
    EXPECT_TRUE(decoded.empty());
}

/// ===================================================================================
/// CasRootShardCodec
/// ===================================================================================

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>

TEST(CasRootShardCodec, RoundTripRefsAndJournal)
{
    RootShard rs;
    rs.shard_version = 42;
    rs.fence_round = 7;

    RefPayload all_1;
    all_1.tree_id = (UInt128(0x11) << 64) | UInt128(0x22);
    all_1.tree_size = 4096;
    all_1.mutable_files["txn_version.txt"] = "100";
    all_1.mutable_files["metadata_version.txt"] = "3";
    rs.refs["all_1_1_0"] = all_1;

    RefPayload all_2;
    all_2.tree_id = (UInt128(0x33) << 64) | UInt128(0x44);
    all_2.tree_size = 8192;
    rs.refs["all_2_2_0"] = all_2;

    rs.journal.push_back({JournalRecord::Op::Add, "all_1_1_0", all_1.tree_id, 40, {}});
    rs.journal.push_back({JournalRecord::Op::Add, "all_2_2_0", all_2.tree_id, 41, {}});
    rs.journal.push_back({JournalRecord::Op::Remove, "all_0_0_0", (UInt128(0x99) << 64), 42, {}});

    const String encoded = encodeRootShard(rs);
    const RootShard d = decodeRootShard(encoded);

    EXPECT_EQ(d.shard_version, 42u);
    EXPECT_EQ(d.fence_round, 7u);

    ASSERT_EQ(d.refs.size(), 2u);
    ASSERT_TRUE(d.refs.count("all_1_1_0"));
    EXPECT_EQ(d.refs.at("all_1_1_0").tree_id, all_1.tree_id);
    EXPECT_EQ(d.refs.at("all_1_1_0").tree_size, 4096u);
    EXPECT_EQ(d.refs.at("all_1_1_0").mutable_files.size(), 2u);
    EXPECT_EQ(d.refs.at("all_1_1_0").mutable_files.at("txn_version.txt"), "100");
    EXPECT_EQ(d.refs.at("all_1_1_0").mutable_files.at("metadata_version.txt"), "3");
    EXPECT_EQ(d.refs.at("all_2_2_0").mutable_files.size(), 0u);

    ASSERT_EQ(d.journal.size(), 3u);
    EXPECT_EQ(d.journal[0].op, JournalRecord::Op::Add);
    EXPECT_EQ(d.journal[0].ref_name, "all_1_1_0");
    EXPECT_EQ(d.journal[0].at_version, 40u);
    EXPECT_EQ(d.journal[2].op, JournalRecord::Op::Remove);
    EXPECT_EQ(d.journal[2].ref_name, "all_0_0_0");
    EXPECT_EQ(d.journal[2].at_version, 42u);
}

TEST(CasRootShardCodec, EmptyManifestRoundTrips)
{
    RootShard rs;  /// {0,0,{},{}}
    const String encoded = encodeRootShard(rs);
    const RootShard d = decodeRootShard(encoded);
    EXPECT_EQ(d.shard_version, 0u);
    EXPECT_EQ(d.fence_round, 0u);
    EXPECT_TRUE(d.refs.empty());
    EXPECT_TRUE(d.journal.empty());
}

TEST(CasRootShardCodec, RefsCanonicalOrderRegardlessOfInsertion)
{
    /// std::map already keeps refs name-sorted, but verify two manifests built in different insertion
    /// order encode byte-identically.
    RootShard a;
    a.refs["zzz"] = RefPayload{UInt128(0x1), 1, {}};
    a.refs["aaa"] = RefPayload{UInt128(0x2), 2, {}};

    RootShard b;
    b.refs["aaa"] = RefPayload{UInt128(0x2), 2, {}};
    b.refs["zzz"] = RefPayload{UInt128(0x1), 1, {}};

    EXPECT_EQ(encodeRootShard(a), encodeRootShard(b));
}

TEST(CasRootShardCodec, JournalAtVersionMonotonicityIsEnforced)
{
    /// The GC fold replays the journal in order under a cursor bound; an out-of-order at_version
    /// would fold silently in vector order (corruption-induced UNDER-count = a wrong delete later)
    /// and a record claiming a version beyond shard_version would be silently skipped by the cursor
    /// window. Both are corruption - fail closed at DECODE. The encoder does NOT validate, so we
    /// encode a deliberately-bad manifest and confirm the decoder rejects it.

    /// Descending at_version.
    {
        RootShard rs;
        rs.shard_version = 5;
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 3, {}});
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 2, {}});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// at_version beyond shard_version.
    {
        RootShard rs;
        rs.shard_version = 1;
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 2, {}});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// EQUAL at_versions are legal (non-decreasing, not strict): dropNamespace appends N Removes at
    /// the same committed version.
    {
        RootShard rs;
        rs.shard_version = 2;
        rs.journal.push_back({JournalRecord::Op::Remove, "a", UInt128(1), 2, {}});
        rs.journal.push_back({JournalRecord::Op::Remove, "b", UInt128(1), 2, {}});
        const RootShard d = decodeRootShard(encodeRootShard(rs));
        ASSERT_EQ(d.journal.size(), 2u);
        EXPECT_EQ(d.journal[0].at_version, 2u);
        EXPECT_EQ(d.journal[1].at_version, 2u);
    }
}

/// ---------- CasRootShardCodec protobuf encoding (B164a) ----------

TEST(CasRootShardCodec, ProtobufEncodingIsBinaryAndRoundTrips)
{
    RootShard rs;
    rs.shard_version = 9;
    rs.fence_round = 3;
    RefPayload p;
    p.tree_id = (UInt128(0xab) << 64) | UInt128(0xcd);
    p.tree_size = 1142;
    p.mutable_files[".ca_mtime"] = "1781588451";
    rs.refs["part_a"] = p;
    rs.journal.push_back({JournalRecord::Op::Add, "part_a", p.tree_id, 8, {}});
    rs.journal.push_back({JournalRecord::Op::Remove, "part_a", p.tree_id, 9, {}});

    const String encoded = encodeRootShard(rs);
    ASSERT_FALSE(encoded.empty());
    EXPECT_NE(encoded.front(), '{');   /// protobuf, not JSON

    const RootShard d = decodeRootShard(encoded);
    EXPECT_EQ(d.shard_version, 9u);
    EXPECT_EQ(d.fence_round, 3u);
    ASSERT_EQ(d.refs.size(), 1u);
    EXPECT_EQ(d.refs.at("part_a").tree_id, p.tree_id);
    EXPECT_EQ(d.refs.at("part_a").tree_size, 1142u);
    EXPECT_EQ(d.refs.at("part_a").mutable_files.at(".ca_mtime"), "1781588451");
    ASSERT_EQ(d.journal.size(), 2u);
    EXPECT_EQ(d.journal[0].op, JournalRecord::Op::Add);
    EXPECT_EQ(d.journal[1].op, JournalRecord::Op::Remove);
    EXPECT_EQ(d.journal[1].at_version, 9u);
}

TEST(CasRootShardCodec, ProtobufEncodingIsDeterministic)
{
    /// Deterministic serialization (sorted map<> entries) keeps the bytes stable across encodes -
    /// golden-test friendly. Includes refs (a map) and a journal (repeated, insertion order).
    RootShard rs;
    rs.shard_version = 5;
    rs.refs["zzz"] = RefPayload{UInt128(0x1), 1, {{"b", "2"}, {"a", "1"}}};
    rs.refs["aaa"] = RefPayload{UInt128(0x2), 2, {}};
    rs.journal.push_back({JournalRecord::Op::Add, "zzz", UInt128(0x1), 5, {}});
    EXPECT_EQ(encodeRootShard(rs), encodeRootShard(rs));
}

TEST(CasRootShardCodec, LargeJournalRoundTrips)
{
    RootShard rs;
    rs.shard_version = 5000;
    for (uint64_t v = 0; v < 2430; ++v)
        rs.journal.push_back({JournalRecord::Op::Add, "p" + std::to_string(v % 38), UInt128(v), v, {}});
    const RootShard d = decodeRootShard(encodeRootShard(rs));
    ASSERT_EQ(d.journal.size(), 2430u);
    EXPECT_EQ(d.journal[2429].at_version, 2429u);
    EXPECT_EQ(d.journal[0].ref_name, "p0");
}

TEST(CasRootShardCodec, FailClosedOnGarbageBytes)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootShard(String("")); });               /// empty
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootShard(String("\xff\xff\xff\xff")); }); /// not protobuf
    /// Bytes that protobuf-parse but carry no (or zero) codec_version are not a conforming manifest.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootShard(String("\x10\x01", 2)); });      /// only shard_version, codec_version=0
}

TEST(CasRootShardCodec, ProtobufFutureCodecVersionThrowsUnknownFormatVersion)
{
    /// A protobuf manifest with codec_version=2 (field 1, varint) from a newer writer must fail
    /// closed, never be mis-read. Bytes: tag(field 1, varint)=0x08, value=2 (> current CODEC_VERSION=1).
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [] { decodeRootShard(String("\x08\x02", 2)); });
}

/// B199-S2: inline closure round-trip (nested staged entries) on the precommit `Add` journal record.
TEST(CasRootShardCodec, JournalAddClosureRoundTrips)
{
    RootShard in;
    in.shard_version = 7;

    JournalRecord rec;
    rec.op = JournalRecord::Op::Add;
    rec.ref_name = "4815";
    rec.tree_id = UInt128(0x54);   /// 'T'
    rec.at_version = 1;

    /// nested closure: tree T -> {Blob B1, Subtree S}; S -> {Blob B2}
    ClosureNode nodeT;
    nodeT.tree_hash = UInt128(0x54);   /// 'T'
    {
        TreeEntry e1;
        e1.placement = Placement::Blob;
        e1.file_hash = UInt128(0x4231);   /// 'B1'
        e1.file_size = 52;
        nodeT.entries.push_back(e1);

        TreeEntry e2;
        e2.placement = Placement::Subtree;
        e2.file_hash = UInt128(0x53);     /// 'S'
        e2.file_size = 10;
        nodeT.entries.push_back(e2);
    }
    rec.closure.push_back(nodeT);

    ClosureNode nodeS;
    nodeS.tree_hash = UInt128(0x53);   /// 'S'
    {
        TreeEntry e;
        e.placement = Placement::Blob;
        e.file_hash = UInt128(0x4232);   /// 'B2'
        e.file_size = 3;
        nodeS.entries.push_back(e);
    }
    rec.closure.push_back(nodeS);

    in.journal.push_back(rec);

    const RootShard out = decodeRootShard(encodeRootShard(in));
    ASSERT_EQ(out.journal.size(), 1u);
    EXPECT_EQ(out.journal[0].closure, in.journal[0].closure);
}

/// ===================================================================================
/// CasWatermark (Task 4)
/// ===================================================================================

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>

TEST(CasWatermark, RoundTrips)
{
    const ServerWatermark w{.server_id = UInt128(0xABCD), .epoch = 7, .min_active = 42, .seq = 3};
    const String body = encodeServerWatermark(w);
    const ServerWatermark r = decodeServerWatermark(body);
    ASSERT_EQ(r.server_id, w.server_id);
    ASSERT_EQ(r.epoch, w.epoch);
    ASSERT_EQ(r.min_active, w.min_active);
    ASSERT_EQ(r.seq, w.seq);
}

/// ===================================================================================
/// CasLayout (Task 5)
/// ===================================================================================

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

TEST(CasLayout, ServerWatermarkKey)
{
    Layout layout("pool");
    const String hex(32, 'a');   // 32-char u128 hex
    ASSERT_EQ(layout.serverWatermarkKey(hex), "pool/roots/" + hex + "/_watermark");
}

/// ---------- envelope fixed-length header padding (pad_to_header_len) ----------

TEST(CasEnvelope, EnvelopeHeaderPaddingReachesTargetLen)
{
    EnvelopeHeader h = makeBlobHeader();
    h.pad_to_header_len = 256;

    String bytes = encodeEnvelopeHeader(h);
    EXPECT_EQ(h.header_len, 256u);
    EXPECT_EQ(bytes.size(), 256u);

    /// The padded header round-trips: the decoder skips the zero-type pad TLV and validates the size
    /// arithmetic header_len + logical_size == object_size.
    const uint64_t object_size = 256 + h.logical_size;
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, object_size, ObjectKind::Blob);
    EXPECT_EQ(d.header_len, 256u);
    EXPECT_EQ(d.logical_hash, h.logical_hash);
    EXPECT_EQ(d.logical_size, h.logical_size);

    /// Not 8-aligned ⇒ BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, []
    {
        EnvelopeHeader bad = makeBlobHeader();
        bad.pad_to_header_len = 100;
        encodeEnvelopeHeader(bad);
    });

    /// Below the natural header length (94+2=96 aligned) ⇒ BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, []
    {
        EnvelopeHeader bad = makeBlobHeader();
        bad.pad_to_header_len = 64;
        encodeEnvelopeHeader(bad);
    });
}

/// ===================================================================================
/// On-disk byte-order goldens (R9): the two non-hex UInt128 wire forms are FROZEN. These pin the
/// EXACT bytes a fixed input encodes to, so routing the codecs through the named `writeU128LE` /
/// `u128ToBytesBE` helpers cannot move a single byte. The constants were captured from the encoders
/// BEFORE the R9 refactor.
/// ===================================================================================

namespace
{
String toHexBytes(const String & s)
{
    String r;
    for (unsigned char c : s)
    {
        static const char * d = "0123456789abcdef";
        r += d[c >> 4];
        r += d[c & 0xf];
    }
    return r;
}
}

/// LE binary form: the envelope header. `logical_hash` (offset [22,38)) appears little-endian in the
/// golden — e.g. 0x0123...3210 serializes as bytes 10 32 54 ... — pinning the LE order.
/// Layout: CABL magic[4] writer_version[2] min_reader_version[2] hash_algo[1] flags[1] header_len[4]
///         logical_size[8] logical_hash[16] domain_id[16] incarnation_tag[16] build_id[16]
///         header_hash[8] align_pad[2] = 96 bytes total (94-byte core + 2 zero alignment bytes).
TEST(CasByteOrderGolden, EnvelopeLittleEndian)
{
    EnvelopeHeader h = makeBlobHeader();
    const String encoded = encodeEnvelopeHeader(h);
    static constexpr std::string_view golden =
        "4341424c01000100010060000000e8030000000000001032547698badcfeefcd"
        "ab89674523014200000000000000000000000000000099000000000000000000"
        "00000000000007000000000000000000000000000000c037e11e6ef26cce0000";
    EXPECT_EQ(toHexBytes(encoded), golden);
}

/// BE 16-byte form: the root-shard manifest's protobuf `tree_id` bytes. The id (0xab<<64)|0xcd
/// appears big-endian in the golden — bytes ...00 ab ...00 cd — pinning the BE order.
TEST(CasByteOrderGolden, RootShardBigEndian)
{
    RootShard rs;
    rs.shard_version = 9;
    rs.fence_round = 3;
    RefPayload p;
    p.tree_id = (UInt128(0xab) << 64) | UInt128(0xcd);
    p.tree_size = 1142;
    p.mutable_files[".ca_mtime"] = "1781588451";
    rs.refs["part_a"] = p;
    rs.journal.push_back({JournalRecord::Op::Add, "part_a", p.tree_id, 8, {}});
    const String encoded = encodeRootShard(rs);
    static constexpr std::string_view golden =
        "08011009180322380a06706172745f61122e0a1000000000000000ab00000000"
        "000000cd10f6081a170a092e63615f6d74696d65120a31373831353838343531"
        "2a1e08011206706172745f611a1000000000000000ab00000000000000cd2008";
    EXPECT_EQ(toHexBytes(encoded), golden);
}
