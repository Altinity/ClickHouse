#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Common/Exception.h>

using namespace DB::Cas;

TEST(CasSourceEdge, IdIsDeterministicAndPathSensitive)
{
    const ManifestId id{RootNamespace{"00/aa@cas@"}, ManifestRef{.writer_epoch = 1, .build_sequence = 15, .manifest_ordinal = 1}};
    EXPECT_EQ(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "a.bin"));           // deterministic
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "b.bin"));           // path-sensitive
    const ManifestId id2{id.root_namespace, ManifestRef{.writer_epoch = 1, .build_sequence = 31, .manifest_ordinal = 1}};
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id2, "a.bin"));          // ref-sensitive
}

TEST(CasSourceEdge, RunKeyRoundTripsAndOrdersByBlobThenSource)
{
    const BlobDigest b1 = BlobDigest::fromU128(UInt128(1));
    const BlobDigest b2 = BlobDigest::fromU128(UInt128(2));
    const UInt128 s1(10), s2(20);
    const SourceEdgeKeyCodec codec(16);

    BlobDigest gb;
    UInt128 gs;
    codec.parse(codec.key(b1, s1), gb, gs);
    EXPECT_EQ(gb, b1); EXPECT_EQ(gs, s1);
    EXPECT_LT(codec.key(b1, s2), codec.key(b2, s1));   // blob_hash is the primary sort
    EXPECT_LT(codec.key(b1, s1), codec.key(b1, s2));   // source_id is the secondary sort
}

TEST(CasSourceEdge, KeyCodecSchema2RoundTripAndRejectsBadSizes)
{
    /// schema-2 (32-byte digest) round trip: key is 48 bytes, parse recovers the full digest.
    BlobDigest d32{};
    for (size_t i = 0; i < d32.bytes.size(); ++i)
        d32.bytes[i] = static_cast<uint8_t>(i + 1);
    const UInt128 sid(0xABCDu);
    const SourceEdgeKeyCodec codec32(32);
    EXPECT_EQ(codec32.keySchema(), kSourceEdgeKeySchemaSha256);
    const String key32 = codec32.key(d32, sid);
    ASSERT_EQ(key32.size(), 48u);
    BlobDigest gb;
    UInt128 gs;
    codec32.parse(key32, gb, gs);
    EXPECT_EQ(gb, d32);
    EXPECT_EQ(gs, sid);

    /// schema-1 (16-byte digest): key is 32 bytes, byte-identical to the old u128ToBytesBE-based key.
    const SourceEdgeKeyCodec codec16(16);
    EXPECT_EQ(codec16.keySchema(), kSourceEdgeKeySchema128);
    const BlobDigest d16 = BlobDigest::fromU128(UInt128(0x0102030405060708ULL));
    const String key16 = codec16.key(d16, sid);
    ASSERT_EQ(key16.size(), 32u);
    EXPECT_EQ(key16, u128ToBytesBE(d16.toU128()) + u128ToBytesBE(sid));

    /// Fail-close: a wrong-size key throws CORRUPTED_DATA, never a silent false.
    EXPECT_THROW(codec16.parse(key32, gb, gs), DB::Exception);
    EXPECT_THROW(codec16.parse(String(20, '\0'), gb, gs), DB::Exception);

    /// Unknown schema/digest length -> NOT_IMPLEMENTED (fail closed).
    EXPECT_THROW(SourceEdgeKeyCodec(20), DB::Exception);
    EXPECT_THROW(sourceEdgeDigestLen(3), DB::Exception);
    EXPECT_THROW(SourceEdgeKeyCodec::forSchema(3), DB::Exception);
}

TEST(CasSourceEdge, KeyOrderSentinelFirstAtLen32)
{
    /// At 32-byte width, the sentinel (source_id 0) sorts before any nonzero source_id for the same
    /// digest, and digest magnitude order is preserved (big-endian raw-byte lexicographic order ==
    /// numeric magnitude order for a width-homogeneous run — the consult's load-bearing fact).
    const SourceEdgeKeyCodec codec(32);
    BlobDigest d{};
    d.bytes[0] = 0x10;
    EXPECT_LT(codec.key(d, UInt128(0)), codec.key(d, UInt128(1)));

    BlobDigest d_small{};
    d_small.bytes[0] = 0x01;
    BlobDigest d_large{};
    d_large.bytes[0] = 0x02;
    EXPECT_LT(codec.key(d_small, UInt128(5)), codec.key(d_large, UInt128(5)));
}
