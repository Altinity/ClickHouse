#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

TEST(CasGcSnap, HashPrefixShard)
{
    UInt128 h = hexToU128("0123456789abcdef0123456789abcdef");
    EXPECT_EQ(hashPrefixShard(h, 1), 0u);
    EXPECT_LT(hashPrefixShard(h, 4), 4u);
    EXPECT_EQ(hashPrefixShard(h, 4), hashPrefixShard(h, 4));
}

TEST(CasGcSnap, InDegreeSetSemanticsAndCandidates)
{
    GcSnap snap;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    snap.addRootEdge("srv1/tbl/0", "part_1", T);
    snap.addRootEdge("srv1/tbl/1", "part_2", T);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T), 2u);
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B), 1u);
    EXPECT_TRUE(snap.isKnown(ObjectKind::Tree, T));
    EXPECT_TRUE(snap.isKnown(ObjectKind::Blob, B));
    snap.addRootEdge("srv1/tbl/0", "part_1", T);              /// duplicate add: no double count
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T), 2u);
    EXPECT_TRUE(snap.removeRootEdge("srv1/tbl/0", "part_1").empty());
    auto cands = snap.removeRootEdge("srv1/tbl/1", "part_2");
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].kind, ObjectKind::Tree);
    EXPECT_EQ(cands[0].hash, T);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T), 0u);
    EXPECT_TRUE(snap.isKnown(ObjectKind::Tree, T));            /// known survives zero in-degree
    /// removing a non-existent edge is a no-op (idempotent replay):
    EXPECT_TRUE(snap.removeRootEdge("srv1/tbl/9", "ghost").empty());
}

TEST(CasGcSnap, ZeroInDegreeKnownEnumerator)
{
    /// the model's GRetire guard is stateless (present ∧ everEdged ∧ InDeg=0): zeroInDegreeKnown
    /// must list ALL known zero-in-degree nodes from durable state, regardless of when they zeroed.
    GcSnap snap;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    snap.addRootEdge("s/0", "p1", T);
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    EXPECT_TRUE(snap.zeroInDegreeKnown().empty());
    snap.removeRootEdge("s/0", "p1");
    auto zs = snap.zeroInDegreeKnown();
    ASSERT_EQ(zs.size(), 1u);                                  /// T zeroed; B still pinned by T's edge
    EXPECT_EQ(zs[0].hash, T);
    /// a freshly decoded snap (no in-memory transition history) reports the same:
    auto d = decodeGcSnap(encodeGcSnap(snap));
    auto zs2 = d.zeroInDegreeKnown();
    ASSERT_EQ(zs2.size(), 1u);
    EXPECT_EQ(zs2[0].hash, T);
}

TEST(CasGcSnap, StripTreeReturnsNewlyZeroChildren)
{
    GcSnap snap;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    const UInt128 P = hexToU128("cc00000000000000000000000000000c");
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    snap.addPackEdge(T, P);
    auto freed = snap.stripTree(T);
    ASSERT_EQ(freed.size(), 2u);                               /// B and P both newly zero
    EXPECT_FALSE(snap.isExpanded(T));
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B), 0u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Pack, P), 0u);
    /// idempotent replay: a second strip is a no-op
    EXPECT_TRUE(snap.stripTree(T).empty());
}

TEST(CasGcSnap, SharedChildSurvivesOneParentStrip)
{
    /// B referenced by two trees; stripping one leaves in-degree 1 — B is NOT freed.
    GcSnap snap;
    const UInt128 T1 = hexToU128("aa00000000000000000000000000000a");
    const UInt128 T2 = hexToU128("ab00000000000000000000000000000a");
    const UInt128 B  = hexToU128("bb00000000000000000000000000000b");
    snap.markExpanded(T1); snap.addTreeEdge(T1, ObjectKind::Blob, B);
    snap.markExpanded(T2); snap.addTreeEdge(T2, ObjectKind::Blob, B);
    EXPECT_TRUE(snap.stripTree(T1).empty());
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B), 1u);
}

TEST(CasGcSnap, RootEdgeRepointIsRemoveThenAdd)
{
    /// (root_shard, part_name) names ONE tree at a time; re-pointing = removeRootEdge + addRootEdge.
    /// removeRootEdge needs no target (the edge id is (shard, part)); verify the OLD target zeroes.
    GcSnap snap;
    const UInt128 T1 = hexToU128("aa00000000000000000000000000000a");
    const UInt128 T2 = hexToU128("ab00000000000000000000000000000a");
    snap.addRootEdge("s/0", "p1", T1);
    auto cands = snap.removeRootEdge("s/0", "p1");
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].hash, T1);
    snap.addRootEdge("s/0", "p1", T2);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T2), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T1), 0u);
}

TEST(CasGcSnap, CodecRoundTripDeterministicAndStrict)
{
    GcSnap snap;
    snap.snap_shard = 2; snap.generation = 9;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    snap.addRootEdge("srv1/tbl/0", "part_1", T);
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    snap.addPackEdge(T, hexToU128("cc00000000000000000000000000000c"));
    auto bytes = encodeGcSnap(snap);
    EXPECT_NE(bytes.find("\"format\":\"cas_gc_snap\""), String::npos);
    auto d = decodeGcSnap(bytes);
    EXPECT_EQ(d.snap_shard, 2u);
    EXPECT_EQ(d.generation, 9u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T), 1u);
    EXPECT_EQ(d.inDegree(ObjectKind::Blob, B), 1u);
    EXPECT_TRUE(d.isExpanded(T));
    EXPECT_EQ(encodeGcSnap(d), bytes);                          /// byte-stable re-encode
    /// strict: bad edge_kind; pack edge with non-pack target_kind; future version
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcSnap(
        R"({"format":"cas_gc_snap","version":1,"snap_shard":0,"generation":0,"edges":[{"edge_kind":"bogus"}],"expanded":[],"known":[]})"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcSnap(
        R"({"format":"cas_gc_snap","version":1,"snap_shard":0,"generation":0,"edges":[{"edge_kind":"pack","parent_tree":"aa00000000000000000000000000000a","target_kind":"blob","target_hash":"bb00000000000000000000000000000b"}],"expanded":[],"known":[]})"); });
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [] { decodeGcSnap(
        R"({"format":"cas_gc_snap","version":2,"snap_shard":0,"generation":0,"edges":[],"expanded":[],"known":[]})"); });
}
