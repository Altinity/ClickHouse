#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <iostream>
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
/// blob edges are recorded into the durable snap ONLY by EXPANDING the tree — reading the tree
/// object on the first folded `Add` and calling addTreeEdge per entry. When that read
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

size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        /// A round that only STRIPPED a tree's child edges or FORGOT an absent node has not reached a
        /// fixpoint: the children it freed (cascade cadence) are deleted only by a SUBSEQUENT round.
        /// The normal delete-cascade keeps this loop alive via `rep.deleted > 0`, but the retire-time
        /// absent-tree path (B199-S1) deletes nothing in the stripping round — it forgets the vanished
        /// tree and strips its edges — so we must also continue while there is pending cascade work
        /// (`cascaded`) or an in-memory forget that the next round acts on (`forgotten_absent`).
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0
            && rep.cascaded == 0 && rep.forgotten_absent == 0)
            break;
    }
    return rounds;
}

/// Publish one ref naming a two-blob tree through the REAL precommit-first transaction sequence —
/// the exact order ContentAddressedTransaction::publishStaging drives (CAT.cpp:241-276):
///
///   recordPendingBlobDep(each blob)   // CAT registers the tokenless pending-blob dep BEFORE stage,
///                                      // so stageTree's W-TREE-BUILD presence check passes with the
///                                      // bodies not yet uploaded (CAT.cpp:558)
///   stageTree(entries)                // LOCAL ONLY — encodes + retains the tree, no upload
///   precommit(tree)                   // phase 1: durable build-root ref `<server>/_precommits`/build_seq
///                                      // -> tree, a UNIQUE non-displaced ref pinning the closure
///   uploadStagedTree(tree)            // pool write #1 — the tree object
///   putBlob(each blob)                // pool writes #2 — the bodies, from in-memory BlobSource
///   publish(ns, ref, tree, {})        // phase 2: the table ref Add, THEN the precommit edge is
///                                      // REMOVED-ON-COMMIT inside publish (CasBuild.cpp:1021-1074:
///                                      // mutateShard on the precommit shard erases refs[build_seq]
///                                      // and appends a Remove journal record). This is the real
///                                      // precommit-removal step, replicated exactly by going through
///                                      // Build::publish rather than hand-rolling it.
///
/// Same ref, two TreeEntry blobs (data.bin / data.cmrk3), but every byte of the closure is born under
/// treeA's own unique precommit ref first — the production precommit-first order. Returns the tree id.
TreeId publishPart2Precommit(
    const StorePtr & s, const RootNamespace & ns, const String & ref,
    const String & payload_a, const String & payload_b)
{
    auto build = s->startBuild({});

    /// CAT records a tokenless pending-blob dep for each staged blob BEFORE stageTree (the bodies are
    /// uploaded only post-precommit), so stageTree's W-TREE-BUILD check is satisfied.
    build->recordPendingBlobDep(u128Of(payload_a), payload_a.size());
    build->recordPendingBlobDep(u128Of(payload_b), payload_b.size());

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

    const TreeId tree = build->stageTree({ea, eb});   /// LOCAL stage (no upload)
    build->precommit(tree);                           /// phase 1: durable build-root edge
    build->uploadStagedTree(tree);                    /// pool write #1: the tree object
    build->putBlob(idOf(payload_a), BlobSource::fromString(payload_a));   /// pool writes #2: bodies
    build->putBlob(idOf(payload_b), BlobSource::fromString(payload_b));
    build->publish(ns, ref, tree, {});                /// phase 2: table ref Add + remove precommit edge
    return tree;
}

/// Reproduce the original raw-API displacement + competing-delete on the SAME (s, ns, ref) and run GC
/// to fixpoint. treeB's distinct blobs displace treeA's via a second Add for the same ref name with no
/// Remove between; treeA's tree object is then removed (deleteExact-by-token) exactly as a competing
/// GC round's landed delete would. Asserts dangling==0 and RETURNS the fsck report so the caller can
/// report `unreachable` (RED if > 0). The publishes are passed in so a caller can interleave a GC fold.
FsckReport displaceDeleteAndGc(
    const StorePtr & s, const std::shared_ptr<InMemoryBackend> & b,
    const RootNamespace & ns, const String & ref, TreeId tree_a)
{
    /// Re-publish the SAME ref to treeB (distinct blobs): last-op-wins displacement of treeA's edge.
    publishPart2Precommit(s, ns, ref, "data-B", "mark-B");

    /// A competing GC round already deleted the displaced treeA object: remove it at the BACKEND so
    /// GC's retire-time HEAD-observe sees it absent. (We assert absence at the backend, not via
    /// s->readTree: the Store keeps a content-addressed immutable tree_cache (CasStore.cpp :449), so a
    /// prior fold that read treeA would still return it from cache — the cache is irrelevant to GC's
    /// retire path, which HEADs the backend object key directly.)
    {
        const String tree_a_key = s->layout().treeKey(tree_a);
        const auto head = b->head(tree_a_key);
        EXPECT_TRUE(head.exists) << "treeA object must exist before we delete it";
        EXPECT_EQ(b->deleteExact(tree_a_key, head.token).kind, DeleteOutcome::Kind::Deleted);
        EXPECT_FALSE(b->head(tree_a_key).exists) << "treeA backend object must be gone after the delete";
    }

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    runGcToFixpoint(gc);
    return runFsck(*s, /*detail=*/false);
}

}

