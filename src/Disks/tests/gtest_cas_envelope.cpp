#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>
#include <string>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB;
using namespace DB::Cas;

namespace
{

/// Builds a full object (header + payload) for a given kind and payload, returns the bytes.
/// (The former `logical_size`/`logical_hash` core fields were dropped 2026-07-11; identity is carried
/// by the content key, not the envelope, and the header is buildable before the payload is known.)
std::string buildObject(ObjectKind kind, const std::string & payload,
                        std::optional<uint32_t> pad = std::nullopt)
{
    EnvelopeHeader h;
    h.kind = kind;
    h.hash_algo = 1;
    h.domain_id = 0x11;
    h.incarnation_tag = 0x22;
    h.build_id = 0x33;
    if (pad)
        h.pad_to_header_len = *pad;
    std::string out = encodeEnvelopeHeader(h);
    out += payload;
    return out;
}

}

TEST(CasEnvelope, BlobRoundTrip)
{
    const std::string payload = "hello payload";
    const std::string obj = buildObject(ObjectKind::Blob, payload);
    const EnvelopeHeader h = decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
    EXPECT_EQ(h.kind, ObjectKind::Blob);
    EXPECT_EQ(h.writer_version, G_BUILD);
    EXPECT_EQ(h.compatibility_version, G_BUILD);
    /// payload starts right after header (derived downstream as object_size - header_len)
    EXPECT_EQ(obj.substr(payloadOffset(h)), payload);
}

TEST(CasEnvelope, MagicEncodesKind)
{
    const std::string blob = buildObject(ObjectKind::Blob, "p");
    EXPECT_EQ(blob.substr(0, 4), "CABL");
}

TEST(CasEnvelope, BadMagicThrows)
{
    std::string obj = buildObject(ObjectKind::Blob, "p");
    obj[0] = 'X';   // corrupt the magic
    try
    {
        decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasEnvelope, FutureCompatibilityVersionFailsClosed)
{
    /// Hand-patch compatibility_version at [6,8) (same wire position, formerly named min_reader_version)
    /// to a future value (G_BUILD+1). The gate is checked BEFORE the header_hash, so decode
    /// deterministically throws UNKNOWN_FORMAT_VERSION (the hash mismatch from the un-recomputed
    /// patch is never reached).
    std::string obj = buildObject(ObjectKind::Blob, "p");
    const uint16_t future_version = static_cast<uint16_t>(G_BUILD + 1);
    obj[6] = static_cast<char>(future_version & 0xFF);
    obj[7] = static_cast<char>((future_version >> 8) & 0xFF);   // compatibility_version = G_BUILD+1 (LE)
    try
    {
        decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
        FAIL() << "expected a fail-closed throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
    }
}

TEST(CasEnvelope, IncarnationZoneDoesNotAffectPayload)
{
    /// Two objects with the SAME payload but DIFFERENT incarnation_tag/build_id encode to different
    /// header bytes, yet both carry the same payload at the same fixed offset — the incarnation zone
    /// (which now carries the WHOLE header, since `logical_hash`/`logical_size` were dropped) never
    /// affects the payload. Identity is the content key `cityHash128(payload)`, not any header field.
    const std::string payload = "same content";
    EnvelopeHeader a; a.kind = ObjectKind::Blob; a.hash_algo = 1;
    a.domain_id = 0x1; a.incarnation_tag = 0xAAAA; a.build_id = 0xBBBB;
    EnvelopeHeader b = a; b.incarnation_tag = 0xCCCC; b.build_id = 0xDDDD;
    const std::string ha = encodeEnvelopeHeader(a);
    const std::string hb = encodeEnvelopeHeader(b);
    EXPECT_NE(ha, hb);   // headers differ (incarnation zone)
    const EnvelopeHeader da = decodeEnvelopeHeader(ha + payload, ha.size() + payload.size(), ObjectKind::Blob);
    const EnvelopeHeader db = decodeEnvelopeHeader(hb + payload, hb.size() + payload.size(), ObjectKind::Blob);
    EXPECT_EQ((ha + payload).substr(payloadOffset(da)), payload);
    EXPECT_EQ((hb + payload).substr(payloadOffset(db)), payload);
}
