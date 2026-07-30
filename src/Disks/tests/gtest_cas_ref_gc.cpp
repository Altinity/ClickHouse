#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include "cas_test_helpers.h"

#include <Common/ProfileEvents.h>

#include <set>

/// Task 12 required GC tests over the snapshot+log ref model (spec 2026-07-11-cas-ref-table-snapshot-log-design).
/// Every fixture produces REAL wire-format ref logs (via the writer or `writeRefLogTxnRaw`, never hand-rolled
/// bytes), and every test proves the fold actually consumed them (cursor advanced / nonzero in-degree), so a
/// silent no-op fold cannot pass vacuously.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

namespace ProfileEvents
{
extern const Event CasRefGlobalListPages;
extern const Event CasRefLogBodyGets;
extern const Event CasRefManifestBodyFoldGets;
extern const Event CasRefEmittedEdges;
extern const Event CasRefCleanupObjectsDeleted;
}

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
const UInt128 kGc2 = hexToU128("00000000000000000000000000000002");

ManifestRef mref(uint64_t seq, uint32_t ord = 1)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = ord};
}

/// Append a committed-ref log at an EXPLICIT sequence (no per-call LIST) -- fast bulk seeding of a
/// >1000-key stream. The ops are replay-valid (birth on the first, then add-precommit + promote).
void seedCommittedAt(
    Backend & backend, const Layout & layout, const RootNamespace & ns, uint64_t seq,
    const String & ref_name, const ManifestRef & mr, bool birth)
{
    std::vector<RefOp> ops;
    if (birth)
        ops.push_back(namespaceBirthOp());
    const std::vector<RefOp> commit_ops = publishCommittedOps(ref_name, mr);
    ops.insert(ops.end(), commit_ops.begin(), commit_ops.end());
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = RefTxnId{1, seq};
    txn.ops = std::move(ops);
    writeRefLogTxnRaw(backend, layout, txn);
}

/// Drive regular rounds, renewing the mount ack after each, until quiescent or `max_rounds`.
size_t runToFixpoint(const PoolPtr & s, Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        if (!rep.acquired_lease)
            continue;
        s->renewWatermarkOnce();
        const bool no_work = rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0;
        if (no_work && !anyCondemnedInSeal(s->backend(), s->layout()))
            break;
    }
    return rounds;
}

bool blobPresent(Backend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)})).exists;
}

/// Denies ONCE the single round-commit `gc/state` CAS that advances `snap_generation` (the losing
/// leader deposed mid-round). The denied round leaves only never-adopted attempt-scoped debris.
class DeposeRoundCommitBackend : public InMemoryBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        if (arm && key == "p/gc/state")
        {
            const auto stored = get(key);
            const uint64_t stored_gen = stored ? decodeGcState(stored->bytes).snap_generation : 0;
            if (decodeGcState(bytes).snap_generation > stored_gen)
            {
                arm = false;
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                    "test-injected: round-commit gc/state CAS denied (losing leader deposed mid-round)");
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }
    bool arm = false;
};
}

/// (1) A >1000-key ref scan folds every pre-existing log exactly once: the cursor advances to the greatest
/// id and every referenced blob has in-degree exactly 1 (folded once, not skipped, not doubled).
TEST(CasRefGc, LargeRefScanFoldsEveryLogExactlyOnce)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    constexpr uint64_t N = 1200;   /// > 1000: forces multi-page LIST paging in the fold's global scan
    for (uint64_t i = 1; i <= N; ++i)
    {
        const ManifestRef mr = mref(i);
        writeManifestRaw(*backend, layout, ns, mr, {blobEntryFor("data", DB::UInt128(i))});
        seedCommittedAt(*backend, layout, ns, /*seq*/ i, "t" + std::to_string(i), mr, /*birth*/ i == 1);
    }

    Gc gc(store, kGc);
    ASSERT_NO_THROW(gc.runRegularRound());

    /// The durable cursor advanced to the greatest log id.
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), N)
        << "the fold must advance the per-table cursor to the greatest pre-existing log id";

    /// Every referenced blob folded EXACTLY once (in-degree 1). Spot-check a spread across the >1000 set.
    for (uint64_t i : {uint64_t{1}, uint64_t{2}, uint64_t{999}, uint64_t{1000}, uint64_t{1001}, N})
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1)
            << "blob " << i << " must be folded exactly once (not skipped, not doubled)";
}

/// (2) A concurrent log appended AFTER the round's scan has passed its table is NOT skipped: the sealed
/// cursor stays below it, and the next round folds it.
TEST(CasRefGc, ConcurrentLogAfterScanIsFoldedNextRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = mref(1);
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t v1 = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    gc.runRegularRound();   /// round 1 folds v1
    ASSERT_EQ(foldCursorOf(*backend, layout, ns, 0), v1);
    ASSERT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 1);

    /// A NEW log lands after the round sealed its cursor at v1 (a concurrent writer).
    const ManifestRef r2 = mref(2);
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    const uint64_t v2 = publishCommittedTransition(*backend, layout, ns, "tbl2", std::nullopt, r2);
    ASSERT_GT(v2, v1);

    /// The sealed cursor is still v1 (< v2) -- the new log was never skipped past.
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), v1)
        << "a log that landed after the scan must remain below the durable cursor, never skipped";

    gc.runRegularRound();   /// round 2 folds v2
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), v2);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1)
        << "the next round must fold the concurrently-appended log";
}

/// (3) Fold barrier: a live precommit whose manifest body is absent clamps the table cursor below its
/// log (an anomaly is recorded), then folds once the body appears.
TEST(CasRefGc, FoldBarrierClampsBelowMissingBodyThenFoldsOnAppear)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef pre = mref(7);
    /// No writeManifestRaw for `pre`: its body is intentionally absent (the live precommit's barrier).
    const uint64_t v = addPrecommitTransition(*backend, layout, ns, DB::UInt128(9), "part", std::nullopt, pre);

    Gc gc(store, kGc);
    RoundReport report;
    ASSERT_NO_THROW(report = gc.runRegularRound());
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0)) << "a missing live-precommit body must record an anomaly";
    EXPECT_LT(foldCursorOf(*backend, layout, ns, 0), v)
        << "the barrier must clamp the durable cursor BELOW the bodiless-precommit log";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 0);

    /// The body appears (the build finished staging): the next fold passes the barrier.
    writeManifestRaw(*backend, layout, ns, pre, {blobEntryFor("p", DB::UInt128(1))});
    gc.runRegularRound();
    EXPECT_GE(foldCursorOf(*backend, layout, ns, 0), v) << "the barrier lifts once the body lands";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 1);
}

