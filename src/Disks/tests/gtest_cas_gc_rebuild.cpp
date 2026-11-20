#include <gtest/gtest.h>

#include <atomic>
#include <expected>
#include <stdexcept>
#include <optional>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/ProfileEvents.h>
#include "cas_sweep_test_support.h"
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace ProfileEvents
{
extern const Event CASGCRefWalkPlansBuilt;
}

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int NETWORK_ERROR;
}

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// An exact read (mirrors the retired `backend->get(key)`).
std::optional<Object> readObj(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).read(key, Retry::standard());
}

/// A HEAD (mirrors the retired `backend->head(key)`).
std::optional<Meta> headObj(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).head(key, Retry::standard());
}

/// A one-shot `create`, asserting it committed (mirrors the retired `backend->putIfAbsent(key, bytes)`).
void createObj(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
}

/// A delete-exact against `key`/`expected` (mirrors the retired `backend->deleteExact(key, token)`);
/// the caller decides whether to assert the outcome.
Removal removeExact(Backend & backend, const String & key, const Etag & expected)
{
    OperationForTest op(backend);
    return (*op).remove(key, expected, Retry::once());
}
}

/// (`CASGCBaselineGuard.FreshStateOverTrimmedJournalsFailsClosed` was removed with the snapshot+log ref
/// model. It asserted that a fresh GC over a MUTABLE shard journal whose folded history had been TRIMMED
/// must refuse, lest it fold only the surviving tails and mass-delete live data. Immutable `_log`/`_snap`
/// objects are never trimmed in place: a fresh GC always reconstructs the FULL ref state via the recovery
/// equation (newest snapshot + later log tail), so the "trimmed history" hazard cannot arise. The
/// vanished-`gc/state` disaster-recovery path is covered by `CASGCRebuild.RecoversLostStateAndConverges`,
/// and the corrupt-bookkeeping guard by `CASGCBaselineGuard.AbsentAdoptedSealFailsClosed`.)

/// A genuinely fresh pool (journals start at version 1) passes the guard — rounds run as today.
TEST(CASGCBaselineGuard, GenuinelyFreshPoolIsUnaffected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
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
TEST(CASGCBaselineGuard, AbsentAdoptedSealFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    /// Corrupt (б): delete the adopted fold seal out from under a healthy gc/state.
    const GcState st = decodeGcState(readObj(*backend, store->layout().gcStateKey())->bytes);
    ASSERT_GT(st.snap_generation, 0u);
    const String seal_key = store->layout().foldSealKey(st.snap_generation, st.snap_attempt);
    const auto sh = headObj(*backend, seal_key);
    ASSERT_TRUE(sh.has_value());
    ASSERT_EQ(removeExact(*backend, seal_key, sh->etag), Removal::Removed);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });
}

/// (а): lose gc/state on a lived-in pool -> guard blocks rounds -> rebuild -> rounds converge: the
/// live blob intact, the round minted strictly above the last one seen, and the REBUILT baseline is a
/// working one — a ref dropped AFTER it is still reclaimed by ordinary rounds.
///
/// A blob dropped BEFORE the rebuild gets no row from the rebuilt baseline, and a rebuild condemns nothing. Here
/// the drop was condemned before the rebuild, so its deletion intent stands; the intent sweep adopts it and
/// ordinary rounds reclaim the body.
TEST(CASGCRebuild, RecoversLostStateAndConverges)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// gc_fold_max_defer_rounds=0: this test drives MANY consecutive rounds via runRoundsUntilAbsent
    /// expecting every one to fold (Phase-4 Lever A would otherwise defer once the pool quiesces,
    /// stalling the reclaim loop below the 8-round budget); force fold-every-round.
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    runRegularRoundReclaiming(gc);
    dropRefTransition(*backend, store->layout(), ns, "tbl_dead", dead_r);
    runRegularRoundReclaiming(gc);   /// -1 folds; eager trim cuts the journal
    store->renewWatermarkOnce();   /// renews the lease + build-watermark floor

    /// Capture the round reached before gc/state is destroyed (the rebuild must mint strictly above it).
    const auto pre_rebuild_got = readObj(*backend, store->layout().gcStateKey());
    ASSERT_TRUE(pre_rebuild_got.has_value());
    const uint64_t pre_rebuild_round = decodeGcState(pre_rebuild_got->bytes).round;
    ASSERT_EQ(removeExact(*backend, store->layout().gcStateKey(), pre_rebuild_got->etag), Removal::Removed);

    Gc gc2(store, hexToU128("00000000000000000000000000000003"));
    /// A fresh GC over the orphaned generation artifacts fails closed: re-folding from a fresh gc/state
    /// collides with a leftover run object (divergent bytes) — the disaster is surfaced, never silently
    /// double-applied. That first round also re-mints a superficially-healthy gc/state (snap_generation 0),
    /// so the recovery is a DELIBERATE force-rebuild (the auto-rebuild correctly refuses to discard a
    /// state that "looks healthy" without the operator's force).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { runRegularRoundReclaiming(gc2); });

    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.committed_refs, 1u);
    EXPECT_EQ(rep.namespaces, 1u);

    /// Round strictly above the fence/state/generation numbers seen so far.
    EXPECT_GT(rep.round, pre_rebuild_round);

    /// The live blob is intact, and blob 2, whose intent stood through the rebuild, is reclaimed.
    for (int i = 0; i < 4; ++i)
    {
        runRegularRoundReclaiming(gc2);
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(headObj(*backend, store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(1))})).has_value());
    EXPECT_FALSE(headObj(*backend, store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(2))})).has_value())
        << "the intent of the pre-rebuild drop was not adopted";

    /// The rebuilt baseline is a WORKING one: a ref published over it and then dropped still folds to
    /// zero and is reclaimed by ordinary rounds. Without this the test would prove only that the
    /// pipeline stopped deleting.
    const ManifestRef post_r = ref(3, 0xA3);
    writeBlobBody(*backend, store->layout(), DB::UInt128(3));
    writeManifestRaw(*backend, store->layout(), ns, post_r, {blobEntryFor("c", DB::UInt128(3))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_post", std::nullopt, post_r);
    runRegularRoundReclaiming(gc2);
    dropRefTransition(*backend, store->layout(), ns, "tbl_post", post_r);
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc2, *backend, store->layout(), DB::UInt128(3)))
        << "the rebuilt baseline must still reclaim what it can actually see";
}

