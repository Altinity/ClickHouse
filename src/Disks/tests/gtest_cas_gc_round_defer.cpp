#include <gtest/gtest.h>

#include <set>
#include <limits>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
const UInt128 kGc = UInt128(0xAB);
}

TEST(CasGcRoundDefer, PredicateTruthTable)
{
    /// threshold=1 (default): defer ONLY when zero shards changed AND no graduation due AND within bound.
    EXPECT_TRUE (shouldDeferRound(/*changed*/0, /*grad_due*/false, /*since*/0, /*threshold*/1, /*max*/8));
    EXPECT_FALSE(shouldDeferRound(1, false, 0, 1, 8));   // a shard changed => fold
    EXPECT_FALSE(shouldDeferRound(0, true,  0, 1, 8));   // graduation due => force fold
    EXPECT_FALSE(shouldDeferRound(0, false, 8, 1, 8));   // defer bound reached => force fold

    /// threshold=3 (batching): defer while accumulated changed shards < threshold, no grad, within bound.
    EXPECT_TRUE (shouldDeferRound(2, false, 0, 3, 8));
    EXPECT_FALSE(shouldDeferRound(3, false, 0, 3, 8));   // reached threshold => fold
    EXPECT_FALSE(shouldDeferRound(2, true,  0, 3, 8));   // graduation due => force fold regardless of size
    EXPECT_FALSE(shouldDeferRound(2, false, 8, 3, 8));   // bound reached => force fold
}

/// graduationDue (retired-in-snapshot T4): read ZERO-I/O from the adopted seal's condemned_summary. An
/// entry whose oldest non-pending condemn round crosses current_round forces it true; a delete_pending
/// entry forces it true regardless of the round; otherwise false.
TEST(CasGcRoundDefer, GraduationDueDetectsDuePendingAndRoundCrossing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();

    /// Adopt a seal whose shard-0 summary holds one condemned-but-not-yet-graduated entry (round 2).
    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/1,
        {{0, CondemnedSummary{.condemned_total = 1, .pending_total = 0,
                              .oldest_nonpending_condemn_round = 2}}});

    Gc gc(store, kGc);
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    EXPECT_FALSE(gc.graduationDueForTest(state, /*current_round=*/2))
        << "oldest non-pending condemn round (2) is not < current_round (2); not yet due to graduate";
    EXPECT_TRUE(gc.graduationDueForTest(state, /*current_round=*/3))
        << "oldest non-pending condemn round (2) < current_round (3) => due to graduate";

    /// Re-adopt a seal whose summary entry is delete_pending: due regardless of the round.
    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/1,
        {{0, CondemnedSummary{.condemned_total = 1, .pending_total = 1,
                              .oldest_nonpending_condemn_round = std::numeric_limits<uint64_t>::max()}}});
    const GcState state_pending = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    EXPECT_TRUE(gc.graduationDueForTest(state_pending, /*current_round=*/0))
        << "a delete_pending entry must force graduationDue true regardless of current_round";
}

/// graduationDue fail-closed: when the adopted seal OBJECT is deleted out from under gc/state, the signal
/// must be TRUE (forces the fold so the round's own fail-closed path surfaces the corrupt bookkeeping),
/// never a silent defer.
TEST(CasGcRoundDefer, GraduationDueFailsClosedWhenSealMissing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();

    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/1,
        {{0, CondemnedSummary{}}});
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    /// Delete the adopted seal object (corrupt destructive bookkeeping).
    const String seal_key = layout.foldSealKey(state.snap_generation, state.snap_attempt);
    const HeadResult h = backend->head(seal_key);
    ASSERT_TRUE(h.exists);
    ASSERT_EQ(backend->deleteExact(seal_key, h.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc(store, kGc);
    EXPECT_TRUE(gc.graduationDueForTest(state, /*current_round=*/5))
        << "a missing adopted seal must fail-closed to a forced fold";
}

/// graduationDue is FALSE on a TOTAL all-zero summary: nothing condemned in any shard => nothing due.
TEST(CasGcRoundDefer, GraduationDueFalseOnAllZeroSummary)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();

    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/2,
        {{0, CondemnedSummary{}}, {1, CondemnedSummary{}}});
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    Gc gc(store, kGc);
    EXPECT_FALSE(gc.graduationDueForTest(state, /*current_round=*/9))
        << "an all-zero total summary means nothing is due to graduate";

    /// Fail-closed if the summary is NOT total over gc_shards (shard 1 missing).
    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/2,
        {{0, CondemnedSummary{}}});
    const GcState partial = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    EXPECT_TRUE(gc.graduationDueForTest(partial, /*current_round=*/9))
        << "a summary not total over gc_shards is corrupt => fail-closed force-fold";
}

