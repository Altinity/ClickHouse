#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <string>
#include <vector>

/// B140 regression guard. The soak's Phase-1 sync run did a `TRUNCATE TABLE` at op 450 and then
/// observed fsck `unreachable` STUCK above zero (1751) while the incremental GC reported
/// `candidates=0` — i.e. the GC believed it was done while orphaned blobs remained. This file
/// reproduces the soak shape at the CORE level (no server, no docker): publish many parts that
/// SHARE blobs (dedup), interleave regular GC rounds with the publishes (so trees get expanded
/// into the durable snap exactly as they would during a steady-state insert workload), then
/// perform the SAME removal a Replicated TRUNCATE issues — a per-ref `dropRef` for every part —
/// and drive the GC to a fixpoint. The invariant under test: after the drops are folded and the
/// cascade runs, `runFsck().unreachable` reaches 0 (every shared blob is reclaimed).
///
/// A `dropNamespace` variant is included as well (the path `removeRecursive` takes for a whole
/// table dir, e.g. DROP TABLE): it journals one Remove per former ref, so the cascade should fold
/// it identically.

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p"});
}

/// Publish one part `ref` with TWO content files whose payloads are passed in. Identical payloads
/// across parts dedup to the SAME blob object (the soak's dedup_ratio ~3.8 comes from exactly this
/// sharing). Returns the tree id.
TreeId publishPart2(
    const StorePtr & s, const String & ns, const String & ref,
    const String & payload_a, const String & payload_b)
{
    auto build = s->startBuild({});
    build->putBlob(idOf(payload_a), BlobSource::fromString(payload_a));
    build->putBlob(idOf(payload_b), BlobSource::fromString(payload_b));

    TreeEntry ea;
    ea.name = "data.bin";
    ea.placement = Placement::Blob;
    ea.file_hash = u128Of(payload_a);
    ea.file_size = payload_a.size();

    TreeEntry eb;
    eb.name = "data.cmrk3";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of(payload_b);
    eb.file_size = payload_b.size();

    const TreeId tree = build->putTree({ea, eb});
    build->publish(RootNamespace{ns}, ref, tree, {});
    return tree;
}

/// Run regular GC rounds until the round does no further reclamation work (a fixpoint). The soak's
/// grace-aware bound waits for `unreachable==0`; here we drive rounds deterministically and stop
/// when a round neither retires nor deletes nor cascades anything new.
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

/// The faithful soak repro: many parts sharing blobs, GC interleaved with the publishes, then a
/// per-ref drop of EVERY ref (Replicated TRUNCATE), then GC to a fixpoint. fsck.unreachable must
/// reach 0 — no orphaned blob may survive.
TEST(CasTruncateReclaim, PerRefDropOfSharedBlobsReclaimsToZero)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};

    constexpr int N = 32;

    /// Publish N parts. Payloads are chosen so blobs are SHARED across parts: data.bin cycles
    /// through 8 distinct contents, data.cmrk3 through 4 — heavy dedup, like the soak.
    std::vector<String> refs;
    for (int i = 0; i < N; ++i)
    {
        const String ref = "all_" + std::to_string(i) + "_" + std::to_string(i) + "_0";
        refs.push_back(ref);
        const String pa = "data-" + std::to_string(i % 8);
        const String pb = "mark-" + std::to_string(i % 4);
        publishPart2(s, ns.string(), ref, pa, pb);

        /// Interleave a GC round every few publishes, so the live trees get EXPANDED into the
        /// durable snap during the insert phase (steady-state GC, as in the soak).
        if (i % 5 == 4)
        {
            Gc gc(s, hexToU128("00000000000000000000000000000001"));
            gc.runRegularRound();
        }
    }

    /// Steady-state GC has nothing to reclaim while the refs are live.
    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        runGcToFixpoint(gc);
        const FsckReport before = runFsck(*s, /*detail=*/false);
        EXPECT_EQ(before.unreachable, 0u) << "live pool must have no unreachable debris";
        EXPECT_EQ(before.dangling, 0u);
        EXPECT_GT(before.reachable, 0u);
    }

    /// TRUNCATE: a Replicated TRUNCATE removes each part dir, which routes to dropRef per ref.
    for (const String & ref : refs)
        s->dropRef(ns, ref);

    /// Every publishing build finished; advance the durable watermark floor past their seqs so the
    /// Task 10 build-watermark guard no longer spares the now-dropped objects (production does this
    /// via the background renewer ~2s; here the renewer is off, so drive it explicitly).
    s->renewWatermarkOnce();

    /// Drive GC to a fixpoint and require full reclamation — this is the B140 assertion.
    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        const size_t rounds = runGcToFixpoint(gc);
        const FsckReport after = runFsck(*s, /*detail=*/false);
        EXPECT_EQ(after.dangling, 0u) << "TRUNCATE must never lose a reachable object";
        EXPECT_EQ(after.unreachable, 0u)
            << "B140: orphaned blobs survived TRUNCATE after " << rounds
            << " GC rounds (reachable=" << after.reachable
            << ", unreachable=" << after.unreachable << ")";
        EXPECT_EQ(after.reachable, 0u) << "no refs remain, so nothing should be reachable";
    }
}