/// (б): a run object named by a healthy state is lost -> the regular round fails closed -> the
/// PLAIN rebuild (no FORCE) recovers, and rounds converge afterwards.
TEST(CASGCRebuild, RecoversLostGenerationArtifact)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    /// Lose one snapshot run object out from under the healthy state.
    const GcState st = decodeGcState(readObj(*backend, store->layout().gcStateKey())->bytes);
    const auto seal = decodeFoldSeal(readObj(*backend, store->layout().foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    ASSERT_FALSE(seal.blob_target_runs.empty());
    const String run_key = seal.blob_target_runs.front().key;
    const auto rh = headObj(*backend, run_key);
    ASSERT_TRUE(rh.has_value());
    ASSERT_EQ(removeExact(*backend, run_key, rh->etag), Removal::Removed);

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
    EXPECT_TRUE(headObj(*backend, store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(1))})).has_value());
}

/// FORCE: a healthy state refuses the plain rebuild; FORCE rebuilds; rounds run clean after.
TEST(CASGCRebuild, HealthyStateRequiresForce)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
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

    backend->resetCounts();
    const uint64_t plans_before
        = ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt];
    const RebuildReport forced = gc.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(forced.performed) << forced.refusal;
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt] - plans_before, 1u)
        << "healthy FORCE REBUILD must share the same one-shot authoritative plan builder";
    EXPECT_EQ(backend->getCount(store->layout().refCatalogKey()), 2u)
        << "healthy FORCE REBUILD may read the catalog for the conclusive drain and the one post-LIST cut only";
    EXPECT_EQ(backend->listCount(store->layout().namespaceStreamRootPrefix()), 1u);
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// A durable log on the chain is history: REBUILD walks past the frontier of a generation 1 `_ckpt` to
/// the first absent key and protects the owner the later log adds.
TEST(CASGCRebuild, CheckpointFrontierAdmitsAVisibleLogAboveIt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/rebuild-frozen-frontier@cas@"};
    const UInt128 life_id{0xF001};
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, life_id);
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards,
        CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = life_id});

    const ManifestRef admitted = ref(1, 0xA1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, ns, admitted, {blobEntryFor("a", DB::UInt128(1))});

    std::vector<RefOp> first_ops{namespaceBirthOp()};
    const auto admitted_ops = publishCommittedOps("admitted", admitted);
    first_ops.insert(first_ops.end(), admitted_ops.begin(), admitted_ops.end());
    fixture::writeRefLogRaw(*backend, layout,
        RefLogTxn{.ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = std::move(first_ops), .prev_epoch_seal = std::nullopt});

    /// The writer never published this record through `_ckpt`, but it is on the chain.
    const ManifestRef unfrontiered = ref(2, 0xB2);
    writeBlobBody(*backend, layout, DB::UInt128(2));
    writeManifestRaw(*backend, layout, ns, unfrontiered, {blobEntryFor("b", DB::UInt128(2))});
    fixture::writeRefLogRaw(*backend, layout,
        RefLogTxn{.ns = ns.string(), .txn_id = RefTxnId{1, 2}, .ops = publishCommittedOps("unfrontiered", unfrontiered),
                  .prev_epoch_seal = std::nullopt});
    ASSERT_TRUE(headObj(*backend, layout.refLogKey(life, RefTxnId{1, 2})).has_value());
    createObj(*backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1));

    Gc gc(store, kGc);
    const RebuildReport report = gc.rebuildBaseline(/*force=*/false);

    ASSERT_TRUE(report.performed) << report.refusal;
    EXPECT_EQ(report.committed_refs, 2u);
}