/// (4) Edge cancellation: a manifest added then removed across a batch nets to zero in-degree and the
/// exclusively-owned blob is reclaimed.
TEST(CasRefGc, EdgeCancellationAddThenRemoveReclaimsBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = mref(1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);   /// +1 for r's blob
    dropRefTransition(*backend, layout, ns, "tbl", r);                          /// -1: the add is cancelled

    Gc gc(store, kGc);
    ASSERT_TRUE(runToFixpoint(store, gc) < 64u) << "the add+remove batch must converge to a fixpoint";

    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 0)
        << "an added-then-removed manifest nets to zero in-degree";
    EXPECT_FALSE(blobPresent(*backend, layout, DB::UInt128(1)))
        << "the net-zero blob is reclaimed";
}

/// (5) A losing generation commit adopts nothing and deletes nothing: a round whose single round-commit
/// `gc/state` CAS is denied (deposed mid-round) must NOT advance the adopted (snap_generation, snap_attempt)
/// and must NOT delete the condemned-but-unadopted blob. Its fold seal is durable only under its OWN
/// never-adopted attempt (harmless debris).
TEST(CasRefGc, LosingGenerationCommitAdoptsNothingDeletesNothing)
{
    auto backend = std::make_shared<DeposeRoundCommitBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = mref(1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();   /// round 1: folds the +1 and adopts it cleanly
    store->renewWatermarkOnce();
    const auto adopted = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(adopted.snap_generation, 0u);

    /// Drop the ref, then run the round whose commit is DENIED (losing leader).
    dropRefTransition(*backend, layout, ns, "tbl", r);
    backend->arm = true;
    EXPECT_ANY_THROW(gc.runRegularRound());
    backend->arm = false;

    /// The deposed round adopted NOTHING: the durable pointers are unchanged...
    const auto after = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    EXPECT_EQ(after.snap_generation, adopted.snap_generation)
        << "a denied round-commit CAS must not advance the adopted generation";
    EXPECT_EQ(after.snap_attempt, adopted.snap_attempt);
    /// ...and it deleted NOTHING: the blob its unadopted fold condemned is still present.
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(1)))
        << "a losing generation commit must never delete a blob against an unadopted fold";
}

/// (6) Ref-object cleanup honors all three conditions: a `_log` is deleted only when a covering snapshot
/// AND the durable cursor both cover it; an older `_snap` is deleted while the newest is kept.
TEST(CasRefGc, RefObjectCleanupHonorsAllThreeConditions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// Two committed publishes -> logs {1,1} and {1,2}.
    const ManifestRef r1 = mref(1);
    const ManifestRef r2 = mref(2);
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    const uint64_t v1 = publishCommittedTransition(*backend, layout, ns, "t1", std::nullopt, r1);
    const uint64_t v2 = publishCommittedTransition(*backend, layout, ns, "t2", std::nullopt, r2);

    /// Two observed snapshots: an OLD one covering only v1, and the NEWEST covering v2. Both are real
    /// wire-format snapshot objects (the recovery codec reads them).
    RefTableSnapshot old_snap = minimalLiveSnapshot(ns.string(), RefTxnId{1, v1},
        {committedRow("t1", r1)});
    RefTableSnapshot new_snap = minimalLiveSnapshot(ns.string(), RefTxnId{1, v2},
        {committedRow("t1", r1), committedRow("t2", r2)});
    writeRefSnapshotRaw(*backend, layout, old_snap);
    writeRefSnapshotRaw(*backend, layout, new_snap);

    const String log_v1_key = layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{1, v1});
    const String log_v2_key = layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{1, v2});
    const String old_snap_key = layout.refSnapshotKey(NamespaceLifeId::stageATransition(ns), RefTxnId{1, v1});
    const String new_snap_key = layout.refSnapshotKey(NamespaceLifeId::stageATransition(ns), RefTxnId{1, v2});
    ASSERT_TRUE(backend->head(log_v1_key).exists);
    ASSERT_TRUE(backend->head(log_v2_key).exists);
    ASSERT_TRUE(backend->head(old_snap_key).exists);

    Gc gc(store, kGc);
    runToFixpoint(store, gc);   /// folds v1,v2 (cursor -> v2) then cleans covered ref objects post-CAS

    /// log v1: snapshot-covered (X=v2 >= v1) AND cursor-covered (durable cursor v2 >= v1) => DELETED.
    EXPECT_FALSE(backend->head(log_v1_key).exists)
        << "a log covered by BOTH the newest snapshot and the durable cursor must be deleted";
    /// log v2 (the frontier log): the coverage/cursor boundary is `<=`, not `<`, so the log for the
    /// newest committed transition is deletable too once its own snapshot is durable -- only the
    /// SNAPSHOT boundary is strict `<` (checked below). Unlike the newest snapshot, the newest log is
    /// NOT specially retained.
    EXPECT_FALSE(backend->head(log_v2_key).exists)
        << "the log for the newest committed transition must also be deleted once it is snapshot- and cursor-covered";
    /// the older snapshot (< newest) is deleted; the newest is retained.
    EXPECT_FALSE(backend->head(old_snap_key).exists) << "an older snapshot must be deleted";
    EXPECT_TRUE(backend->head(new_snap_key).exists) << "the newest snapshot must be retained";
}

