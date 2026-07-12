#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(const String &, uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
}

/// Committed new_manifest => +1 per blob entry (BlobInDegreeMatchesActiveManifests).
/// After a fold, gc/state records snap_attempt == the folding leader's lease.seq, and the fold seal
/// lives under (snap_generation, snap_attempt).
TEST(CasGcFold, FoldAdoptsAttemptEqualsLeaseSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();

    const auto st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(st.snap_attempt, st.lease.seq);
    EXPECT_GT(st.snap_generation, 0u);
    /// The one-pass round's fold seal is durable under (snap_generation, snap_attempt) — the adopted
    /// attempt locates it (a seal under any other attempt would be unadopted debris).
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists);
}

TEST(CasGcFold, CommittedAddEmitsPlusOnePerBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r,
        {blobEntryFor("a", DB::UInt128(1)), blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);
}

/// Owner removal => -1 per blob entry; in-degree returns to 0.
TEST(CasGcFold, RemovalEmitsMinusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// Precommit with a PRESENT, valid body => +1.
TEST(CasGcFold, PrecommitBodyPresentEmitsPlusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
}

/// Precommit whose body is ABSENT => NO delta (control #4); the 404 must NOT throw.
TEST(CasGcFold, PrecommitMissingBodyEmitsNoDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// FOLD BARRIER (control #23): a LIVE precommit binding whose body is missing does NOT advance the
/// durable fold cursor past its activation event; when the body appears the cursor advances.
TEST(CasGcFold, FoldBarrierHaltsCursorAtLiveMissingBodyPrecommit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const uint64_t v = addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), v);   // barrier: halted at the activation

    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_GE(foldCursorOf(*backend, store->layout(), ns, 0), v);   // barrier lifted by activation
}

/// Promote of an already-activated precommit is a PURE OWNER MOVE: NO delta, body not condemned.
TEST(CasGcFold, PromoteOfActivatedPrecommitEmitsNoDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    promoteTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", r);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // unchanged, still pinned
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);   // not condemned
}

/// Committed add naming a MISSING body (404) => clamp + anomaly, never a guessed +1, never a throw.
TEST(CasGcFold, CommittedMissingBodyClampsCursorAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const uint64_t v = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // no body
    Gc gc(store, kGc);
    RoundReport report;
    EXPECT_NO_THROW(report = gc.runRegularRound());
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), v);
}

/// A body whose self-ref disagrees (PRESENT but INVALID) => hard fail closed (controls #19/#20).
TEST(CasGcFold, RefMismatchFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    PartManifest bad;
    bad.ref = ref("srv-a:1", 1, 0xBB);   // != r
    bad.root_namespace_id = ns;
    bad.entries = {blobEntryFor("a", DB::UInt128(1))};
    bad.payload_digest = computePayloadDigest(bad);
    backend->putIfAbsent(store->layout().manifestKey(ManifestId{ns, r}), encodePartManifest(bad));
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ gc.runRegularRound(); });
}

/// Owner-removal whose OLD committed body is gone at removal-fold => clamp + anomaly, no partial -1.
TEST(CasGcFold, RemovalWithMissingOldBodyClampsAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();   // +1; blob 1 in-degree 1

    const uint64_t removal_version = dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    deleteManifestBody(*backend, store->layout(), ManifestId{ns, r});   // body gone before its decrement

    RoundReport report;
    EXPECT_NO_THROW(report = gc.runRegularRound());
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // unchanged: no silent -1
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), removal_version);
}

/// (The two `CasGcFold.IncarnationMismatchRestartsFoldAtZero*` tests were removed with the snapshot+log
/// ref model: they injected a stale per-shard fold cursor beyond the live mutable shard's version and
/// asserted the fold RESET the cursor to 0 on an incarnation mismatch. There is no mutable per-shard
/// cursor to stale-reset anymore -- the durable cursor is a strictly-increasing `RefTxnId`, and a
/// recreated namespace uses a GREATER `writer_epoch`, so the ABA hazard is impossible by construction.
/// The ref-model equivalent -- `remove_namespace` then a later `namespace_birth` with a greater id folds
/// normally -- is covered by `gtest_cas_gc_shard_incarnation.cpp` and `gtest_cas_ref_gc.cpp`.)

