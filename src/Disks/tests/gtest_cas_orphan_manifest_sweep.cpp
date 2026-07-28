#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"
#include <limits>
#include <vector>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
constexpr uint64_t kWriterEpoch = 7;
const String kServerRoot = "00";
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = kWriterEpoch, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// The §6 deletion premise (`manifestDeletionPremise`) is a SECOND precondition on every deletion below,
/// alongside the watermark eligibility these tests are about: a manifest of an epoch-`E` build is
/// deletable only once the namespace's sealed fold cursor sits in an epoch strictly above `E`. Tests
/// whose subject is the eligibility or ownership rule therefore have to establish it, or they would
/// assert a deletion the premise (not the rule under test) prevented. Tests whose subject is RETENTION
/// deliberately do NOT call this — see `CasSweepDeletionPremise` for the premise's own coverage.
void seedConsumedSealCursor(InMemoryBackend & backend, const Layout & layout, const RootNamespace & ns)
{
    seedFoldCursorForTest(backend, layout, ns, RefTxnId{kWriterEpoch + 1, 1});
}
}

/// A staged-but-unowned body in an ELIGIBLE prefix, absent from the owner view, is deleted (#7).
TEST(CasOrphanManifestSweep, EligibleAndUnownedIsDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);   // 6 > 5 => eligible
    seedConsumedSealCursor(*backend, store->layout(), ns);

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// A body that IS in the owner view (committed) is NEVER swept (#8).
TEST(CasOrphanManifestSweep, OwnedBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // now owned
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// GC-WEDGE regression (2026-07-10): a COMMITTED ref that has been DROPPED but whose removal `-1` is NOT
/// yet sealed (transition_version above the sealed fold cursor, which is 0 for this fresh pool) must
/// SURVIVE the sweep — the GC fold still needs the body to emit the `-1` (delete-after-sealed-decrements).
/// A promoted build retires its build_seq, so the prefix is watermark-eligible; before the fix the sweep
/// deleted the body in the dropRef→fold window → the removal-fold then clamped FOREVER on the missing
/// committed body → pool-wide GC stop. The pending-removal protection now covers COMMITTED (not only
/// PRECOMMIT) removals.
///
/// SINCE THE §6 PREMISE, this shape is held by TWO independent facts: the tail-removal protection this
/// test is named for, and the premise's rule (1) — the fixture seals no fold cursor, so epoch
/// `kWriterEpoch`'s closing seal is not consumed either. They cannot be separated HERE: the removal log
/// sits in a lower epoch than the build, so any cursor high enough to satisfy rule (1) would also sit
/// above the log and stop the tail scan from reading it at all. The case where the tail-removal
/// protection is the ONLY thing standing — a removal in a LATER epoch, which is the direction removals
/// actually cross — is `CasSweepDeletionPremise.AnUnconsumedTailRemovalRetainsItsTarget`.
TEST(CasOrphanManifestSweep, PendingCommittedRemovalBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);   // committed owner
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);   // dropped: pending committed removal, -1 unsealed
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);   // 6 > 5 => prefix eligible

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists)
        << "a dropped-but-unsealed committed manifest body must survive the sweep (delete-after-sealed-"
           "decrements) — else the removal-fold clamps forever on the missing body (GC-WEDGE-2026-07-10)";
}

/// The sweep emits NO blob deltas: the in-degree generation is unchanged.
TEST(CasOrphanManifestSweep, EmitsNoBlobDeltas)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);
    // The sweep must not advance the in-degree generation: capture it AFTER the fixture's own seal.
    const uint64_t gen_before = currentGenerationOf(*backend, store->layout());

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_EQ(currentGenerationOf(*backend, store->layout()), gen_before);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

TEST(CasOrphanManifestSweep, CursorPageAdvancesAndWrapsWithListBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xE1);
    const ManifestRef r2 = ref(5, 0xE2);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);

    const ManifestSweepResult first = sweepManifestCursorPage(*store, "", /*list_budget*/1, /*delete_budget*/0);
    EXPECT_EQ(first.listed, 1u);
    EXPECT_FALSE(first.wrapped);
    EXPECT_FALSE(first.next_cursor.empty());

    const ManifestSweepResult second = sweepManifestCursorPage(*store, first.next_cursor, /*list_budget*/100, /*delete_budget*/0);
    EXPECT_GE(second.listed, 1u);
    EXPECT_TRUE(second.wrapped);
    EXPECT_TRUE(second.next_cursor.empty());
}

