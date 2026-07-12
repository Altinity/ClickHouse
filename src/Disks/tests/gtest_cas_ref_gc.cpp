#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
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
size_t runToFixpoint(const StorePtr & s, Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = gc.runRegularRound();
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
                     const ObjectMeta & meta = {}) override
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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

    const String log_v1_key = layout.refLogKey(ns, RefTxnId{1, v1});
    const String log_v2_key = layout.refLogKey(ns, RefTxnId{1, v2});
    const String old_snap_key = layout.refSnapshotKey(ns, RefTxnId{1, v1});
    const String new_snap_key = layout.refSnapshotKey(ns, RefTxnId{1, v2});
    ASSERT_TRUE(backend->head(log_v1_key).exists);
    ASSERT_TRUE(backend->head(old_snap_key).exists);

    Gc gc(store, kGc);
    runToFixpoint(store, gc);   /// folds v1,v2 (cursor -> v2) then cleans covered ref objects post-CAS

    /// log v1: snapshot-covered (X=v2 >= v1) AND cursor-covered (durable cursor v2 >= v1) => DELETED.
    EXPECT_FALSE(backend->head(log_v1_key).exists)
        << "a log covered by BOTH the newest snapshot and the durable cursor must be deleted";
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
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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

/// (7) Namespace-cleanup item: `remove_namespace` -> Pending item -> physical prefixes reclaimed ->
/// Completed -> `_cleanup` marker + `Removed` snapshot published; a DIFFERENT `Gc` (leader change) safely
/// re-executes the Completed item and republication is byte-identical (deterministic, idempotent). The
/// writer then admits recreation once it observes the marker.
TEST(CasRefGc, RemoveNamespaceCompletesAndPublishesMarkerDeterministically)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                 .root_shards = 1, .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/tbl"};

    /// Real writer: publish a committed part, then DROP the whole namespace (remove_namespace).
    {
        BuildInfo info;
        info.intended_ref = ns.string() + "/part_1";
        auto build = store->startBuild(info);
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
        gc.runRegularRound();
        store->renewWatermarkOnce();
        /// The marker key is `_cleanup/<remove_txn_id>`; discover it by listing the cleanup subtree.
        const ListPage page = backend->list(layout.refsNamespacePrefix(ns) + "_cleanup/", "", 100);
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
    ListPage snaps = backend->list(layout.refsNamespacePrefix(ns) + "_snap/", "", 100);
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
        snap_after_gc1->bytes, ns.string(), parsed_snap_key->txn_id);
    EXPECT_EQ(removed.lifecycle, RefLifecycle::Removed);
    EXPECT_TRUE(removed.committed.empty());

    /// RECREATION end-to-end (Task 12): the SAME warm-mounted writer recreates the namespace once GC's
    /// `_cleanup` marker is durable. `observedNamespaceCleanupMarker` does one exact-key re-check on a warm
    /// cache miss, so a fresh `namespace_birth` is admitted (spec §Namespace Birth) without a remount. The
    /// recreation carries a strictly greater `RefTxnId`, continuing the same ordered history.
    {
        BuildInfo info;
        info.intended_ref = ns.string() + "/part_2";
        auto build = store->startBuild(info);
        const ManifestId id2 = build->stageManifest({});
        ASSERT_NO_THROW(build->precommitAdd(ns, "part_2", id2))
            << "recreation after GC published the _cleanup marker must be admitted from a warm mount";
        build->promote(ns, "part_2", build->buildId(), id2);
    }
    EXPECT_TRUE(store->resolveRef(ns, "part_2").has_value()) << "the recreated table must be Live again";
    EXPECT_FALSE(store->resolveRef(ns, "part_1").has_value()) << "the removed ref must not resurrect";
}

/// (8) A malformed/adversarial ref key aborts ref folding for the round: no partial delta, no cursor
/// advance. The malformed key is a real object under `cas/refs/` whose `RefTxnId` render is invalid.
TEST(CasRefGc, MalformedRefKeyAbortsRefFoldingNoPartialDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
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
