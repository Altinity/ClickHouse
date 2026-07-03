#include <gtest/gtest.h>

#include <optional>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
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
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
}

/// Guard (spec Part 1): a fold with NO baseline must refuse when a shard journal proves trimmed
/// history — otherwise a fresh GC folds only the surviving tails and mass-deletes live data.
TEST(CasGcBaselineGuard, FreshStateOverTrimmedJournalsFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);   /// gc_trim_min_events = 0 => eager trim
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();   /// trims the folded events (eager gate)

    /// Disaster: gc/state vanishes on a lived-in pool.
    const HeadResult st = backend->head(store->layout().gcStateKey());
    ASSERT_TRUE(st.exists);
    ASSERT_EQ(backend->deleteExact(store->layout().gcStateKey(), st.token).kind, DeleteOutcome::Kind::Deleted);

    /// A fresh GC (fresh leader id — the old lease died with the state) must REFUSE, not delete.
    Gc gc2(store, hexToU128("00000000000000000000000000000002"));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists)
        << "the guard must fire BEFORE any destructive step";
}

/// A genuinely fresh pool (journals start at version 1) passes the guard — rounds run as today.
TEST(CasGcBaselineGuard, GenuinelyFreshPoolIsUnaffected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// (б) audit: snap_generation > 0 whose adopted fold seal is ABSENT must be CORRUPTED_DATA,
/// never silently treated as an empty baseline.
TEST(CasGcBaselineGuard, AbsentAdoptedSealFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    /// Corrupt (б): delete the adopted fold seal out from under a healthy gc/state.
    const GcState st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    ASSERT_GT(st.snap_generation, 0u);
    const String seal_key = store->layout().foldSealKey(st.snap_generation, st.snap_attempt);
    const HeadResult sh = backend->head(seal_key);
    ASSERT_TRUE(sh.exists);
    ASSERT_EQ(backend->deleteExact(seal_key, sh.token).kind, DeleteOutcome::Kind::Deleted);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });
}

/// (а): lose gc/state on a lived-in pool -> guard blocks rounds -> rebuild -> rounds converge:
/// the dropped blob is reclaimed, the live blob intact, the round minted above every mount ack.
TEST(CasGcRebuild, RecoversLostStateAndConverges)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef live_r = ref(1, 0xA1);
    const ManifestRef dead_r = ref(2, 0xA2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, live_r, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, dead_r, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_live", std::nullopt, live_r);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_dead", std::nullopt, dead_r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl_dead", dead_r);
    gc.runRegularRound();   /// -1 folds; eager trim cuts the journal
    store->renewWatermarkOnce();   /// the mount ack advances to the current round

    const HeadResult st = backend->head(store->layout().gcStateKey());
    ASSERT_EQ(backend->deleteExact(store->layout().gcStateKey(), st.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc2(store, hexToU128("00000000000000000000000000000003"));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });

    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.committed_refs, 1u);
    EXPECT_EQ(rep.namespaces, 1u);

    /// Round strictly above the mount's surviving ack.
    const auto mount_got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(mount_got.has_value());
    EXPECT_GT(rep.round, decodeMountLease(mount_got->bytes).observed_gc_round);

    /// Regular rounds converge: blob 2 (unreferenced) reclaimed, blob 1 intact.
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc2, *backend, store->layout(), DB::UInt128(2)));
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
}

/// (б): a run object named by a healthy state is lost -> the regular round fails closed -> the
/// PLAIN rebuild (no FORCE) recovers, and rounds converge afterwards.
TEST(CasGcRebuild, RecoversLostGenerationArtifact)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    /// Lose one snapshot run object out from under the healthy state.
    const GcState st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    const auto seal = decodeFoldSeal(backend->get(store->layout().foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    ASSERT_FALSE(seal.blob_target_runs.empty());
    const String run_key = seal.blob_target_runs.front().key;
    const HeadResult rh = backend->head(run_key);
    ASSERT_TRUE(rh.exists);
    ASSERT_EQ(backend->deleteExact(run_key, rh.token).kind, DeleteOutcome::Kind::Deleted);

    /// A pure ref-carry round would not read the lost run; land a REAL delta so the fold's
    /// three-cursor merge must stream the prior run — and fails closed on its absence.
    const ManifestRef r2 = ref(2, 0xB7);
    writeBlobBody(*backend, store->layout(), DB::UInt128(3));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("c", DB::UInt128(3))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });

    const RebuildReport rep = gc.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
}

