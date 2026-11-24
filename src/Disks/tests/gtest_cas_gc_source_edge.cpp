#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/Exception.h>

using namespace DB::Cas;

TEST(CASSourceEdge, IdIsDeterministicAndPathSensitive)
{
    const ManifestId id{RootNamespace{"00/aa@cas@"}, ManifestRef{.writer_epoch = 1, .build_sequence = 15, .manifest_ordinal = 1}};
    EXPECT_EQ(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "a.bin"));           // deterministic
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "b.bin"));           // path-sensitive
    const ManifestId id2{id.root_namespace, ManifestRef{.writer_epoch = 1, .build_sequence = 31, .manifest_ordinal = 1}};
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id2, "a.bin"));          // ref-sensitive
}

TEST(CASSourceEdge, RunKeyOrdersByBlobThenSource)
{
    const BlobRef b1{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(1))};
    const BlobRef b2{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(2))};
    const UInt128 s1(10);
    const UInt128 s2(20);

    EXPECT_LT((SourceEdgeKey{b1, s2}), (SourceEdgeKey{b2, s1}));   // ref is the primary sort
    EXPECT_LT((SourceEdgeKey{b1, s1}), (SourceEdgeKey{b1, s2}));   // source_id is the secondary sort
}

TEST(CASSourceEdge, KeyOrderSentinelFirstAtLen32)
{
    /// At sha256 width, the sentinel (source_id 0) sorts before any nonzero source_id for the same
    /// digest, and digest magnitude order is preserved.
    BlobDigest d{};
    d.bytes[0] = 0x10;
    const BlobRef ref{BlobHashAlgo::Sha256, d};
    EXPECT_LT((SourceEdgeKey{ref, UInt128(0)}), (SourceEdgeKey{ref, UInt128(1)}));

    BlobDigest d_small{};
    d_small.bytes[0] = 0x01;
    BlobDigest d_large{};
    d_large.bytes[0] = 0x02;
    const BlobRef ref_small{BlobHashAlgo::Sha256, d_small};
    const BlobRef ref_large{BlobHashAlgo::Sha256, d_large};
    EXPECT_LT((SourceEdgeKey{ref_small, UInt128(5)}), (SourceEdgeKey{ref_large, UInt128(5)}));
}
