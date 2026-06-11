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