/// A catalog-admitted life without its exact checkpoint has no bounded recovery frontier. REBUILD
/// must refuse rather than falling back to a list-derived history and publishing a baseline it cannot
/// prove complete.
TEST(CASGCRebuild, LiveCatalogLifeWithoutCheckpointFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/rebuild-missing-checkpoint@cas@"};
    const UInt128 life_id{0xF002};
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards,
        CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = life_id});

    const ManifestRef admitted = ref(1, 0xA2);
    writeBlobBody(*backend, layout, DB::UInt128(2));
    writeManifestRaw(*backend, layout, ns, admitted, {blobEntryFor("a", DB::UInt128(2))});
    std::vector<RefOp> ops{namespaceBirthOp()};
    const auto admitted_ops = publishCommittedOps("admitted", admitted);
    ops.insert(ops.end(), admitted_ops.begin(), admitted_ops.end());
    fixture::writeRefLogRaw(*backend, layout,
        RefLogTxn{.ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = std::move(ops), .prev_epoch_seal = std::nullopt});

    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)gc.rebuildBaseline(/*force=*/false); });

    const auto state = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(state);
    EXPECT_EQ(decodeGcState(state->bytes).snap_generation, 0u)
        << "a rejected recovery must not adopt a new baseline";
}

/// A syntactically valid snapshot at an OLDER `EpochSeal` id must not let REBUILD synthesize a
/// baseline. The forged base differs from `last_epoch_seal`, so metadata equality cannot reject it;
/// REBUILD must use the retained same-id log witness.
TEST(CASGCRebuild, CheckpointSnapshotAtOlderEpochSealFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/rebuild-checkpoint-base-seal@cas@"};
    const UInt128 life_id{0xF003};
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards,
        CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = life_id});
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, life_id);

    const RefLogTxn birth{
        .ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()},
        .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(*backend, layout, birth);
    RefOp seal;
    seal.kind = RefOpKind::EpochSeal;
    const RefLogTxn seal_txn{
        .ns = ns.string(), .txn_id = RefTxnId{1, 2}, .ops = {seal},
        .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(*backend, layout, seal_txn);
    RefOp later_seal;
    later_seal.kind = RefOpKind::EpochSeal;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = RefTxnId{2, 1}, .ops = {later_seal},
        .prev_epoch_seal = RefTxnId{1, 2}});
    RefTableState through_seal;
    applyRefLogTxn(through_seal, birth);
    applyRefLogTxn(through_seal, seal_txn);
    writeRefSnapshotRaw(*backend, layout, snapshotOf(through_seal, ns.string()));

    createObj(*backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = RefTxnId{1, 2},
        .last_epoch_seal = RefTxnId{2, 1}}, 1));

    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)gc.rebuildBaseline(/*force=*/false); });

    const auto state = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(state);
    EXPECT_EQ(decodeGcState(state->bytes).snap_generation, 0u)
        << "a rejected checkpoint base must not publish a REBUILD baseline";
}

TEST(CASGCRebuild, DamagedGenerationZeroStatePerformsNoCatalogDrainMutation)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/removing-without-parent@cas@"};
    const UInt128 life_id{91};
    CasRefCatalog::casAdmitEntry(op, layout, 1, CatalogEntry{
        .ns = ns, .state = NsState::Live, .incarnation = life_id});
    CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });
    createObj(*backend, layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(ns, life_id)), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1));
    const uint64_t catalog_cas_before = backend->putOverwriteCount(layout.refCatalogKey());
    const uint64_t plans_before
        = ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt];

    Gc gc(store, kGc);
    const RebuildReport report = gc.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(report.performed) << report.refusal;
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt] - plans_before, 1u);
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_cas_before);
    const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(op, layout);
    ASSERT_EQ(catalog.catalog.entries.size(), 1u);
    EXPECT_EQ(catalog.catalog.entries[0].state, NsState::Removing);
    EXPECT_EQ(catalog.catalog.entries[0].incarnation, life_id);
}

/// Refusal: a committed owner with a MISSING manifest body is data loss — the rebuild refuses,
/// names the owner, and writes nothing (gc/state stays absent).
TEST(CASGCRebuild, MissingCommittedManifestRefuses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
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
    const auto st = headObj(*backend, store->layout().gcStateKey());
    ASSERT_TRUE(st.has_value());
    removeExact(*backend, store->layout().gcStateKey(), st->etag);
    const String mkey = store->layout().manifestKey(ManifestId{ns, b});
    const auto mh = headObj(*backend, mkey);
    ASSERT_TRUE(mh.has_value());
    removeExact(*backend, mkey, mh->etag);

    Gc gc2(store, hexToU128("00000000000000000000000000000004"));
    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    EXPECT_FALSE(rep.performed);
    EXPECT_NE(rep.refusal.find("tbl_b"), String::npos) << rep.refusal;
    /// The lease acquire minted a gen-0 bootstrap body (that is the acquire's contract, not the
    /// rebuild's); the rebuild's own contract is that NO baseline was blessed by the refusal.
    const auto post = readObj(*backend, store->layout().gcStateKey());
    ASSERT_TRUE(post.has_value());
    const GcState post_state = decodeGcState(post->bytes);
    EXPECT_EQ(post_state.snap_generation, 0u) << "a refused rebuild must not adopt a baseline";
}

