#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include "cas_test_helpers.h"

#include <mutex>
#include <set>

/// THE DESTRUCTIVE-ROUND FRONTIER PROOF (spec 2026-07-27 "ref chain complete cut" §5).
///
/// Reachability is a property of the WHOLE POOL. A blob is unreferenced only if no namespace anywhere
/// owns an edge to it, so a round that deletes one is asserting something about every namespace at
/// once -- including the ones it never looked at. Task 7 made the per-namespace half of that assertion
/// cheap and exact: one `GET` at the cursor's arithmetic successor, absent means end-of-stream. Task 8
/// made a namespace that could NOT be walked say so durably. What neither can supply is the SET those
/// proofs have to cover, and that is what this task is about.
///
/// So the gate has three terms, and a round destroys only when all three are clear:
///
///     suppress_destructive = any anomaly this round
///                          OR any hold the seal carries
///                          OR the frontier is incomplete
///
/// The second term is STRUCTURAL. Every hold recorded today also records an anomaly, so the first term
/// happens to imply it -- but the invariant is the hold SET, not that coincidence, and the gate reads
/// the seal directly so that a future change to anomaly recording cannot quietly open it.
///
/// The third term is where Stage A admits what it cannot do. The universe is
/// `(sealed cursors) ∪ (this round's hint)`, two sources that can BOTH omit a namespace at once, and
/// the scenario that makes that fatal is the one these tests open with: a hidden acked `+1` in a
/// namespace neither source names, while a visible `-1` elsewhere drives the shared blob's OBSERVABLE
/// in-degree to zero. Every proof the round holds comes back clean and the blob is still owned. So
/// `UniversePolicy::kDefault` is `StageA_Suppressed` and production destroys NOTHING this stage; the
/// per-namespace logic below is reached only by a test that has constructed a closed universe and says
/// so. Stage B's catalog makes the universe knowable and its Task 7b flips that one constant.
///
/// Everything here is written so that flip is a source change and not a redesign: the per-namespace
/// proofs, the quiet-namespace probes, the budget, and every gated site are live and tested now.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// A backend that serves every key by exact GET while HIDING selected keys from every LIST -- the
/// observed lying-store shape, and the only way to build the cross-namespace scenario: the hidden
/// namespace's records stay durable and readable, so a round that KNOWS to look for them finds them,
/// while a round that only enumerates never learns they exist.
class HintHoleBackend : public CountingBackend
{
public:
    /// Hide every key under `prefix` from LIST. Prefix-based rather than per-key so a namespace can be
    /// hidden wholesale, including the objects a later publish adds.
    void hidePrefix(const String & prefix)
    {
        std::lock_guard lock(m);
        hidden_prefixes.push_back(prefix);
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        std::lock_guard lock(m);
        if (hidden_prefixes.empty())
            return page;
        std::erase_if(page.keys, [&](const ListedKey & k)
        {
            for (const String & hidden : hidden_prefixes)
                if (k.key.starts_with(hidden))
                    return true;
            return false;
        });
        return page;
    }

private:
    mutable std::mutex m;
    std::vector<String> hidden_prefixes;
};

/// A pool whose GC frontier-probe budget is set explicitly. Everything else matches `openPoolForTest`.
PoolPtr openPoolWithProbeBudget(std::shared_ptr<InMemoryBackend> backend, uint64_t budget)
{
    return Pool::open(std::move(backend),
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_frontier_probe_budget = budget, .gc_fold_max_defer_rounds = 0});
}

/// Publish `ref_name` in `ns` pinning `blob`, allocating the next ref-log id. Writes the blob body and
/// the manifest body too, so the published edge is one GC can actually fold.
ManifestRef publish(Backend & backend, const Layout & layout, const RootNamespace & ns,
                    const String & ref_name, uint64_t build_sequence, const DB::UInt128 & blob)
{
    const ManifestRef mref{.writer_epoch = 1, .build_sequence = build_sequence, .manifest_ordinal = 1};
    writeBlobBody(backend, layout, blob);
    writeManifestRaw(backend, layout, ns, mref, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(backend, layout, ns, ref_name, std::nullopt, mref);
    return mref;
}

/// The blob key for a raw hash, as the tests spell it.
String blobKeyOf(const Layout & layout, const DB::UInt128 & hash)
{
    return layout.blobKey(legacyMetaTestRef(hash));
}

/// The sealed fold cursor for `ns` as a full `RefTxnId`. Every seed here allocates `writer_epoch = 1`,
/// which is what `foldCursorOf` (returning the sequence alone) assumes too.
RefTxnId sealedCursorOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    return RefTxnId{1, foldCursorOf(backend, layout, ns, /*shard*/ 0)};
}

/// Drive `rounds` GC rounds under the given policy, renewing the store's watermark between them the way
/// the production scheduler does.
void drive(const PoolPtr & store, Gc & gc, int rounds, UniversePolicy policy)
{
    for (int i = 0; i < rounds; ++i)
    {
        gc.runRegularRound({}, /*allow_steal*/true, policy);
        store->renewWatermarkOnce();
    }
}

/// Every key the backend was asked to delete, rendered for a failing assertion's message.
String deletedKeysMessage(const CountingBackend & backend)
{
    String out;
    for (const String & key : backend.deletedKeys())
        out += "\n    " + key;
    return out.empty() ? String{" (none)"} : out;
}

}

