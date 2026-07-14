#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
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
}

/// A staged-but-unowned body in an ELIGIBLE prefix, absent from the owner view, is deleted (#7).
TEST(CasOrphanManifestSweep, EligibleAndUnownedIsDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);   // 6 > 5 => eligible

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// A body that IS in the owner view (committed) is NEVER swept (#8).
TEST(CasOrphanManifestSweep, OwnedBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
TEST(CasOrphanManifestSweep, PendingCommittedRemovalBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    // No GC state / no generation seal exists before the sweep; the sweep must not create one.
    const uint64_t gen_before = currentGenerationOf(*backend, store->layout());

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_EQ(currentGenerationOf(*backend, store->layout()), gen_before);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

TEST(CasOrphanManifestSweep, CursorPageAdvancesAndWrapsWithListBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAC);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_GE(result.listed, 1u);
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageRespectsDeleteBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xAD);
    const ManifestRef r2 = ref(5, 0xAE);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/1);
    EXPECT_EQ(result.deleted, 1u);
    const bool first_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r1})).exists;
    const bool second_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r2})).exists;
    EXPECT_NE(first_exists, second_exists);
}

TEST(CasOrphanManifestSweep, CursorPageSkipsOwnedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);

    /// A recovery seal: snapshot_id = {2, UINT64_MAX} (the epoch-closing upper bound recovery for
    /// writer_epoch 3 publishes to close dead epoch 2), sealed_from = {2, 3} (the greatest id that
    /// recovery's LIST actually observed).
    RefTableSnapshot seal = minimalLiveSnapshot(ns.string(), RefTxnId{2, std::numeric_limits<uint64_t>::max()});
    seal.sealed_from = RefTxnId{2, 3};
    writeRefSnapshotRaw(*backend, store->layout(), seal);

    /// A late log at {2, 7}: sealed_from (3) < 7 <= snapshot_id (UINT64_MAX) -- provably late.
    writeRefLogTxnRaw(*backend, store->layout(), RefLogTxn{ns.string(), RefTxnId{2, 7}, {}});

    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);   // prefix eligible

    std::vector<CasEvent> events;
    store->setEventSink([&](const CasEvent & e) { events.push_back(e); });

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, CasEventType::RefLateLogDetected);
    EXPECT_EQ(events[0].namespace_, ns.string());
    EXPECT_EQ(events[0].at_version, 7u);
    EXPECT_TRUE(backend->head(store->layout().refLogKey(ns, RefTxnId{2, 7})).exists)
        << "the detector only reports the late log -- it must never delete it (that is GC's ordinary "
           "covered-log cleanup's job once folding catches up)";
}