/// Task 13 (spec §implementation-impact / §GC Budget): one fold+clean round increments every ref-intake
/// observability counter -- global LIST pages (Q), log-body GETs (K), manifest-body fold GETs (H), emitted
/// manifest edges, and cleaned old ref objects (D). Before/after deltas prove each site actually fires.
TEST(CasRefGc, RefIntakeIncrementsObservabilityCounters)
{
    using ProfileEvents::global_counters;
    const auto list_pages_before = global_counters[ProfileEvents::CasRefGlobalListPages].load();
    const auto log_gets_before   = global_counters[ProfileEvents::CasRefLogBodyGets].load();
    const auto mf_gets_before    = global_counters[ProfileEvents::CasRefManifestBodyFoldGets].load();
    const auto edges_before      = global_counters[ProfileEvents::CasRefEmittedEdges].load();
    const auto cleaned_before    = global_counters[ProfileEvents::CasRefCleanupObjectsDeleted].load();

    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = mref(1);
    const ManifestRef r2 = mref(2);
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    const uint64_t v1 = publishCommittedTransition(*backend, layout, ns, "t1", std::nullopt, r1);
    const uint64_t v2 = publishCommittedTransition(*backend, layout, ns, "t2", std::nullopt, r2);
    /// A newest snapshot covering v2 so cleanup can delete the covered logs once folded.
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), RefTxnId{1, v2}, {committedRow("t1", r1), committedRow("t2", r2)}));
    (void)v1;

    Gc gc(store, kGc);
    runToFixpoint(store, gc);

    EXPECT_GT(global_counters[ProfileEvents::CasRefGlobalListPages].load(), list_pages_before);
    EXPECT_GT(global_counters[ProfileEvents::CasRefLogBodyGets].load(), log_gets_before);
    EXPECT_GT(global_counters[ProfileEvents::CasRefManifestBodyFoldGets].load(), mf_gets_before);
    EXPECT_GT(global_counters[ProfileEvents::CasRefEmittedEdges].load(), edges_before);
    EXPECT_GT(global_counters[ProfileEvents::CasRefCleanupObjectsDeleted].load(), cleaned_before);
}

/// Task 13 e2e (in-process regression twin of the rustfs integration test): the whole snapshot+log
/// lifecycle over real wire-format objects and real GC rounds -- publish committed refs across two
/// tables, replace one (dropping a blob), publish a covering snapshot, drive GC to a fixpoint, and
/// assert the fold + ref-object cleanup + snapshot lifecycle plus the two read-only consumers:
/// `runFsck(*store).clean()` (the fsck CLI's verdict, oracle included) and `gc.previewDeletes().empty()`
/// (what `ca-gc-dryrun` reports). This is the deterministic permanent twin the unit sweep keeps running.
TEST(CasRefGc, RefSnaplogLifecycleE2E)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns_a{"00/aa@cas@"};
    const RootNamespace ns_b{"00/bb@cas@"};

    /// Two tables with committed refs naming present manifests + blobs (insert-like). ns_a's ref is then
    /// re-published to a second manifest, dropping the first manifest's blob (a replace: -1 old, +1 new).
    const ManifestRef a1 = mref(1);
    const ManifestRef a2 = mref(2);
    const ManifestRef b1 = mref(3);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeBlobBody(*backend, layout, DB::UInt128(2));
    writeBlobBody(*backend, layout, DB::UInt128(3));
    writeManifestRaw(*backend, layout, ns_a, a1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, layout, ns_a, a2, {blobEntryFor("a", DB::UInt128(2))});
    writeManifestRaw(*backend, layout, ns_b, b1, {blobEntryFor("b", DB::UInt128(3))});
    const uint64_t va1 = publishCommittedTransition(*backend, layout, ns_a, "t", std::nullopt, a1);
    const uint64_t va2 = publishCommittedTransition(*backend, layout, ns_a, "t", a1, a2);   /// replace a1 -> a2
    publishCommittedTransition(*backend, layout, ns_b, "t", std::nullopt, b1);

    /// The writer's compaction: a snapshot of ns_a covering its greatest log (va2), the same
    /// deterministic bytes the oracle recomputes.
    const RefTableState sa = recoverRefTable(*backend, layout, ns_a);
    writeRefSnapshotRaw(*backend, layout, snapshotOf(sa, ns_a.string()));

    Gc gc(store, kGc);
    runToFixpoint(store, gc);

    /// Snapshot lifecycle: the covering snapshot is retained; the covered logs (folded + snapshot-covered)
    /// are cleaned; the replaced manifest's blob is reclaimed while the live blobs survive.
    EXPECT_TRUE(backend->head(layout.refSnapshotKey(NamespaceLifeId::stageATransition(ns_a), RefTxnId{1, va2})).exists)
        << "covering snapshot retained";
    EXPECT_FALSE(backend->head(layout.refLogKey(NamespaceLifeId::stageATransition(ns_a), RefTxnId{1, va1})).exists) << "covered log cleaned";
    EXPECT_FALSE(blobPresent(*backend, layout, DB::UInt128(1))) << "replaced blob reclaimed";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(2))) << "live blob survives";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(3))) << "other table's blob survives";

    /// Read-only consumers agree: fsck clean (no dangle) and ca-gc-dryrun empty. The snapshot oracle takes
    /// its SKIP path here -- va1, a log covered by the retained va2 snapshot, was cleaned in this same run,
    /// so the oracle cannot reconstruct the state AT va2 and returns without comparing (spec: a cleaned
    /// covered log makes the oracle unavailable, not an error; `snapshot_oracle_checked` stays 0). The
    /// `snapshot_oracle_mismatches == 0` below therefore holds trivially, NOT via an active byte-compare;
    /// the positive byte-compare path (covered logs still surviving) is exercised by
    /// `CasFsckSnapshotOracle.PublishedSnapshotMatchingReplayIsClean`.
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_EQ(rep.snapshot_oracle_mismatches, 0u);
    EXPECT_TRUE(gc.previewDeletes().empty()) << "ca-gc-dryrun equivalent: no pending content deletes";
}