/// FORCE: a healthy state refuses the plain rebuild; FORCE rebuilds; rounds run clean after.
TEST(CasGcRebuild, HealthyStateRequiresForce)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    const RebuildReport refused = gc.rebuildBaseline(/*force*/ false);
    EXPECT_FALSE(refused.performed);
    EXPECT_NE(refused.refusal.find("FORCE"), String::npos);

    const RebuildReport forced = gc.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(forced.performed) << forced.refusal;
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// Refusal: a committed owner with a MISSING manifest body is data loss — the rebuild refuses,
/// names the owner, and writes nothing (gc/state stays absent).
TEST(CasGcRebuild, MissingCommittedManifestRefuses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef a = ref(1, 0xA1);
    const ManifestRef b = ref(2, 0xB2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, a, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, b, {blobEntryFor("b", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_a", std::nullopt, a);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_b", std::nullopt, b);
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();   /// trim

    /// Disaster pair: gc/state lost AND tbl_b's manifest body lost.
    const HeadResult st = backend->head(store->layout().gcStateKey());
    backend->deleteExact(store->layout().gcStateKey(), st.token);
    const String mkey = store->layout().manifestKey(ManifestId{ns, b});
    const HeadResult mh = backend->head(mkey);
    ASSERT_TRUE(mh.exists);
    backend->deleteExact(mkey, mh.token);

    Gc gc2(store, hexToU128("00000000000000000000000000000004"));
    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    EXPECT_FALSE(rep.performed);
    EXPECT_NE(rep.refusal.find("tbl_b"), String::npos) << rep.refusal;
    /// The lease acquire minted a gen-0 bootstrap body (that is the acquire's contract, not the
    /// rebuild's); the rebuild's own contract is that NO baseline was blessed by the refusal.
    const auto post = backend->get(store->layout().gcStateKey());
    ASSERT_TRUE(post.has_value());
    const GcState post_state = decodeGcState(post->bytes);
    EXPECT_EQ(post_state.snap_generation, 0u) << "a refused rebuild must not adopt a baseline";
    EXPECT_TRUE(post_state.retired_refs.empty());
}

/// A live precommit with a durable body contributes edges (no clamp); the rebuilt baseline
/// protects its blob from condemnation.
TEST(CasGcRebuild, LivePrecommitEdgesIncluded)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef pre = ref(7, 0xC1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(9));
    writeManifestRaw(*backend, store->layout(), ns, pre, {blobEntryFor("p", DB::UInt128(9))});
    addPrecommitTransition(*backend, store->layout(), ns, /*build_id*/ DB::UInt128(0x77), "part_pre", std::nullopt, pre);

    /// No round before the rebuild: the journal still carries the create-precommit event (a round's
    /// eager trim would cut it — the trimmed-but-live case is the next test). gc/state absent =>
    /// the plain rebuild is allowed.
    Gc gc2(store, hexToU128("00000000000000000000000000000005"));
    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.live_precommits, 1u);
    EXPECT_EQ(rep.clamped_shards, 0u);

    /// The precommit's blob is edge-protected: rounds never reclaim it while the precommit lives.
    for (int i = 0; i < 4; ++i)
    {
        gc2.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(9))))).exists);
}

/// O(budget) attempt iteration: a tiny edge budget forces multi-batch folding; the rebuilt
/// baseline still protects every committed blob (same convergence as the single-batch path).
TEST(CasGcRebuild, BatchedRebuildProtectsAllRefs)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    for (uint64_t i = 1; i <= 6; ++i)
    {
        writeBlobBody(*backend, store->layout(), DB::UInt128(i));
        const ManifestRef r = ref(i, 0xA0 + i);
        writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("f", DB::UInt128(i))});
        publishCommittedTransition(*backend, store->layout(), ns, "tbl_" + std::to_string(i), std::nullopt, r);
    }
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();
    const HeadResult st = backend->head(store->layout().gcStateKey());
    backend->deleteExact(store->layout().gcStateKey(), st.token);

    Gc gc2(store, hexToU128("00000000000000000000000000000006"));
    gc2.setRebuildEdgeBudgetForTest(2);   /// forces multiple attempt-iterated batches
    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.committed_refs, 6u);

    for (int i = 0; i < 5; ++i)
    {
        gc2.runRegularRound();
        store->renewWatermarkOnce();
    }
    for (uint64_t i = 1; i <= 6; ++i)
        EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(i))))).exists)
            << "blob " << i;
}

/// Trimmed-but-live (design delta 2): the precommit's journal evidence is gone (trim), the build
/// is NOT provably dead (a live build holds min_active down) — the unowned-alive sweep must
/// over-protect the manifest's edges.
TEST(CasGcRebuild, UnownedAliveManifestOverProtected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    /// A LIVE build pins min_active at its build_seq, so higher build sequences are not provably dead.
    auto live_build = store->startBuild({});
    store->renewWatermarkOnce();

    /// An unowned manifest from build_seq 7 (no journal events at all — the trimmed shape).
    const ManifestRef pre = ref(7, 0xC1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(9));
    writeManifestRaw(*backend, store->layout(), ns, pre, {blobEntryFor("p", DB::UInt128(9))});
    /// The namespace must be discoverable: give it one committed ref on another manifest.
    const ManifestRef anchor = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, anchor, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, anchor);

    Gc gc(store, hexToU128("00000000000000000000000000000007"));
    const RebuildReport rep = gc.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.unowned_alive_manifests, 1u);

    /// Over-protected: rounds never reclaim the unowned-alive manifest's blob.
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(9))))).exists);
}