/// B199-S2 Task-7 regression: GC must NOT throw `LOGICAL_ERROR` when folding a precommit `Add` whose
/// inline closure is EMPTY. Both production adopt paths precommit an ADOPTED (unstaged) root, which
/// `buildStagedClosure` leaves empty: replication relink (`adoptTree` + `precommit`) and rename/move
/// (`republishRef` → `adoptEvidence` + `precommit`). The Task-5 fold added an empty-closure guard that
/// THREW under the precommit namespace, so GC broke on the first fold over such an `Add`.
///
/// REACHABILITY (the subtle part the single-server wiring harness CANNOT exercise): the empty-closure
/// guard fires only when the adopted root is still UN-EXPANDED in the shared snap when its precommit
/// `Add` folds. On one server the staged build that originally created the tree leaves a non-empty
/// precommit `Add` that expands the tree FIRST, so a same-server re-adopt always sees it expanded. The
/// genuine trigger is RECEIVER-side replication: a second server adopts a tree BUILT ELSEWHERE, so its
/// pool has no prior expanding `Add` for that tree, and its precommit `Add` (empty closure) folds
/// un-expanded. Modelled here as two `Store`s over ONE shared backend (sender A, receiver B); B's
/// precommit namespace sorts before A's so B's adopt `Add` folds first, against the un-expanded tree.
///
/// Fixed by the unified fold source: an empty closure ⇒ `readTree` the adopted root (kept present by
/// the live source it was adopted from) instead of throwing. The fold expands it and GC proceeds.

/// Sender A commits a tree; receiver B (a second Store over the SAME backend) ADOPTS it (`adoptTree` +
/// `precommit` + `publish`, exactly `adoptPart`). B's GC must fold B's adopted-root precommit `Add`
/// (empty closure, un-expanded tree) without throwing, and reclaim nothing erroneously (dangling==0).
TEST(CasGcAdoptedPrecommit, RelinkAdoptedRootFoldDoesNotThrow)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto sa = Store::open(b, PoolConfig{.pool_prefix = "p", .server_id = hexToU128("ffffffffffffffffffffffffffffffff")});
    auto sb = Store::open(b, PoolConfig{.pool_prefix = "p", .server_id = hexToU128("00000000000000000000000000000001")});

    const TreeId t = publishPart2Precommit(sa, RootNamespace{"srv_a/tbl"}, "all_1_1_0", "data-A", "mark-A");

    /// Receiver B adopts the committed tree (present in the shared backend) and publishes its own ref —
    /// the relink. `adoptTree` does not stage the tree, so B's precommit `Add` carries an empty closure.
    auto build_b = sb->startBuild({});
    build_b->adoptTree(t);
    build_b->precommit(t);
    build_b->publish(RootNamespace{"srv_b/tbl"}, "all_1_1_0", t, {});

    Gc gc(sb, hexToU128("00000000000000000000000000000099"));
    EXPECT_NO_THROW(runGcToFixpoint(gc))
        << "GC must fold the adopted-root precommit Add (empty inline closure) by reading the committed "
        << "tree, not throw LOGICAL_ERROR";

    const FsckReport after = runFsck(*sb, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "the adopted closure stays reachable — no data loss";
    EXPECT_EQ(after.unreachable, 0u) << "no space leak — the adopted tree's blobs are reachable, not orphaned";
    /// Both refs resolve to T and its blobs are reachable.
    EXPECT_TRUE(sb->resolveRef(RootNamespace{"srv_b/tbl"}, "all_1_1_0").has_value());
}