/// `listRefPrefix`'s `changed_shards`: with the fold seal covering shard s at its current token, a quiescent pool reports
/// 0; after one publish to a ref in shard s, it reports 1.
TEST(CasGcRoundDefer, ChangedShardCountIsZeroWhenQuiescent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};

    writeBlobBody(*backend, layout, UInt128(1));
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// fold; the round's own trim then rewrites the
                                                         /// shard (compacting the just-folded event), so
                                                         /// its sealed token is the PRE-trim snapshot.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// a second, work-free round: nothing left to
                                                         /// trim, so THIS round's fold seal finally
                                                         /// captures the shard's actual current token.

    const GcState quiescent_state = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    EXPECT_EQ(gc.listRefPrefixForTest(quiescent_state).changed_shards, 0u)
        << "a quiescent shard (listed token == sealed token) must not count as changed";

    /// Publish a second ref into the SAME shard: its LISTED token now differs from what
    /// `quiescent_state`'s adopted fold seal recorded.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 0xBB};
    writeBlobBody(*backend, layout, UInt128(2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", UInt128(2))});
    publishCommittedTransition(*backend, layout, ns, "tbl2", std::nullopt, r2);

    EXPECT_EQ(gc.listRefPrefixForTest(quiescent_state).changed_shards, 1u)
        << "one shard whose token advanced since the sealed generation must count as changed";
}

/// Mutation caught: widening the hot LIST from `cas/ns/stream/` to `cas/ns/` would offer `_ckpt` and
/// `_files` state objects to the fold. The backend-observed result set must contain both immutable
/// stream kinds and neither state kind.
TEST(CasGcRoundDefer, HotEnumerationOffersLogsAndSnapshotsButNeverCheckpointOrFiles)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"name-must-not-appear"}, UInt128{0x123});
    const RefTxnId id{1, 1};
    const String log_key = layout.refLogKey(life, id);
    const String snap_key = layout.refSnapshotKey(life, id);
    const String ckpt_key = layout.refCkptKey(life);
    const String file_key = layout.namespaceFileKey(life, "f");
    ASSERT_EQ(backend->putIfAbsent(log_key, "log").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(snap_key, "snap").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(ckpt_key, "ckpt").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(file_key, "file").outcome, PutOutcome::Done);
    backend->resetCounts();

    Gc gc(store, kGc);
    const RefScanSummary scan = gc.listRefPrefixForTest(GcState{});
    const std::set<String> offered(scan.keys.begin(), scan.keys.end());
    EXPECT_EQ(offered, (std::set<String>{log_key, snap_key}));
    EXPECT_EQ(backend->listCount(layout.namespaceStreamRootPrefix()), 1u);
    EXPECT_EQ(backend->listCount(layout.namespaceRootPrefix()), 0u);
    EXPECT_EQ(backend->listCount(layout.namespaceStateRootPrefix()), 0u);
}

/// The cut precedes LIST. A life first observed by that LIST but absent from the earlier cut is
/// post-cut/unknown, never inert debris, and forces the round to defer without reading its body.
TEST(CasGcRoundDefer, LifeAbsentFromThePreListCatalogCutDefersTheRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const NamespaceLifeId unknown = NamespaceLifeId::fromCatalogEntry(RootNamespace{"cannot-authorize"}, UInt128{0x456});
    const String log_key = layout.refLogKey(unknown, RefTxnId{1, 1});
    ASSERT_EQ(backend->putIfAbsent(log_key, "not-read-on-defer").outcome, PutOutcome::Done);
    backend->resetCounts();

    Gc gc(store, kGc);
    const RoundReport report = gc.runRegularRound({}, /*allow_steal=*/true, UniversePolicy::AuthoritativeForTest);
    EXPECT_TRUE(report.deferred);
    EXPECT_EQ(backend->getCount(log_key), 0u);
    EXPECT_EQ(backend->deleteTotal(), 0u);
}