/// Task 4 (SYSTEM CONTENT ADDRESSED GC REBUILD): a rebuild refuses when ANOTHER Gc instance holds
/// the lease, even under FORCE (FORCE bypasses the "healthy state" refusal, not the lease). Gc A's
/// runRegularRound freshly acquires/renews the lease; Gc B (a different gc_id) must see it as live.
TEST(CasGcRebuild, LeaseConflictRefuses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc_a(store, kGc);
    gc_a.runRegularRound();   /// Gc A acquires/renews the lease.

    Gc gc_b(store, hexToU128("00000000000000000000000000000002"));
    const RebuildReport rep = gc_b.rebuildBaseline(/*force*/ true);
    EXPECT_FALSE(rep.performed);
    EXPECT_NE(rep.refusal.find("lease"), String::npos) << rep.refusal;
    EXPECT_NE(rep.refusal.find("leader"), String::npos) << rep.refusal;
}

/// CLAMP SUPPRESSION regression (2026-07-03 night soak: 31 dangling blobs). A committed +1 for
/// blob X lands on a shard whose fold cursor is CLAMPED (behind a bodiless precommit — the fold
/// barrier), while X's only FOLDED edge (a committed ref on ANOTHER shard) drops. Without
/// suppression the pipeline condemns, graduates and DELETES X while its landed +1 sits unfolded
/// behind the clamp; the clamp release then folds the +1 into a DANGLING reference (the model's
/// SabotageSkipChangedShard, realized). With suppression a clamped pass neither graduates nor
/// redeletes; X survives until the clamp clears, after which the +1 folds and X is SPARED.
TEST(CasGcClampSuppression, LandedEdgeBehindClampNeverDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    /// Folded baseline: blob X referenced by committed tbl_a (manifest m1) on shard 1.
    const ManifestRef m1 = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, m1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_a", std::nullopt, m1, /*shard*/1);
    Gc gc(store, kGc);
    gc.runRegularRound();
    store->renewWatermarkOnce();

    /// The CLAMP on shard 0: a bodiless precommit (fold barrier — its manifest body never written).
    const ManifestRef pre = ref(9, 0xEE);
    addPrecommitTransition(*backend, store->layout(), ns, /*build_id*/ DB::UInt128(0x99), "part_pre",
                           std::nullopt, pre, /*shard*/0);

    /// BEHIND the clamp: a committed +1 for X (manifest m2, tbl_b) on shard 0 — landed, unfoldable
    /// until the barrier clears. Then tbl_a drops on shard 1 — X's only FOLDED edge disappears.
    const ManifestRef m2 = ref(2, 0xB2);
    writeManifestRaw(*backend, store->layout(), ns, m2, {blobEntryFor("b", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_b", std::nullopt, m2, /*shard*/0);
    dropRefTransition(*backend, store->layout(), ns, "tbl_a", m1, /*shard*/1);

    /// Rounds with acks current: X reaches folded in-degree 0 and is condemned, but every pass is
    /// CLAMPED (the bodiless precommit persists), so nothing may graduate or delete.
    /// Observability (2026-07-03): every clamp emits a gc_fold_clamp event with the reason.
    std::vector<CasEvent> seen;
    store->setEventSink([&](const CasEvent & e){ if (e.type == CasEventType::GcFoldClamp) seen.push_back(e); });
    const String blob_key = store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))));
    for (int i = 0; i < 6; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
        ASSERT_TRUE(backend->head(blob_key).exists)
            << "round " << i << ": X was deleted while its landed +1 sat unfolded behind the clamp";
    }

    ASSERT_FALSE(seen.empty()) << "each clamped pass must emit a gc_fold_clamp event";
    EXPECT_NE(seen.front().reason.find("fold barrier"), String::npos);
    EXPECT_EQ(seen.front().detail.at("shard"), "0");
    store->setEventSink(nullptr);

    /// Release the clamp: the precommit's body lands (the build finished staging). The next rounds
    /// fold through the barrier, m2's +1 lands, and X is SPARED (entry dropped, blob intact).
    writeManifestRaw(*backend, store->layout(), ns, pre, {blobEntryFor("p", DB::UInt128(1))});
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(backend->head(blob_key).exists);
    /// And the pipeline is unwedged: a genuinely-unreferenced blob still gets reclaimed.
    const ManifestRef m3 = ref(3, 0xC3);
    writeBlobBody(*backend, store->layout(), DB::UInt128(5));
    writeManifestRaw(*backend, store->layout(), ns, m3, {blobEntryFor("c", DB::UInt128(5))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_c", std::nullopt, m3, /*shard*/1);
    gc.runRegularRound();
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl_c", m3, /*shard*/1);
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc, *backend, store->layout(), DB::UInt128(5)));
}
