#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <string>
#include <vector>

namespace DB::ErrorCodes
{
extern const int FILE_DOESNT_EXIST;
extern const int ABORTED;
}

/// B140 regression: the incremental GC leaks the child blobs of a tree that is DISPLACED (its ref
/// re-pointed) within one fold window AND whose tree object has vanished before the fold reads it
/// (a competing GC round's landed delete). This reproduces the soak symptom — `runRegularRound`
/// reaches a true fixpoint (`candidates=0 deleted=0 cascaded=0`) while a full-sweep `runFsck` still
/// sees orphaned blobs (zero trees; dangling=0, so INV-NO-LOSS holds — a space leak, not data loss).
///
/// ROOT CAUSE (CasGc.cpp foldShardRecords, the once-per-tree expansion at :732-797): a tree's child
/// blob/pack edges are recorded into the durable snap ONLY by EXPANDING the tree — reading the tree
/// object on the first folded `Add` and calling addTreeEdge/addPackEdge per entry. When that read
/// throws FILE_DOESNT_EXIST and a later record for the SAME ref proves the edge was displaced
/// (`displaced_later`, :760-770), the branch sets `tree_present = false` and SKIPS the whole
/// expansion block (:776-796): no edges are added and the tree is never markExpanded. Consequently:
///   - the displaced tree's blobs are never inserted into the snap's `known` set (addEdge seeds
///     `known`), so `zeroInDegreeKnown` can never surface them as retire candidates; and
///   - `stripTree` of the displaced/deleted tree frees nothing (it strips only edges recorded at
///     expansion — see CasGc.cpp :347-349), so the blobs are unreachable by the incremental GC
///     FOREVER. Only a full-sweep (the spec's §8 Full GC reachability walk) can rediscover them.
///
/// This is the structural gap the design assigns to Full GC — regular GC reclaims only
/// JOURNAL-KNOWN-and-EXPANDED nodes; a vanished-before-expansion tree's children are exactly the
/// "drift (snap diverging from truth)" class §8 repairs. The test asserts the DESIRED end state
/// (blobs reclaimed). It is RED today and documents the leak; it goes green once B140 is fixed
/// (either an expand-before-displace ordering guarantee in the writer/fold, or a Full-GC backstop).

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

/// Publish one ref naming a two-blob tree with the given payloads. No GC round is interleaved, so
/// the tree is NOT expanded into the durable snap before it can be displaced. Returns the tree id.
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

/// The deterministic core repro of the soak's two-server churn: a ref is published to treeA and then
/// re-published to treeB (last-op-wins displacement, two consecutive `Add`s in one fold window with
/// no `Remove` between), and treeA's object is removed before the GC ever folds — exactly as a
/// competing GC round's landed delete would. The fold then takes the `displaced_later` skip and
/// never records treeA's blob edges, so treeA's UNIQUE blobs (not shared with treeB) leak.
///
/// `data-A` / `mark-A` are referenced ONLY by treeA; treeB references the distinct `data-B` /
/// `mark-B`. After GC reaches a fixpoint the live ref points at treeB, so treeB's blobs are
/// reachable; treeA's two blobs are unreachable orphans. The desired post-fix state is
/// `unreachable == 0` (treeA's blobs reclaimed). dangling stays 0 throughout — no data loss.
TEST(CasGcLeak, DisplacedUnexpandedTreeBlobsLeak)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    /// Publish treeA (unique blobs data-A / mark-A). NO GC round runs here, so treeA is never
    /// expanded into the snap.
    const TreeId tree_a = publishPart2(s, ns.string(), ref, "data-A", "mark-A");

    /// Re-publish the SAME ref to treeB (distinct blobs data-B / mark-B): last-op-wins, a second
    /// `Add` for the ref with no `Remove` between — treeA's root edge is displaced in this fold
    /// window.
    publishPart2(s, ns.string(), ref, "data-B", "mark-B");

    /// A competing GC round already deleted the displaced treeA object: make its read 404. The
    /// deleteExact-by-token is exactly how the GC's content-delete site removes an object, so this
    /// faithfully reproduces "the object is GONE when the fold tries to expand it".
    {
        const String tree_a_key = s->layout().treeKey(tree_a);
        const auto head = b->head(tree_a_key);
        ASSERT_TRUE(head.exists) << "treeA object must exist before we delete it";
        ASSERT_EQ(b->deleteExact(tree_a_key, head.token).kind, DeleteOutcome::Kind::Deleted);
        /// Sanity: the Store now 404s on treeA, driving the fold's FILE_DOESNT_EXIST branch.
        DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readTree(tree_a); });
    }

    /// Drive the incremental GC to a true fixpoint.
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const size_t rounds = runGcToFixpoint(gc);

    const FsckReport after = runFsck(*s, /*detail=*/false);

    /// INV-NO-LOSS holds: treeB and its blobs are reachable, nothing dangles.
    EXPECT_EQ(after.dangling, 0u) << "displacement must never lose a reachable object";
    EXPECT_GT(after.reachable, 0u) << "the live ref points at treeB; treeB's closure is reachable";

    /// THE B140 ASSERTION (RED today): treeA's two unique orphaned blobs must be reclaimed by the
    /// time the GC reaches a fixpoint. They are not — the displaced_later skip never recorded their
    /// edges, so the incremental GC cannot see them. unreachable == 2 today (data-A, mark-A).
    EXPECT_EQ(after.unreachable, 0u)
        << "B140: displaced-and-vanished treeA's blobs leaked after " << rounds
        << " GC rounds reached a fixpoint (reachable=" << after.reachable
        << ", unreachable=" << after.unreachable << "); the fold's displaced_later skip "
        << "(CasGc.cpp :771-773) never recorded treeA's blob edges into the snap";
}

