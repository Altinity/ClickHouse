#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
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

/// graduationDue: a delete_pending entry, and an entry whose condemn_round < min_ack, each force it true;
/// an entry with condemn_round >= min_ack and not delete_pending leaves it false.
TEST(CasGcRoundDefer, GraduationDueDetectsDuePendingAndFloorCrossing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const Layout & layout = store->layout();

    /// Seed the CURRENT retired list for gc-shard 0 with one condemned-but-not-floor-passed entry.
    injectRetire(*backend, layout, /*round*/0, /*fence_seq*/0, /*shard*/0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = {}, .token = {}, .size = 0,
                      .condemn_round = 2, .delete_pending = false}});

    Gc gc(store, kGc);
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    EXPECT_FALSE(gc.graduationDueForTest(state, /*min_ack=*/2))
        << "condemn_round (2) is not < min_ack (2); not yet due to graduate";
    EXPECT_TRUE(gc.graduationDueForTest(state, /*min_ack=*/3))
        << "condemn_round (2) < min_ack (3) => due to graduate";

    /// Re-seed the SAME entry (same retired-list object) as delete_pending: due regardless of the floor.
    const String retired_key = state.retired_refs.at(0);
    const auto current = backend->get(retired_key);
    ASSERT_TRUE(current.has_value());
    backend->putOverwrite(retired_key,
        encodeRetiredSet(RetiredSet{.entries = {RetiredEntry{.kind = ObjectKind::Blob, .hash = {}, .token = {},
                                                              .size = 0, .condemn_round = 2, .delete_pending = true}}}),
        current->token);

    EXPECT_TRUE(gc.graduationDueForTest(state, /*min_ack=*/0))
        << "delete_pending must force graduationDue true regardless of the ack floor";
}

/// changedShardCount: with the fold seal covering shard s at its current token, a quiescent pool reports
/// 0; after one publish to a ref in shard s, it reports 1.
TEST(CasGcRoundDefer, ChangedShardCountIsZeroWhenQuiescent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    EXPECT_EQ(gc.changedShardCountForTest(quiescent_state), 0u)
        << "a quiescent shard (listed token == sealed token) must not count as changed";

    /// Publish a second ref into the SAME shard: its LISTED token now differs from what
    /// `quiescent_state`'s adopted fold seal recorded.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 0xBB};
    writeBlobBody(*backend, layout, UInt128(2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", UInt128(2))});
    publishCommittedTransition(*backend, layout, ns, "tbl2", std::nullopt, r2);

    EXPECT_EQ(gc.changedShardCountForTest(quiescent_state), 1u)
        << "one shard whose token advanced since the sealed generation must count as changed";
}

/// ---- Task 4: the DEFER short-circuit wired into runRegularRound ----

/// Idle round re-adopts: after a settled round, a subsequent round with zero changed shards and no
/// graduation due sets report.deferred=true and performs dramatically less generation-run I/O than a
/// real fold round (no `blob_target` run object touched at all -- the fold never runs). Snap
/// generation/attempt are untouched (the snapshot is not rebuilt).
///
/// SETTLING NOTE: with eager trim (gc_trim_min_events=0, the openStoreForTest default) a shard's token
/// is rewritten by trim AFTER the fold seal captures it, so reaching true quiescence takes 2 rounds
/// (fold-then-trim lag) -- mirrors CasGcRoundDefer.ChangedShardCountIsZeroWhenQuiescent above.
TEST(CasGcRoundDefer, IdleRoundDefersAndReadsNoGeneration)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    ASSERT_FALSE(gc.runRegularRound().deferred);   /// round 1: folds the +1 (trim-lag not yet settled)

    backend->resetCounts();
    const RoundReport settle = gc.runRegularRound();   /// round 2: still folds (sees round 1's own
                                                        /// trim-rewrite as "changed"); this fold's seal
                                                        /// finally captures the shard's true current token.
    ASSERT_FALSE(settle.deferred);
    const uint64_t fold_round_gets = backend->getTotal();
    EXPECT_GT(fold_round_gets, 0u) << "sanity: a real fold round performs some GETs";

    const auto st_before = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);

    backend->resetCounts();
    const RoundReport rep = gc.runRegularRound();   /// round 3: genuinely quiesced now => must defer
    const uint64_t defer_round_gets = backend->getTotal();

    EXPECT_TRUE(rep.deferred) << "a settled idle round must re-adopt the sealed generation, not fold";

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
/// over state.retired_refs and changedShardCount's discovery must both settle to "nothing due" once
/// quiesced, regardless of how many gc-shards partition the retired bookkeeping.
TEST(CasGcRoundDefer, IdleRoundDefersUnderShardedGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                   .gc_shards = 2, .gc_trim_min_events = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    ASSERT_FALSE(gc.runRegularRound().deferred);   /// round 1: folds
    ASSERT_FALSE(gc.runRegularRound().deferred);   /// round 2: settles the trim-lag mismatch

    const RoundReport rep = gc.runRegularRound();   /// round 3: quiesced
    EXPECT_TRUE(rep.deferred) << "idle pool under gc_shards=2 must defer once settled";
}

/// The +1 guard (mirror of the 2026-06-27 leak): a blob condemned + published delete_pending, then
/// re-referenced WHILE it is pending, must NOT be over-deleted -- the due graduation forces a fold
/// (never a defer) that sees the +1 and spares the blob.
TEST(CasGcRoundDefer, DueGraduationForcesFoldAndSparesReReferencedBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const UInt128 blob(1);
    const ManifestRef r1{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);

    gc.runRegularRound();                 /// folds the +1; blob referenced
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);   /// the -1 condemns it

    gc.runRegularRound();                 /// the condemning round
    store->renewWatermarkOnce();

    /// Drive rounds until the entry graduates (published delete_pending) -- mirrors
    /// CasGcAckFloor.CondemnThenDeleteNextRoundAfterAcks. It is still PRESENT at that pass, and the
    /// ack floor is by construction already past its condemn_round (that is what graduated it).
    bool saw_pending = false;
    for (int i = 0; i < 6 && !saw_pending; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
        for (const RetiredEntry & e : currentRetiredSet(*backend, store->layout(), /*shard*/0).entries)
            if (e.hash == blob && e.delete_pending)
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
    const RoundReport rep = gc.runRegularRound();
    EXPECT_FALSE(rep.deferred) << "a due graduation must force a fold, never defer";
    EXPECT_FALSE(blobAbsent(*backend, store->layout(), blob)) << "the re-referenced blob must survive";

    const FsckReport fsck = runFsck(*store, /*detail*/true);
    EXPECT_EQ(fsck.dangling, 0u);
}

/// Bounded deferral: with a large fold_threshold and a small standing delta (one shard changed,
/// forever, since deferring never resolves it), at most gc_fold_max_defer_rounds consecutive rounds
/// defer, then one round forces a fold (the liveness bound).
TEST(CasGcRoundDefer, BoundedDeferralForcesFoldWithinWindow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                   .gc_trim_min_events = 0, .gc_fold_threshold = 100, .gc_fold_max_defer_rounds = 3});
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