/// Rename/move path: `republishRef` adopts the source tree via `adoptEvidence` (tokenless) and
/// precommits it — also an ADOPTED root with an empty inline closure. Same shared-backend, sender/
/// receiver shape so the rename's precommit `Add` folds against an un-expanded tree.
TEST(CasGcAdoptedPrecommit, RepublishAdoptedRootFoldDoesNotThrow)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto sa = Store::open(b, PoolConfig{.pool_prefix = "p", .server_id = hexToU128("ffffffffffffffffffffffffffffffff")});
    auto sb = Store::open(b, PoolConfig{.pool_prefix = "p", .server_id = hexToU128("00000000000000000000000000000001")});

    const TreeId t = publishPart2Precommit(sa, RootNamespace{"srv_a/tbl"}, "all_1_1_0", "data-A", "mark-A");

    /// Mirror `republishRef`: adopt the tree by tokenless evidence, precommit, publish under the
    /// destination ref. The adopted root is not staged, so the precommit `Add`'s inline closure is empty.
    auto build_b = sb->startBuild({});
    TreeEntry tree_evidence;
    tree_evidence.placement = Placement::Subtree;
    tree_evidence.file_hash = hexToU128(t.string());
    tree_evidence.file_size = 0;
    build_b->adoptEvidence(tree_evidence);
    build_b->precommit(t);
    build_b->publish(RootNamespace{"srv_b/tbl"}, "delete_tmp_all_1_1_0", t, {});

    Gc gc(sb, hexToU128("00000000000000000000000000000099"));
    EXPECT_NO_THROW(runGcToFixpoint(gc))
        << "GC must fold the republished adopted-root precommit Add (empty inline closure) without throwing";

    const FsckReport after = runFsck(*sb, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "the renamed/republished closure stays reachable — no data loss";
    EXPECT_EQ(after.unreachable, 0u) << "no space leak — the republished tree's blobs are reachable, not orphaned";
    EXPECT_TRUE(sb->resolveRef(RootNamespace{"srv_b/tbl"}, "delete_tmp_all_1_1_0").has_value());
}

/// B199-S2 Task-7 follow-up regression (point #4): per-node 404 tolerance must NOT discard the STAGED
/// portion of a closure when a deeper ADOPTED subtree's object is transiently absent. A build STAGES a
/// parent `P` (in the inline closure) whose entries reference an ADOPTED subtree `C` (tokenless
/// evidence — NOT staged, NOT in the closure) plus `P`'s OWN unique blob; `C`'s object is absent at
/// fold (never uploaded — a relink/rename race). The build is then ABANDONED.
///
/// The Task-7 first cut used a single `pending_incomplete` bool: any tolerated 404 (here `C`) discarded
/// ALL buffered marks/edges, so `P`'s staged edge `P→blob` was dropped, `P` never expanded, and on
/// abandon the cascade freed nothing — `P`'s OWN unique blob leaked (the exact S2 class we close).
/// With per-node tolerance, `P`'s staged edges/mark survive (only `C` is left pending/unexpanded), so
/// the abandon cascade strips `P`'s edges and reclaims `P`'s blob: `unreachable == 0`.
TEST(CasGcAdoptedPrecommit, StagedParentWithAbsentAdoptedSubtreeReclaimsOwnBlobs)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);

    /// Compute an adopted subtree `C`'s tree id WITHOUT uploading its object (a throwaway local stage):
    /// `C`'s object is therefore absent in the pool, so `readTree(C)` 404s at fold time.
    TreeId c_tree;
    {
        auto throwaway = s->startBuild({});
        throwaway->recordPendingBlobDep(u128Of("c-blob"), 6);
        TreeEntry ce;
        ce.name = "c.bin";
        ce.placement = Placement::Blob;
        ce.file_hash = u128Of("c-blob");
        ce.file_size = 6;
        c_tree = throwaway->stageTree({ce});   /// LOCAL only — C's object is never uploaded
        throwaway->abandon();
    }

    /// The real build: adopt `C` by tokenless evidence (no presence assertion), then STAGE a parent `P`
    /// referencing `C` as a Subtree plus `P`'s OWN unique blob `p-blob`. precommit + upload P's object +
    /// putBlob P's body — but NEVER publish (the build is abandoned below).
    auto build = s->startBuild({});
    TreeEntry c_evidence;
    c_evidence.placement = Placement::Subtree;
    c_evidence.file_hash = hexToU128(c_tree.string());
    c_evidence.file_size = 0;
    build->adoptEvidence(c_evidence);               /// C is adopted — NOT staged, so NOT in the closure
    build->recordPendingBlobDep(u128Of("p-blob"), 6);

    TreeEntry pe_sub;
    pe_sub.name = "child";
    pe_sub.placement = Placement::Subtree;
    pe_sub.file_hash = hexToU128(c_tree.string());
    pe_sub.file_size = 0;
    TreeEntry pe_blob;
    pe_blob.name = "p.bin";
    pe_blob.placement = Placement::Blob;
    pe_blob.file_hash = u128Of("p-blob");
    pe_blob.file_size = 6;

    const TreeId p_tree = build->stageTree({pe_sub, pe_blob});   /// P is STAGED → in the inline closure
    build->precommit(p_tree);
    build->uploadStagedTree(p_tree);                              /// P's object present
    build->putBlob(idOf("p-blob"), BlobSource::fromString("p-blob"));   /// P's own unique blob present
    build->abandon();                                            /// no commit — the build is abandoned

    /// Build finished (abandoned) → advance min_active past its seq so GC reclaims the orphaned precommit.
    s->renewWatermarkOnce();

    /// Drive enough rounds for the full reclaim cascade: round 0 folds P's precommit Add (per-node
    /// tolerance records P's staged edges, C left pending) and reclaims the abandoned ref (appends a
    /// Remove); a later round folds the Remove, retires P, and the cascade frees + deletes P's blob.
    /// (runGcToFixpoint's heuristic would stop after round 0, whose RETIRE ran on the pre-reclaim snap
    /// and reports zero work — so use a fixed adequate round count instead.)
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    EXPECT_NO_THROW({
        for (int i = 0; i < 8; ++i)
            gc.runRegularRound();
    });

    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "INV-NO-LOSS: nothing reachable was lost";
    EXPECT_EQ(after.unreachable, 0u)
        << "P's OWN unique blob (p-blob) must be reclaimed despite the adopted subtree C's object being "
        << "absent at fold — per-node tolerance keeps P's staged edges so the abandon cascade frees "
        << "P's blob; unreachable=" << after.unreachable;
}