/// ===================== THE KILL SHOT: A HIDDEN `+1` IN AN UNKNOWN NAMESPACE =====================
///
/// Two namespaces share one blob. `visible` publishes it and then drops it, so the round observes
/// `+1` then `-1` and reads the blob's in-degree as zero. `hidden` also owns it -- durably, acked,
/// readable by exact key -- but is absent from the round's LIST hint AND has never been folded, so it
/// has no sealed cursor either. Neither source names it, and every per-namespace proof the round can
/// take comes back clean.
///
/// This is the scenario the Stage-A constant exists for, and the three arms below are the whole
/// argument: the production default refuses; flipping the constant while the namespace is genuinely
/// outside the universe DELETES the blob (so the constant is load-bearing, not decorative); and once
/// the namespace is inside the universe, the probe finds the hidden `+1` and the blob is safe on the
/// per-namespace logic alone -- which is exactly what Stage B will rely on.

namespace
{
/// Build the shared-blob scenario. `hidden` owns `blob` and is hidden from every LIST; `visible`
/// publishes and drops it. Returns the pool.
PoolPtr buildCrossNamespaceScenario(const std::shared_ptr<HintHoleBackend> & backend,
                                    const RootNamespace & hidden, const RootNamespace & visible,
                                    const DB::UInt128 & blob, bool fold_hidden_first)
{
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    if (fold_hidden_first)
    {
        /// Give the hidden namespace a sealed cursor, WITHOUT folding the edge under test. It publishes
        /// an unrelated blob and one round folds that; from then on the namespace is in the universe via
        /// its cursor even after the hint stops naming it.
        ///
        /// The unrelated blob is what makes this arm mean anything: if the shared blob's `+1` had
        /// already been folded by the seeding round, the blob would survive on the DURABLE in-degree and
        /// the test would pass whether or not the round probes anything. Publishing it only AFTER the
        /// seal puts it strictly above the cursor, so the probe is the one and only thing that can find
        /// it.
        publish(*backend, layout, hidden, "seed_ref", 7, DB::UInt128(0x5eed));
        Gc seed(store, kGc);
        seed.runRegularRound();
        store->renewWatermarkOnce();
    }

    publish(*backend, layout, hidden, "kept_ref", 1, blob);
    backend->hidePrefix(layout.refsNamespacePrefix(hidden));

    const ManifestRef dropped = publish(*backend, layout, visible, "dropped_ref", 2, blob);
    dropRefTransition(*backend, layout, visible, "dropped_ref", dropped);
    return store;
}
}

TEST(CasGcFrontierGate, HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault)
{
    auto backend = std::make_shared<HintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/false);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::kDefault);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the blob a hidden namespace still owns must survive";
    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "the production default proves no frontier, so the round destroys NOTHING. Deleted:"
        << deletedKeysMessage(*backend);
}

/// THE CONSTANT IS LOAD-BEARING. Same pool, same hidden `+1`, and the ONLY difference is that the
/// caller asserts its universe is closed -- which here is a lie the test tells deliberately. The blob
/// is deleted. Without this arm the first test proves nothing: a gate that suppressed for some
/// unrelated reason would pass it too.
TEST(CasGcFrontierGate, TheSameHiddenPlusOneIsDeletedOnceTheUniverseIsDeclaredAuthoritative)
{
    auto backend = std::make_shared<HintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/false);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists)
        << "with the universe declared closed and the owner outside it, the blob IS deleted -- this is "
           "the data loss `StageA_Suppressed` withholds";
}

/// AND THE PER-NAMESPACE LOGIC IS WHAT SAVES IT. Identical to the arm above except that the hidden
/// namespace was folded once first, so it carries a sealed cursor and is therefore IN the universe even
/// though the hint has gone silent about it. The round probes its expected-next by exact key, finds the
/// record the listing hid, folds the `+1`, and the blob is never condemned.
TEST(CasGcFrontierGate, AKnownNamespaceIsProbedByExactKeyAndItsHiddenEdgeSavesTheBlob)
{
    auto backend = std::make_shared<HintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/true);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the cursor kept the namespace in the universe, so its frontier was probed and its edge folded";
}

/// ===================== THE PRODUCTION DEFAULT IS INERT, PROOFS OR NO PROOFS =====================
///
/// A pool with nothing hidden, nothing held, no anomaly, and every namespace walked to an honest
/// end-of-stream: every per-namespace proof is green. The production default still destroys nothing,
/// because the term it fails is the one about the SET, not about any namespace in it.
TEST(CasGcFrontierGate, ProductionDefaultDestroysNothingEvenWithEveryProofGreen)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0xdead);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    dropRefTransition(*backend, layout, ns, "ref_1", mref);

    Gc gc(store, kGc);
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::kDefault);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);
    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "no anomaly, no hold, every namespace proven -- and still inert, because Stage A cannot "
           "prove the universe. Deleted:" << deletedKeysMessage(*backend);

    /// The control: the very same pool reclaims once the universe is declared closed. Without it this
    /// test would also pass on a pool where nothing was ever condemnable.
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);
    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the pool WAS reclaimable; only the universe seam was holding it";
}

