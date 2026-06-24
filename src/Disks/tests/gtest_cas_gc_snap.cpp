#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int UNKNOWN_FORMAT_VERSION;
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
    /// same-target duplicate add: no double count, no displaced candidate
    EXPECT_TRUE(snap.addRootEdge("srv1/tbl/0", "part_1", T).empty());
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

TEST(CasGcSnap, ForgetRemovesNodeFromKnown)
{
    /// P9: a deleted node's `known` membership is removed so the retire scan stops re-deriving it.
    GcSnap snap;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    snap.addRootEdge("s/0", "p1", T);
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);

    /// Drop the root edge: T -> in-degree 0, still known => a zero-in-degree candidate.
    snap.removeRootEdge("s/0", "p1");
    ASSERT_TRUE(snap.isKnown(ObjectKind::Tree, T));
    {
        bool tree_is_candidate = false;
        for (const auto & c : snap.zeroInDegreeKnown())
            if (c.kind == ObjectKind::Tree && c.hash == T)
                tree_is_candidate = true;
        ASSERT_TRUE(tree_is_candidate);
    }

    /// Forget it: gone from `known` and from the candidate set; idempotent.
    snap.forget(ObjectKind::Tree, T);
    EXPECT_FALSE(snap.isKnown(ObjectKind::Tree, T));
    for (const auto & c : snap.zeroInDegreeKnown())
        EXPECT_FALSE(c.kind == ObjectKind::Tree && c.hash == T);
    snap.forget(ObjectKind::Tree, T);   /// no-op, must not throw

    /// A later edge re-references the hash => re-added to `known` (the resurrection path).
    snap.addRootEdge("s/0", "p1", T);
    EXPECT_TRUE(snap.isKnown(ObjectKind::Tree, T));
}

TEST(CasGcSnap, ForgetSurvivesEncodeDecode)
{
    /// P9: the prune is durable — a forgotten node stays forgotten through the snap codec.
    GcSnap snap;
    const UInt128 A = hexToU128("aa00000000000000000000000000000a");
    const UInt128 C = hexToU128("cc00000000000000000000000000000c");
    snap.addRootEdge("s/0", "p1", A);
    snap.addRootEdge("s/0", "p2", C);
    snap.removeRootEdge("s/0", "p1");   /// A -> in-degree 0, known
    snap.forget(ObjectKind::Tree, A);

    const GcSnap round = decodeGcSnap(encodeGcSnap(snap));
    EXPECT_FALSE(round.isKnown(ObjectKind::Tree, A));
    EXPECT_TRUE(round.isKnown(ObjectKind::Tree, C));
}

TEST(CasGcSnap, StripTreeReturnsNewlyZeroChildren)
{
    GcSnap snap;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    const UInt128 C = hexToU128("cc00000000000000000000000000000c");
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    snap.addTreeEdge(T, ObjectKind::Tree, C);
    auto freed = snap.stripTree(T);
    ASSERT_EQ(freed.size(), 2u);                               /// B and C both newly zero
    EXPECT_FALSE(snap.isExpanded(T));
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B), 0u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, C), 0u);
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

TEST(CasGcSnap, RootEdgeAddOverAddIsLastOpWins)
{
    /// Spec §7: root edges are last-op-wins. `Build::publish` legally re-publishes an existing
    /// ref with a NEW tree (consecutive journal Adds, no Remove between); the LIVE tree (T2)
    /// must end up counted and the displaced OLD tree (T1) becomes the zero-in-degree candidate.
    GcSnap snap;
    const UInt128 T1 = hexToU128("aa00000000000000000000000000000a");
    const UInt128 T2 = hexToU128("ab00000000000000000000000000000a");
    EXPECT_TRUE(snap.addRootEdge("s/0", "p1", T1).empty());
    auto cands = snap.addRootEdge("s/0", "p1", T2);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T2), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T1), 0u);
    ASSERT_EQ(cands.size(), 1u);                               /// displaced old target zeroed
    EXPECT_EQ(cands[0].kind, ObjectKind::Tree);
    EXPECT_EQ(cands[0].hash, T1);
    auto zs = snap.zeroInDegreeKnown();
    ASSERT_EQ(zs.size(), 1u);                                  /// T1 is a candidate; T2 is NOT
    EXPECT_EQ(zs[0].hash, T1);
    /// a freshly decoded snap agrees (the durable document carries the re-pointed edge):
    auto d = decodeGcSnap(encodeGcSnap(snap));
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T2), 1u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T1), 0u);
    auto zs2 = d.zeroInDegreeKnown();
    ASSERT_EQ(zs2.size(), 1u);
    EXPECT_EQ(zs2[0].hash, T1);
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
    auto bytes = encodeGcSnap(snap);
    auto d = decodeGcSnap(bytes);
    EXPECT_EQ(d.snap_shard, 2u);
    EXPECT_EQ(d.generation, 9u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T), 1u);
    EXPECT_EQ(d.inDegree(ObjectKind::Blob, B), 1u);
    EXPECT_TRUE(d.isExpanded(T));
    EXPECT_EQ(encodeGcSnap(d), bytes);                          /// byte-stable re-encode
}