/// REUSE-vs-GC race (2026-06-17 soak investigation): models the user's hypothesis — "reuse of an
/// object that is being deleted". A build ADOPTS a committed blob B's token (B present, not yet
/// condemned), the committed ref pinning B is DROPPED, GC retires+deletes B AND COMPLETES the round
/// (dropping B's retired set), and only THEN does the build publish a manifest naming B.
///
/// The publish gate (Build::publish) refreshes the retire-view and runs `checkAndResolveDeps` (which
/// re-HEADs and throws on a deleted dep) ONLY when `retireView().round() < max(shard.fence_round, registry_fence)`.
/// GC's R3 fences every shard every round, so the shard fence_round keeps advancing past the build's
/// adopt-time view — which SHOULD force the refresh+re-HEAD and catch the deletion (ABORTED→retry).
/// This test asserts that protection holds end-to-end for the local single-namespace path:
/// `dangling==0`. If it FAILS, the local gate has the gap the soak hit; if it PASSES, the local path
/// is safe and the soak's dangling lives elsewhere (cross-node replication fetch-relink reuse).
TEST(CasReuseGcRace, ReuseOfBlobDeletedBeforePublish)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String B = "shared-blob-payload";
    const String U = "build2-unique-blob";

    /// build1: commit part_1 -> T1 -> blob B.
    {
        auto build1 = s->startBuild({});
        build1->putBlob(idOf(B), BlobSource::fromString(B));
        TreeEntry e;
        e.name = "data.bin";
        e.placement = Placement::Blob;
        e.file_hash = u128Of(B);
        e.file_size = B.size();
        const TreeId t1 = build1->putTree({e});
        build1->publish(ns, "part_1", t1, {});
    }

    /// build2: adopt B via tokenless evidence (replaces the former reuseBlob(false)); build2 also
    /// uploads its OWN unique blob U so its tree is distinct and U is build2-owned
    /// (heartbeat-protected) — only B is at risk of being collected.
    auto build2 = s->startBuild({});
    TreeEntry eb;
    eb.name = "data.bin";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of(B);
    eb.file_size = B.size();
    build2->adoptEvidence(eb);   /// tokenless dep (no HEAD) — replaces reuseBlob(false)
    build2->putBlob(idOf(U), BlobSource::fromString(U));
    TreeEntry eu;
    eu.name = "uniq.bin";
    eu.placement = Placement::Blob;
    eu.file_hash = u128Of(U);
    eu.file_size = U.size();
    const TreeId tree2 = build2->putTree({eb, eu});

    /// Drop the committed pin on B and advance the watermark so B (owned by the finished build1) is
    /// not spared. build2 has NOT published, so B is unreferenced in the journal GC folds.
    s->dropRef(ns, "part_1");
    s->renewWatermarkOnce();

    /// GC reclaims T1 and B to a fixpoint, completing the rounds (dropping their retired sets).
    Gc gc(s, u128Of("gc-reuse-race"));
    runGcToFixpoint(gc);
    ASSERT_FALSE(b->head(s->layout().blobKey(BlobId{u128ToHex(u128Of(B))})).exists)
        << "GC must have deleted the now-unreferenced reused blob B";

    /// build2 publishes part_2 -> tree2 -> {B, U}. The gate must NOT commit a ref to the deleted B.
    /// publish may throw a retryable ABORTED if the gate re-observes the loss — that is CORRECT
    /// (no dangle); only a SILENT commit of a dangling ref is the bug. Drive any retry to convergence.
    bool published = false;
    for (int attempt = 0; attempt < 8 && !published; ++attempt)
    {
        try
        {
            build2->publish(ns, "part_2", tree2, {});
            published = true;
        }
        catch (const DB::Exception & e)
        {
            /// ABORTED = the gate re-observed the lost dep and asked us to retry (re-upload B's body).
            /// The tokenless adopt has no body; a real caller would re-derive it from source.
            /// Here we just confirm the gate did NOT silently dangle. A non-ABORTED throw is unexpected.
            if (e.code() != DB::ErrorCodes::ABORTED)
                throw;
            break;   /// fail-closed (no dangle) is the safe outcome; stop.
        }
    }

    /// THE ASSERTION: no reachable object is missing. A dangling B == the soak's INV-NO-LOSS finding.
    const FsckReport rep = runFsck(*s, /*detail=*/true);
    EXPECT_EQ(rep.dangling, 0u)
        << "reuse adopted a token GC later deleted; the publish gate must resurrect/re-observe B (or "
           "fail closed), never commit a dangling ref (dangling=" << rep.dangling
        << ", reachable=" << rep.reachable << ")";
}