/// ===================== EVERY DESTRUCTIVE SITE, INDIVIDUALLY =====================
///
/// The inventory as an assertion. The pool below has real work waiting at every gated site: a
/// graduated blob to delete, an owner-removed manifest body to delete, aged generations to prune and
/// hand off, ref logs and snapshots covered by a durable snapshot, and a removed namespace with a
/// Pending cleanup item. A suppressed round issues ZERO deletes against ALL of them, and the per-site
/// assertions name which one leaked if any does.

namespace
{
/// A pool with destructive work pending at every site, plus a few completed rounds so generations have
/// aged past the retention floor. Returns the hash of a blob whose in-degree has dropped to zero.
DB::UInt128 buildPoolWithWorkAtEverySite(const std::shared_ptr<CountingBackend> & backend,
                                         const PoolPtr & store, Gc & gc)
{
    const Layout & layout = store->layout();
    const RootNamespace live{"00/live@cas@"};
    const RootNamespace doomed{"00/doomed@cas@"};
    const DB::UInt128 blob(0xfeed);

    /// A long-lived namespace that keeps publishing, so snapshots and covered logs accumulate and
    /// generations keep advancing past the retention floor.
    for (uint64_t i = 1; i <= 4; ++i)
    {
        publish(*backend, layout, live, "ref_" + std::to_string(i), i, DB::UInt128(0x1000 + i));
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    /// The condemnable blob: published in `doomed`, then dropped. Its manifest body becomes
    /// owner-removed cleanup work at the same time.
    const ManifestRef mref = publish(*backend, layout, doomed, "doomed_ref", 9, blob);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, doomed, "doomed_ref", mref);
    return blob;
}
}

TEST(CasGcFrontierGate, EveryInventoriedDestructiveSiteIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    /// From here on the rounds run on the production default: every site has work queued and every site
    /// must decline it.
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 6, UniversePolicy::kDefault);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "a suppressed round performs NO destructive work at any site. Deleted:"
        << deletedKeysMessage(*backend);

    /// And the same statement per site, so a failure names the leak rather than just its count.
    EXPECT_EQ(backend->deleteCountForKeysContaining("/blobs/"), 0u) << "pre-CAS blob delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u) << "manifest-body delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "generation prune and hand-off reclaim";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/refs/"), 0u)
        << "covered-log / superseded-snapshot cleanup";

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);

    /// The control again: the identical pool DOES reclaim at those sites once the universe is closed,
    /// so the zeros above are the gate at work and not an empty work queue.
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);
    EXPECT_GT(backend->deleteTotal(), 0u)
        << "the work queue was real -- an authoritative round drains it";
    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists);
}