/// A live precommit with a durable body contributes edges (no clamp); the rebuilt baseline
/// protects its blob from condemnation.
TEST(CASGCRebuild, LivePrecommitEdgesIncluded)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef pre = ref(7, 0xC1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(9));
    writeManifestRaw(*backend, store->layout(), ns, pre, {blobEntryFor("p", DB::UInt128(9))});
    addPrecommitTransition(
        *backend, store->layout(), ns, /*build_id*/ DB::UInt128(0x77), "part_pre", std::nullopt, pre);

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
    EXPECT_TRUE(headObj(*backend, store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(9))})).has_value());
}

/// O(budget) attempt iteration: a tiny edge budget forces multi-batch folding; the rebuilt
/// baseline still protects every committed blob (same convergence as the single-batch path).
TEST(CASGCRebuild, BatchedRebuildProtectsAllRefs)
{
    auto backend = std::make_shared<InMemoryBackend>();
    constexpr uint64_t gc_shards = 2;
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_shards = gc_shards});
    const RootNamespace ns{"00/aa@cas@"};
    constexpr uint64_t edge_budget = 2;
    constexpr uint64_t refs_per_shard = edge_budget + 1;
    std::vector<UInt128> blobs;
    for (uint64_t shard = 0; shard < gc_shards; ++shard)
    {
        for (uint64_t i = 0; i < refs_per_shard; ++i)
        {
            const uint64_t sequence = blobs.size() + 1;
            const UInt128 blob = (UInt128{shard + gc_shards * i} << 64) | UInt128{sequence};
            ASSERT_EQ(blobShard(legacyMetaTestRef(blob), gc_shards), shard);
            blobs.push_back(blob);
            writeBlobBody(*backend, store->layout(), blob);
            const ManifestRef r = ref(sequence, 0xA0 + sequence);
            writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("f", blob)});
            publishCommittedTransition(
                *backend, store->layout(), ns, "tbl_" + std::to_string(sequence), std::nullopt, r);
        }
    }
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();
    const auto st = headObj(*backend, store->layout().gcStateKey());
    ASSERT_TRUE(st.has_value());
    removeExact(*backend, store->layout().gcStateKey(), st->etag);

    Gc gc2(store, hexToU128("00000000000000000000000000000006"));
    /// Every shard has `edge_budget + 1` live edges, so each independently crosses the flush budget;
    /// a test with only a pool-wide excess would not prove multiple batches for every non-empty shard.
    gc2.setRebuildEdgeBudgetForTest(edge_budget);
    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.committed_refs, blobs.size());

    /// Multiple rebuild flushes still converge to one authoritative row domain: no more than one
    /// canonical seq-0 `blob_run` per shard and exactly one `condemned` per shard. These are the cardinalities the
    /// catalog admission reservation over-covers independently of catalog-entry count.
    const GcState rebuilt_state = decodeGcState(readObj(*backend, store->layout().gcStateKey())->bytes);
    const CasFoldSeal rebuilt_seal = decodeFoldSeal(
        readObj(*backend, store->layout().foldSealKey(
            rebuilt_state.snap_generation, rebuilt_state.snap_attempt))->bytes,
        store->layout(), gc_shards);
    ASSERT_EQ(rebuilt_seal.condemned_summary.size(), gc_shards);
    bool run_seen[gc_shards] = {false, false};
    ASSERT_EQ(rebuilt_seal.blob_target_runs.size(), gc_shards);
    for (const RunRef & run : rebuilt_seal.blob_target_runs)
    {
        ASSERT_LT(run.shard, gc_shards);
        EXPECT_FALSE(run_seen[run.shard]);
        run_seen[run.shard] = true;
        const auto parsed = store->layout().parseBlobTargetRunKey(run.key);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->shard, run.shard);
        EXPECT_EQ(parsed->generation, run.key_generation);
        EXPECT_EQ(parsed->seq, 0u);
    }
    EXPECT_TRUE(run_seen[0]);
    EXPECT_TRUE(run_seen[1]);

    for (int i = 0; i < 5; ++i)
    {
        gc2.runRegularRound();
        store->renewWatermarkOnce();
    }
    for (const UInt128 blob : blobs)
        EXPECT_TRUE(headObj(*backend, store->layout().blobKey(legacyMetaTestRef(blob))).has_value())
            << "blob " << u128ToHex(blob);
}

/// Trimmed-but-live (design delta 2): the precommit's journal evidence is gone (trim), the build
/// is NOT provably dead (a live build holds min_active_build_sequence down) — the unowned-alive sweep must
/// over-protect the manifest's edges.
TEST(CASGCRebuild, UnownedAliveManifestOverProtected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    /// A LIVE build pins min_active_build_sequence at its build_seq, so higher build sequences are not provably dead.
    auto live_build = store->beginPartWrite({});
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
    EXPECT_TRUE(headObj(*backend, store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(9))})).has_value());
}