/// T0 (2026-07-02 snapshot-streaming): an idle round — no journal changes, no retired entries — touches
/// ZERO run objects. After one populated round, reset the counters and run a no-op round; the fold must
/// carry the parent generation's `RunRef` verbatim into the new fold_seal (same key, same checksum, same
/// generation) and NOT read or write any `.../blob_target/...` object.
TEST(CasGcFold, EmptyDeltaShardCarriesParentRunRef)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    /// gc_fold_max_defer_rounds=0 forces fold-every-round: this test exercises the pure-ref-carry FOLD
    /// path on an idle round; without it the round would DEFER (re-adopt the sealed generation) and never
    /// mint the carried generation this test inspects.
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();   // round 1: folds the +1, seals the gen-1 blob_target run

    const auto st1 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    const auto parent_seal = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st1.snap_generation, st1.snap_attempt))->bytes);
    ASSERT_EQ(parent_seal.blob_target_runs.size(), 1u);
    const RunRef parent_ref = parent_seal.blob_target_runs.front();

    backend->resetCounts();
    gc.runRegularRound();   // round 2: no changes => pure ref-carry, zero run I/O

    EXPECT_EQ(backend->ioCountForKeysContaining("/blob_target/"), 0u)
        << "idle round must not GET/getStream/PUT any blob_target run object";

    const auto st2 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_GT(st2.snap_generation, st1.snap_generation);
    const auto new_seal = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st2.snap_generation, st2.snap_attempt))->bytes);
    ASSERT_EQ(new_seal.blob_target_runs.size(), 1u);
    const RunRef carried = new_seal.blob_target_runs.front();
    EXPECT_EQ(carried.key, parent_ref.key) << "carried ref points at the PARENT generation's run key";
    EXPECT_EQ(carried.checksum, parent_ref.checksum);
    EXPECT_EQ(carried.shard, 0u);
    EXPECT_EQ(carried.generation, st1.snap_generation)
        << "the carried ref names the generation whose key namespace physically holds the object";
}

/// The round AFTER a ref-carry, with a real delta, folds THROUGH the carried ref: the new generation's
/// run is produced from the OLD-generation run (resolved via the carried ref, not by key construction)
/// merged with the delta, and the resulting in-degree is correct.
TEST(CasGcFold, FoldResolvesThroughCarriedRef)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    gc.runRegularRound();   // gen 1: blob 1 in-degree 1
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    gc.runRegularRound();   // gen 2: no delta => carries the gen-1 ref
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "in-degree resolves through the carried parent ref";

    // A real delta on the NEXT round must fold through the carried ref and drop blob 1 to zero.
    const ManifestRef r2 = ref("srv-a:2", 2, 0xBB);
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);

    gc.runRegularRound();   // gen 3: -1 on blob 1 (old owner dropped), +1 on blob 2
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0)
        << "fold through the carried ref applied the -1 correctly";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);
}

/// previewDeletes resolves runs through the current seal's refs, not by key construction. After a
/// pure ref-carry round the current seal's `blob_target_runs` point at an OLDER generation's key; the
/// preview must open that physical object via the ref and report the correct in-degree — here blob 1 is
/// still referenced, so its carried-ref-resolved in-degree is 1 and it is NOT surfaced as a candidate.
/// (A carried ref that the preview failed to resolve would mis-open the run and either throw or spuriously
/// surface the still-referenced blob.)
TEST(CasGcFold, PreviewResolvesCarriedRef)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// gc_fold_max_defer_rounds=0 forces the idle second round to FOLD (pure ref-carry) rather than
    /// DEFER, so the current seal's `blob_target_runs` point at the parent generation's key (the carried
    /// ref this test resolves through).
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const UInt128 blob = DB::UInt128(1);
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();   // gen 1: blob referenced, in-degree 1
    const auto st1 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);

    gc.runRegularRound();   // gen 2: no delta, no retired => pure ref-carry (ref points back at gen 1)
    const auto st2 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    ASSERT_GT(st2.snap_generation, st1.snap_generation);
    const auto seal2 = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st2.snap_generation, st2.snap_attempt))->bytes);
    ASSERT_EQ(seal2.blob_target_runs.size(), 1u);
    ASSERT_EQ(seal2.blob_target_runs.front().generation, st1.snap_generation)
        << "the current seal's ref physically lives at the parent generation (carried, not reconstructed)";

    // The preview resolves the carried ref (a gen-1 physical key) and computes in-degree 1 => blob 1 is
    // not a delete candidate. Resolution-by-ref is the property under test.
    const auto preview = gc.previewDeletes();
    for (const auto & e : preview)
        EXPECT_NE(e.ref, (DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(blob)})) << "still-referenced blob must not be surfaced (carried ref resolved to in-degree 1)";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), blob), 1)
        << "in-degree through the carried parent ref is 1";
}