/// The generation prune's cursor must not move on a suppressed round either. It is a monotone
/// high-water mark that the wholesale prune never revisits, so a cursor that advanced past a generation
/// this round declined to delete would strand that generation's whole prefix with no reclaimer left.
TEST(CasGcFrontierGate, ASuppressedRoundDoesNotAdvanceTheGenerationPruneCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    Gc gc(store, kGc);
    for (uint64_t i = 1; i <= 6; ++i)
    {
        publish(*backend, layout, ns, "ref_" + std::to_string(i), i, DB::UInt128(0x2000 + i));
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    const uint64_t pruned_through_before =
        decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through;

    for (uint64_t i = 7; i <= 10; ++i)
    {
        publish(*backend, layout, ns, "ref_" + std::to_string(i), i, DB::UInt128(0x2000 + i));
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through,
              pruned_through_before)
        << "the retention cursor is a high-water mark; it may not pass a generation nothing deleted";
}

/// THE HAND-OFF RECLAIM, WHICH THE INVENTORY TEST ABOVE CANNOT REACH. This site only fires for a
/// generation the wholesale prune SKIPPED while a live ref still pinned it (so the retention cursor
/// moved past it and will never revisit it) and which a later round's ref then moves off. Building that
/// takes a deliberately idle shard and a retention cursor driven past it, which is why it gets its own
/// test rather than riding on the inventory pool.
///
/// It is reachable under suppression precisely because FOLDING still happens on a suppressed round: the
/// ref moves off the old generation exactly as it would otherwise, and only the reclaim is withheld.
TEST(CasGcFrontierGate, TheHandOffReclaimIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_snap_generations_to_keep = 1, .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = publish(*backend, layout, ns, "tbl", 1, DB::UInt128(0xa1));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const uint64_t old_gen = decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_generation;
    const String old_prefix = layout.gcGenPrefix(old_gen);
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty());

    /// Idle-carry the ref until the retention cursor is strictly PAST its generation. Until then an
    /// ordinary prune could still reclaim it and the hand-off would not be the load-bearing path.
    for (int i = 0; i < 6; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    ASSERT_GT(decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through, old_gen)
        << "the generation must be behind the retention cursor before the hand-off is exercised";
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "and still retained, because a live ref pins it";

    /// A real delta moves the shard's run off the old generation. This is the round the hand-off would
    /// reclaim it on -- and it runs on the production default.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, DB::UInt128(0xb2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("data.bin", DB::UInt128(0xb2))});
    publishCommittedTransition(*backend, layout, ns, "tbl", r1, r2);

    backend->resetCounts();
    gc.runRegularRound();

    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "a suppressed round hands nothing off. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "the superseded generation's prefix survives a suppressed round intact";

    /// AND THE OPPORTUNITY IS CONSUMED, NOT DEFERRED -- the one place in this task where the gate
    /// costs something permanent, so it is asserted here rather than left to be discovered later.
    ///
    /// The hand-off is a one-shot DIFFERENCE: it compares the PARENT seal's runs against the new
    /// seal's, and the suppressed round above already folded the delta, so the next round's parent
    /// seal no longer mentions the old generation. Nothing revisits it -- the retention cursor is
    /// already past it and the prune never goes back. The prefix is left to `fsck`, which is exactly
    /// the outcome the site's own doc comment already records for a crash in the same window ("the
    /// cursor already advanced, so a plain retry will NOT re-attempt it; fsck is the backstop").
    /// Bounded (one small run per shard per occurrence) and not a correctness problem -- but in
    /// Stage A every round is suppressed, so every such transition leaks rather than one in a crash.
    ///
    /// The hand-off itself is not going untested: `CasGcRetention.HandOffDeletesSupersededRef` drives
    /// the same transition on an authoritative round and asserts the prefix IS reclaimed.
    runRegularRoundReclaiming(gc);
    EXPECT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "the hand-off is a one-shot difference: the suppressed round consumed it, so the prefix is "
           "now fsck's problem rather than a later round's";
}

/// THE ORPHAN-MANIFEST SWEEP, which the inventory pool above also cannot reach: it only deletes bodies
/// that no ref names AND whose build is provably dead by the durable watermark floor, so it needs a
/// pool seeded with exactly that -- orphan bodies and a floor above them.
///
/// It is gated with its CURSOR, not just its deletes. The cursor paces a cold-prefix enumeration and
/// nothing revisits a range it passed, so advancing it on a round that swept nothing would silently
/// skip that range forever. A suppressed round therefore declines the whole pass.
TEST(CasGcFrontierGate, TheOrphanManifestSweepAndItsCursorAreInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "gc-runner",
                   .manifest_sweep_list_budget_keys = 1, .manifest_sweep_delete_budget_keys = 1,
                   .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/aa@cas@"};

    /// Two manifest bodies no ref ever named, under a build the durable floor has already passed.
    const ManifestRef r1{.writer_epoch = 5, .build_sequence = 0xCA01, .manifest_ordinal = 1};
    const ManifestRef r2{.writer_epoch = 5, .build_sequence = 0xCA02, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", DB::UInt128(0xa1))});
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(0xb2))});
    setWatermarkMinActive(*backend, layout, "test", r1.writer_epoch, /*min_active*/ 0xCA03);
    /// The §6 deletion premise is a second precondition on the CONTROL arm below: a manifest of an
    /// epoch-`E` build is deletable only once the namespace's sealed fold cursor sits in an epoch
    /// strictly above `E`. Sealing that cursor here is what keeps this test about the GATE — without it
    /// the control arm would stop deleting for the premise's reason, and a removed gate would no longer
    /// show up as a difference between the two arms. A real round rewrites this row with the same cursor
    /// (the namespace is known, quiet and unheld, so the walk probes `cursor+1`, finds the frontier and
    /// carries the cursor), so the seeded fact survives every round below.
    seedFoldCursorForTest(*backend, layout, ns, RefTxnId{r1.writer_epoch + 1, 1});

    Gc gc(store, kGc);
    backend->resetCounts();
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u)
        << "a suppressed round sweeps nothing. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, r1})).exists);
    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, r2})).exists);
    EXPECT_TRUE(decodeGcState(backend->get(layout.gcStateKey())->bytes).manifest_sweep_cursor.empty())
        << "the sweep cursor must not advance over a range the round declined to sweep -- nothing "
           "revisits it";

    /// The control: the same orphans ARE swept once the universe is authoritative.
    for (int i = 0; i < 4; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    EXPECT_FALSE(backend->head(layout.manifestKey(ManifestId{ns, r1})).exists);
    EXPECT_FALSE(backend->head(layout.manifestKey(ManifestId{ns, r2})).exists);
}


/// ===================== QUIET NAMESPACES AND THE PROBE BUDGET =====================

