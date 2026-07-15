#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
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
    h.domain_id = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
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
    EXPECT_GE(h.header_len, 70u);
    EXPECT_EQ(bytes.size(), h.header_len);

    const uint64_t object_size = bytes.size();
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, object_size, ObjectKind::Blob);

    EXPECT_EQ(d.kind, ObjectKind::Blob);
    EXPECT_EQ(d.hash_algo, 1u);
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
    /// 70-byte core + 2 zero alignment bytes = 72 on disk.
    EXPECT_EQ(h.header_len, 72u);

    EnvelopeHeader d = decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob);
    EXPECT_FALSE(d.provenance.has_value());
    EXPECT_FALSE(d.intended_ref.has_value());
}

TEST(CasEnvelope, PayloadOffsetHelper)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(payloadOffset(d), static_cast<uint64_t>(d.header_len));
}

/// ---------- validation throw-paths ----------

TEST(CasEnvelope, FutureCompatibilityVersionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// compatibility_version is at [6,8) LE (same wire position, formerly named min_reader_version).
    /// Patch to G_BUILD+1 to drive the fail-closed path.
    const uint16_t future_version = static_cast<uint16_t>(G_BUILD + 1);
    bytes[6] = static_cast<char>(future_version & 0xFF);
    bytes[7] = static_cast<char>((future_version >> 8) & 0xFF);
    expectThrowsCode(
        DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob); });
}

TEST(CasEnvelope, BadHeaderLenThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    /// header_len = 64 (< 70) at offset 10; the header hash would mismatch too, but the range check
    /// (header_len < CORE_HEADER_LEN) fires first.
    bytes[10] = 64;
    EXPECT_THROW(decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob), DB::Exception);
}

TEST(CasEnvelope, CorruptedHeaderFieldFailsHeaderHash)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[20] ^= 0xff;  /// flip a byte inside domain_id [14,30) -> header hash mismatch
    expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob); });
}

TEST(CasEnvelope, CorruptedStoredHeaderHashThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    String bytes = encodeEnvelopeHeader(h);
    bytes[64] ^= 0xff;  /// flip a byte inside the stored header_hash itself ([62,70))
    expectThrowsCode(
        DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob); });
}

TEST(CasEnvelope, CriticalUnknownExtensionThrows)
{
    EnvelopeHeader h = makeBlobHeader();
    h.flags_has_critical_extension = true;
    h.unknown_critical_tlv = true;  /// emit an unknown TLV type with the critical flag set
    String bytes = encodeEnvelopeHeader(h);
    expectThrowsCode(
        DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob); });
}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

/// ---------- envelope fixed-length header padding (pad_to_header_len) ----------

TEST(CasEnvelope, EnvelopeHeaderPaddingReachesTargetLen)
{
    EnvelopeHeader h = makeBlobHeader();
    h.pad_to_header_len = 256;

    String bytes = encodeEnvelopeHeader(h);
    EXPECT_EQ(h.header_len, 256u);
    EXPECT_EQ(bytes.size(), 256u);

    /// The padded header round-trips: the decoder skips the zero-type pad TLV. (The size-arithmetic
    /// check was dropped with the `logical_size` field 2026-07-11.)
    EnvelopeHeader d = decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(d.header_len, 256u);
    EXPECT_EQ(d.domain_id, h.domain_id);

    /// Not 8-aligned ⇒ BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, []
    {
        EnvelopeHeader bad = makeBlobHeader();
        bad.pad_to_header_len = 100;
        encodeEnvelopeHeader(bad);
    });

    /// Below the natural header length (70+2=72 aligned) ⇒ BAD_ARGUMENTS.
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

/// LE binary form: the envelope header. `domain_id` (offset [14,30)) appears little-endian in the
/// golden — e.g. 0x0123...3210 serializes as bytes 10 32 54 ... — pinning the LE order. (The former
/// `logical_size`/`logical_hash` core fields were dropped 2026-07-11.)
/// Layout: CABL magic[4] writer_version[2] compatibility_version[2] hash_algo[1] flags[1] header_len[4]
///         domain_id[16] incarnation_tag[16] build_id[16] header_hash[8] align_pad[2] = 72 bytes total
///         (70-byte core + 2 zero alignment bytes).
TEST(CasByteOrderGolden, EnvelopeLittleEndian)
{
    EnvelopeHeader h = makeBlobHeader();
    const String encoded = encodeEnvelopeHeader(h);
    /// writer_version/compatibility_version bytes (offsets [4,6) and [6,8)) are `G_BUILD` as LE
    /// uint16 -- Task 12 raised `G_BUILD` 2 -> 3 for the ref snapshot+log format (an older build cannot
    /// decode the immutable _log/_snap ref objects), so both fields and the header_hash they feed into
    /// moved from this golden's previous 2-valued bytes. Pre-release (no on-disk compat to preserve):
    /// the golden is updated to the new byte-for-byte truth, not pinned to the retired generation-2 shape.
    static constexpr std::string_view golden =
        "4341424c030003000100480000001032547698badcfeefcdab8967452301"
        "99000000000000000000000000000000070000000000000000000000000000"
        "005ec9eff87ef1183500" "00";
    EXPECT_EQ(toHexBytes(encoded), golden);
}