/// The unknown-life gate covers every immutable stream kind, not only logs. A snapshot-only life does
/// not contribute `changed_shards`, so forcing fold-every-round makes this test distinguish the safety
/// defer from the ordinary idle-round optimization.
TEST(CasGcRoundDefer, SnapshotLifeAbsentFromThePreListCatalogCutDefersTheRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const NamespaceLifeId unknown = NamespaceLifeId::fromCatalogEntry(RootNamespace{"cannot-authorize"}, UInt128{0x457});
    const String snapshot_key = layout.refSnapshotKey(unknown, RefTxnId{1, 1});
    ASSERT_EQ(backend->putIfAbsent(snapshot_key, "not-read-on-defer").outcome, PutOutcome::Done);
    backend->resetCounts();

    Gc gc(store, kGc);
    const RoundReport report = gc.runRegularRound({}, /*allow_steal=*/true, UniversePolicy::AuthoritativeForTest);
    EXPECT_TRUE(report.deferred);
    EXPECT_EQ(backend->getCount(snapshot_key), 0u);
    EXPECT_EQ(backend->deleteTotal(), 0u);
}

/// ---- Task 4: the DEFER short-circuit wired into runRegularRound ----

/// Idle round re-adopts: after a settled round, a subsequent round with zero changed shards and no
/// graduation due sets report.deferred=true and performs dramatically less generation-run I/O than a
/// real fold round (no `blob_target` run object touched at all -- the fold never runs). Snap
/// generation/attempt are untouched (the snapshot is not rebuilt).
///
/// SETTLING NOTE: immutable `_log` objects are never trimmed in place (unlike the legacy mutable shard
/// journal, whose fold-then-trim token rewrite forced a second settling round), so the pool quiesces the
/// round AFTER the folding round -- the very next round defers.
TEST(CasGcRoundDefer, IdleRoundDefersAndReadsNoGeneration)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    backend->resetCounts();
    const RoundReport fold_rep = gc.runRegularRound();   /// round 1: folds the +1 (no trim-lag, quiesces at once)
    ASSERT_FALSE(fold_rep.deferred);
    const uint64_t fold_round_gets = backend->getTotal();
    EXPECT_GT(fold_round_gets, 0u) << "sanity: a real fold round performs some GETs";

    const auto st_before = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);

    backend->resetCounts();
    const RoundReport rep = gc.runRegularRound();   /// round 2: genuinely quiesced now => must defer
    const uint64_t defer_round_gets = backend->getTotal();

    EXPECT_TRUE(rep.deferred) << "a settled idle round must re-adopt the sealed generation, not fold";
    /// A deferred round mints no new round (CasGc.cpp:runRegularRound's defer branch), so the honest
    /// `report.round` is the round that was ALREADY adopted before this round started -- the same round
    /// the preceding fold round committed. Guards against the bug where the defer path returned WITHOUT
    /// ever assigning `report.round`, leaving it at its zero-initialized default and making every
    /// deferred round print `CA GC round 0` regardless of how far GC had actually progressed.
    EXPECT_NE(rep.round, 0u) << "a deferred round must report a truthful, nonzero round number";
    EXPECT_EQ(rep.round, fold_rep.round)
        << "a deferred round re-adopts the already-committed round, not a fabricated new one";

    const auto st_after = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(st_after.snap_generation, st_before.snap_generation)
        << "a deferred round must not mint a new generation (snapshot rebuild elided)";
    EXPECT_EQ(st_after.snap_attempt, st_before.snap_attempt);

    /// SECONDARY (not over-fit to "exactly 0 gets" -- the decision itself pays a bounded retired-list +
    /// discovery-LIST cost that may share the same get counter): the deferred round touches NO
    /// blob_target run object at all (fold never runs, so foldDeltasIntoGeneration never executes), and
    /// its total get volume sits far below a genuine fold round's.
    EXPECT_EQ(backend->ioCountForKeysContaining("/blob_target/"), 0u)
        << "a deferred round must never GET/getStream/PUT any blob_target run object";
    EXPECT_LT(defer_round_gets, fold_round_gets)
        << "a deferred round's read volume must sit far below a real fold round's";
}