/// THE TALLY ARITHMETIC, at a PARTIAL budget — the case neither 0 nor the default reaches.
///
/// `frontier_namespaces` is the denominator an operator reads as "the round's universe", and the
/// integration test reads it too. It has to describe the set the round actually SEALED: the walked
/// namespaces plus the ones the budget skipped whose cursors were carried, and nothing else. With
/// three quiet namespaces and a budget of one, the round probes exactly one of them, carries the other
/// two verbatim, and the published tally must be 3 = 1 proven + 2 unprobed — a denominator strictly
/// larger than the numerator, which is the shape that suppresses.
TEST(CasGcFrontierGate, APartialProbeBudgetPublishesATallyThatMatchesTheSealedSet)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolWithProbeBudget(backend, /*budget*/ 1);
    const Layout & layout = store->layout();
    const RootNamespace a{"00/quiet_a@cas@"};
    const RootNamespace b{"00/quiet_b@cas@"};
    const RootNamespace c{"00/quiet_c@cas@"};

    for (const RootNamespace & ns : {a, b, c})
        publish(*backend, layout, ns, "ref_1", 1, DB::UInt128(0x300 + ns.string().size()));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    for (const RootNamespace & ns : {a, b, c})
        ASSERT_NE(sealedCursorOf(*backend, layout, ns), (RefTxnId{})) << ns.string();

    /// All three go unhinted at once, so the budget of one cannot cover them.
    for (const RootNamespace & ns : {a, b, c})
        backend->hidePrefix(layout.refsNamespacePrefix(ns));

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    ASSERT_FALSE(intake.empty()) << "the intake phase must have emitted its row";
    EXPECT_EQ(intake["unhinted_quiet_walked"], 1u) << "the budget permits exactly one probe";
    EXPECT_EQ(intake["frontier_unprobed_budget"], 2u) << "the other two are not probed at all";
    EXPECT_EQ(intake["frontier_proven"], 1u) << "only the probed namespace can be proven";
    EXPECT_EQ(intake["frontier_namespaces"], 3u)
        << "the denominator is the SEALED set: 1 walked + 2 carried. It is derived from the rows the "
           "carry loop added, so it cannot describe a universe different from the seal";
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"])
        << "a partial budget must leave the frontier incomplete";

    /// And the seal really does carry all three rows — the denominator's claim, checked against the
    /// object it describes rather than against another counter.
    for (const RootNamespace & ns : {a, b, c})
        EXPECT_NE(sealedCursorOf(*backend, layout, ns), (RefTxnId{}))
            << "every namespace in the tally must have a sealed cursor: " << ns.string();
}

/// A namespace the hint has stopped mentioning but whose cursor the seal still carries costs exactly
/// ONE exact `GET` -- at its expected-next position -- and that `GET` coming back absent IS its
/// frontier proof.
TEST(CasGcFrontierGate, AQuietKnownNamespaceCostsExactlyOneExactGet)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};

    publish(*backend, layout, quiet, "ref_1", 1, DB::UInt128(0x11));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    const RefTxnId sealed = sealedCursorOf(*backend, layout, quiet);
    ASSERT_NE(sealed, (RefTxnId{})) << "the seeding round must have sealed a cursor to carry";

    /// Now the store stops listing the namespace entirely.
    backend->hidePrefix(layout.refsNamespacePrefix(quiet));
    backend->resetCounts();
    runRegularRoundReclaiming(gc);

    const String expected_next =
        layout.refLogKey(quiet, RefTxnId{sealed.writer_epoch, sealed.ref_sequence + 1});
    EXPECT_EQ(backend->getCount(expected_next), 1u)
        << "exactly one exact probe at the arithmetic successor of the sealed cursor";
}

/// A namespace that was WRONGLY quiet -- the hint hid a record that is durably there -- is walked this
/// round, not next: the probe finds the record and the walk continues from it.
TEST(CasGcFrontierGate, AWronglyQuietNamespaceIsWalkedTheSameRound)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const DB::UInt128 late_blob(0x77);

    publish(*backend, layout, quiet, "ref_1", 1, DB::UInt128(0x11));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const RefTxnId sealed_before = sealedCursorOf(*backend, layout, quiet);

    /// A second publish lands, and the store hides the namespace from every LIST at the same moment.
    publish(*backend, layout, quiet, "ref_2", 2, late_blob);
    backend->hidePrefix(layout.refsNamespacePrefix(quiet));

    runRegularRoundReclaiming(gc);

    EXPECT_LT(sealed_before, sealedCursorOf(*backend, layout, quiet))
        << "the probe found the hidden record, so the walk folded it and the cursor advanced";
    EXPECT_GT(inDegreeOf(*backend, layout, late_blob), 0)
        << "the hidden publish's edge folded this round -- the hint never mentioned it";
}

