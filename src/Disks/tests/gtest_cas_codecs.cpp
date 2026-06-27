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

TEST(CasEnvelope, FutureCompatibilityVersionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// compatibility_version is at [6,8) LE (same wire position, formerly named min_reader_version).
    /// Patch to G_BUILD+1 to drive the fail-closed path.
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
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_format.pb.h>

namespace Proto = ::clickhouse::cas::format;

namespace
{
    OwnerBinding committedBinding(const String & ref, const ManifestRef & mr)
    {
        return OwnerBinding{OwnerKind::Committed, ref, UInt128(0), mr};
    }
    OwnerBinding precommitBinding(const String & final_ref, UInt128 build_id, const ManifestRef & mr)
    {
        return OwnerBinding{OwnerKind::Precommit, final_ref, build_id, mr};
    }
}

TEST(CasRootShardCodec, RoundTripInterleavedOwnerEvents)
{
    RootShard r;
    r.shard_version = 12;
    r.fence_round = 3;
    const ManifestRef mr{"srv-a:42", 1042, UInt128(0xABCDEF)};   /// the part this build owns
    const ManifestRef mr_other{"srv-a:42", 1043, UInt128(0x112233)};
    const UInt128 build_id(0x5678);
    r.refs["all_1_1_0"] = RootRef{"all_1_1_0", mr, {{"txn_version.txt", "5"}}, 1700000000000ULL};

    /// ONE ordered journal with INTERLEAVED kinds: create-precommit, publish-committed (a different
    /// ref), promote (precommit->committed, SAME manifest_ref), then drop. transition_version increases.
    r.journal.push_back(RootOwnerEvent{8, std::nullopt, precommitBinding("all_1_1_0", build_id, mr)});
    r.journal.push_back(RootOwnerEvent{9, std::nullopt, committedBinding("other_2_2_0", mr_other)});
    r.journal.push_back(RootOwnerEvent{10, precommitBinding("all_1_1_0", build_id, mr), committedBinding("all_1_1_0", mr)});
    r.journal.push_back(RootOwnerEvent{11, committedBinding("other_2_2_0", mr_other), std::nullopt});

    const String bytes = encodeRootShard(r);
    const RootShard back = decodeRootShard(bytes);
    EXPECT_EQ(back, r);
    /// Byte-equality: deterministic encoder => re-encode is byte-identical (resume/adoption).
    EXPECT_EQ(encodeRootShard(back), bytes);

    /// transition_version order is preserved end-to-end (the single stream is folded in this order).
    ASSERT_EQ(back.journal.size(), 4u);
    for (size_t i = 1; i < back.journal.size(); ++i)
        EXPECT_LT(back.journal[i - 1].transition_version, back.journal[i].transition_version);

    /// The promote event is a pure owner move: SAME manifest_ref in old (Precommit) and new (Committed).
    const RootOwnerEvent & promote = back.journal[2];
    ASSERT_TRUE(promote.old_binding && promote.new_binding);
    EXPECT_EQ(promote.old_binding->owner_kind, OwnerKind::Precommit);
    EXPECT_EQ(promote.new_binding->owner_kind, OwnerKind::Committed);
    EXPECT_EQ(promote.old_binding->manifest_ref, promote.new_binding->manifest_ref);

    /// RootRef payload round-trips.
    ASSERT_TRUE(back.refs.contains("all_1_1_0"));
    EXPECT_EQ(back.refs.at("all_1_1_0").manifest_ref, mr);
    EXPECT_EQ(back.refs.at("all_1_1_0").mutable_files.at("txn_version.txt"), "5");
    EXPECT_EQ(back.refs.at("all_1_1_0").published_at_ms, 1700000000000ULL);
}

TEST(CasRootShardCodec, OptionalBindingAbsenceIsDistinguished)
{
    RootShard r;
    r.shard_version = 1;
    /// A drop event: old committed binding present, new absent.
    r.journal.push_back(RootOwnerEvent{1, committedBinding("p", ManifestRef{"w", 1, UInt128(9)}), std::nullopt});
    const RootShard back = decodeRootShard(encodeRootShard(r));
    EXPECT_TRUE(back.journal.at(0).old_binding.has_value());
    EXPECT_FALSE(back.journal.at(0).new_binding.has_value());
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
    const ManifestRef m1{"w", 1, UInt128(0x1)};
    const ManifestRef m2{"w", 2, UInt128(0x2)};
    RootShard a;
    a.refs["zzz"] = RootRef{"zzz", m1, {}, 0};
    a.refs["aaa"] = RootRef{"aaa", m2, {}, 0};

    RootShard b;
    b.refs["aaa"] = RootRef{"aaa", m2, {}, 0};
    b.refs["zzz"] = RootRef{"zzz", m1, {}, 0};

    EXPECT_EQ(encodeRootShard(a), encodeRootShard(b));
}

TEST(CasRootShardCodec, JournalTransitionVersionMonotonicityIsEnforced)
{
    /// The GC fold replays the journal in transition_version order under a cursor bound; an out-of-order
    /// transition_version would fold silently in vector order (a mis-count = a wrong delete later) and a
    /// record claiming a version beyond shard_version would be silently skipped by the cursor window.
    /// Both are corruption - fail closed at DECODE.
    const ManifestRef mr{"w", 1, UInt128(1)};

    /// Descending transition_version.
    {
        RootShard rs;
        rs.shard_version = 5;
        rs.journal.push_back(RootOwnerEvent{3, std::nullopt, committedBinding("r", mr)});
        rs.journal.push_back(RootOwnerEvent{2, std::nullopt, committedBinding("r", mr)});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// transition_version beyond shard_version.
    {
        RootShard rs;
        rs.shard_version = 1;
        rs.journal.push_back(RootOwnerEvent{2, std::nullopt, committedBinding("r", mr)});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// EQUAL transition_versions are legal (non-decreasing, not strict): dropNamespace appends N events
    /// at the same committed version.
    {
        RootShard rs;
        rs.shard_version = 2;
        rs.journal.push_back(RootOwnerEvent{2, committedBinding("a", mr), std::nullopt});
        rs.journal.push_back(RootOwnerEvent{2, committedBinding("b", mr), std::nullopt});
        const RootShard d = decodeRootShard(encodeRootShard(rs));
        ASSERT_EQ(d.journal.size(), 2u);
        EXPECT_EQ(d.journal[0].transition_version, 2u);
        EXPECT_EQ(d.journal[1].transition_version, 2u);
    }
    /// An event with NEITHER binding is corruption (folds to a no-op).
    {
        RootShard rs;
        rs.shard_version = 2;
        rs.journal.push_back(RootOwnerEvent{1, std::nullopt, std::nullopt});
        const String bytes = encodeRootShard(rs);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
}

TEST(CasRootShardCodec, ProtobufEncodingIsDeterministic)
{
    RootShard rs;
    rs.shard_version = 5;
    rs.refs["zzz"] = RootRef{"zzz", ManifestRef{"w", 1, UInt128(0x1)}, {{"b", "2"}, {"a", "1"}}, 0};
    rs.refs["aaa"] = RootRef{"aaa", ManifestRef{"w", 2, UInt128(0x2)}, {}, 0};
    rs.journal.push_back(RootOwnerEvent{5, std::nullopt, committedBinding("zzz", ManifestRef{"w", 1, UInt128(0x1)})});
    EXPECT_EQ(encodeRootShard(rs), encodeRootShard(rs));
}

TEST(CasRootShardCodec, LargeJournalRoundTrips)
{
    RootShard rs;
    rs.shard_version = 5000;
    for (uint64_t v = 0; v < 2430; ++v)
        rs.journal.push_back(RootOwnerEvent{v, std::nullopt,
            committedBinding("p" + std::to_string(v % 38), ManifestRef{"w", v, UInt128(v)})});
    const RootShard d = decodeRootShard(encodeRootShard(rs));
    ASSERT_EQ(d.journal.size(), 2430u);
    EXPECT_EQ(d.journal[2429].transition_version, 2429u);
    EXPECT_EQ(d.journal[0].new_binding->ref_name, "p0");
}

TEST(CasRootShardCodec, FailClosedOnGarbageBytes)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootShard(String("")); });               /// empty
    /// A valid protobuf with a wrong magic in CasHeader.magic => CORRUPTED_DATA.
    {
        Proto::RootShardManifest msg;
        auto * hdr = msg.mutable_header();
        hdr->set_magic(magicFor(FormatId::PoolMeta));   /// CAPM != CARS
        hdr->set_compatibility_version(currentCompatibilityVersion());
        std::string bytes;
        msg.SerializeToString(&bytes);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRootShard(bytes); });
    }
    /// Pure garbage bytes that fail protobuf parse => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeRootShard(String("\xff\xff\xff\xff")); });
}

