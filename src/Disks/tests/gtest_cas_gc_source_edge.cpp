#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>

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
    const UInt128 b1(1), b2(2), s1(10), s2(20);
    UInt128 gb, gs;
    ASSERT_TRUE(parseSrcEdgeRunKey(srcEdgeRunKey(b1, s1), gb, gs));
    EXPECT_EQ(gb, b1); EXPECT_EQ(gs, s1);
    EXPECT_LT(srcEdgeRunKey(b1, s2), srcEdgeRunKey(b2, s1));   // blob_hash is the primary sort
    EXPECT_LT(srcEdgeRunKey(b1, s1), srcEdgeRunKey(b1, s2));   // source_id is the secondary sort
}