/// When the probe budget runs out before every known namespace has been reached, the round still
/// commits (cursors seal, the fold advances) and destroys NOTHING. The unprobed namespaces keep their
/// cursors: dropping one because a round ran out of budget would hand the next round a namespace to
/// re-fold from `{0, 0}`, which is far worse than the unproven frontier it already is.
TEST(CasGcFrontierGate, AnExhaustedProbeBudgetSealsCursorsAndDeletesNothing)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolWithProbeBudget(backend, /*budget*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const RootNamespace busy{"00/busy@cas@"};
    const DB::UInt128 blob(0xbeef);

    publish(*backend, layout, quiet, "quiet_ref", 1, DB::UInt128(0x11));
    const ManifestRef mref = publish(*backend, layout, busy, "busy_ref", 2, blob);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const RefTxnId quiet_cursor = sealedCursorOf(*backend, layout, quiet);
    ASSERT_NE(quiet_cursor, (RefTxnId{}));

    /// The quiet namespace goes unhinted, and the budget is zero, so it is never probed. The busy
    /// namespace meanwhile drops its ref, which would otherwise condemn and delete the blob.
    backend->hidePrefix(layout.refsNamespacePrefix(quiet));
    dropRefTransition(*backend, layout, busy, "busy_ref", mref);

    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "budget exhausted means an unproven frontier means no destruction. Deleted:"
        << deletedKeysMessage(*backend);
    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);
    EXPECT_EQ(sealedCursorOf(*backend, layout, quiet), quiet_cursor)
        << "the unprobed namespace's cursor rides verbatim -- it is never dropped";
    EXPECT_GT(decodeGcState(backend->get(layout.gcStateKey())->bytes).round, 1u)
        << "the round still commits; only its destructive half is withheld";
}

/// ===================== A CARRIED HOLD SUPPRESSES, STRUCTURALLY =====================
///
/// The gate's second term reads the HOLD SET off the seal it is about to make durable, instead of
/// relying on the fact that a hold also happens to record an anomaly. Proving that takes the one shape
/// where the two come apart: a hold DETECTED by an earlier round and merely CARRIED by this one, on a
/// round whose own walk ended quietly and recorded nothing.
///
/// Reaching it takes the lying store. Round 1 sees the gap at {1,3} below a LISTED {1,4} -- impossible
/// under contiguity -- and holds there. The store then stops listing {1,4}. Round 2's walk finds
/// nothing above the absent {1,3} (the carried hold sits AT {1,3}, and a position is not a witness
/// strictly above itself), so it reads an honest frontier and detects nothing at all. The hold rides
/// forward regardless, because a hold clears only by RESOLVING its position -- and it is the hold set,
/// not that round's silence, that has to keep the round from destroying.
TEST(CasGcFrontierGate, ACarriedHoldSuppressesOnARoundThatDetectedNothing)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace held{"00/held@cas@"};
    const RootNamespace busy{"00/busy@cas@"};
    const DB::UInt128 blob(0xbeef);

    /// {1,3} never existed while {1,4} is durable and listed.
    publish(*backend, layout, held, "ref_1", 1, DB::UInt128(0x21));
    publish(*backend, layout, held, "ref_2", 2, DB::UInt128(0x22));
    const ManifestRef orphan_ref{.writer_epoch = 1, .build_sequence = 4, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, DB::UInt128(0x24));
    writeManifestRaw(*backend, layout, held, orphan_ref, {blobEntryFor("data.bin", DB::UInt128(0x24))});
    RefLogTxn txn;
    txn.ns = held.string();
    txn.txn_id = RefTxnId{1, 4};
    txn.ops = publishCommittedOps("ref_4", orphan_ref);
    writeRefLogTxnRaw(*backend, layout, txn);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_EQ(sealedCursorOf(*backend, layout, held), (RefTxnId{1, 2}))
        << "round 1 must stop below the gap and hold there";

    /// The witness goes quiet. From here the walk of `held` detects nothing whatsoever.
    backend->hidePrefix(layout.refLogKey(held, RefTxnId{1, 4}));

    /// Meanwhile a blob elsewhere becomes condemnable, so the round has real destructive work to decline.
    const ManifestRef mref = publish(*backend, layout, busy, "busy_ref", 9, blob);
    dropRefTransition(*backend, layout, busy, "busy_ref", mref);

    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "a hold the seal still carries suppresses the round even when the round itself saw nothing. "
           "Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);
    EXPECT_EQ(sealedCursorOf(*backend, layout, held), (RefTxnId{1, 2}))
        << "and the quiet has not cleared the hold -- its position is still unresolved";
}

/// ===================== THE TEMPORAL LEMMA, ALL THREE ARMS =====================
///
/// The gate says WHEN a round may destroy. These say that even a round which may destroy cannot
/// destroy a blob some edge still owns, over the three interleavings that matter.

/// ARM (a): a `+1` that lands after this round's probes and is followed by the SAME round's
/// condemnation. Round pacing makes it safe on its own: an entry condemned at round K cannot graduate
/// before K+1 and cannot be deleted before K+2, so the round that condemns never deletes.
TEST(CasGcFrontierGate, ABlobCondemnedThisRoundIsNeverDeletedThisRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0xc04d);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    backend->resetCounts();
    runRegularRoundReclaiming(gc);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the condemning round must not also delete";
    EXPECT_EQ(backend->deleteCount(blobKeyOf(layout, blob)), 0u)
        << "not merely still present -- the delete was never attempted";
}

/// ARM (c) of the temporal lemma is the delete-site in-degree re-read, and it is NORMATIVE (spec §5,
/// third arm): an edge folded AFTER the condemnation but BEFORE the delete pass spares the blob
/// outright, `indeg > 0` winning over `delete_pending` past the floor. The other two arms bound WHEN
/// and WHAT a delete may remove; only this one asks whether the blob is still referenced at the moment
/// the pass decides.
TEST(CasGcFrontierGate, ALateEdgeSparesADeletePendingBlobAtTheDeleteSite)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0x1a7e);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    /// Condemn it, then graduate it to delete_pending.
    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    runRegularRoundReclaiming(gc);            /// condemn
    store->renewWatermarkOnce();
    runRegularRoundReclaiming(gc);            /// graduate: delete_pending published
    store->renewWatermarkOnce();

    /// A new owner appears BEFORE the delete pass. The pass recomputes the in-degree from the merge it
    /// just ran and finds it nonzero.
    const ManifestRef revived{.writer_epoch = 1, .build_sequence = 42, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, revived, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(*backend, layout, ns, "revived_ref", std::nullopt, revived);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the delete-site in-degree re-read spares a blob a fresh edge re-referenced";
    EXPECT_GT(inDegreeOf(*backend, layout, blob), 0);
}

/// ARM (b): a TOKENED adoption of an already-delete-pending blob. The writer's admit gate reads the
/// `Condemned` meta, refuses to adopt the dying incarnation, and rematerializes from its own source as
/// a FRESH incarnation -- so the delayed exact-token delete the previous round published finds a
/// different token and removes nothing. The blob's identity is preserved by re-upload, never by
/// reviving the condemned object.
TEST(CasGcFrontierGate, AResurrectedIncarnationSurvivesTheDelayedStaleTokenDelete)
{
    ensureBlobUploadPoolForTest();
    ensureCondemnedUploadAdmissionForTest();

    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// A REAL content-addressed blob, so the writer path below addresses exactly the object GC condemns.
    const String payload = "frontier-gate-resurrect-payload";
    const DB::UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);
    const String key = layout.blobKey(id);
    String raw_body(store->poolMeta().blob_header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*backend, layout, hash, raw_body);

    /// Publish and drop it so GC condemns and then graduates it to delete_pending.
    const ManifestRef mref{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, mref, {blobEntryFor("data.bin", hash)});
    publishCommittedTransition(*backend, layout, ns, "ref_1", std::nullopt, mref);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    runRegularRoundReclaiming(gc);            /// condemn: writes the durable Condemned meta
    store->renewWatermarkOnce();
    runRegularRoundReclaiming(gc);            /// graduate: publishes delete_pending against THIS token
    store->renewWatermarkOnce();

    const Token condemned_token = backend->head(key).token;
    const auto condemned_meta = loadMetaForTest(*backend, layout, hash);
    ASSERT_TRUE(condemned_meta.has_value());
    ASSERT_EQ(condemned_meta->meta.state, MetaState::Condemned)
        << "the delete GC is about to execute must be backed by durable Condemned evidence";

    /// A writer now adopts the blob through the REAL admit gate. It point-reads the Condemned meta,
    /// refuses to adopt the dying incarnation, and rematerializes from its OWN source bytes -- never by
    /// reading the condemned object. The key ends up holding a DIFFERENT incarnation.
    auto build = store->beginPartWrite({});
    const PutBlobResult uploaded = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(uploaded.ref, id);
    const Token fresh_token = backend->head(key).token;
    ASSERT_NE(fresh_token, condemned_token) << "a resurrect must displace the condemned incarnation";

    /// GC's delayed delete still names the OLD token. It cannot touch the new object.
    drive(store, gc, /*rounds*/ 2, UniversePolicy::AuthoritativeForTest);

    ASSERT_TRUE(backend->head(key).exists)
        << "the resurrected incarnation survives the delete published against its predecessor";
    EXPECT_EQ(backend->head(key).token, fresh_token) << "and it is still the writer's incarnation";
    EXPECT_EQ(backend->deleteExact(key, condemned_token).kind, DeleteOutcome::Kind::TokenMismatch)
        << "the condemned token can never remove the fresh object (INV-NO-RETURN)";
}

