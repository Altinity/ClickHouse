#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

using namespace DB::Cas;

TEST(CasLayout, KeyShapes)
{
    Layout l{"p"};
    EXPECT_EQ(l.blobKey(BlobId{"00aabb"}), "p/blobs/00/00aabb");
    EXPECT_EQ(l.treeKey(TreeId{"ffee01"}), "p/trees/ff/ffee01");
    EXPECT_EQ(l.packKey(PackId{"0102aa"}), "p/packs/01/0102aa");
    EXPECT_EQ(l.rootShardKey("srv1", "tbl-uuid", 3), "p/roots/srv1/tbl-uuid/3");
    EXPECT_EQ(l.rootNamespacePrefix("srv1", "tbl-uuid"), "p/roots/srv1/tbl-uuid/");
    EXPECT_EQ(l.gcStateKey(), "p/gc/state");
    EXPECT_EQ(l.gcSnapKey(7, 2), "p/gc/snap/7/2");
    EXPECT_EQ(l.retiredKey(4, 9, 1), "p/gc/retired/4.9/1");
    EXPECT_EQ(l.outcomesKey(4, 9, 1), "p/gc/outcomes/4.9/1");
    EXPECT_EQ(l.checkpointKey(12), "p/gc/checkpoint/12");
    EXPECT_EQ(l.buildHeartbeatKey("deadbeef00"), "p/builds/de/deadbeef00");
    EXPECT_EQ(l.poolMetaKey(), "p/_pool_meta");
}

TEST(CasLayout, ShortIdThrows)
{
    Layout l{"p"};
    EXPECT_THROW(l.blobKey(BlobId{"x"}), DB::Exception);    // < 2 chars
    EXPECT_THROW(l.treeKey(TreeId{""}), DB::Exception);      // empty
    EXPECT_NO_THROW(l.blobKey(BlobId{"ab"}));                // exactly 2 chars is OK
}