/// A NON-eligible prefix (no watermark fact) deletes NOTHING (#9: frozen-seq is not authority).
TEST(CasOrphanManifestSweep, NoWatermarkIsNotAuthority)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    // No setWatermarkMinActive — no durable fact => not eligible.
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageDeletesEligibleUnownedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAC);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_GE(result.listed, 1u);
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageRespectsDeleteBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xAD);
    const ManifestRef r2 = ref(5, 0xAE);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/1);
    EXPECT_EQ(result.deleted, 1u);
    const bool first_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r1})).exists;
    const bool second_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r2})).exists;
    EXPECT_NE(first_exists, second_exists);
}

TEST(CasOrphanManifestSweep, CursorPageSkipsOwnedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAF);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// rev.6 Task 12 (spec §anomaly-policy): a `_log` id listed strictly above a recovery seal's
/// `sealed_from` and at-or-below the seal's own `snapshot_id` provably materialized AFTER the
/// recovery `LIST` that produced the seal -- a T_mat violation. The sweep (which already LISTs the
/// `_log` region for orphan-manifest protection) must report it via one `RefLateLogDetected` event
/// and NEVER GET its body to "revive" it (the resurrect invariant): no owner state is derived from
/// it, and the sweep itself never deletes ref-log objects (GC's ordinary covered-log cleanup does,
/// once folding catches up).
TEST(CasSweepLateLog, LogBetweenSealedFromAndSealIdIsReportedNotRevived)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"00/aa@cas@"};
    const Layout layout("p");   // matches openPoolForTest's PoolConfig.pool_prefix
    registerNamespaceRaw(*backend, layout, ns);

    /// A recovery seal: snapshot_id = {2, UINT64_MAX} (the epoch-closing upper bound recovery for
    /// writer_epoch 3 publishes to close dead epoch 2), sealed_from = {2, 3} (the greatest id that
    /// recovery's LIST actually observed).
    RefTableSnapshot seal = minimalLiveSnapshot(ns.string(), RefTxnId{2, std::numeric_limits<uint64_t>::max()});
    seal.sealed_from = RefTxnId{2, 3};
    writeRefSnapshotRaw(*backend, layout, seal);

    /// A late log at {2, 7}: sealed_from (3) < 7 <= snapshot_id (UINT64_MAX) -- provably late.
    const RefTxnId late_log_id{2, 7};
    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), late_log_id, {}, std::nullopt});

    setWatermarkMinActive(*backend, layout, kServerRoot, kWriterEpoch, /*min_active*/6);   // prefix eligible

    /// Seed a fold-seal/gc-state fixture so `sealedRefCursor` (CasOrphanManifestSweep.cpp) sits AT
    /// the late log's id. This isolates the no-GET assertion below from the pre-existing, UNRELATED
    /// tail-removal-protection loop in `activeManifestKeys`, which GETs any log ABOVE the fold cursor
    /// for orphan-manifest protection regardless of lateness -- with no such fixture that loop would
    /// legitimately GET this exact log (a confound, not a detector bug). With the cursor at {2,7},
    /// `!(cursor < id)` skips it there too, so the ONLY remaining way `late_log_id`'s body could be
    /// read is a bug in the late-log detector itself.
    CasFoldSeal fold_seal;
    fold_seal.generation = 1;
    ShardCoverage coverage;
    coverage.last_folded_ref_id = late_log_id;
    fold_seal.per_ns_shard[cursorKey(ns, /*shard*/0)] = coverage;
    backend->putIfAbsent(layout.foldSealKey(/*generation*/1, /*attempt*/1), encodeFoldSeal(fold_seal));
    GcState gc_state;
    gc_state.snap_generation = 1;
    gc_state.snap_attempt = 1;
    backend->putIfAbsent(layout.gcStateKey(), encodeGcState(gc_state));

    /// A delegating backend that counts GETs on the late log's exact key -- the
    /// GetCountingBackend/INV-1 pattern from gtest_cas_part_write.cpp:602-660
    /// (CasPartWriteTxn.PutBlobCondemnedDedupNeverGetsTheDyingObject).
    struct GetCountingBackend final : public Backend
    {
        explicit GetCountingBackend(BackendPtr inner_, String watched_key_)
            : inner(std::move(inner_)), watched_key(std::move(watched_key_)) {}
        size_t get_count = 0;

        HeadResult head(const String & k) override { return inner->head(k); }
        std::optional<GetResult> get(const String & k, Range r) override
        {
            if (k == watched_key)
                ++get_count;
            return inner->get(k, r);
        }
        std::optional<GetStreamResult> getStream(const String & k, Range r) override { return inner->getStream(k, r); }
        ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        PutResult putIfAbsent(const String & k, const String & bts, const ObjectMeta & m) override { return inner->putIfAbsent(k, bts, m); }
        WriteSinkPtr putIfAbsentStream(const String & k, const ObjectMeta & m) override { return inner->putIfAbsentStream(k, m); }
        PutResult putOverwrite(const String & k, const String & bts, const Token & e, const ObjectMeta & m) override { return inner->putOverwrite(k, bts, e, m); }
        CasResult casPut(const String & k, const String & bts, const std::optional<Token> & e, const ObjectMeta & m) override { return inner->casPut(k, bts, e, m); }
        DeleteOutcome deleteExact(const String & k, const Token & tok) override { return inner->deleteExact(k, tok); }
        bool supportsListTokens() const override { return inner->supportsListTokens(); }
    private:
        BackendPtr inner;
        String watched_key;
    };

    const String watched_key = layout.refLogKey(ns, late_log_id);
    auto counting = std::make_shared<GetCountingBackend>(backend, watched_key);
    /// The sink target must outlive the Pool: `~Pool` emits terminate events into the sink.
    std::vector<CasEvent> events;
    /// Restart over pre-seeded pool content: establish `_pool_meta` first (Task 7 zero-write bootstrap).
    seedPoolMetaForRestart(*backend);
    auto store = Pool::open(counting, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    store->setEventSink([&](const CasEvent & e) { events.push_back(e); });

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});

    EXPECT_EQ(counting->get_count, 0u)
        << "the late-log detector must never GET the log body to \"revive\" it -- the resurrect "
           "invariant. (The fold-seal fixture above covers the late log at the cursor, so the "
           "pre-existing tail-removal-protection loop -- unrelated to this detector -- does not "
           "confound this assertion either.)";

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, CasEventType::RefLateLogDetected);
    EXPECT_EQ(events[0].namespace_, ns.string());
    EXPECT_EQ(events[0].at_version, 7u);
    EXPECT_TRUE(backend->head(watched_key).exists)
        << "the detector only reports the late log -- it must never delete it (that is GC's ordinary "
           "covered-log cleanup's job once folding catches up)";
}