TEST(CasRootShardCodec, CasHeaderFutureCompatibilityVersionThrowsUnknownFormatVersion)
{
    Proto::RootShardManifest msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Manifest));
    hdr->set_compatibility_version(G_BUILD + 1);   /// future object
    std::string bytes;
    msg.SerializeToString(&bytes);
    expectThrowsCode(
        DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeRootShard(bytes); });
}

TEST(CasRootShardCodec, CasHeaderRoundTrips)
{
    RootShard rs;
    rs.shard_version = 3;
    const String encoded = encodeRootShard(rs);
    Proto::RootShardManifest msg;
    ASSERT_TRUE(msg.ParseFromString(encoded));
    EXPECT_EQ(msg.header().magic(), magicFor(FormatId::Manifest));
    EXPECT_EQ(msg.header().writer_version(), currentWriterVersion());
    EXPECT_EQ(msg.header().compatibility_version(), currentCompatibilityVersion());
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
/// Layout: CABL magic[4] writer_version[2] compatibility_version[2] hash_algo[1] flags[1] header_len[4]
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
/// appears big-endian in the encoding — bytes ...00 ab ...00 cd — pinning the BE order.
/// With the converged header model the manifest is pure protobuf (no binary prefix); the CasHeader
/// is field 1, so the wire starts with field-1 tag (0x0A) then the header sub-message bytes.
TEST(CasByteOrderGolden, RootShardBigEndian)
{
    RootShard rs;
    rs.shard_version = 9;
    rs.fence_round = 3;
    const ManifestRef mr{"w", 7, (UInt128(0xab) << 64) | UInt128(0xcd)};
    rs.refs["part_a"] = RootRef{"part_a", mr, {}, 1781588451000ULL};
    rs.journal.push_back(RootOwnerEvent{8, std::nullopt,
        OwnerBinding{OwnerKind::Committed, "part_a", UInt128(0), mr}});
    const String encoded = encodeRootShard(rs);
    ASSERT_FALSE(encoded.empty());
    /// Pure protobuf: the first byte is a field tag, not the old ASCII magic.
    /// (Field 1 = CasHeader, wire type 2 = LEN => tag byte 0x0A.)
    EXPECT_EQ(static_cast<uint8_t>(encoded[0]), 0x0Au);
    /// Decode round-trips the BE bytes correctly (the full round-trip is the functional pin).
    const RootShard d = decodeRootShard(encoded);
    EXPECT_EQ(d.refs.at("part_a").manifest_ref, mr);
    EXPECT_EQ(d.refs.at("part_a").published_at_ms, 1781588451000ULL);
    /// Verify the encoded bytes contain manifest_instance_id in big-endian:
    /// (0xab<<64)|0xcd -> 16 bytes: 00000000000000ab 00000000000000cd.
    const String encoded_hex = toHexBytes(encoded);
    EXPECT_NE(encoded_hex.find("00000000000000ab00000000000000cd"), std::string::npos)
        << "manifest_instance_id not found big-endian in protobuf encoding (hex): " << encoded_hex;
}
