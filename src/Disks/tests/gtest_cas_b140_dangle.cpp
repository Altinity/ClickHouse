#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <string>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;

namespace
{

StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p"});
}

size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    return rounds;
}

}

/// B140-DANGLE (the 2-node soak's INV-NO-LOSS finding, deterministic single-process reproduction).
///
/// ROOT INVARIANT (broken): "a `markExpanded` tree that is still LIVE (in-degree >= 1) has ALL its
/// tree->blob child edges present in the durable GC snapshot." The once-per-tree expansion marker is
/// the ONLY writer of those edges (CasGc.cpp foldShardRecords: `if (!isExpanded)` :805 gates the
/// read+`markExpanded` :865). If a tree T is left `markExpanded` WITHOUT its child edges while a live
/// ref pins it (root edge => in-degree >= 1) and the fold cursor has advanced past T's Add, the fold
/// NEVER re-expands T, so T's blob B sits at in-degree 0. `zeroInDegreeKnown` (CasGcSnap.cpp:250)
/// surfaces B, retire observes its token, and the SINGLE content-delete site (CasGc.cpp:249) deletes
/// B — even though the live ref rb -> T -> B references it. fsck (walking authoritative refs, never
/// gc/snap) then finds B reachable-but-missing => dangling. This matches the soak exactly: GC
/// computed in-degree 0 for a blob a live (merged) tree references, because the fold never recorded
/// that tree's edges (round 334 cross-node reuse -> round 340 delete).
///
/// MECHANISM (b), the cascade-vs-recreate marker race the spec comment at CasGc.cpp:402-407 warns
/// about: a strip cleared/skipped T's re-expansion while T's recreate Add landed at or below the
/// advanced cursor, leaving the marker set without the child edge. A SUBAGENT empirically verified
/// (built+ran 9 natural single-process interleavings) that the fold is NOT buggy in single-process
/// sequential operation: the `displaced_later` skip (CasGc.cpp:830) is fail-closed for any live ref
/// (rethrows FILE_DOESNT_EXIST when there is no later same-ref record), and the cascade advances the
/// cursor in the SAME CAS that clears the marker (CasGc.cpp:448-468) with a fold-through-fence
/// recheck (CasGc.cpp:182-209). The broken invariant therefore arises only across rounds/leaders
/// (2-node soak); the likely PRODUCER on the leader is the cross-node replication relink
/// (adoptFromTree tokenless-evidence, CasBuild.cpp:607-630) — to be audited with the fold invariant.
/// We inject that exact durable mid-state (built only through the public GcSnap API:
/// addRootEdge/markExpanded/addTreeEdge/stripTree) so the single content-delete fires deterministically
/// (no clocks, no sleeps, no concurrency).
///
/// RED today (dangling == 1). Goes green once the fold/cascade guarantees a live-pinned tree's blob
/// edges are present in the snap before any of its children can be retired (e.g. re-expand on a
/// folded Add when a child edge is missing, or never advance the cursor past an Add whose tree is not
/// fully edge-recorded, or refuse to retire a candidate whose live parent is markExpanded).
TEST(CasGcDangle, MarkedExpandedWithoutEdgesDeletesLivePinnedBlob)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const std::string ns = "srv1/tbl";
    const uint64_t shard = shardOfForTest("rb", s->poolMeta().root_shards);
    const std::string cursor_key = ns + "/" + std::to_string(shard);

    /// Publish rb -> Tb -> {B} for real: the manifest, Tb's object and B's object all exist, and rb
    /// resolves LIVE (this is the "merged-then-detached" part that must keep referencing B).
    auto build = s->startBuild({});
    build->putBlob(idOf("B"), BlobSource::fromString("B"));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("B");
    e.file_size = std::string("B").size();
    const TreeId Tb = build->putTree({e});
    build->publish(RootNamespace{ns}, "rb", Tb, {});
    build.reset();
    s->renewWatermarkOnce();

    const UInt128 tb_hash = hexToU128(Tb.string());

    /// Construct the CORRUPT durable snap that the cascade-vs-recreate marker race produces:
    ///   - Tb has a ROOT edge (in-degree 1 => LIVE, never a retire candidate) and is markExpanded
    ///     (so the fold will NOT re-expand it and re-record its child edges), but its Tb->B tree edge
    ///     is ABSENT;
    ///   - B is in `known` with in-degree 0 (a zero-in-degree candidate) — exactly what a strip of a
    ///     sibling parent leaves when Tb's re-expansion was skipped.
    GcSnap snap;
    snap.addRootEdge(cursor_key, "rb", tb_hash);                       /// Tb live (in-degree 1)
    snap.markExpanded(tb_hash);                                        /// marker set => no re-expansion
    snap.addTreeEdge(UInt128(0xDEAD), ObjectKind::Blob, u128Of("B"));  /// seed B into `known` (in-degree 1)
    snap.stripTree(UInt128(0xDEAD));                                   /// free B: known, in-degree 0
    snap.snap_shard = 0;
    snap.generation = 1;

    /// Persist gc/snap@gen1 and gc/state pointing at it, with folded_cursor advanced PAST rb's Add so
    /// the fold finds no new records and never re-expands Tb.
    const auto shard_obj = b->get(s->layout().rootShardKey(RootNamespace{ns}, shard));
    ASSERT_TRUE(shard_obj.has_value());
    const RootShard root = decodeRootShard(shard_obj->bytes);

    GcState st;
    st.snap_generation = 1;
    st.snap_shards = 1;
    st.folded_cursor[cursor_key] = root.shard_version;
    ASSERT_EQ(b->putIfAbsent(s->layout().gcSnapKey(1, 0), encodeGcSnap(snap)), PutOutcome::Done);
    const auto state_head = b->head(s->layout().gcStateKey());
    if (state_head.exists)
        b->putOverwrite(s->layout().gcStateKey(), encodeGcState(st), state_head.token);
    else
        b->putIfAbsent(s->layout().gcStateKey(), encodeGcState(st));
    s->renewWatermarkOnce();

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const size_t rounds = runGcToFixpoint(gc);

    /// rb still resolves through a present Tb — the tree is LIVE.
    EXPECT_TRUE(s->resolveRef(RootNamespace{ns}, "rb").has_value());
    EXPECT_TRUE(b->head(s->layout().treeKey(Tb)).exists);

    const FsckReport rep = runFsck(*s, /*detail=*/true);

    /// THE DANGLE ASSERTION (RED today): GC must NEVER delete a blob a live ref references. Today it
    /// does — B's only recorded in-degree was 0 because Tb was markExpanded without its Tb->B edge.
    EXPECT_EQ(rep.dangling, 0u)
        << "B140-dangle: GC deleted a blob still referenced by the live ref rb -> Tb after " << rounds
        << " rounds (dangling=" << rep.dangling << ", reachable=" << rep.reachable
        << ", B_present=" << b->head(s->layout().blobKey(idOf("B"))).exists << "); the fold left Tb "
        << "markExpanded without its tree->blob edge (CasGc.cpp foldShardRecords :805/:865), so "
        << "zeroInDegreeKnown surfaced B and the single content-delete site (CasGc.cpp:249) deleted it";
}