/// (7) Namespace-cleanup item: `remove_namespace` -> Pending item -> physical prefixes reclaimed ->
/// Completed -> `_cleanup` marker + `Removed` snapshot published; a DIFFERENT `Gc` (leader change) safely
/// re-executes the Completed item and republication is byte-identical (deterministic, idempotent). The
/// writer then admits recreation once it observes the marker.
TEST(CasRefGc, RemoveNamespaceCompletesAndPublishesMarkerDeterministically)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                 .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/tbl"};

    /// Real writer: publish a committed part, then DROP the whole namespace (remove_namespace).
    {
        PartWriteInfo info;
        info.intended_ref = ns.string() + "/part_1";
        auto build = store->beginPartWrite(info);
        const ManifestId id = build->stageManifest({});
        build->precommitAdd(ns, "part_1", id);
        build->promote(ns, "part_1", build->buildId(), id);
    }
    store->dropNamespace(ns);
    store->renewWatermarkOnce();

    /// The removal transaction routes a `{ns, remove_txn_id}` namespace-cleanup item; drive GC until it
    /// reaches Completed and publishes the `_cleanup` marker (physical prefixes reclaimed first).
    Gc gc(store, kGc);
    String marker_key;
    bool marker_seen = false;
    for (int i = 0; i < 24 && !marker_seen; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        /// The marker key is `_cleanup/<remove_txn_id>`; discover it by listing the cleanup subtree.
        const ListPage page = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_cleanup/", "", 100);
        if (!page.keys.empty())
        {
            marker_key = page.keys.front().key;
            marker_seen = true;
        }
    }
    ASSERT_TRUE(marker_seen) << "GC must publish the namespace `_cleanup` marker once the item completes";

    /// Capture the marker + published `Removed` snapshot bytes.
    const auto marker_after_gc1 = backend->get(marker_key);
    ASSERT_TRUE(marker_after_gc1.has_value());
    ListPage snaps = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_snap/", "", 100);
    ASSERT_FALSE(snaps.keys.empty()) << "a Removed snapshot must be published for the removed namespace";
    const String removed_snap_key = snaps.keys.front().key;
    const auto snap_after_gc1 = backend->get(removed_snap_key);
    ASSERT_TRUE(snap_after_gc1.has_value());

    /// DEPOSED-LEADER LATE PUBLICATION: a SECOND Gc (different id) re-executes the Completed item. Both
    /// publications are strict `putIfAbsent`, so the second is a byte-equal no-op -- never CORRUPTED_DATA,
    /// and the resulting objects are byte-identical regardless of which leader wrote them (both derive the
    /// bytes solely from {ns, remove_txn_id}: the deterministic Removed-snapshot encoding + empty marker).
    Gc gc2(store, kGc2);
    ASSERT_NO_THROW(runToFixpoint(store, gc2));
    const auto marker_after_gc2 = backend->get(marker_key);
    const auto snap_after_gc2 = backend->get(removed_snap_key);
    ASSERT_TRUE(marker_after_gc2.has_value());
    ASSERT_TRUE(snap_after_gc2.has_value());
    EXPECT_EQ(marker_after_gc1->bytes, marker_after_gc2->bytes)
        << "the `_cleanup` marker must be byte-identical across leaders (deterministic republication)";
    EXPECT_EQ(snap_after_gc1->bytes, snap_after_gc2->bytes)
        << "the Removed snapshot must be byte-identical across leaders";

    /// The published `Removed` snapshot decodes to a Removed lifecycle carrying the removal id (proof the
    /// deterministic republication is the real snapshot, not opaque bytes).
    const auto parsed_snap_key = layout.parseRefObjectKey(removed_snap_key);
    ASSERT_TRUE(parsed_snap_key.has_value());
    const RefTableSnapshot removed = decodeRefTableSnapshot(
        openObject(FormatId::RefSnapshot, snap_after_gc1->bytes), ns.string(), parsed_snap_key->txn_id);
    EXPECT_EQ(removed.lifecycle, RefLifecycle::Removed);
    EXPECT_TRUE(removed.committed.empty());

    /// RECREATION end-to-end (Task 12): the SAME warm-mounted writer recreates the namespace once GC's
    /// `_cleanup` marker is durable. `observedNamespaceCleanupMarker` does one exact-key re-check on a warm
    /// cache miss, so a fresh `namespace_birth` is admitted (spec §Namespace Birth) without a remount. The
    /// recreation carries a strictly greater `RefTxnId`, continuing the same ordered history.
    {
        PartWriteInfo info;
        info.intended_ref = ns.string() + "/part_2";
        auto build = store->beginPartWrite(info);
        const ManifestId id2 = build->stageManifest({});
        ASSERT_NO_THROW(build->precommitAdd(ns, "part_2", id2))
            << "recreation after GC published the _cleanup marker must be admitted from a warm mount";
        build->promote(ns, "part_2", build->buildId(), id2);
    }
    EXPECT_TRUE(store->resolveRef(ns, "part_2").has_value()) << "the recreated table must be Live again";
    EXPECT_FALSE(store->resolveRef(ns, "part_1").has_value()) << "the removed ref must not resurrect";
}

/// (7b) LIVENESS (spec §Step 6 bounded tails): the round that COMPLETES a `remove_namespace` removal --
/// i.e. the round in which GC republishes the `Removed` snapshot because the writer stopped before doing
/// so (spec §Namespace Removal: "if it stops first, the namespace-cleanup item republishes it") -- must
/// also clean the namespace's now-covered `_log` debris. The defect: `cleanupRefObjects` ran BEFORE
/// `runNamespaceCleanupPasses` republished the `Removed` snapshot, so the completing round never saw its
/// own covering snapshot; on a quiesced pool no later fold reruns cleanup with the snapshot visible, so
/// the covered logs persist as `cas/refs/` debris indefinitely. Drive GC only until the removal completes
/// (the `_cleanup` marker appears) -- the natural quiescence point for a removed namespace, mirroring
/// what a real GC scheduler reaches -- then assert the covered logs are gone, leaving only the
/// constant-size `Removed` snapshot + `_cleanup` marker tombstone (spec §Object Layout: "Phase 1 never
/// deletes markers").
TEST(CasRefGc, RemovedNamespaceCoveredLogsCleanedByCompletingRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Default fold cadence (gc_fold_max_defer_rounds = 8): the quiesced-pool regime where the gap bites.
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/tbl"};

    /// Real writer: publish several committed parts (each is a `_log`), then DROP the whole namespace.
    for (int i = 1; i <= 4; ++i)
    {
        PartWriteInfo info;
        const String part = "part_" + std::to_string(i);
        info.intended_ref = ns.string() + "/" + part;
        auto build = store->beginPartWrite(info);
        const ManifestId id = build->stageManifest({});
        build->precommitAdd(ns, part, id);
        build->promote(ns, part, build->buildId(), id);
    }
    store->dropNamespace(ns);
    store->renewWatermarkOnce();

    const auto countKind = [&](RefObjectKind want) -> size_t
    {
        size_t n = 0;
        const ListPage page = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)), "", 10000);
        for (const ListedKey & lk : page.keys)
            if (const auto p = layout.parseRefObjectKey(lk.key); p && p->kind == want)
                ++n;
        return n;
    };

    /// Model "the writer stopped before publishing the Removed snapshot": remove every `_snap` object the
    /// writer published at drop time, so GC's namespace-cleanup item is the sole publisher of the Removed
    /// snapshot (spec §Namespace Removal republication path). This is exactly the interleaving that
    /// surfaces the ordering gap -- when the writer DOES publish, `cleanupRefObjects` already sees the
    /// covering snapshot from round one and the gap never bites.
    {
        const ListPage snaps = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_snap/", "", 10000);
        for (const ListedKey & lk : snaps.keys)
        {
            const auto h = backend->head(lk.key);
            if (h.exists)
                backend->deleteExact(lk.key, h.token);
        }
    }
    ASSERT_EQ(countKind(RefObjectKind::Snap), 0u) << "the writer's Removed snapshot is gone (writer stopped)";
    /// Sanity: there ARE `_log` objects to clean (a vacuous fold must not let this "pass").
    ASSERT_GT(countKind(RefObjectKind::Log), 0u) << "the dropped namespace must have _log objects to clean";

    /// Drive rounds ONLY until the removal completes (the `_cleanup` marker is published). Do NOT
    /// over-drive: a later forced fold would mask the defect by cleaning on a subsequent round.
    Gc gc(store, kGc);
    bool completed = false;
    for (int i = 0; i < 32 && !completed; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        completed = countKind(RefObjectKind::Cleanup) > 0;
    }
    ASSERT_TRUE(completed) << "GC must complete the namespace removal (publish the `_cleanup` marker)";

    /// The completing round must have cleaned the covered logs; only the tombstone remains.
    EXPECT_EQ(countKind(RefObjectKind::Log), 0u)
        << "the round that completes the removal must clean the namespace's covered `_log` debris "
           "(spec §Step 6 bounded tails); leaving them behind is the liveness gap";
    EXPECT_EQ(countKind(RefObjectKind::Snap), 1u)
        << "only the constant-size Removed snapshot tombstone remains";
    EXPECT_EQ(countKind(RefObjectKind::Cleanup), 1u) << "the `_cleanup` marker tombstone remains";
}