/// A thorough non-trivial round-trip: many distinct hashes across both edge kinds, multiple
/// root edges, tree+subtree children, expansion markers, and known nodes that carry zero in-degree
/// (so the known set is NOT merely the edge-target closure). The decoded snap must agree with the
/// original on EVERY observable (counts, in-degrees, expansion, known, zero-in-degree candidates),
/// and the re-encode must be byte-identical (canonical, deterministic ordering).
TEST(CasGcSnap, CodecRoundTripNonTrivial)
{
    GcSnap snap;
    snap.snap_shard = 7;
    snap.generation = 12345;

    const UInt128 T1 = hexToU128("aa00000000000000000000000000000a");
    const UInt128 T2 = hexToU128("ab00000000000000000000000000000a");
    const UInt128 T3 = hexToU128("ac00000000000000000000000000000a");
    const UInt128 B1 = hexToU128("bb00000000000000000000000000000b");
    const UInt128 B2 = hexToU128("bc00000000000000000000000000000b");
    const UInt128 SUB = hexToU128("dd00000000000000000000000000000d");   /// tree-as-tree-child (subtree)

    /// Root edges (last-op-wins id = (root_shard, part_name)).
    snap.addRootEdge("srv1/tbl/0", "part_1", T1);
    snap.addRootEdge("srv1/tbl/0", "part_2", T2);
    snap.addRootEdge("srv2/tbl/1", "part_1", T1);                /// T1 shared by two refs => indeg 2

    /// Tree children: blobs and a subtree (tree child of a tree).
    snap.markExpanded(T1);
    snap.addTreeEdge(T1, ObjectKind::Blob, B1);
    snap.addTreeEdge(T1, ObjectKind::Blob, B2);
    snap.addTreeEdge(T1, ObjectKind::Tree, SUB);                 /// subtree edge

    snap.markExpanded(T2);
    snap.addTreeEdge(T2, ObjectKind::Blob, B1);                 /// B1 shared across T1 and T2 => indeg 2

    /// T3 becomes a known node with zero in-degree: it was a root target, then displaced (the
    /// edge removed). `known` survives a zero-in-degree transition, so T3 is a retire candidate.
    snap.addRootEdge("srv3/tbl/0", "part_1", T3);
    snap.removeRootEdge("srv3/tbl/0", "part_1");
    snap.markExpanded(T3);

    /// Sanity on the original before round-trip.
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T1), 2u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, T2), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B1), 2u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, B2), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, SUB), 1u);

    auto bytes = encodeGcSnap(snap);
    auto d = decodeGcSnap(bytes);

    /// Scalars.
    EXPECT_EQ(d.snap_shard, 7u);
    EXPECT_EQ(d.generation, 12345u);

    /// In-degrees survive for every node and kind.
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T1), 2u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T2), 1u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T3), 0u);
    EXPECT_EQ(d.inDegree(ObjectKind::Blob, B1), 2u);
    EXPECT_EQ(d.inDegree(ObjectKind::Blob, B2), 1u);
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, SUB), 1u);

    /// Known set survives, including the zero-in-degree node T3.
    EXPECT_TRUE(d.isKnown(ObjectKind::Tree, T1));
    EXPECT_TRUE(d.isKnown(ObjectKind::Tree, T2));
    EXPECT_TRUE(d.isKnown(ObjectKind::Tree, T3));
    EXPECT_TRUE(d.isKnown(ObjectKind::Tree, SUB));
    EXPECT_TRUE(d.isKnown(ObjectKind::Blob, B1));
    EXPECT_TRUE(d.isKnown(ObjectKind::Blob, B2));

    /// Expansion markers survive.
    EXPECT_TRUE(d.isExpanded(T1));
    EXPECT_TRUE(d.isExpanded(T2));
    EXPECT_TRUE(d.isExpanded(T3));

    /// Zero-in-degree candidate set agrees (only T3 here).
    auto zs = d.zeroInDegreeKnown();
    ASSERT_EQ(zs.size(), 1u);
    EXPECT_EQ(zs[0].kind, ObjectKind::Tree);
    EXPECT_EQ(zs[0].hash, T3);

    /// Removing a shared edge in the decoded snap behaves identically (in-degree bookkeeping
    /// was fully reconstructed, not approximated).
    EXPECT_TRUE(d.removeRootEdge("srv1/tbl/0", "part_1").empty());   /// T1 still pinned by srv2 ref
    EXPECT_EQ(d.inDegree(ObjectKind::Tree, T1), 1u);

    /// Canonical, deterministic re-encode (byte-stable) for the unmodified copy.
    EXPECT_EQ(encodeGcSnap(decodeGcSnap(bytes)), bytes);
}