/// A mid-log clamp must be RECOVERABLE (spec §Step 3 transaction atomicity). A single log carrying two
/// ops -- [drop committed A (a `-1` whose body is present at removal-fold), add precommit B (whose body is
/// transiently absent)] -- clamps on B. The `-1` on A must NOT be merged into the round's owner-removed
/// cleanup, because the post-CAS body delete would then reclaim A's body while A's edge stays unfolded
/// behind the clamp; the next re-fold of that same log would then find A's body missing and clamp forever
/// (a permanent pool-wide destructive freeze). With per-log staging, A's body survives the clamp round and
/// the log folds cleanly once B's body reappears.
TEST(CasGcFold, MidLogClampPreservesEarlierRemovalBodyAndRecovers)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef a = ref("srv-a:1", 1, 0xAA);
    const ManifestRef b = ref("srv-a:2", 2, 0xBB);

    /// Round 0: commit A (references blob 1). A's body is present and folds a +1.
    writeManifestRaw(*backend, store->layout(), ns, a, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "r1", std::nullopt, a);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// ONE log with two ops: drop committed A (`-1`, body present), then add precommit B (`+1`, body
    /// staged then removed => a transient 404 clamps the log after A's `-1` already folded).
    writeManifestRaw(*backend, store->layout(), ns, b, {blobEntryFor("b", DB::UInt128(2))});
    deleteManifestBody(*backend, store->layout(), ManifestId{ns, b});   // B's body absent => clamp
    const uint64_t log_seq = appendRefLogSeed(*backend, store->layout(), ns,
        {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "r1", a}, std::nullopt),
         ownerTransitionOp(std::nullopt, RefOwnerBinding{RefOwnerKind::Precommit, "r2", b})});

    const RoundReport clamp_report = gc.runRegularRound();
    EXPECT_TRUE(clamp_report.hasAnomaly(ns, /*shard*/0)) << "the missing B body must clamp this log";
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), log_seq) << "the clamp halts the cursor below the log";
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, a})).exists)
        << "A's body must survive the clamp round: its `-1` was staged, not merged, so no post-CAS delete "
           "reclaimed it -- otherwise the re-fold would clamp on A's missing body forever";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1) << "A's `-1` was not adopted (clamp)";

    /// The transient 404 heals: B's body reappears. The next round re-folds the SAME log cleanly.
    writeManifestRaw(*backend, store->layout(), ns, b, {blobEntryFor("b", DB::UInt128(2))});
    const RoundReport clean_report = gc.runRegularRound();
    EXPECT_FALSE(clean_report.hasAnomaly(ns, /*shard*/0)) << "with both bodies present the log folds; no clamp";
    EXPECT_GE(foldCursorOf(*backend, store->layout(), ns, 0), log_seq) << "the cursor advanced past the recovered log";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0) << "A's `-1` applied";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1) << "B's `+1` applied";
}

/// A `+1` precommit whose body is PERMANENTLY absent and whose build is below the durable watermark floor
/// (provably dead -- the exact fact the orphan sweep uses to reclaim the body) must be SKIPPED, not held on
/// the fold barrier forever. Without a terminal rule this table clamps every round with no resolution (a
/// late-predecessor precommit whose body was already reclaimed). The watermark is seeded so the precommit's
/// build is dead; the fold must advance the cursor past the log and record no clamp anomaly.
TEST(CasGcFold, DeadPrecommitWithMissingBodyIsSkippedNotClampedForever)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    /// The namespace's server-root prefix is "srv"; seed its watermark floor so build_sequence 5 is retired.
    const RootNamespace ns{"srv/tbl"};
    setWatermarkMinActive(*backend, store->layout(), "srv", /*writer_epoch*/1, /*min_active*/10);

    /// A precommit naming a build (writer_epoch 1, build_sequence 5) whose body is never written.
    const ManifestRef dead = ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1};
    const uint64_t log_seq =
        addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "r1", std::nullopt, dead);

    Gc gc(store, kGc);
    const RoundReport report = gc.runRegularRound();
    EXPECT_FALSE(report.hasAnomaly(ns, /*shard*/0))
        << "a provably-dead precommit's missing body is skipped, not clamped";
    EXPECT_GE(foldCursorOf(*backend, store->layout(), ns, 0), log_seq)
        << "the fold advanced past the log instead of holding the barrier forever";

    /// A second identical round stays clean (terminal resolution, not a recurring clamp).
    const RoundReport report2 = gc.runRegularRound();
    EXPECT_FALSE(report2.hasAnomaly(ns, /*shard*/0)) << "the resolution is terminal: no recurring clamp";
}