/// (8) A malformed/adversarial ref key aborts ref folding for the round: no partial delta, no cursor
/// advance. The malformed key is a real object under `cas/refs/` whose `RefTxnId` render is invalid.
TEST(CasRefGc, MalformedRefKeyAbortsRefFoldingNoPartialDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = mref(1);
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);

    /// Plant a malformed ref key under the ref prefix (a `_log` with a non-canonical id render).
    backend->putIfAbsent(layout.casRefsPrefix() + ns.string() + "/_log/not-a-valid-txn-id", "garbage");

    Gc gc(store, kGc);
    /// The fold's `groupRefKeys` rejects the unrecognized key and ABORTS ref folding for the round (spec
    /// §Step 2: a malformed key cannot produce a partial ref delta or authorize destructive work). The
    /// round CATCHES this internally and survives -- it must not propagate, and must not fold anything.
    ASSERT_NO_THROW(gc.runRegularRound());

    /// No partial delta, no cursor advance: the valid log's blob was NOT folded.
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 0)
        << "a malformed ref key must abort the round before any partial ref delta lands";
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), 0u)
        << "the durable cursor must not advance on an aborted round";
}

/// (8b) The un-incarnated (Stage A) key shape is the OTHER way a ref key can be malformed, and it must
/// land on exactly the path (8) pins -- abort ref folding, record the anomaly, COMPLETE the round.
///
/// It gets its own test because the failure mode is worse than a lost round. The parser REFUSES this
/// shape by name rather than returning `std::nullopt`, so it is the one malformed key that can throw
/// from the round's global `cas/refs/` enumeration, which runs in `defer_decision` -- before the fold,
/// and outside the fold's catch. Escaping there does not merely fail one round: GC is the only thing
/// that could ever delete the key, so a round that dies on it dies on it again every time, forever.
/// The enumeration must therefore absorb the refusal per key and leave the key unindexed in
/// `scan.keys`, exactly as it already does for every other malformed shape, and let `groupRefKeys`
/// raise it once where the round is ready to catch it.
TEST(CasRefGc, UnIncarnatedRefKeyAbortsRefFoldingWithoutWedgingTheRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = mref(1);
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);

    /// A ref log at the un-incarnated Stage A shape `<ns>/_log/<id>.zst`. Built by hand because no
    /// helper can mint one any more -- which is the point: only a foreign or corrupt writer can put
    /// this key here, and the pool must survive finding it.
    const String un_incarnated =
        layout.casRefsPrefix() + ns.string() + "/_log/" + renderRefTxnId(RefTxnId{1, 1}) + ".zst";
    ASSERT_EQ(backend->putIfAbsent(un_incarnated, "garbage").outcome, PutOutcome::Done);

    Gc gc(store, kGc);
    RoundReport rep;
    ASSERT_NO_THROW(rep = gc.runRegularRound())
        << "the round must COMPLETE: a key GC alone could remove must never abort the round that would";
    EXPECT_TRUE(rep.hasAnomaly(RootNamespace{}, /*shard*/ 0))
        << "the refusal must surface as the fold's abort anomaly, not vanish";
    EXPECT_EQ(rep.deleted, 0u);
    EXPECT_EQ(rep.redeleted, 0u);

    /// Same fail-close as (8): no partial delta, no cursor advance.
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 0)
        << "an aborted ref fold must land no partial ref delta";
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), 0u)
        << "the durable cursor must not advance on an aborted round";

    /// The wedge is only visible over time: the key is still there (nothing deletes it), so a second
    /// round meets it again. It must survive that one too.
    ASSERT_TRUE(backend->head(un_incarnated).exists) << "precondition: nothing removed the key";
    ASSERT_NO_THROW(gc.runRegularRound()) << "a round that dies on this key would die on it forever";
}