/// ARM (c): a TOKENLESS relink -- the receiver adopts by evidence, holding no token at all. Safety
/// then rests entirely on ORDER, so the operation journal has to show it: the receiver's `+1` is
/// durable BEFORE the source releases its own committed edge, and no point in the schedule leaves the
/// blob with zero durable owners.
TEST(CasGcFrontierGate, ATokenlessRelinkMakesTheReceiverEdgeDurableBeforeTheSourceReleases)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace source{"00/source@cas@"};
    const RootNamespace receiver{"00/receiver@cas@"};
    const DB::UInt128 blob(0x8e11);

    const ManifestRef source_ref = publish(*backend, layout, source, "part_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_GT(inDegreeOf(*backend, layout, blob), 0);

    /// The relink, in the only order the protocol permits: the receiver's manifest body and its
    /// committed edge first (tokenless -- it never HEADs the blob), and only afterwards the source's
    /// removal. Between the two writes the blob has TWO durable owners; it never has zero.
    const ManifestRef receiver_ref{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, receiver, receiver_ref, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(*backend, layout, receiver, "part_1", std::nullopt, receiver_ref);

    /// The round that observes ONLY the receiver's `+1` -- the exact midpoint of the schedule.
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_GE(inDegreeOf(*backend, layout, blob), 2)
        << "at the midpoint both owners are durable; the handoff never dips to zero";

    dropRefTransition(*backend, layout, source, "part_1", source_ref);
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the source released its edge only after the receiver's was durable, so nothing may collect it";
    EXPECT_EQ(inDegreeOf(*backend, layout, blob), 1)
        << "the receiver is the sole remaining owner";
}

/// ===================== CLEANUP RANGES ARE COMPUTED, NOT ENUMERATED =====================
///
/// `planRefCleanup` is pure, so the boundary arithmetic is pinned directly rather than inferred from a
/// round's side effects. The checkpoint can only ever TIGHTEN both boundaries.

TEST(CasGcFrontierGateCleanupRange, CoveredLogsStopAtTheMinimumOfCheckpointAndCursor)
{
    RefTableListing listing;
    listing.logs = {{1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}};
    listing.snapshots = {{1, 5}};

    /// No checkpoint: the boundary is min(newest snapshot, cursor) = {1,4}.
    const RefCleanupPlan without = planRefCleanup(listing, RefTxnId{1, 4}, {});
    EXPECT_EQ(without.deletable_logs, (std::vector<RefTxnId>{{1, 1}, {1, 2}, {1, 3}, {1, 4}}));

    /// A checkpoint BELOW the cursor tightens it to {1,2}.
    const RefCleanupPlan with = planRefCleanup(listing, RefTxnId{1, 4}, {}, std::nullopt, RefTxnId{1, 2});
    EXPECT_EQ(with.deletable_logs, (std::vector<RefTxnId>{{1, 1}, {1, 2}}))
        << "nothing above min(checkpoint, cursor) may be deleted";

    /// A checkpoint ABOVE both changes nothing: it can only tighten.
    const RefCleanupPlan ahead = planRefCleanup(listing, RefTxnId{1, 4}, {}, std::nullopt, RefTxnId{1, 9});
    EXPECT_EQ(ahead.deletable_logs, without.deletable_logs)
        << "a checkpoint ahead of the round's own observations never widens the range";
}

TEST(CasGcFrontierGateCleanupRange, ASnapshotAtTheCheckpointSurvivesAndOnlyStrictlyOlderOnesGo)
{
    RefTableListing listing;
    listing.logs = {{1, 1}, {1, 2}, {1, 3}};
    listing.snapshots = {{1, 1}, {1, 2}, {1, 3}};

    /// Without a checkpoint the newest snapshot is the boundary and the two older ones go.
    const RefCleanupPlan without = planRefCleanup(listing, RefTxnId{1, 3}, {});
    EXPECT_EQ(without.deletable_snapshots, (std::vector<RefTxnId>{{1, 1}, {1, 2}}));

    /// With the checkpoint AT {1,2}, only {1,1} is strictly below it. The snapshot the checkpoint names
    /// is the one a recovering reader samples, so it must survive its own cleanup.
    const RefCleanupPlan with = planRefCleanup(listing, RefTxnId{1, 3}, {}, std::nullopt, RefTxnId{1, 2});
    EXPECT_EQ(with.deletable_snapshots, (std::vector<RefTxnId>{{1, 1}}));

    /// The oldest checkpoint deletes nothing at all.
    const RefCleanupPlan oldest = planRefCleanup(listing, RefTxnId{1, 3}, {}, std::nullopt, RefTxnId{1, 1});
    EXPECT_TRUE(oldest.deletable_snapshots.empty());
}

/// ===================== THE `_ckpt` LEAK BACKSTOP =====================
///
/// The removed namespace's checkpoint object is the one ref object nothing else reclaims: the covered-
/// log cleanup handles logs and snapshots, the Pending pass's two physical passes handle
/// `cas/manifests/<ns>/` and the verbatim files, and `namespacePhysicallyEmpty` -- which decides
/// Pending -> Completed -- never looks under `cas/refs/` at all. A leaked `_ckpt` is therefore
/// invisible to the completion condition while remaining visible to `serverRootSubtreeEmpty`, which
/// leaves a fully drained server root permanently unreclaimable.

TEST(CasGcFrontierGate, ThePendingCleanupPassDeletesTheRemovedNamespaceCheckpoint)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/removed@cas@"};
    const RootNamespace live{"00/live@cas@"};

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    /// Both namespaces have a `_ckpt`; only one of them is being removed.
    backend->putIfAbsent(layout.refCkptKey(removed), "checkpoint-body");
    backend->putIfAbsent(layout.refCkptKey(live), "checkpoint-body");

    const RefTxnId remove_txn{1, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[removed.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = removed, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/false);

    EXPECT_FALSE(backend->head(layout.refCkptKey(removed)).exists)
        << "the removed namespace's checkpoint is reclaimed by its Pending pass -- nothing else would";
    EXPECT_TRUE(backend->head(layout.refCkptKey(live)).exists)
        << "a live namespace's checkpoint is untouched: the delete is a KNOWN KEY, not a prefix sweep";
}

/// The `_ckpt` delete is a destructive site like every other, and the gate holds it like every other.
TEST(CasGcFrontierGate, TheCheckpointDeleteIsHeldBySuppressionLikeEveryOtherSite)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/removed@cas@"};

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    backend->putIfAbsent(layout.refCkptKey(removed), "checkpoint-body");

    const RefTxnId remove_txn{1, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[removed.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = removed, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/true);

    EXPECT_TRUE(backend->head(layout.refCkptKey(removed)).exists)
        << "a suppressed round leaves the checkpoint alone, exactly as it leaves every other object";
}

/// A recreation that landed while the pass was running publishes the `_cleanup` marker's precondition,
/// and the per-key marker guard must stop the checkpoint delete too -- otherwise the backstop would
/// delete the RECREATED incarnation's live checkpoint.
TEST(CasGcFrontierGate, TheCheckpointDeleteAbortsOnTheRecreationMarker)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/removed@cas@"};

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    const RefTxnId remove_txn{1, 5};
    /// A successor already Completed this item and the writer recreated the namespace, so the marker is
    /// durable and the `_ckpt` under this key belongs to the NEW incarnation.
    backend->putIfAbsent(layout.refCleanupMarkerKey(removed, remove_txn), String{});
    backend->putIfAbsent(layout.refCkptKey(removed), "recreated-checkpoint");

    CasFoldSeal seal;
    seal.ns_cleanup_items[removed.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = removed, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/false);

    EXPECT_TRUE(backend->head(layout.refCkptKey(removed)).exists)
        << "the marker is the recreation precondition; the checkpoint delete honours it like every "
           "other delete in the pass";
}