/// fix-round F9 (author-review: `reportLateLogsIfAny` re-emits the same warning + `RefLateLogDetected`
/// event every sweep pass, with no dedup, until GC's ordinary covered-log cleanup removes the log --
/// which can be many rounds later). Two passes over the SAME durably-late log: with a `LateLogDedup`
/// latch threaded through, only the FIRST pass emits; without one (the default, `nullptr` -- every
/// pre-existing caller), both passes emit, preserving the original always-report behaviour exactly.
TEST(CasSweepLateLog, SecondPassSuppressedWithDedupLatchButNotWithoutOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"00/aa@cas@"};
    const Layout layout("p");
    registerNamespaceRaw(*backend, layout, ns);

    RefTableSnapshot seal = minimalLiveSnapshot(ns.string(), RefTxnId{2, std::numeric_limits<uint64_t>::max()});
    seal.sealed_from = RefTxnId{2, 3};
    writeRefSnapshotRaw(*backend, layout, seal);
    const RefTxnId late_log_id{2, 7};
    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), late_log_id, {}, std::nullopt});
    setWatermarkMinActive(*backend, layout, kServerRoot, kWriterEpoch, /*min_active*/6);

    /// The sink target must outlive the Pool: `~Pool` emits terminate events into the sink.
    std::vector<CasEvent> events;
    /// Restart over pre-seeded pool content: establish `_pool_meta` first (Task 7 zero-write bootstrap).
    seedPoolMetaForRestart(*backend);
    auto store = openPoolForTest(backend);
    store->setEventSink([&](const CasEvent & e) { events.push_back(e); });

    /// Two passes WITH a dedup latch threaded through: the log is still durably there (nothing folds
    /// or deletes it between passes), so without the fix this would emit twice.
    LateLogDedup dedup;
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5},
                   /*warnings*/ nullptr, &dedup);
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5},
                   /*warnings*/ nullptr, &dedup);

    size_t late_log_events = 0;
    for (const CasEvent & e : events)
        if (e.type == CasEventType::RefLateLogDetected)
            ++late_log_events;
    EXPECT_EQ(late_log_events, 1u)
        << "a second pass over the SAME still-durable late log must not re-emit with a dedup latch";

    /// Two MORE passes with NO latch (the default): the pre-existing always-report behaviour is
    /// preserved exactly -- every pre-existing caller (none of them pass a latch) is unaffected.
    events.clear();
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});

    late_log_events = 0;
    for (const CasEvent & e : events)
        if (e.type == CasEventType::RefLateLogDetected)
            ++late_log_events;
    EXPECT_EQ(late_log_events, 2u)
        << "with no latch (every pre-existing caller), both passes must still emit -- unchanged default";
}
