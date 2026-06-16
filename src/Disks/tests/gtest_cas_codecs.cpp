#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
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
    EXPECT_GE(h.header_len, 96u);
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
    EXPECT_EQ(d.index_len, 0u);
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
    EXPECT_EQ(h.header_len, 96u);

    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob);
    EXPECT_FALSE(d.provenance.has_value());
    EXPECT_FALSE(d.intended_ref.has_value());
}

TEST(CasEnvelope, PayloadOffsetHelper)
{
    EnvelopeHeader h = makeBlobHeader();
    h.kind = ObjectKind::Pack;
    h.index_len = 32;
    h.logical_size = 32 + 968;   /// logical_size covers [header_len, EOF): index (32) + payload (968)
    String bytes = encodeEnvelopeHeader(h);
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Pack);
    EXPECT_EQ(payloadOffset(d), static_cast<uint64_t>(d.header_len) + d.index_len);
}

TEST(CasEnvelope, PackSizeArithmeticPerFrozenSpec)
{
    /// Pins the frozen-spec pack arithmetic (spec §3.1): logical_size = object_size - header_len
    /// UNIFORMLY — for a pack with an I-byte index and a P-byte payload region,
    /// logical_size = I + P and object_size = header_len + I + P (NO extra index_len term).
    constexpr uint64_t I = 64;
    constexpr uint64_t P = 5000;

    EnvelopeHeader h = makeBlobHeader();
    h.kind = ObjectKind::Pack;
    h.index_len = I;
    h.logical_size = I + P;
    String bytes = encodeEnvelopeHeader(h);

    /// The correct object size decodes...
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + I + P, ObjectKind::Pack);
    EXPECT_EQ(d.logical_size, I + P);
    EXPECT_EQ(d.index_len, I);
    EXPECT_EQ(payloadOffset(d), static_cast<uint64_t>(d.header_len) + I);

    /// ... while the rejected reading (object_size = header_len + index_len + logical_size,
    /// i.e. logical_size excluding the index) must throw.
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + I + h.logical_size, ObjectKind::Pack), DB::Exception);

    /// index_len larger than logical_size is structurally impossible and must throw.
    EnvelopeHeader bad = makeBlobHeader();
    bad.kind = ObjectKind::Pack;
    bad.index_len = 100;
    bad.logical_size = 50;
    String bad_bytes = encodeEnvelopeHeader(bad);
    EXPECT_THROW(decodeEnvelopeHeader(bad_bytes, bad.header_len + bad.logical_size, ObjectKind::Pack), DB::Exception);
}

/// ---------- validation throw-paths ----------

TEST(CasEnvelope, BadMagicThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[0] = 'X';
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, FutureVersionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[4] = 2;  /// format_version = 2 (version is validated before the header hash, so the code is pinned)
    expectThrowsCode(
        DB::ErrorCodes::NOT_IMPLEMENTED,
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
    /// header_len = 90 (< 96) at offset 8; the header hash would mismatch too, but range check fires first.
    bytes[8] = 90;
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, NonzeroIndexLenOnBlobThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[12] = 8;  /// index_len = 8 on a blob
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
    bytes[24] ^= 0xff;  /// flip a byte inside logical_hash -> header hash mismatch
    expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob); });
}

TEST(CasEnvelope, CorruptedStoredHeaderHashThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[92] ^= 0xff;  /// flip a byte inside the stored header_hash itself ([88,96))
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
        DB::ErrorCodes::NOT_IMPLEMENTED,
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

TreeEntry packSliceEntry(const String & name, UInt128 hash, uint64_t size, UInt128 pack, uint64_t off, uint64_t len)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::PackSlice;
    e.file_hash = hash;
    e.file_size = size;
    e.pack_hash = pack;
    e.pack_offset = off;
    e.pack_length = len;
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
        packSliceEntry("small.mrk", (UInt128(0xccc) << 64) | UInt128(0xddd), 64,
                       (UInt128(0xeee) << 64) | UInt128(0xfff), 12345, 64),
        subtreeEntry("nested", (UInt128(0x1) << 64) | UInt128(0x2), 200),
    };
}

}

