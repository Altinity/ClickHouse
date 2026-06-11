#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Common/Exception.h>

using namespace DB::Cas;

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

/// ---------- CRC32C ----------

TEST(CasEnvelopeCrc, KnownVector)
{
    EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
}

TEST(CasEnvelopeCrc, EmptyIsZero)
{
    EXPECT_EQ(crc32c("", 0), 0u);
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
    String bytes = encodeEnvelopeHeader(h);
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, h.header_len + h.index_len + h.logical_size, ObjectKind::Pack);
    EXPECT_EQ(payloadOffset(d), static_cast<uint64_t>(d.header_len) + d.index_len);
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
    bytes[4] = 2;  /// format_version = 2
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
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
    /// header_len = 90 (< 96) at offset 8; recompute crc would mismatch too, but range check fires first.
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

TEST(CasEnvelope, CorruptedCrcThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[24] ^= 0xff;  /// flip a byte inside logical_hash -> crc mismatch
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, NonzeroReservedThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[92] = 1;  /// reserved0 nonzero
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, CriticalUnknownExtensionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    h.flags_has_critical_extension = true;
    h.unknown_critical_tlv = true;  /// emit an unknown TLV type with the critical flag set
    String bytes = encodeEnvelopeHeader(h);
    EXPECT_THROW(decodeEnvelopeHeader(bytes, h.header_len + h.logical_size, ObjectKind::Blob), DB::Exception);
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