/// Task 4 (SYSTEM CAS GC REBUILD): a rebuild refuses when ANOTHER Gc instance holds
/// the lease, even under FORCE (FORCE bypasses the "healthy state" refusal, not the lease). Gc A's
/// runRegularRound freshly acquires/renews the lease; Gc B (a different gc_id) must see it as live.
TEST(CASGCRebuild, LeaseConflictRefuses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
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

/// A rebuild CONDEMNS NOTHING (spec §7). The zero-edge condemnation that used to live here — a
/// `blobs/` LIST whose every unreached body was condemned into the rebuilt run — was the
/// r5-finding-4 data-loss vector: the rebuild's own traversal is listing-driven, so a hidden
/// durable owner made this pass condemn acked data. Its removal, the NAMED residual it leaves
/// (manifest-less orphans are retained until register R4's build/upload registry), and the
/// hold-carry that had to survive the removal are all covered by
/// `gtest_cas_rebuild_condemn_nothing.cpp`.

/// CLAMP SUPPRESSION regression (2026-07-03 night soak: 31 dangling blobs). A committed +1 for
/// blob X lands on a shard whose fold cursor is CLAMPED (behind a bodiless precommit — the fold
/// barrier), while X's only FOLDED edge (a committed ref on ANOTHER shard) drops. Without
/// suppression the pipeline condemns, graduates and DELETES X while its landed +1 sits unfolded
/// behind the clamp; the clamp release then folds the +1 into a DANGLING reference (the model's
/// SabotageSkipChangedShard, realized). With suppression a clamped pass neither graduates nor
/// redeletes; X survives until the clamp clears, after which the +1 folds and X is SPARED.
TEST(CASGCClampSuppression, LandedEdgeBehindClampNeverDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Heap-owned, not a plain local: declaring it before the Pool (ASan 2026-07-09) only protects
    /// against an ordinary same-thread unwind, not a detached background completion holding an extra
    /// `shared_from_this()` that can still be running on another thread after this frame returns.
    auto seen = std::make_shared<DB::Cas::tests::SharedEventLog>();
    auto store = openPoolForTest(backend);
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
    store->setEventSink([seen](const CasEvent & e)
    {
        if (e.type == CasEventType::GcFoldClamp)
            seen->push(e);
    });
    const String blob_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(1))});
    for (int i = 0; i < 6; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
        ASSERT_TRUE(headObj(*backend, blob_key).has_value())
            << "round " << i << ": X was deleted while its landed +1 sat unfolded behind the clamp";
    }

    const std::vector<CasEvent> observed_events = seen->snapshot();
    ASSERT_FALSE(observed_events.empty()) << "each clamped pass must emit a gc_fold_clamp event";
    EXPECT_NE(observed_events.front().reason.find("fold barrier"), String::npos);
    /// Snapshot+log ref model: the clamp is per-table (one ref-log stream per namespace, no ref shards),
    /// so the event names the clamped `log` and the `resolved_through` cursor rather than a shard number.
    EXPECT_TRUE(observed_events.front().detail.contains("log"))
        << "clamp event must name the clamped log id";
    EXPECT_TRUE(observed_events.front().detail.contains("resolved_through"))
        << "clamp event must name the cursor it resolved through";
    store->setEventSink(nullptr);

    /// Release the clamp: the precommit's body lands (the build finished staging). The next rounds
    /// fold through the barrier, m2's +1 lands, and X is SPARED (entry dropped, blob intact).
    writeManifestRaw(*backend, store->layout(), ns, pre, {blobEntryFor("p", DB::UInt128(1))});
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(headObj(*backend, blob_key).has_value());
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

namespace
{

/// A catalog-admitted life whose chain is `{1,1}` birth, `T{1,2}->5`, `H{5,1}<-{1,2}`, under a
/// generation 3 `_ckpt`.
NamespaceLifeId seedSkippedEpochChain(Backend & backend, const Layout & layout, const RootNamespace & ns,
                                      const UInt128 & life_id, uint64_t gc_shards)
{
    OperationForTest op(backend);
    CasRefCatalog::casAdmitEntry(*op, layout, gc_shards, CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = life_id});
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, life_id);
    const auto put = [&](const RefLogTxn & txn, uint32_t generation)
    {
        createObj(backend, layout.refLogKey(life, txn.txn_id),
            sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(txn, generation)));
    };
    put(RefLogTxn{ns.string(), RefTxnId{1, 1}, {namespaceBirthOp()}, std::nullopt}, 1);
    put(tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5), 3);
    put(RefLogTxn{ns.string(), RefTxnId{5, 1}, {}, RefTxnId{1, 2}}, 3);
    RefCkpt ckpt = refCkptOf(1);
    createObj(backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(ckpt, 3));
    return life;
}

}