/// Coverage gap (Task 13a): a ref log at a CANONICAL key but with an undecodable BODY -- distinct from a
/// malformed *key* (which aborts earlier at the group step, above). This exercises the
/// GET-then-decode-throw path.
///
/// Its blast radius is the NAMESPACE, not the round (spec §5: the whole-round abort survives only for a
/// key that cannot be attributed to any namespace). The body sits at the position the arithmetic walk
/// reads next, so the walk stops there: everything below it stays folded (a transaction applies
/// atomically -- there is no partial delta either way), the cursor never moves past it, and the recorded
/// anomaly suppresses every destructive step of the round, so nothing the unfolded tail might still
/// reference can be reclaimed.
TEST(CasRefGc, InvalidRefLogBodyHoldsNamespaceNoPartialDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = mref(1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 1) << "published and folded";

    /// Now DROP the ref, so the blob is genuinely unreferenced once that record folds, and only then
    /// plant the invalid body at the walk's very next position. This ordering is what makes the
    /// suppression assertion below mean something: asserting that a LIVE blob survives a held round
    /// proves nothing, since a live blob is never reclaimable in the first place.
    const uint64_t dropped = dropRefTransition(*backend, layout, ns, "tbl", r);

    /// A canonical `_log` key (groupRefKeys accepts it) whose body cannot be decoded: the fold GETs it
    /// and `decodeRefLogTxn` throws.
    const String garbage_key = layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{1, dropped + 1});
    backend->putIfAbsent(garbage_key, "garbage-not-a-valid-reflog-body");

    /// Eight rounds under the hold. Each one catches the hold internally and survives.
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_NO_THROW(runRegularRoundReclaiming(gc));
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), dropped)
        << "the durable cursor must stop BELOW the invalid record, and never advance past it";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 0)
        << "the complete transaction below the invalid body folded -- the drop applied, so the blob is "
           "unreferenced and would be reclaimed by any unsuppressed round";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(1)))
        << "the held namespace's anomaly suppresses graduation and pending deletes: an unreferenced "
           "blob is NOT reclaimed while any namespace is held, because the unfolded tail behind the "
           "hold may still name it";

    /// DELETING THE EVIDENCE DOES NOT RELEASE THE HOLD. The hold is durable and clears by exactly one
    /// event -- the fold resolving its offending position -- so an object that stops answering does not
    /// turn the gap into a frontier. It is the same observation a lying store produces, and it is
    /// precisely what made the hold necessary; if an absent could clear it, the whole mechanism would
    /// be defeated by the corruption it exists to survive. (Before durable holds this delete DID
    /// release the namespace, which is the hole Task 8 closed.)
    const HeadResult h = backend->head(garbage_key);
    ASSERT_TRUE(h.exists);
    ASSERT_EQ(backend->deleteExact(garbage_key, h.token).kind, DeleteOutcome::Kind::Deleted);

    for (int i = 0; i < 4; ++i)
    {
        ASSERT_NO_THROW(runRegularRoundReclaiming(gc));
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(1)))
        << "the hold still stands: nothing resolved the offending position, an absent proved nothing";

    /// REPAIR is the release: a DECODABLE record at the offending position. The fold reads it, folds
    /// through it, seals a cursor above it -- and only then does the namespace stop being held and
    /// destruction resume. `publishCommittedTransition` allocates the freed id, which is exactly the
    /// position the hold names.
    const ManifestRef r2 = mref(2);
    writeBlobBody(*backend, layout, DB::UInt128(2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    ASSERT_EQ(publishCommittedTransition(*backend, layout, ns, "tbl2", std::nullopt, r2), dropped + 1)
        << "the repair must land ON the held position, not above it";

    ASSERT_TRUE(runToFixpoint(store, gc) < 64u) << "the released namespace must converge";
    EXPECT_EQ(foldCursorOf(*backend, layout, ns, 0), dropped + 1) << "the walk folded through the hold";
    EXPECT_FALSE(blobPresent(*backend, layout, DB::UInt128(1)))
        << "once the hold clears, the unreferenced blob is reclaimed -- so the survival above was the "
           "suppression doing its job, not the blob being unreclaimable";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(2))) << "the repair's own blob is referenced";
}

/// Coverage gap (Task 13a): the per-table baseline guard (spec §Offline Recovery) has no positive-trip
/// test at HEAD -- the adapted successor of the retired CasGcBaselineGuard.FreshStateOverTrimmedJournals
/// contract. A table whose logs at/below its newest snapshot are gone and that has no sealed fold cursor
/// is the "a prior fold advanced+cleaned covered logs, then gc/state was lost" signature: folding it from
/// {0,0} would emit no edges and mass-condemn its still-referenced blob. GC must refuse the round before
/// any delete. The existing CasGcBaselineGuard tests cover only the genuinely-fresh pass case and the
/// adopted-seal-missing guard, not this branch.
TEST(CasRefGc, BaselineGuardRefusesWhenSnapshotSurvivesWithoutLogsOrCursor)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();

    /// Table A is healthy (a committed ref with its manifest+blob, no snapshot), giving GC a normal table
    /// to fold in the same round.
    const RootNamespace ns_a{"00/aa@cas@"};
    const ManifestRef ra = mref(1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, ns_a, ra, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, layout, ns_a, "ta", std::nullopt, ra);

    /// Table B is poisoned: a durable snapshot survives, but its logs at/below it are GONE and B has no
    /// sealed cursor (first round -> no adopted parent cursors). This is the exact baseline-guard input.
    const RootNamespace ns_b{"00/bb@cas@"};
    const ManifestRef rb = mref(2);
    writeBlobBody(*backend, layout, DB::UInt128(2));
    writeManifestRaw(*backend, layout, ns_b, rb, {blobEntryFor("b", DB::UInt128(2))});
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns_b.string(), RefTxnId{1, 5},
        {committedRow("tb", rb)}));

    /// The baseline guard must fail closed BEFORE any destructive step (first round: no prior fold seal,
    /// so the failure can only come from the baseline guard, not the seal-divergence guard).
    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(1))) << "table A's blob survives the refusal";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(2)))
        << "table B's blob must NOT be condemned -- the guard fires before any delete";
}

/// (C3) A stale GC leader's `Pending` namespace-cleanup pass must NOT delete a recreated namespace's live
/// data. A leader deposed after its round CAS resumes its pass after a successor Completed the item,
/// published the `_cleanup` marker, and the writer recreated the namespace (successor-epoch manifests +
/// verbatim files). The marker's presence is the exact recreation precondition (spec §Namespace Birth):
/// the pass must abort on it. On the unfixed code the fresh LIST + exact-token delete reclaims the
/// recreated objects (verbatim files carry no epoch at all).
TEST(CasRefGc, StaleLeaderPendingPassAbortsOnCompletionMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// Establish gc/state under leader kGc (a real round commits round + lease.owner = kGc).
    Gc gc(store, kGc);
    gc.runRegularRound();
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_EQ(st.lease.owner, kGc);

    /// The removed incarnation removed at epoch 1; the stale leader still holds a Pending item for it.
    const RefTxnId remove_txn{1, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[ns.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = ns, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    /// A successor Completed the item + the writer recreated the namespace: the `_cleanup` marker is
    /// durable, and recreation wrote a successor-epoch (2 > 1) manifest and a verbatim file at a fixed key.
    backend->putIfAbsent(layout.refCleanupMarkerKey(NamespaceLifeId::stageATransition(ns), remove_txn), String{});
    const ManifestRef recreated = ManifestRef{.writer_epoch = 2, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, recreated, {blobEntryFor("a", DB::UInt128(1))});
    const String file_key
        = layout.namespaceFilesPrefix(NamespaceLifeId::stageATransition(ns)) + "format_version.txt";
    backend->putIfAbsent(file_key, "1");

    /// The stale leader runs its Pending pass at its (still-durable) round: the marker HEAD must abort it.
    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/false);

    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, recreated})).exists)
        << "the recreated successor-epoch manifest must survive the stale pass";
    EXPECT_TRUE(backend->head(file_key).exists)
        << "the recreated verbatim file must survive: the marker HEAD aborts the pass before deleting it";
}

