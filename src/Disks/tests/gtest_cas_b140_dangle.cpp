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
using DB::Cas::tests::u128Of;

namespace
{

/// root_shards=1 so both refs live in the single root shard 0 — the dangle is about the SINGLE
/// snap shard's in-degree, and one cursor_key covers both refs.
StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
}

size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        RoundReport rep;
        try
        {
            rep = gc.runRegularRound();
        }
        catch (const DB::Exception &)
        {
            /// The fail-closed coherence guard refused this round (CORRUPTED_DATA): no delete
            /// happened, the live blob is safe. Stop — re-running would just throw again.
            break;
        }
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    return rounds;
}

}

/// B140-DANGLE — the soak's INV-NO-LOSS finding, as a deterministic single-process reproduction of
/// the REAL (ground-truth-decoded) mechanism: a SHARED / deduplicated blob under-count via cursor
/// incoherence. (This replaces the earlier "marker-without-edges" injection, which was REFUTED by
/// decoding the real soak snaps: `markers_with_ZERO_child_edges = 0` — that state never occurs.)
///
/// GROUND TRUTH (gens 1280/1281; §6 = `utils/ca-soak/logs/p9_instr_correlation.txt`): a content-shared
/// blob `B` is referenced by TWO live part-trees — `T_live` (older) and `T_cur` (a newer, still-live
/// part, cross-node `adopt`-published, token unchanged). GC's snap recorded only `T_live -> B`; the
/// live `T_cur -> B` edge was NEVER folded into the snap. When `T_live`'s part was dropped and the
/// cascade stripped it, GC saw `inDeg(B) = 0` and deleted `B` — while the live ref `rb_cur -> T_cur`
/// still resolves to it. `fsck` (walking authoritative refs, never `gc/snap`) then finds `B`
/// reachable-but-missing => `dangling` = data loss.
///
/// WHY `T_cur -> B` was missing (the PINNED class — `CaB140DangleMerge.tla`): the committed
/// `gc/state.folded_cursor` can run AHEAD of the actual fold extent of the committed snap edges,
/// because the snap codec omits the cursor (`CasGcSnap.cpp:264`) — the snap and `folded_cursor` are
/// two separately-durable values with no enforced coherence. The fold then treats `T_cur`'s `Add`
/// (at a version <= the ahead cursor) as already-folded and never records its edge, and the gated
/// journal trim (`CasGc.cpp:144`) drops that record — losing the edge permanently. The exact
/// cross-leader interleaving that desyncs the two is unpinned; this test injects the resulting
/// FAITHFUL durable mid-state (T_live expanded WITH its edge — no marker-without-edges; `folded_cursor`
/// past `T_cur`'s `Add`; the snap missing `T_cur -> B`) and lets a REAL GC round delete `B`.
///
/// RED today (`dangling == 1`). Goes GREEN once the fix (cursor-in-snap: the per-shard cursor is part
/// of the committed snap, so `folded_cursor` cannot diverge from the snap's edges; plus the
/// fail-closed coherence assertion at the trim/delete boundary) refuses to delete on an incoherent
/// `(snap, folded_cursor)` pair — `B` survives, `dangling == 0`.
/// Spec: docs/superpowers/specs/2026-06-18-ca-b140-dangle-fix-v2-design.md
TEST(CasGcDangle, SharedBlobUnderCountDeletesLivePinnedBlob)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = 0;
    const std::string cursor_key = ns.string() + "/" + std::to_string(shard);

    /// rb_live -> T_live -> { data.bin: B }. B is uploaded here (build1).
    {
        auto build = s->startBuild({});
        build->putBlob(idOf("B"), BlobSource::fromString("B"));
        TreeEntry e;
        e.name = "data.bin";
        e.placement = Placement::Blob;
        e.file_hash = u128Of("B");
        e.file_size = std::string("B").size();
        const TreeId t_live = build->putTree({e});
        build->publish(ns, "rb_live", t_live, {});
        s->renewWatermarkOnce();
    }
    /// Resolve T_live's hash via the live ref (the build is gone).
    const auto t_live_opt = s->resolveRef(ns, "rb_live");
    ASSERT_TRUE(t_live_opt.has_value());
    const UInt128 tlive_hash = hexToU128(t_live_opt->tree_id.string());

    /// rb_cur -> T_cur -> { other.bin: B }. A DISTINCT tree (different entry name) that REUSES the
    /// same shared blob B (dedup, token unchanged — the soak's cross-node `adopt`). Still live.
    {
        auto build = s->startBuild({});
        build->reuseBlob(idOf("B"), /*body_recreatable=*/false);
        TreeEntry e;
        e.name = "other.bin";
        e.placement = Placement::Blob;
        e.file_hash = u128Of("B");
        e.file_size = std::string("B").size();
        const TreeId t_cur = build->putTree({e});
        build->publish(ns, "rb_cur", t_cur, {});
        s->renewWatermarkOnce();
    }

    /// The committed cursor we will claim: PAST both Adds (rb_live and rb_cur). The snap, however,
    /// will reflect ONLY rb_live -> T_live -> B — the cursor-skip under-count.
    const auto shard_obj = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(shard_obj.has_value());
    const uint64_t cursor_past_both = decodeRootShard(shard_obj->bytes).shard_version;

    /// Inject the FAITHFUL under-counted snap@gen1:
    ///   - T_live is LIVE (root edge => in-degree 1) and markExpanded WITH its T_live->B edge present
    ///     (faithful — not marker-without-edges); B is `known` with in-degree 1.
    ///   - T_cur and rb_cur are ABSENT from the snap (their Add was treated as already-folded by the
    ///     ahead cursor and never recorded) — the missing live edge.
    GcSnap snap;
    snap.addRootEdge(cursor_key, "rb_live", tlive_hash);
    snap.markExpanded(tlive_hash);
    snap.addTreeEdge(tlive_hash, ObjectKind::Blob, u128Of("B"));   /// T_live -> B, seeds B into known (in-deg 1)
    snap.snap_shard = 0;
    snap.generation = 1;

    /// B140-dangle fix: the cursor now lives in the snap, not gc/state. Inject the same broken
    /// state: cursor in snap shard 0 AHEAD of the actual edges (T_cur's Add was "cursor-skipped"
    /// but T_cur->B is absent from the snap). In normal GC operation this divergence cannot occur
    /// (cursor and edges are written atomically), but we inject it to show the old failure mode.
    snap.folded_cursor[cursor_key] = cursor_past_both;             /// cursor AHEAD of the snap's real extent
    GcState st;
    st.snap_generation = 1;
    st.snap_shards = 1;
    ASSERT_EQ(b->putIfAbsent(s->layout().gcSnapKey(1, 0), encodeGcSnap(snap)), PutOutcome::Done);
    const auto state_head = b->head(s->layout().gcStateKey());
    if (state_head.exists)
        b->putOverwrite(s->layout().gcStateKey(), encodeGcState(st), state_head.token);
    else
        b->putIfAbsent(s->layout().gcStateKey(), encodeGcState(st));

    /// Drop rb_live (its rem lands ABOVE cursor_past_both, so the fold WILL process it): the strip of
    /// T_live drops the only T_live->B edge the snap knows, taking in-degree(B) to 0.
    s->dropRef(ns, "rb_live");
    s->renewWatermarkOnce();

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const size_t rounds = runGcToFixpoint(gc);

    /// rb_cur is still LIVE and still resolves through a present T_cur — its blob B must survive.
    ASSERT_TRUE(s->resolveRef(ns, "rb_cur").has_value());

    const FsckReport rep = runFsck(*s, /*detail=*/true);

    /// THE DANGLE ASSERTION (RED today): GC must NEVER delete a blob a live ref references. Today it
    /// does — B's only recorded in-degree was from T_live; the live T_cur->B edge was never in the
    /// snap (cursor ahead of the snap's edges), so stripping T_live took B to in-degree 0 and the
    /// single content-delete site (CasGc.cpp:249) deleted it.
    EXPECT_EQ(rep.dangling, 0u)
        << "B140-dangle: GC deleted shared blob B still referenced by the live ref rb_cur -> T_cur "
        << "after " << rounds << " rounds (dangling=" << rep.dangling << ", reachable=" << rep.reachable
        << ", B_present=" << b->head(s->layout().blobKey(idOf("B"))).exists << "); the committed "
        << "folded_cursor ran ahead of the snap edges, so T_cur->B was never folded (cursor-skip "
        << "under-count of a deduplicated blob).";
}