TEST(CASGCRebuild, RebuildFollowsASkippedEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const UInt128 life_id{0xF101};
    seedSkippedEpochChain(*backend, layout, RootNamespace{"00/rebuild-skipped-epoch@cas@"}, life_id,
                          store->poolConfig().gc_shards);

    Gc gc(store, kGc);
    const RebuildReport report = gc.rebuildBaseline(/*force=*/false);
    ASSERT_TRUE(report.performed) << report.refusal;

    const GcState st = decodeGcState(readObj(*backend, layout.gcStateKey())->bytes);
    const CasFoldSeal seal = decodeFoldSeal(readObj(*backend, layout.foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    const auto row = seal.ref_lives.find(life_id);
    ASSERT_NE(row, seal.ref_lives.end());
    EXPECT_EQ(row->second.coverage.last_folded_ref_id, (RefTxnId{5, 1}));
}

TEST(CASGCRebuild, RebuildFailsWhenTheBaseMoves)
{
    auto backend = std::make_shared<SweepReadHookBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const NamespaceLifeId life = seedSkippedEpochChain(
        *backend, layout, RootNamespace{"00/rebuild-base-moves@cas@"}, UInt128{0xF102}, store->poolConfig().gc_shards);
    backend->arm(layout.refLogKey(life, RefTxnId{1, 1}), 1, [&]
    {
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.refCkptKey(life), Retry::once());
        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.refCkptKey(life), current->bytes, current->etag, Retry::once())));
    });

    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)gc.rebuildBaseline(/*force=*/false); });
    EXPECT_TRUE(backend->didFire());
    const auto state = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(state);
    EXPECT_EQ(decodeGcState(state->bytes).snap_generation, 0u) << "a moved base must not adopt a new baseline";
}

namespace
{

bool isWriteRequest(JournalVerb verb)
{
    return verb == JournalVerb::Write || verb == JournalVerb::Remove || verb == JournalVerb::RemoveMany
        || verb == JournalVerb::Publish;
}

/// The keys written after the last read or `HEAD` of `probed_key`, which must have been read.
std::vector<String> writesAfterLastProbeOf(const std::vector<JournalEntry> & journal, const String & probed_key)
{
    std::optional<size_t> last_probe;
    for (size_t i = 0; i < journal.size(); ++i)
        if (!journal[i].keys.empty() && journal[i].keys.front() == probed_key
            && (journal[i].verb == JournalVerb::Read || journal[i].verb == JournalVerb::Head))
            last_probe = i;
    EXPECT_TRUE(last_probe.has_value()) << "the rebuild never read " << probed_key;
    std::vector<String> written;
    for (size_t i = last_probe.value_or(journal.size()); i < journal.size(); ++i)
        if (isWriteRequest(journal[i].verb))
            written.insert(written.end(), journal[i].keys.begin(), journal[i].keys.end());
    return written;
}

/// A pool whose `gc/state` is lost and whose committed `tbl_b` names a manifest that is gone: the rebuild
/// refuses after its lease claim. Returns the missing manifest's id.
ManifestId seedLostStateWithMissingCommittedManifest(Backend & backend, const PoolPtr & store)
{
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef a = ref(1, 0xA1);
    const ManifestRef b = ref(2, 0xB2);
    writeBlobBody(backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(backend, store->layout(), ns, a, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(backend, store->layout(), ns, b, {blobEntryFor("b", DB::UInt128(1))});
    publishCommittedTransition(backend, store->layout(), ns, "tbl_a", std::nullopt, a);
    publishCommittedTransition(backend, store->layout(), ns, "tbl_b", std::nullopt, b);
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();
    const auto st = headObj(backend, store->layout().gcStateKey());
    EXPECT_TRUE(st.has_value());
    removeExact(backend, store->layout().gcStateKey(), st->etag);
    const ManifestId id{ns, b};
    deleteManifestBody(backend, store->layout(), id);
    return id;
}

}

/// Each `SYSTEM CAS GC REBUILD` takes a fresh GC identity and never steals, so a refusal that kept its
/// lease claim would make every retry refuse as a competing leader.
TEST(CASGCRebuild, RefusedRebuildGivesBackItsLeaseClaim)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const ManifestId missing = seedLostStateWithMissingCommittedManifest(*backend, store);
    ASSERT_FALSE(readObj(*backend, layout.gcStateKey()).has_value());

    backend->clearJournal();
    Gc first(store, hexToU128("00000000000000000000000000000011"));
    const RebuildReport refused = first.rebuildBaseline(/*force*/ false);
    ASSERT_FALSE(refused.performed);
    EXPECT_NE(refused.refusal.find("tbl_b"), String::npos) << refused.refusal;
    EXPECT_EQ(writesAfterLastProbeOf(backend->journal(), layout.manifestKey(missing)),
              std::vector<String>{layout.gcStateKey()}) << "the only write after the refusal is the give-back";
    const auto given_back = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(given_back.has_value()) << "the claim created gc/state, and the give-back keeps the object";
    const GcState given_back_state = decodeGcState(given_back->bytes);
    EXPECT_EQ(given_back_state.lease.owner, DB::UInt128(0));
    EXPECT_EQ(given_back_state.lease.seq, 0u);
    EXPECT_EQ(given_back_state.snap_generation, 0u);

    /// Over a present state with the never-held lease, a refusal leaves the bytes as they were. That state
    /// is generation 0 and looks healthy, so from here on the rebuild needs FORCE.
    Gc second(store, hexToU128("00000000000000000000000000000012"));
    const RebuildReport refused_again = second.rebuildBaseline(/*force*/ true);
    ASSERT_FALSE(refused_again.performed);
    EXPECT_NE(refused_again.refusal.find("tbl_b"), String::npos) << refused_again.refusal;
    const auto after_second = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(after_second.has_value());
    EXPECT_EQ(after_second->bytes, given_back->bytes);
    EXPECT_NE(tokenText(after_second->etag), tokenText(given_back->etag)) << "the claim and the give-back both wrote";

    writeManifestRaw(*backend, layout, missing.root_namespace, missing.ref, {blobEntryFor("b", DB::UInt128(1))});
    Gc retry(store, hexToU128("00000000000000000000000000000013"));
    const RebuildReport rep = retry.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.committed_refs, 2u);
}