/// (C3/N3) The `Pending` pass re-reads gc/state and aborts when a successor advanced the ROUND (strictly
/// incremented on every commit), never trusting the lease `seq` (a deposed-then-re-elected owner can
/// present the same seq). A deposed leader executing round R while durable gc/state is already R+1 must
/// delete nothing.
TEST(CasRefGc, StaleLeaderPendingPassAbortsWhenRoundAdvanced)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    Gc gc(store, kGc);
    gc.runRegularRound();
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_EQ(st.lease.owner, kGc);

    const RefTxnId remove_txn{2, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[ns.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = ns, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    /// A removed-incarnation manifest (epoch 2 <= remove epoch 2) the epoch filter would otherwise delete;
    /// no marker present, so ONLY the round-freshness guard can spare it.
    const ManifestRef removed_incarnation = ManifestRef{.writer_epoch = 2, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, removed_incarnation, {blobEntryFor("a", DB::UInt128(1))});

    /// Durable gc/state advances to a newer round (a successor committed) while this leader still holds R.
    {
        GcState advanced = st;
        advanced.round = st.round + 1;
        const HeadResult h = backend->head(layout.gcStateKey());
        backend->putOverwrite(layout.gcStateKey(), encodeGcState(advanced), h.token);
    }

    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, /*new_round=*/st.round, /*suppress_destructive*/false);

    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, removed_incarnation})).exists)
        << "a deposed leader (durable round advanced past its own) must delete nothing";
}

/// (C3) Manifest deletes are epoch-timing-independent: even when the pass runs (round fresh, no marker), a
/// manifest whose `writer_epoch` exceeds the removed incarnation's is recreated data and is never deleted,
/// while a manifest at/below the removed epoch is removed-incarnation debris and is reclaimed.
TEST(CasRefGc, PendingPassEpochFiltersManifestDeletes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    Gc gc(store, kGc);
    gc.runRegularRound();
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    const RefTxnId remove_txn{2, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[ns.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = ns, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    const ManifestRef old_incarnation = ManifestRef{.writer_epoch = 2, .build_sequence = 1, .manifest_ordinal = 1};
    const ManifestRef recreated = ManifestRef{.writer_epoch = 3, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, old_incarnation, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, layout, ns, recreated, {blobEntryFor("b", DB::UInt128(2))});

    /// No marker (marker guard inert), round fresh (round guard passes): the epoch filter is the guard.
    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/false);

    EXPECT_FALSE(backend->head(layout.manifestKey(ManifestId{ns, old_incarnation})).exists)
        << "a manifest at/below the removed incarnation's epoch is removed-incarnation debris and is reclaimed";
    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, recreated})).exists)
        << "a greater-epoch manifest is recreated data and must never be deleted, regardless of pass timing";
}

/// (I2) A recreated namespace must not drive a per-round republish/delete churn, and Completed cleanup
/// items must not accumulate in the seal forever. After a removed namespace is recreated with a Live
/// snapshot that supersedes the `Removed` one, `cleanupRefObjects` deletes the superseded `Removed`
/// snapshot; on the unfixed code the next round's Completed republication re-creates it and the round
/// after deletes it again -- one PUT + one DELETE every round, per recreated namespace, forever. The fix
/// retires the item once its artifacts are durably observed (marker present AND superseded), so neither
/// the churn nor unbounded seal growth occurs.
TEST(CasRefGc, RecreatedNamespaceRetiresCleanupItemAndStopsChurn)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                 .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/tbl"};

    /// Real writer: publish a committed part, then DROP the whole namespace.
    {
        PartWriteInfo info;
        info.intended_ref = ns.string() + "/part_1";
        auto build = store->beginPartWrite(info);
        const ManifestId id = build->stageManifest({});
        build->precommitAdd(ns, "part_1", id);
        build->promote(ns, "part_1", build->buildId(), id);
    }
    store->dropNamespace(ns);
    store->renewWatermarkOnce();

    /// Drive GC until the removal Completes (the `_cleanup` marker is published).
    Gc gc(store, kGc);
    RefTxnId remove_txn{};
    for (int i = 0; i < 32 && remove_txn == RefTxnId{}; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        const ListPage m = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_cleanup/", "", 10);
        if (!m.keys.empty())
        {
            const auto p = layout.parseRefObjectKey(m.keys.front().key);
            ASSERT_TRUE(p.has_value());
            remove_txn = p->txn_id;
        }
    }
    ASSERT_FALSE(remove_txn == RefTxnId{}) << "GC must complete the removal (publish the marker)";
    const String removed_snap_key = layout.refSnapshotKey(NamespaceLifeId::stageATransition(ns), remove_txn);
    ASSERT_TRUE(backend->head(removed_snap_key).exists) << "completion published the Removed snapshot";

    /// RECREATE: a fresh committed log with an id above the removal, plus a Live snapshot that supersedes
    /// the Removed snapshot (the writer's post-recreation compaction).
    const ManifestRef rec = mref(remove_txn.ref_sequence + 10);
    writeManifestRaw(*backend, layout, ns, rec, {blobEntryFor("z", DB::UInt128(99))});
    const uint64_t rec_log = appendRefLogSeed(*backend, layout, ns, publishCommittedOps("part_2", rec));
    const RefTxnId live_snapshot_id{1, rec_log};
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), live_snapshot_id, {committedRow("part_2", rec)}));
    ASSERT_GT(live_snapshot_id, remove_txn) << "the recreation's Live snapshot must supersede the Removed one";

    /// Run several folding rounds; measure how often the Removed snapshot is re-created.
    backend->resetCounts();
    for (int i = 0; i < 6; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(backend->putCount(removed_snap_key), 0u)
        << "the superseded Removed snapshot must not be re-created every round (the churn)";
    EXPECT_FALSE(backend->head(removed_snap_key).exists)
        << "the Removed snapshot stays deleted once a recreated namespace supersedes it";

    const uint64_t gen = currentGenerationOf(*backend, layout);
    const uint64_t attempt = currentAttemptOf(*backend, layout);
    const CasFoldSeal seal = decodeFoldSeal(backend->get(layout.foldSealKey(gen, attempt))->bytes);
    EXPECT_EQ(seal.ns_cleanup_items.count(ns.string() + "\n" + renderRefTxnId(remove_txn)), 0u)
        << "the Completed cleanup item retires once its artifacts are durably observed (no unbounded seal growth)";
}