/// The same idle-defer property under a sharded blob-target GC (gc_shards=2): graduationDue's loop
/// over state.retired_refs and `listRefPrefix`'s discovery must both settle to "nothing due" once
/// quiesced, regardless of how many gc-shards partition the retired bookkeeping.
TEST(CasGcRoundDefer, IdleRoundDefersUnderShardedGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_shards = 2});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    ASSERT_FALSE(gc.runRegularRound().deferred);   /// round 1: folds the publish

    /// Immutable `_log` objects are never trimmed in place, so there is no fold-then-trim token-rewrite
    /// lag: the pool quiesces after the folding round, and the very next round defers.
    const RoundReport rep = gc.runRegularRound();   /// round 2: quiesced
    EXPECT_TRUE(rep.deferred) << "idle pool under gc_shards=2 must defer once settled";
}

/// The +1 guard (mirror of the 2026-06-27 leak): a blob condemned + published delete_pending, then
/// re-referenced WHILE it is pending, must NOT be over-deleted -- the due graduation forces a fold
/// (never a defer) that sees the +1 and spares the blob.
TEST(CasGcRoundDefer, DueGraduationForcesFoldAndSparesReReferencedBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const UInt128 blob(1);
    const ManifestRef r1{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);

    runRegularRoundReclaiming(gc);                 /// folds the +1; blob referenced
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);   /// the -1 condemns it

    runRegularRoundReclaiming(gc);                 /// the condemning round
    store->renewWatermarkOnce();

    /// Drive rounds until the entry graduates (published delete_pending) -- mirrors
    /// CasGcAckFloor.CondemnThenDeleteNextRoundAfterAcks. It is still PRESENT at that pass, and the
    /// ack floor is by construction already past its condemn_round (that is what graduated it).
    bool saw_pending = false;
    for (int i = 0; i < 6 && !saw_pending; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        for (const RetiredEntry & e : currentRetiredSet(*backend, store->layout(), /*shard*/0))
            if (e.ref == DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(blob)} && e.delete_pending)
                saw_pending = true;
    }
    ASSERT_TRUE(saw_pending) << "entry never reached delete_pending";
    ASSERT_FALSE(blobAbsent(*backend, store->layout(), blob)) << "pending: still present this pass";

    /// While B sits delete_pending, a NEW manifest re-references it -- a genuine +1 racing the
    /// already-published pending delete.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 0xBB};
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);

    /// The next pass would otherwise execute B's pending exact-token delete; graduationDue must force
    /// a FOLD (never a DEFER) so the +1 is folded in and the blob is spared, not deleted.
    const RoundReport rep = runRegularRoundReclaiming(gc);
    EXPECT_FALSE(rep.deferred) << "a due graduation must force a fold, never defer";
    EXPECT_FALSE(blobAbsent(*backend, store->layout(), blob)) << "the re-referenced blob must survive";

    const FsckReport fsck = runFsck(*store, /*detail*/true);
    EXPECT_EQ(fsck.dangling, 0u);
}