/// Fail-closed binary decode: bad magic, truncated input, bad enum bytes, a duplicate edge id,
/// and a future version => the pinned typed errors.
TEST(CasGcSnap, DecodeFailClosed)
{
    /// A valid encoded snap to mutate.
    GcSnap snap;
    snap.snap_shard = 1; snap.generation = 2;
    const UInt128 T = hexToU128("aa00000000000000000000000000000a");
    const UInt128 B = hexToU128("bb00000000000000000000000000000b");
    snap.addRootEdge("s/0", "p1", T);
    snap.markExpanded(T);
    snap.addTreeEdge(T, ObjectKind::Blob, B);
    const String good = encodeGcSnap(snap);

    /// Empty / too-short input is corruption (truncated).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcSnap(""); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(good.substr(0, 2)); });

    /// Bad magic.
    {
        String bad = good;
        bad[0] = 'X';
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(bad); });
    }

    /// Truncated mid-stream (drop the last byte of a valid document).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(good.substr(0, good.size() - 1)); });

    /// Future version (byte right after the 4-byte magic) => UNKNOWN_FORMAT_VERSION.
    {
        String future = good;
        future[4] = static_cast<char>(0xFF);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodeGcSnap(future); });
    }
}

namespace
{
/// Minimal binary writer mirroring the gc/snap layout, for hand-crafting malformed documents
/// (little-endian; magic "CAGS" + version 3 / codec RAW (0) header).
struct SnapWriter
{
    String buf;
    void raw(const char * p, size_t n) { buf.append(p, n); }
    void u8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
    void hash(const UInt128 & h) { for (int i = 0; i < 16; ++i) buf.push_back(static_cast<char>((h >> (8 * i)) & 0xFF)); }
    void str(const String & s) { u16(static_cast<uint16_t>(s.size())); raw(s.data(), s.size()); }
    void header() { raw("CAGS", 4); u8(1); u8(0); u64(0); u64(0); }   /// version 1, codec RAW (0), shard 0, generation 0
};
}