/// (I2) The Completed branch still repairs a crash-lost `Removed` snapshot (marker durable, snapshot lost
/// between the two publishes, NO recreation): it republishes the snapshot exactly once (idempotent, not
/// churned), and once the snapshot is durably observed the item retires. This is the corner the
/// artifact-absence gate must preserve -- gating republication on marker-only would drop this repair.
TEST(CasRefGc, CompletedItemRepublishesCrashLostRemovedSnapshotThenRetires)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                 .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/tbl"};

    {
        PartWriteInfo info;
        info.intended_ref = ns.string() + "/part_1";
        auto build = store->beginPartWrite(info);
        const ManifestId id = build->stageManifest({});
        build->precommitAdd(ns, "part_1", id);
        build->promote(ns, "part_1", build->buildId(), id);
    }
    store->dropNamespace(ns);
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    RefTxnId remove_txn{};
    for (int i = 0; i < 32 && remove_txn == RefTxnId{}; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        const ListPage m = backend->list(layout.refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_cleanup/", "", 10);
        if (!m.keys.empty())
        {
            const auto p = layout.parseRefObjectKey(m.keys.front().key);
            ASSERT_TRUE(p.has_value());
            remove_txn = p->txn_id;
        }
    }
    ASSERT_FALSE(remove_txn == RefTxnId{});
    const String removed_snap_key = layout.refSnapshotKey(NamespaceLifeId::stageATransition(ns), remove_txn);
    ASSERT_TRUE(backend->head(removed_snap_key).exists);

    /// Model a crash between the marker PUT and the snapshot PUT: delete the Removed snapshot, keep the
    /// marker, and do NOT recreate the namespace (no newer snapshot supersedes it).
    { const HeadResult h = backend->head(removed_snap_key); backend->deleteExact(removed_snap_key, h.token); }
    ASSERT_FALSE(backend->head(removed_snap_key).exists);

    backend->resetCounts();
    runRegularRoundReclaiming(gc);   /// the Completed branch republishes the lost snapshot (absent AND not superseded)
    store->renewWatermarkOnce();
    EXPECT_TRUE(backend->head(removed_snap_key).exists) << "a crash-lost Removed snapshot is repaired";
    EXPECT_EQ(backend->putCount(removed_snap_key), 1u) << "republished exactly once (idempotent, not churned)";

    runRegularRoundReclaiming(gc);   /// now the artifacts are durably observed -> the item retires
    store->renewWatermarkOnce();
    const uint64_t gen = currentGenerationOf(*backend, layout);
    const uint64_t attempt = currentAttemptOf(*backend, layout);
    const CasFoldSeal seal = decodeFoldSeal(backend->get(layout.foldSealKey(gen, attempt))->bytes);
    EXPECT_EQ(seal.ns_cleanup_items.count(ns.string() + "\n" + renderRefTxnId(remove_txn)), 0u)
        << "once the marker + Removed snapshot are durably observed the item retires";
}

namespace
{
/// Makes the `_cleanup` marker read as ABSENT on its first HEAD and PRESENT thereafter -- a successor
/// publishing it mid-pass, exactly the window the per-KEY marker guard must close (the per-PAGE HEAD saw
/// it absent, then the per-key HEAD sees it present before the delete).
class MarkerAppearsAfterFirstHeadBackend : public InMemoryBackend
{
public:
    String marker_key;
    int marker_heads = 0;
    HeadResult head(const String & key) override
    {
        if (key == marker_key && ++marker_heads >= 2)
            return HeadResult{.exists = true, .size = 0, .token = Token{}, .attributes = {}};
        return InMemoryBackend::head(key);
    }
};
}

/// (C3 hardening) The per-KEY marker HEAD on the manifest branch closes the mid-pass recreation window the
/// epoch filter alone cannot: a WARM recreation reuses the same `live_writer_epoch` (bumped only at
/// open/remount), so its manifest carries `writer_epoch` EQUAL to the removed incarnation's and the
/// `> remove epoch` skip would NOT spare it. Here the marker is absent at the per-page HEAD (the pass
/// proceeds) but present before the same-epoch manifest's per-key HEAD -- which must abort and spare it.
TEST(CasRefGc, PendingPassPerKeyMarkerGuardSparesWarmSameEpochRecreation)
{
    auto backend = std::make_shared<MarkerAppearsAfterFirstHeadBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    Gc gc(store, kGc);
    gc.runRegularRound();
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_EQ(st.lease.owner, kGc);

    /// Removed at epoch 2; a WARM recreation writes a manifest at the SAME epoch 2 (greater build_sequence),
    /// which the `> remove epoch` skip does NOT spare -- only the per-key marker HEAD can.
    const RefTxnId remove_txn{2, 5};
    CasFoldSeal seal;
    seal.ns_cleanup_items[ns.string() + "\n" + renderRefTxnId(remove_txn)] =
        RefNsCleanupItem{.ns = ns, .remove_txn_id = remove_txn, .state = RefNsCleanupState::Pending};

    const ManifestRef warm_recreated = ManifestRef{.writer_epoch = 2, .build_sequence = 9, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, warm_recreated, {blobEntryFor("a", DB::UInt128(1))});

    /// Absent at the per-page HEAD (marker_heads == 1), present at the per-key HEAD (>= 2).
    backend->marker_key = layout.refCleanupMarkerKey(NamespaceLifeId::stageATransition(ns), remove_txn);

    gc.runNamespaceCleanupPassesForTest(seal, /*ref_tables*/{}, st.round, /*suppress_destructive*/false);

    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, warm_recreated})).exists)
        << "a same-epoch warm recreation's manifest must survive: the per-key marker HEAD aborts the pass "
           "even though the epoch filter would not spare it and the per-page HEAD saw the marker absent";
}