/// The displaced-and-vanished-tree leak repro that USED to live here (`DisplacedUnexpandedTreeBlobsLeak`)
/// drove `build->publish(...)` DIRECTLY with NO precommit — a path production never takes. It has been
/// rewritten to run through the GENUINE `ContentAddressedTransaction`/wiring entry point as
/// `CaWiringGc.DisplacedTreeBlobsReclaimedThroughRealPath` (`gtest_ca_wiring.cpp`), where the
/// precommit-first B199-S2 inline-closure fold + B199-S1 retire-strip reclaim treeA's blobs. The two
/// deterministic Build-level precommit repros below (`..._S1_FoldBetween` / `..._S2_NoFoldBetween`)
/// stay as focused unit-level coverage of the same fix.

/// DIAGNOSTIC (2026-06-23): is the B140 leak reachable through the REAL precommit-first transaction
/// path, or is it only a raw-publish-API hazard that precommit-first prevents? The original
/// `DisplacedUnexpandedTreeBlobsLeak` above drives `build->publish(...)` DIRECTLY (no precommit), but
/// the real `ContentAddressedTransaction::publishStaging` ALWAYS goes precommit-first
/// (stageTree → precommit → uploadStagedTree → putBlob → publish), and `publish` REMOVES the precommit
/// edge on commit (CasBuild.cpp:1021-1074). `publishPart2Precommit` above replays that exact sequence.
///
/// HYPOTHESIS (going in): because treeA is born under its OWN unique, non-displaced precommit ref
/// (name = build_seq), GC's fold expands treeA via the precommit Add (recording treeA→{data-A, mark-A})
/// before treeA can be displaced/deleted, so the cascade reclaims treeA's blobs ⇒ unreachable==0.
///
/// EMPIRICAL VERDICT (both scenarios RED, unreachable==2): the leak IS reachable through the
/// precommit-first path — precommit does NOT save us. The hypothesis was HALF right and instructive:
/// the precommit ref DOES make treeA get expanded (an event-sink probe confirmed `tree_expand` for
/// treeA driven by the `_precommits` Add — CasGc.cpp foldShardRecords :1233-1350 — so treeA→data-A and
/// treeA→mark-A ARE recorded in the snap, with both blobs seeded into `known`). The `displaced_later`
/// expansion skip (:1261-1305) is therefore NOT the operative mechanism in this path — it is the raw
/// test's mechanism, not this one.
///
/// The leak was one pipeline stage LATER, in retire (CasGc.cpp). After the precommit Remove and the
/// table-ref displacement, treeA hits in-degree 0 and becomes a zero-in-degree retire candidate. The
/// competing delete already removed treeA's OBJECT, so retire's HEAD-observe sees it absent — B199-S1
/// landed the fix here: the retire absent-tree branch now `stripTree`s treeA (releasing its recorded
/// child edges) BEFORE forgetting it, so treeA→{data-A, mark-A} are decremented to in-degree 0 and the
/// children surface as candidates and are reclaimed.
///
/// GREEN (B199-S1 + B199-S2): the precommit Add carries treeA's closure INLINE on the journal record,
/// so the fold records treeA→{data-A, mark-A} from the recorded closure even when the tree object has
/// vanished — no readTree — and the B199-S1 retire strip then releases them. unreachable==0.
///
/// S1 (contrast): one GC fold runs AFTER treeA's precommit/commit but BEFORE the displacement, so
/// treeA is unambiguously expanded with its object present; the B199-S1 retire strip reclaims its
/// children. (Note: that first fold also populates the Store's content-addressed tree_cache for treeA,
/// so the post-delete `readTree` sanity 404 in the helper is a non-fatal EXPECT, not a hard assertion.)
TEST(CasGcLeak, DisplacedUnexpandedTreeBlobsLeakPrecommitPath_S1_FoldBetween)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    /// treeA born under its own unique precommit ref, fully committed (table ref + precommit removed).
    const TreeId tree_a = publishPart2Precommit(s, ns, ref, "data-A", "mark-A");

    /// CONTRAST: a GC fold runs HERE, before any displacement — treeA's object is still present, so the
    /// fold expands treeA and records its blob edges into the durable snap.
    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        runGcToFixpoint(gc);
    }

    const FsckReport after = displaceDeleteAndGc(s, b, ns, ref, tree_a);

    EXPECT_EQ(after.dangling, 0u) << "S1: displacement must never lose a reachable object";
    EXPECT_GT(after.reachable, 0u) << "S1: the live ref points at treeB; treeB's closure is reachable";

    /// REPORT (not silently): an interleaved fold should have expanded treeA, so its blobs are reclaimed.
    std::cout << "[CA-DIAG] S1 (fold between): unreachable=" << after.unreachable
              << " reachable=" << after.reachable << " dangling=" << after.dangling
              << "  => " << (after.unreachable == 0 ? "GREEN" : "RED") << std::endl;
    EXPECT_EQ(after.unreachable, 0u)
        << "S1: an interleaved GC fold expanded treeA via its precommit ref, yet treeA's unique blobs "
        << "still leaked — when the vanished-object treeA hit in-degree 0, retire's P9 forget "
        << "(CasGc.cpp :1036) dropped it from `known` WITHOUT stripping its child edges, so data-A / "
        << "mark-A never reached in-degree 0 (unreachable=" << after.unreachable << ")";
}