TEST(CasTreeCodec, RoundTripAllPlacements)
{
    auto entries = sampleEntries();
    const String encoded = encodeTree(entries);
    const auto decoded = decodeTree(encoded);

    ASSERT_EQ(decoded.size(), 4u);

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
        else if (e.name == "small.mrk")
        {
            EXPECT_EQ(e.placement, Placement::PackSlice);
            EXPECT_EQ(e.pack_hash, (UInt128(0xeee) << 64) | UInt128(0xfff));
            EXPECT_EQ(e.pack_offset, 12345u);
            EXPECT_EQ(e.pack_length, 64u);
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

    /// And the tree id is stable too.
    EXPECT_EQ(treeIdFor(ea), treeIdFor(eb));
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
    /// Corrupt the placement byte of the first entry. Layout: "CATR"(4) ver(1) count(4) then
    /// per entry: name_len(2) name placement(1) ...
    /// First entry name is the smallest: "data.bin" (8 chars). placement byte at 9+2+8 = ... compute:
    /// header = 4+1+4 = 9; name_len(2) at 9; name 8 bytes at 11; placement at 19.
    encoded[19] = 99;
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
    /// A corrupted u32 inline-bytes length field must be rejected by the bounds check in
    /// `readFixedBytes` BEFORE any allocation: without that check, a single flipped length field
    /// would transiently allocate up to 4 GiB, surfacing as MEMORY_LIMIT_EXCEEDED under a memory
    /// tracker instead of the pinned CORRUPTED_DATA (and this test would take noticeably longer).
    std::vector<TreeEntry> entries = {inlineEntry("f", "hello")};
    String encoded = encodeTree(entries);

    /// CATR layout (see `encodeTree`): magic(4) ver(1) count(4) = 9 bytes of header, then for the
    /// single entry: name_len(2) name(1, "f") placement(1) file_hash(16) file_size(8); the u32
    /// little-endian inline_bytes length therefore sits at offset 9 + 2 + 1 + 1 + 16 + 8 = 37.
    constexpr size_t len_offset = 9 + 2 + 1 + 1 + 16 + 8;
    ASSERT_LE(len_offset + 4, encoded.size());
    /// Sanity: the bytes at the computed offset hold the known length of "hello" (5, LE).
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset]), 5u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 1]), 0u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 2]), 0u);
    ASSERT_EQ(static_cast<unsigned char>(encoded[len_offset + 3]), 0u);

    /// Patch the length to ~4 GiB (0xFFFFFFF0 little-endian).
    encoded[len_offset] = static_cast<char>(0xF0);
    encoded[len_offset + 1] = static_cast<char>(0xFF);
    encoded[len_offset + 2] = static_cast<char>(0xFF);
    encoded[len_offset + 3] = static_cast<char>(0xFF);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeTree(encoded); });
}

TEST(CasTreeCodec, BadMagicThrows)
{
    auto entries = sampleEntries();
    String encoded = encodeTree(entries);
    encoded[0] = 'X';
    EXPECT_THROW(decodeTree(encoded), DB::Exception);
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

    rs.journal.push_back({JournalRecord::Op::Add, "all_1_1_0", all_1.tree_id, 40});
    rs.journal.push_back({JournalRecord::Op::Add, "all_2_2_0", all_2.tree_id, 41});
    rs.journal.push_back({JournalRecord::Op::Remove, "all_0_0_0", (UInt128(0x99) << 64), 42});

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
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 3});
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 2});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// at_version beyond shard_version.
    {
        RootShard rs;
        rs.shard_version = 1;
        rs.journal.push_back({JournalRecord::Op::Add, "r", UInt128(1), 2});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// EQUAL at_versions are legal (non-decreasing, not strict): dropNamespace appends N Removes at
    /// the same committed version.
    {
        RootShard rs;
        rs.shard_version = 2;
        rs.journal.push_back({JournalRecord::Op::Remove, "a", UInt128(1), 2});
        rs.journal.push_back({JournalRecord::Op::Remove, "b", UInt128(1), 2});
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
    rs.journal.push_back({JournalRecord::Op::Add, "part_a", p.tree_id, 8});
    rs.journal.push_back({JournalRecord::Op::Remove, "part_a", p.tree_id, 9});

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
    rs.journal.push_back({JournalRecord::Op::Add, "zzz", UInt128(0x1), 5});
    EXPECT_EQ(encodeRootShard(rs), encodeRootShard(rs));
}

TEST(CasRootShardCodec, LargeJournalRoundTrips)
{
    RootShard rs;
    rs.shard_version = 5000;
    for (uint64_t v = 0; v < 2430; ++v)
        rs.journal.push_back({JournalRecord::Op::Add, "p" + std::to_string(v % 38), UInt128(v), v});
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

TEST(CasRootShardCodec, ProtobufFutureCodecVersionThrowsNotImplemented)
{
    /// A protobuf manifest with codec_version=2 (field 1, varint) from a newer writer must fail
    /// closed, never be mis-read. Bytes: tag(field 1, varint)=0x08, value=2 (> current CODEC_VERSION=1).
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [] { decodeRootShard(String("\x08\x02", 2)); });
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
    ASSERT_EQ(layout.serverWatermarkKey(hex), "pool/servers/aa/" + hex);
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

    /// Below the natural header length (96) ⇒ BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, []
    {
        EnvelopeHeader bad = makeBlobHeader();
        bad.pad_to_header_len = 64;
        encodeEnvelopeHeader(bad);
    });
}