/// Companion to the test above: it proves `graduationDue` is the SOLE fold trigger at the assertion
/// round. `DueGraduationForcesFoldAndSparesReReferencedBlob` opens its store at the DEFAULT
/// `gc_fold_threshold` (1), so at its assertion round the +1 re-reference ALSO makes
/// `changed_shards (>= 1) >= fold_threshold (1)` true -- that branch of `shouldDeferRound` would force
/// the very same fold even if `graduationDue` were deleted or hard-wired false. Here `gc_fold_threshold`
/// and `gc_fold_max_defer_rounds` are both set to 1000, so neither the changed-shards branch (one
/// changed shard is nowhere near 1000) nor the liveness-bound branch (this is round 1) can fire --
/// `graduationDue` is the ONLY thing in `shouldDeferRound` that can force this round's fold, making
/// `EXPECT_FALSE(rep.deferred)` below load-bearing for `graduationDue` specifically.
TEST(CasGcRoundDefer, DueGraduationIsSoleFoldTriggerAtHighThreshold)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_threshold = 1000, .gc_fold_max_defer_rounds = 1000});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const UInt128 blob(1);

    Gc gc(store, kGc);
    /// Warm-up round on the still-empty pool: `gc/state` does not exist yet, so lease acquisition takes
    /// the create-fresh path and succeeds immediately (`gc_id` becomes the owner in storage). This
    /// matters because the `injectCondemnedSummarySeal` seeding below writes `gc/state` directly, and a fresh `Gc`
    /// object's FIRST-EVER `acquireOrRenewLease` call against a PRE-EXISTING lease it has never observed
    /// refuses to steal it (two-observation safety against stealing from a live incumbent) -- it would
    /// return `acquired_lease=false` and the round would bail out BEFORE the fold-decision code, making
    /// `EXPECT_FALSE(rep.deferred)` below vacuously true regardless of `graduationDue`. Running this
    /// warm-up round FIRST makes `gc_id` the observed incumbent, so the assertion round's lease RENEWAL
    /// (not a steal) succeeds unconditionally and the round actually reaches the decision it's testing.
    gc.runRegularRound();

    writeBlobBody(*backend, layout, blob);

    /// Seed the adopted fold seal's condemned_summary with B already `delete_pending` (pending_total = 1),
    /// mirroring `CasGcRoundDefer.GraduationDueDetectsDuePendingAndRoundCrossing`. Retired-in-snapshot
    /// (T4): graduationDue reads this summary ZERO-I/O off the adopted seal — a delete_pending entry forces
    /// it true regardless of the round. At `gc_fold_threshold = 1000` a real condemn -> graduate pipeline of
    /// `runRegularRound` calls is not usable to set this up: every round before graduation would ITSELF
    /// defer (nothing due yet, and changed_shards never nears 1000), so the due-pending summary is injected
    /// directly instead of driven through real rounds.
    injectCondemnedSummarySeal(*backend, layout, /*generation*/1, /*attempt*/1, /*gc_shards*/1,
        {{0, CondemnedSummary{.condemned_total = 1, .pending_total = 1,
                              .oldest_nonpending_condemn_round = std::numeric_limits<uint64_t>::max()}}});

    /// The +1: a fresh manifest re-references B while it sits `delete_pending` -- one changed shard,
    /// far below the threshold of 1000.
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xBB};
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);

    const RoundReport rep = gc.runRegularRound();

    /// DISCRIMINATING (load-bearing): with graduationDue intact, the due delete_pending entry forces
    /// the fold. If graduationDue were broken/hard-wired false, changed_shards (1) < threshold (1000)
    /// and the defer bound (1000) is nowhere near reached, so `shouldDeferRound` would return true and
    /// this round would DEFER instead.
    EXPECT_FALSE(rep.deferred) << "a due graduation must be the SOLE fold trigger at a high fold threshold";
    EXPECT_FALSE(blobAbsent(*backend, layout, blob)) << "the re-referenced blob must survive the forced fold";

    const FsckReport fsck = runFsck(*store, /*detail*/true);
    EXPECT_EQ(fsck.dangling, 0u);
}

/// Bounded deferral: with a large fold_threshold and a small standing delta (one shard changed,
/// forever, since deferring never resolves it), at most gc_fold_max_defer_rounds consecutive rounds
/// defer, then one round forces a fold (the liveness bound).
TEST(CasGcRoundDefer, BoundedDeferralForcesFoldWithinWindow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_threshold = 100, .gc_fold_max_defer_rounds = 3});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    for (int i = 0; i < 3; ++i)
    {
        const RoundReport rep = gc.runRegularRound();
        EXPECT_TRUE(rep.deferred) << "round " << (i + 1) << " is within the defer bound";
    }
    const RoundReport rep4 = gc.runRegularRound();
    EXPECT_FALSE(rep4.deferred) << "the 4th round hits the defer bound and must force-fold";
}