/// S2 (DECISIVE, faithful to the original): NO GC fold between treeA's build and the treeB
/// displacement + treeA-deletion. This is the worst case — exactly the original test's "no GC round
/// interleaved" — but driven through the precommit-first path. treeA's precommit Add and its
/// precommit Remove (removed-on-commit) and the treeB displacement and treeA's object-delete are all
/// pending when the single GC fold finally runs.
///
/// GREEN (B199-S2): the worst case — treeA's object is ALREADY deleted when the single fold runs — is
/// now closed. The precommit Add carries treeA's closure INLINE on the journal record, so the fold
/// records treeA→{data-A, mark-A} from the recorded closure WITHOUT any readTree (no 404, no skip), and
/// the B199-S1 retire absent-tree strip then releases those edges so the children reach in-degree 0 and
/// are reclaimed. unreachable==0; dangling==0 throughout (no data loss).
TEST(CasGcLeak, DisplacedUnexpandedTreeBlobsLeakPrecommitPath_S2_NoFoldBetween)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    /// treeA born under its own unique precommit ref, fully committed — but NO GC fold runs before the
    /// displacement + delete below (the decisive worst case).
    const TreeId tree_a = publishPart2Precommit(s, ns, ref, "data-A", "mark-A");

    const FsckReport after = displaceDeleteAndGc(s, b, ns, ref, tree_a);

    EXPECT_EQ(after.dangling, 0u) << "S2: displacement must never lose a reachable object";
    EXPECT_GT(after.reachable, 0u) << "S2: the live ref points at treeB; treeB's closure is reachable";

    std::cout << "[CA-DIAG] S2 (no fold between): unreachable=" << after.unreachable
              << " reachable=" << after.reachable << " dangling=" << after.dangling
              << "  => " << (after.unreachable == 0 ? "GREEN" : "RED") << std::endl;
    EXPECT_EQ(after.unreachable, 0u)
        << "S2: B199-S2 should reclaim treeA's blobs even though its object was already deleted when the "
        << "single fold ran — the precommit Add's INLINE closure records treeA→{data-A, mark-A} without a "
        << "readTree, and the B199-S1 retire strip releases them; treeA's unique blobs must be reclaimed "
        << "(unreachable=" << after.unreachable << ")";
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