/// The give-back is one conditional write: when `gc/state` moved after the claim, it is not retried, the
/// original refusal stands, and the lease left behind is named in a warning.
TEST(CASGCRebuild, LostLeaseGiveBackKeepsTheRefusalAndWarns)
{
    auto backend = std::make_shared<SweepReadHookBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const ManifestId missing = seedLostStateWithMissingCommittedManifest(*backend, store);
    backend->arm(layout.manifestKey(missing), 1, [&]
    {
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.gcStateKey(), Retry::once());
        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.gcStateKey(), current->bytes, current->etag, Retry::once())));
    });

    const UInt128 rebuild_id = hexToU128("00000000000000000000000000000014");
    ScopedLogCapture capture("CasGc", "warning");
    Gc gc(store, rebuild_id);
    const RebuildReport refused = gc.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(backend->didFire());
    EXPECT_FALSE(refused.performed);
    EXPECT_NE(refused.refusal.find("tbl_b"), String::npos) << refused.refusal;
    EXPECT_EQ(capture.count("could not give back its GC lease"), 1u) << capture.captured();
    EXPECT_EQ(decodeGcState(readObj(*backend, layout.gcStateKey())->bytes).lease.owner, rebuild_id)
        << "a lost give-back leaves the claim in place";
}

namespace
{

const RootNamespace kWriterNs{"00/aa@cas@"};
const UInt128 kReusedBody{7};

/// Writer X has a durable precommit of `part_x` whose manifest is still uploading, and it reused body
/// `kReusedBody`, which no committed part names.
ManifestRef seedBodilessPrecommitOverReusedBody(Backend & backend, const Layout & layout)
{
    writeBlobBody(backend, layout, kReusedBody);
    const ManifestRef pre = ref(5, 0xC5);
    addPrecommitTransition(backend, layout, kWriterNs, /*build_id*/ DB::UInt128(0x55), "part_x", std::nullopt, pre);
    return pre;
}

}

/// A rebuild derives edges from owner state, so a live precommit whose manifest it cannot read leaves
/// blobs uncounted; installing that baseline lets a later zero transition delete a body X still uses.
TEST(CASGCRebuild, RefusesABaselineThatCannotCountALivePrecommit)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const ManifestRef anchor = ref(1, 0xA1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, kWriterNs, anchor, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, kWriterNs, "tbl", std::nullopt, anchor);
    {
        Gc gc(store, kGc);
        gc.runRegularRound();
    }
    /// An adopted baseline under a lease nobody holds, which a rebuild with a fresh identity can claim.
    {
        const auto current = readObj(*backend, layout.gcStateKey());
        ASSERT_TRUE(current.has_value());
        GcState freed = decodeGcState(current->bytes);
        ASSERT_GT(freed.snap_generation, 0u);
        freed.lease = GcLease{};
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.gcStateKey(), encodeGcState(freed), current->etag, Retry::once())));
    }

    const ManifestRef pre = seedBodilessPrecommitOverReusedBody(*backend, layout);
    const auto before = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(before.has_value());

    backend->clearJournal();
    Gc rebuild(store, hexToU128("00000000000000000000000000000021"));
    const RebuildReport refused = rebuild.rebuildBaseline(/*force*/ true);
    EXPECT_FALSE(refused.performed);
    EXPECT_NE(refused.refusal.find("live precommit '00/aa@cas@/part_x'"), String::npos) << refused.refusal;
    EXPECT_NE(refused.refusal.find("stale-precommit sweep"), String::npos) << refused.refusal;

    const auto after = readObj(*backend, layout.gcStateKey());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bytes, before->bytes) << "a refused rebuild changed gc/state";
    EXPECT_EQ(writesAfterLastProbeOf(backend->journal(), layout.manifestKey(ManifestId{kWriterNs, pre})),
              std::vector<String>{layout.gcStateKey()}) << "the only write after the refusal is the lease give-back";
}

/// The loss sequence behind the refusal: the precommit's manifest lands, X promotes, and another part
/// adds and drops the reused body. The body must survive every round that follows.
TEST(CASGCRebuild, LivePrecommitWithoutAManifestKeepsItsBodyThroughARebuild)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const ManifestRef pre = seedBodilessPrecommitOverReusedBody(*backend, layout);

    Gc gc(store, kGc);
    const RebuildReport refused = gc.rebuildBaseline(/*force*/ false);
    ASSERT_FALSE(refused.performed);
    ASSERT_NE(refused.refusal.find("live precommit '00/aa@cas@/part_x'"), String::npos) << refused.refusal;

    writeManifestRaw(*backend, layout, kWriterNs, pre, {blobEntryFor("x", kReusedBody)});
    promoteTransition(*backend, layout, kWriterNs, /*build_id*/ DB::UInt128(0x55), "part_x", pre);
    const ManifestRef other = ref(6, 0xD6);
    writeManifestRaw(*backend, layout, kWriterNs, other, {blobEntryFor("o", kReusedBody)});
    publishCommittedTransition(*backend, layout, kWriterNs, "part_other", std::nullopt, other);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, kWriterNs, "part_other", other);

    EXPECT_FALSE(runRoundsUntilAbsent(store, gc, *backend, layout, kReusedBody))
        << "the body committed part_x names was deleted";
}