/// Hand-crafted malformed binary documents fail closed with the pinned typed errors.
TEST(CasGcSnap, DecodeFailClosedBinaryFields)
{
    const UInt128 T  = hexToU128("aa00000000000000000000000000000a");
    const UInt128 T2 = hexToU128("ab00000000000000000000000000000a");
    const UInt128 B  = hexToU128("bb00000000000000000000000000000b");

    /// Invalid edge-kind byte (0).
    {
        SnapWriter w; w.header();
        w.u32(1); w.u8(0);   /// one edge, bogus edge kind
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(w.buf); });
    }

    /// Invalid edge-kind byte (3 — no longer a valid EdgeKind after pack removal).
    {
        SnapWriter w; w.header();
        w.u32(1);
        w.u8(3);             /// bogus edge kind
        w.hash(T);           /// parent_tree
        w.u8(1);             /// target_kind = Blob
        w.hash(B);           /// target_hash
        w.u32(0);            /// expanded
        w.u32(0);            /// known
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(w.buf); });
    }

    /// Two root edges with the same (root_shard, part_name) => duplicate canonical id.
    {
        SnapWriter w; w.header();
        w.u32(2);
        for (const auto & target : {T, T2})
        {
            w.u8(1);                 /// EdgeKind::Root
            w.str("s/0"); w.str("p1");
            w.u8(2);                 /// target_kind = Tree
            w.hash(target);
        }
        w.u32(0);                    /// expanded
        w.u32(0);                    /// known
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeGcSnap(w.buf); });
    }

    /// A well-formed single-root-edge document decodes (positive control for the writer).
    {
        SnapWriter w; w.header();
        w.u32(1);
        w.u8(1); w.str("s/0"); w.str("p1"); w.u8(2); w.hash(T);
        w.u32(0); w.u32(0);   /// expanded, known
        w.u32(0);              /// folded_cursor (B140-dangle fix: cursor count = 0)
        auto d = decodeGcSnap(w.buf);
        EXPECT_EQ(d.inDegree(ObjectKind::Tree, T), 1u);
    }
}

/// B140-dangle fix: the per-shard fold cursor is part of the snap's durable identity, so two snaps
/// with identical edges but DIFFERENT cursors encode to DIFFERENT bytes. This is what makes
/// byte-equal generation adoption cursor-aware (a leader cannot adopt an edge-equal snap folded to a
/// different cursor), structurally preventing the cursor-skip under-count.
TEST(CasGcSnap, CursorIsPartOfSnapIdentity)
{
    using namespace DB::Cas;
    GcSnap a;
    a.snap_shard = 0;
    a.generation = 7;
    a.addRootEdge("srv1/tbl/0", "part_1", UInt128{0xABCD});
    GcSnap b = a;                                  /// identical edges/expanded/known

    a.folded_cursor["srv1/tbl/0"] = 10;
    b.folded_cursor["srv1/tbl/0"] = 11;            /// only the cursor differs

    EXPECT_NE(encodeGcSnap(a), encodeGcSnap(b))
        << "snaps folded to different cursors must not be byte-equal (else adoption is cursor-blind)";

    EXPECT_EQ(decodeGcSnap(encodeGcSnap(a)).folded_cursor.at("srv1/tbl/0"), 10u);
}

/// The binary encoding is materially smaller than the previous JSON form for a realistic snap —
/// the motivating win (less parse/serialize CPU and a smaller S3 blob). A blob/tree edge in
/// JSON cost ~150-200 bytes (hex hashes + key names); binary costs ~33-50 bytes.
TEST(CasGcSnap, BinaryEncodingIsCompact)
{
    GcSnap snap;
    snap.snap_shard = 3;
    snap.generation = 42;
    for (uint64_t i = 0; i < 200; ++i)
    {
        const UInt128 tree = (static_cast<UInt128>(i + 1) << 64) | (i + 7);
        const UInt128 blob = (static_cast<UInt128>(i + 100) << 64) | (i + 3);
        snap.addRootEdge("srv/tbl/0", "part_" + std::to_string(i), tree);
        snap.markExpanded(tree);
        snap.addTreeEdge(tree, ObjectKind::Blob, blob);
    }
    const auto bytes = encodeGcSnap(snap);
    GTEST_LOG_(INFO) << "gc/snap binary size for 200 root + 200 tree edges: " << bytes.size() << " bytes";
    /// 200 root + 200 tree edges, plus expanded + known: a few tens of KiB in binary, where the
    /// old JSON form (hex hashes + repeated key names per edge) was several times larger.
    EXPECT_LT(bytes.size(), 40u * 1024u);
    /// Round-trips.
    auto d = decodeGcSnap(bytes);
    EXPECT_EQ(encodeGcSnap(d), bytes);
}