/// Mirrors the soak exactly: TRUNCATE at "op 450" (drop every live ref), then CONTINUE inserting
/// (the soak's ops 451..599 had min_op=451) while the GC keeps running, then a final drive to a
/// fixpoint. The post-truncate inserts must not stall reclamation of the pre-truncate orphans.
/// Also asserts a TIGHT bound on the number of rounds reclamation needs (the soak's 180s budget at
/// gc_interval=30s only buys ~6 rounds, so the core must reach a fixpoint well inside that).
TEST(CasTruncateReclaim, TruncateThenKeepInsertingStillReclaims)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Pre-truncate generation (the soak's ops < 451).
    std::vector<String> pre_refs;
    for (int i = 0; i < 24; ++i)
    {
        const String ref = "pre_" + std::to_string(i);
        pre_refs.push_back(ref);
        publishPart2(s, ns.string(), ref, "p-data-" + std::to_string(i % 6), "p-mark-" + std::to_string(i % 3));
        if (i % 5 == 4)
        {
            Gc gc(s, hexToU128("00000000000000000000000000000001"));
            gc.runRegularRound();
        }
    }

    /// TRUNCATE: drop every pre-truncate ref (per-ref dropRef).
    for (const String & ref : pre_refs)
        s->dropRef(ns, ref);

    /// Continue inserting AFTER the truncate (the soak's ops 451..599), interleaving GC rounds.
    for (int i = 0; i < 24; ++i)
    {
        publishPart2(s, ns.string(), "post_" + std::to_string(i),
                     "q-data-" + std::to_string(i % 6), "q-mark-" + std::to_string(i % 3));
        if (i % 5 == 4)
        {
            Gc gc(s, hexToU128("00000000000000000000000000000001"));
            gc.runRegularRound();
        }
    }

    /// All publishing builds finished; advance the durable watermark floor past their seqs so the
    /// Task 10 build-watermark guard no longer spares the dropped objects (the background renewer is
    /// off in this test, so drive it explicitly — production renews ~2s off the write path).
    s->renewWatermarkOnce();

    /// Drive to a fixpoint. unreachable must reach 0 (the pre-truncate orphans are gone) while the
    /// post-truncate refs stay reachable.
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const size_t rounds = runGcToFixpoint(gc);
    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u);
    EXPECT_EQ(after.unreachable, 0u)
        << "B140: pre-truncate orphans survived after " << rounds << " GC rounds";
    EXPECT_GT(after.reachable, 0u) << "post-truncate refs must stay reachable";
    /// Tight round bound: the dead subgraph drains in tree-round + blob-round + a confirm round.
    EXPECT_LE(rounds, 4u) << "reclamation took too many rounds (soak budget is ~6 at 30s/round)";
}

/// The DROP TABLE path: removeRecursive of a table dir calls dropNamespace, which journals one
/// Remove per former ref. Same reclamation invariant.
TEST(CasTruncateReclaim, DropNamespaceOfSharedBlobsReclaimsToZero)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};

    constexpr int N = 32;
    for (int i = 0; i < N; ++i)
    {
        const String ref = "all_" + std::to_string(i) + "_" + std::to_string(i) + "_0";
        const String pa = "data-" + std::to_string(i % 8);
        const String pb = "mark-" + std::to_string(i % 4);
        publishPart2(s, ns.string(), ref, pa, pb);
        if (i % 5 == 4)
        {
            Gc gc(s, hexToU128("00000000000000000000000000000001"));
            gc.runRegularRound();
        }
    }

    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        runGcToFixpoint(gc);
    }

    /// DROP TABLE: the whole namespace is tombstoned at once (one Remove per ref in the journal).
    s->dropNamespace(ns);

    /// Every publishing build finished; advance the durable watermark floor past their seqs so the
    /// Task 10 build-watermark guard no longer spares the dropped objects (renewer off here).
    s->renewWatermarkOnce();

    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        const size_t rounds = runGcToFixpoint(gc);
        const FsckReport after = runFsck(*s, /*detail=*/false);
        EXPECT_EQ(after.dangling, 0u);
        EXPECT_EQ(after.unreachable, 0u)
            << "B140: orphaned blobs survived DROP (dropNamespace) after " << rounds << " GC rounds";
        EXPECT_EQ(after.reachable, 0u);
    }
}