/// An error thrown after the claim, not a returned refusal, still reaches the caller when `gc/state` moved after the
/// claim.
TEST(CASGCRebuild, LostLeaseGiveBackKeepsAThrownErrorAndWarns)
{
    auto backend = std::make_shared<SweepReadHookBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const NamespaceLifeId life = seedSkippedEpochChain(
        *backend, layout, RootNamespace{"00/rebuild-lost-give-back@cas@"}, UInt128{0xF103}, store->poolConfig().gc_shards);
    /// The moved checkpoint makes the rebuild throw; the moved `gc/state` makes its give-back lose.
    backend->arm(layout.refLogKey(life, RefTxnId{1, 1}), 1, [&]
    {
        OperationForTest op(*backend);
        for (const String & key : {layout.refCkptKey(life), layout.gcStateKey()})
        {
            const auto current = (*op).read(key, Retry::once());
            ASSERT_TRUE(current.has_value());
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, current->bytes, current->etag, Retry::once())));
        }
    });

    const UInt128 rebuild_id = hexToU128("00000000000000000000000000000024");
    ScopedLogCapture capture("CasGc", "warning");
    Gc gc(store, rebuild_id);
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)gc.rebuildBaseline(/*force*/ false); });
    ASSERT_TRUE(backend->didFire());
    EXPECT_EQ(capture.count("could not give back its GC lease"), 1u) << capture.captured();
    EXPECT_EQ(decodeGcState(readObj(*backend, layout.gcStateKey())->bytes).lease.owner, rebuild_id);
}

namespace
{

/// Fails every conditional write to `gc/state` while `fail_gc_state_replace` is set.
class GcStateReplaceFailsBackend final : public CountingBackend
{
public:
    std::atomic<bool> fail_gc_state_replace{false};
    String gc_state_key;

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             DB::Cas::TransportAccess & access) override
    {
        if (fail_gc_state_replace && expected_value && key == gc_state_key)
            throw std::runtime_error("GcStateReplaceFailsBackend: the give-back write fails");
        return CountingBackend::write(key, bytes, expected_value, access);
    }
};

}

/// The give-back never throws: when its own write fails, the caller still gets the refusal, with a warning.
TEST(CASGCRebuild, FailingLeaseGiveBackLeavesTheRefusalToTheCaller)
{
    auto backend = std::make_shared<GcStateReplaceFailsBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    backend->gc_state_key = layout.gcStateKey();
    backend->fail_gc_state_replace = true;
    seedBodilessPrecommitOverReusedBody(*backend, layout);

    const UInt128 rebuild_id = hexToU128("00000000000000000000000000000025");
    ScopedLogCapture capture("CasGc", "warning");
    Gc gc(store, rebuild_id);
    const RebuildReport refused = gc.rebuildBaseline(/*force*/ false);
    EXPECT_FALSE(refused.performed);
    EXPECT_NE(refused.refusal.find("live precommit '00/aa@cas@/part_x'"), String::npos) << refused.refusal;
    EXPECT_EQ(capture.count("could not give back its GC lease"), 1u) << capture.captured();
    EXPECT_EQ(capture.count("the give-back write fails"), 1u) << capture.captured();
    EXPECT_EQ(decodeGcState(readObj(*backend, layout.gcStateKey())->bytes).lease.owner, rebuild_id)
        << "a failed give-back leaves the claim in place";
}

/// The refusal is a wait, not a verdict: once the manifest is readable, a rebuild with a fresh GC identity,
/// as each run of `clickhouse-disks cas-gc-rebuild` takes, counts the precommit.
TEST(CASGCRebuild, RebuildRefusedForAnUnreadableManifestSucceedsOnceItIsReadable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const ManifestRef pre = seedBodilessPrecommitOverReusedBody(*backend, layout);

    Gc refused(store, hexToU128("00000000000000000000000000000022"));
    const RebuildReport refusal = refused.rebuildBaseline(/*force*/ false);
    ASSERT_FALSE(refusal.performed);
    ASSERT_NE(refusal.refusal.find("live precommit '00/aa@cas@/part_x'"), String::npos) << refusal.refusal;

    writeManifestRaw(*backend, layout, kWriterNs, pre, {blobEntryFor("x", kReusedBody)});
    /// The refused attempt left a generation-0 `gc/state` with the never-held lease, which looks healthy.
    Gc retry(store, hexToU128("00000000000000000000000000000023"));
    const RebuildReport rep = retry.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_EQ(rep.live_precommits, 1u);
    EXPECT_EQ(rep.clamped_shards, 0u);
}
