#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_gc_marker_test_support.h>
#include <Disks/tests/cas_test_helpers.h>

#include <base/scope_guard.h>

#include <chrono>
#include <exception>
#include <variant>
#include <future>
#include <thread>
#include <utility>

using namespace DB::Cas;
using namespace DB::Cas::tests;
using DB::Cas::tests::MetaWriteLatchBackend;
using DB::Cas::tests::awaitLatchEntered;
using DB::Cas::tests::openRequestsForTest;

namespace
{
constexpr auto kGcId = "0000000000000000000000000000002a";

static_assert(noexcept(std::declval<GcMetaWriter &>().drainOnExitNoThrow()),
    "round-exit meta-pool cleanup must not throw from the scope guard");
}

/// A real condemn-marker job may be in flight when its `Gc` is destroyed. The job holds everything it
/// touches, so the pool's join completes it correctly rather than racing member teardown -- and the
/// marker it was writing is durable afterwards.
///
/// This asserts function, not ordering: the release may land before, during or after destruction
/// begins, and all three are sound. Nothing here detects a job that wrongly captured its owner --
/// that is prevented by there being no API to write one.
TEST(CASGcMetaWriter, RealCondemnMarkerJobCompletesAcrossOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf("1");
    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    backend->arm();
    gc->metaWriterForTest().scheduleCondemnMarkerWrite(ref, /*condemn_round=*/1, /*size=*/128);

    awaitLatchEntered(*backend);

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();

    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const auto meta = loadMeta(op, store->layout(), ref);
    ASSERT_TRUE(meta) << "the condemn marker was lost across owner destruction";
    EXPECT_EQ(meta->meta.state, MetaState::Condemned);
    EXPECT_EQ(meta->meta.condemn_round, 1u);
}

/// Same lifetime property for the other production job. `deleteConfirmedMeta` returns before the latch
/// unless the marker is `Condemned` at a round not newer than the job's, so the meta must be seeded that
/// way first -- otherwise the wait above is waiting for something that will never happen.
TEST(CASGcMetaWriter, RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf("2");
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(putMetaIfAbsent(
        op, store->layout(), ref, BlobMeta{.state = MetaState::Condemned, .condemn_round = 1, .size = 64})));
    ASSERT_TRUE(loadMeta(op, store->layout(), ref));

    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    backend->arm();
    gc->metaWriterForTest().scheduleConfirmedMetaDelete(ref, /*condemn_round=*/1);

    awaitLatchEntered(*backend);

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();

    EXPECT_FALSE(loadMeta(op, store->layout(), ref))
        << "the confirmed-meta delete was lost across owner destruction";
}

/// A round that throws must not leave its meta jobs running into the next round: their effects would
/// land inside the next round's marker reads and counter deltas.
///
/// The round is made to throw at its outcome-log write, with the confirmed-meta delete it scheduled a
/// few lines earlier held inside the backend. The round must then BLOCK, draining, until that job is
/// released -- so the test asserts the round has NOT returned while the job is still held, releases,
/// and only then joins.
TEST(CASGcMetaWriter, ThrowingRoundDrainsBeforeReturning)
{
    auto backend = std::make_shared<DB::Cas::tests::OutcomeLogFaultBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    /// Fixture: one part written and dropped, then rounds driven until the NEXT round is the one that
    /// deletes -- the round that both schedules a confirmed-meta delete and writes an outcome log.
    const RootNamespace ns{"test/tbl"};
    const String ref_name = "all_0_0_0";
    const String payload = "round-drain-payload";
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref_name;
    auto build = store->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = DB::Cas::tests::idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId manifest_id = build->stageManifest({entry});
    build->precommitAdd(ns, ref_name, manifest_id);
    build->putBlob(entry.ref, BlobSource::fromString(payload));
    build->promote(ns, ref_name, build->buildId(), manifest_id);
    store->dropRef(ns, ref_name);
    store->renewWatermarkOnce();

    Gc gc(store, DB::Cas::tests::u128Of(kGcId));

    size_t rounds = 0;
    while (true)
    {
        bool delete_pending = false;
        for (const auto & entry_to_delete : gc.previewDeletes())
            delete_pending |= entry_to_delete.reason == "delete_pending";
        if (delete_pending)
            break;

        ASSERT_LT(++rounds, 16u) << "no round ever reached a pending delete -- fixture is wrong";
        ASSERT_NO_THROW(gc.runRegularRound());
        store->renewWatermarkOnce();
    }

    const uint64_t scheduled_before = gc.metaWriterForTest().scheduled();

    backend->arm();
    backend->fail_outcome_logs.store(true);

    /// Return the outcome instead of asserting on the worker thread: a gtest assertion raised off the
    /// main thread is not reliably reported, and this one distinguishes the two ways the test can go
    /// wrong, so it must be visible.
    auto round = std::async(std::launch::async, [&]
    {
        try
        {
            gc.runRegularRound();
            return false;
        }
        catch (...)
        {
            return true;
        }
    });

    awaitLatchEntered(*backend);
    EXPECT_GT(gc.metaWriterForTest().scheduled(), scheduled_before)
        << "the faulted round scheduled no meta job -- it cannot be the deleting round";

    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a meta job was still in flight -- it did not drain on its "
           "throwing exit";

    backend->release();
    EXPECT_TRUE(round.get())
        << "the round completed normally -- the outcome-log fault never fired, so the timeout above "
           "was the round blocking in its own `meta_pool_wait`, not in the drain under test";

    EXPECT_EQ(gc.metaWriterForTest().scheduled(), gc.metaWriterForTest().completed());
}

namespace
{

/// The delete job, scheduled the way `Gc::applyRedeleteOutcome` schedules it, and drained.
void runConfirmedMetaDelete(Gc & gc, const BlobRef & ref, uint64_t condemn_round)
{
    gc.metaWriterForTest().scheduleConfirmedMetaDelete(ref, condemn_round);
    gc.metaWriterForTest().drain();
}

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend)
{
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteSparesANewerRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u1");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(5)));
    const auto before = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(before.has_value());

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value()) << "a job for round 3 deleted a Condemned(5) marker";
    EXPECT_EQ(after->meta.condemn_round, 5u);
    EXPECT_EQ(after->etag, before->etag);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteSparesACleanMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u2");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, cleanMarker()));

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value()) << "a job deleted a Clean marker";
    EXPECT_EQ(after->meta.state, MetaState::Clean);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteRemovesItsOwnOrAnOlderRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef own = idOf("u3-own");
    const BlobRef older = idOf("u3-older");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), own, condemnedAt(3)));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), older, condemnedAt(2)));

    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, own, 3);
    runConfirmedMetaDelete(gc, older, 3);

    EXPECT_FALSE(markerOf(*backend, store->layout(), own).has_value());
    EXPECT_FALSE(markerOf(*backend, store->layout(), older).has_value());
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteRefusesAMarkerChangedAfterItsRead)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u6");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(3)));

    backend->setHoldDeletes(true);
    Gc gc(store, u128Of(kGcId));
    runConfirmedMetaDelete(gc, ref, 3);
    ASSERT_EQ(backend->pendingDeletes(), 1u) << "the job read Condemned(3) and sent its delete";

    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(5)));
    EXPECT_EQ(backend->landPendingDelete(0), Backend::RawRemoval::Mismatch);

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->meta.condemn_round, 5u);
}

TEST(CASGcMetaWriter, ConfirmedMetaDeleteLeavesAnUndecodableMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u7");
    const String key = store->layout().blobMetaKey(ref);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "not a marker", Retry::once())));
    }

    const uint64_t anomalies_before = metaWriteAnomalies();
    Gc gc(store, u128Of(kGcId));
    ASSERT_NO_THROW(runConfirmedMetaDelete(gc, ref, 3));

    EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 1u);
    OperationForTest op(*backend);
    const auto raw = (*op).read(key, Retry::standard());
    ASSERT_TRUE(raw.has_value()) << "the job deleted a marker it could not read";
    EXPECT_EQ(raw->bytes, "not a marker");
}

TEST(CASGcMetaWriter, NewLeaderDeletesAGraduatedEntry)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x5005);
    const BlobRef x = gateRef(hash);
    {
        Gc gc_a(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc_a, *backend, "tbl", 1, {hash}, c));
        ASSERT_EQ(runGateRound(store, gc_a).graduated, 1u);
        ASSERT_TRUE(retiredEntryFor(*backend, layout, x)->delete_pending);
    }

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    Gc gc_b(store, kGateLeaderB);
    const RoundReport report = runGateRound(store, gc_b);

    EXPECT_EQ(report.deleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    EXPECT_FALSE(markerOf(*backend, layout, x).has_value()) << "the new leader's job left an orphan marker";
}

TEST(CASGcMetaWriter, GraduationAcceptsANewerRoundMarker)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x9009);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, condemnedAt(c + 3)));

    const uint64_t carries_before = unconfirmedCarries();
    backend->resetCounts();
    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    EXPECT_EQ(unconfirmedCarries() - carries_before, 0u);
    EXPECT_EQ(backend->writeCount(layout.blobMetaKey(x)), 0u) << "an accepted gate writes nothing";

    EXPECT_EQ(runGateRound(store, gc).redeleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value()) << "the job for round " << c << " deleted a newer marker";
    EXPECT_EQ(marker->meta.condemn_round, c + 3);
}

namespace
{

enum class ReleaseLeaderA
{
    BeforeGraduation,
    AfterGraduation,
};

/// A deposed leader removes incarnation T0 and parks before its marker delete. Its successor finishes T0,
/// condemns the next incarnation T1 of the same content, and graduates it. The deposed leader's delayed job
/// must leave T1's marker, so a writer arriving after the delete round's cut republishes instead of adopting.
void staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA release)
{
    auto parking = std::make_shared<ApplyParking>();
    auto backend = std::make_shared<HeadHookBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 1, /*graduation_budget*/ 0, parkingHook(parking));
    const Layout & layout = store->layout();
    const String ns = "test/tbl";
    const String payload = "stale-meta-delete-payload";
    const BlobRef x = idOf(payload);
    parking->target = x;

    ASSERT_EQ(writeOnePart(store, ns, "p0", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p0");
    store->renewWatermarkOnce();
    Gc gc_a(store, kGateLeaderA);
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_a, [&]
    {
        const auto entry = retiredEntryFor(*backend, layout, x);
        return entry && entry->delete_pending;
    }));
    const uint64_t c0 = retiredEntryFor(*backend, layout, x)->condemn_round;

    /// A's commit `CAS` loses once B has the lease; whether its round throws or returns, only the committed
    /// round is asserted.
    parking->armed.store(true);
    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            runGateRound(store, gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    SCOPE_EXIT({
        parking->barrier.release();
        if (leader_a.joinable())
            leader_a.join();
    });
    parking->barrier.waitUntilArrived();
    ASSERT_FALSE(blobPresent(*backend, layout, x)) << "A removed T0 before parking";

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    Gc gc_b(store, kGateLeaderB);
    runGateRound(store, gc_b);
    ASSERT_FALSE(retiredEntryFor(*backend, layout, x).has_value());
    ASSERT_FALSE(markerOf(*backend, layout, x).has_value()) << "B's own job removes T0's marker";

    ASSERT_EQ(writeOnePart(store, ns, "p1", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p1");
    store->renewWatermarkOnce();
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_b, [&] { return retiredEntryFor(*backend, layout, x).has_value(); }));
    const uint64_t c1 = retiredEntryFor(*backend, layout, x)->condemn_round;
    ASSERT_GE(c1, c0 + 2);

    const auto releaseLeaderA = [&]
    {
        parking->barrier.release();
        leader_a.join();
    };
    if (release == ReleaseLeaderA::BeforeGraduation)
        releaseLeaderA();
    ASSERT_EQ(runGateRound(store, gc_b).graduated, 1u);
    if (release == ReleaseLeaderA::AfterGraduation)
        releaseLeaderA();
    ASSERT_EQ(committedGcRound(*backend, layout), c1 + 1) << "A's attempt committed after losing its lease";

    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value()) << "A's delayed job removed the marker of T1";
    EXPECT_EQ(marker->meta.state, MetaState::Condemned);
    EXPECT_EQ(marker->meta.condemn_round, c1);

    std::optional<BlobMaterializationAction> w2;
    backend->armOnHead(layout.blobKey(x), [&] { w2 = writeOnePart(store, ns, "p2", payload); });
    const RoundReport report = runGateRound(store, gc_b);
    ASSERT_TRUE(w2.has_value()) << "the redelete never observed the blob";
    EXPECT_EQ(*w2, BlobMaterializationAction::Published) << "W2 adopted the incarnation B deletes";
    EXPECT_EQ(report.replaced, 1u);
    ASSERT_TRUE(blobPresent(*backend, layout, x));
    OperationForTest op(*backend);
    const auto body = (*op).read(layout.blobKey(x), Retry::standard());
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE(body->bytes.ends_with(payload)) << "W2's part names a blob that no longer holds its content";
}

}

TEST(CASGcMetaWriter, StaleMetaDeleteSparesSuccessorMarkerBeforeGraduation)
{
    staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA::BeforeGraduation);
}

TEST(CASGcMetaWriter, StaleMetaDeleteSparesSuccessorMarkerAfterGraduation)
{
    staleMetaDeleteSparesSuccessorMarker(ReleaseLeaderA::AfterGraduation);
}

TEST(CASGcMetaWriter, GraduationReadsStayWithinTheBudget)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 1);
        const Layout & layout = store->layout();
        const std::vector<UInt128> hashes{DB::UInt128(0x2001), DB::UInt128(0x2002), DB::UInt128(0x2003)};
        {
            Gc gc(store, kGateLeaderA);
            uint64_t c = 0;
            ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, hashes, c));
        }

        /// A fresh `Gc`: nothing in process can stand in for a read.
        Gc gc(store, kGateLeaderA);
        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u);

        uint64_t gets = 0;
        for (const UInt128 & hash : hashes)
            gets += backend->getCount(layout.blobMetaKey(gateRef(hash)));
        EXPECT_EQ(gets, 1u) << "the gate read entries the budget did not admit";
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
    }
}

TEST(CASGcMetaWriter, GraduationRereadsTheMarkerOwnMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x1001);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, cleanMarker()));

    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "graduated over a Clean marker";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.state, MetaState::Condemned);
    EXPECT_EQ(marker->meta.condemn_round, c + 1) << "the retry stamps the round that refused";

    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
}

TEST(CASGcMetaWriter, GraduationRereadsTheMarkerLostAttemptMarker)
{
    auto parking = std::make_shared<ApplyParking>();
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 1, /*graduation_budget*/ 0, parkingHook(parking));
    const Layout & layout = store->layout();
    const UInt128 x_hash = DB::UInt128(0x1101);
    const UInt128 y_hash = DB::UInt128(0x1102);
    const BlobRef x = gateRef(x_hash);
    parking->target = gateRef(y_hash);

    /// X is owned throughout; Y is condemned and graduated, so A's next round both redeletes Y (and parks on it)
    /// and condemns X.
    Gc gc_a(store, kGateLeaderA);
    const ManifestRef x_manifest = publishBlobs(*backend, layout, "x", 1, {x_hash});
    uint64_t y_round = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc_a, *backend, "y", 2, {y_hash}, y_round));
    ASSERT_EQ(runGateRound(store, gc_a).graduated, 1u);
    dropBlobs(*backend, layout, "x", x_manifest);
    store->renewWatermarkOnce();
    const uint64_t c = committedGcRound(*backend, layout) + 1;

    parking->armed.store(true);
    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            runGateRound(store, gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    SCOPE_EXIT({
        parking->barrier.release();
        if (leader_a.joinable())
            leader_a.join();
    });
    parking->barrier.waitUntilArrived();
    ASSERT_NO_FATAL_FAILURE(awaitCondition([&]
    {
        const auto marker = markerOf(*backend, layout, x);
        return marker && marker->meta.state == MetaState::Condemned;
    }));

    /// B commits round c on its own condemnation of X, and the marker A's losing attempt wrote goes.
    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderB));
    {
        Gc gc_b(store, kGateLeaderB);
        runGateRound(store, gc_b);
    }
    ASSERT_EQ(committedGcRound(*backend, layout), c);
    ASSERT_EQ(retiredEntryFor(*backend, layout, x)->condemn_round, c);
    ASSERT_NO_FATAL_FAILURE(removeMarker(*backend, layout, x));

    parking->barrier.release();
    leader_a.join();
    ASSERT_EQ(committedGcRound(*backend, layout), c) << "A's losing attempt committed";

    ASSERT_NO_FATAL_FAILURE(handGcLeaseTo(*backend, layout, kGateLeaderA));
    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc_a).graduated, 0u) << "A graduated on its own attempt's marker, which is gone";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.condemn_round, c + 1);
    EXPECT_EQ(runGateRound(store, gc_a).graduated, 1u);
}

TEST(CASGcMetaWriter, GraduationAndDeleteRequestCounts)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x4004);
        const String meta_key = layout.blobMetaKey(gateRef(hash));
        Gc gc(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->resetCounts();
        ASSERT_EQ(runGateRound(store, gc).graduated, 1u);
        EXPECT_EQ(backend->getCount(meta_key), 1u) << "graduation reads the marker";
        EXPECT_EQ(backend->writeCount(meta_key), 0u);
        EXPECT_EQ(backend->deleteCount(meta_key), 0u);

        backend->resetCounts();
        ASSERT_EQ(runGateRound(store, gc).redeleted, 1u);
        EXPECT_EQ(backend->getCount(meta_key), 1u) << "a pending entry passes no gate; only its job reads";
        EXPECT_EQ(backend->deleteCount(meta_key), 1u);
        EXPECT_FALSE(markerOf(*backend, layout, gateRef(hash)).has_value());
    }
}

TEST(CASGcMetaWriter, LatePublisherCleanMissesTheRetriedMarker)
{
    auto backend = std::make_shared<ContentEtagBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const String ns = "test/tbl";
    const String payload = "late-publisher-clean";
    const BlobRef x = idOf(payload);

    ASSERT_EQ(writeOnePart(store, ns, "p1", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p1");
    store->renewWatermarkOnce();
    Gc gc(store, kGateLeaderA);
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&] { return retiredEntryFor(*backend, layout, x).has_value(); }));
    const uint64_t c = retiredEntryFor(*backend, layout, x)->condemn_round;
    const auto condemned = markerOf(*backend, layout, x);
    ASSERT_TRUE(condemned && condemned->meta.state == MetaState::Condemned);
    const Etag m = condemned->etag;
    const BlobMeta clean{.state = MetaState::Clean, .condemn_round = 0, .size = condemned->meta.size};

    /// The publisher's reissued `Clean If-Match M` lands; its first attempt is still in flight.
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(casMeta(*op, layout, x, m, clean)));
    }

    const uint64_t carries_before = unconfirmedCarries();
    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "graduated over the publisher's Clean";
    EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
    const auto retried = markerOf(*backend, layout, x);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->meta.condemn_round, c + 1);
    EXPECT_NE(retried->etag, m) << "the retry recreated the bytes the late request names";
    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);

    /// The first attempt lands now: its precondition names bytes no marker carries.
    {
        OperationForTest op(*backend);
        EXPECT_FALSE(std::holds_alternative<Committed>(casMeta(*op, layout, x, m, clean)));
    }

    std::optional<BlobMaterializationAction> w2;
    backend->armOnHead(layout.blobKey(x), [&] { w2 = writeOnePart(store, ns, "p2", payload); });
    const RoundReport report = runGateRound(store, gc);
    ASSERT_TRUE(w2.has_value()) << "the redelete never observed the blob";
    EXPECT_EQ(*w2, BlobMaterializationAction::Published) << "W2 adopted the incarnation GC deletes";
    EXPECT_EQ(report.replaced, 1u);
    EXPECT_TRUE(blobPresent(*backend, layout, x));
}

TEST(CASGcMetaWriter, GraduationCarriesAnUndecodableMarker)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x1010);
        const BlobRef x = gateRef(hash);
        Gc gc(store, kGateLeaderA);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
        {
            OperationForTest op(*backend);
            const String key = layout.blobMetaKey(x);
            const auto current = (*op).read(key, Retry::standard());
            ASSERT_TRUE(current.has_value());
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, "not a marker", current->etag, Retry::once())));
        }

        const uint64_t anomalies_before = metaWriteAnomalies();
        const uint64_t carries_before = unconfirmedCarries();
        RoundReport report;
        ASSERT_NO_THROW(report = runGateRound(store, gc));
        EXPECT_EQ(report.graduated, 0u) << "graduated on a marker nobody could read";
        EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 2u) << "the gate's read and the retry's read";
        EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
        EXPECT_EQ(committedGcRound(*backend, layout), c + 1) << "the round committed";
        EXPECT_FALSE(retiredEntryFor(*backend, layout, x)->delete_pending);
        EXPECT_TRUE(blobPresent(*backend, layout, x));
    }
}

TEST(CASGcMetaWriter, CondemnMarkerWriteRestampsAnOlderRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef ref = idOf("u4");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), ref, condemnedAt(3, 64)));

    Gc gc(store, u128Of(kGcId));
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(ref, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().drain();

    const auto after = markerOf(*backend, store->layout(), ref);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->meta.state, MetaState::Condemned);
    EXPECT_EQ(after->meta.condemn_round, 5u) << "an older-round marker may be another incarnation's";
}

TEST(CASGcMetaWriter, CondemnMarkerWriteLeavesAnEqualOrNewerRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    const BlobRef equal = idOf("u5-equal");
    const BlobRef newer = idOf("u5-newer");
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), equal, condemnedAt(5, 64)));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, store->layout(), newer, condemnedAt(7, 64)));
    const auto equal_before = markerOf(*backend, store->layout(), equal);
    const auto newer_before = markerOf(*backend, store->layout(), newer);

    Gc gc(store, u128Of(kGcId));
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(equal, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().scheduleCondemnMarkerWrite(newer, /*condemn_round=*/5, /*size=*/64);
    gc.metaWriterForTest().drain();

    EXPECT_EQ(markerOf(*backend, store->layout(), equal)->etag, equal_before->etag);
    EXPECT_EQ(markerOf(*backend, store->layout(), newer)->etag, newer_before->etag);
    EXPECT_EQ(markerOf(*backend, store->layout(), newer)->meta.condemn_round, 7u);
}

TEST(CASGcMetaWriter, GraduationRefusesAnOlderRoundMarker)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x2202);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_GE(c, 2u);
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, condemnedAt(c - 1)));

    EXPECT_EQ(runGateRound(store, gc).graduated, 0u) << "accepted an older-round marker";
    const auto restamped = markerOf(*backend, layout, x);
    ASSERT_TRUE(restamped.has_value());
    EXPECT_EQ(restamped->meta.condemn_round, c + 1) << "the retry left the older round in place";

    EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    EXPECT_EQ(runGateRound(store, gc).redeleted, 1u);
    EXPECT_FALSE(blobPresent(*backend, layout, x));
    const auto orphan = markerOf(*backend, layout, x);
    ASSERT_TRUE(orphan.has_value()) << "the job for round " << c << " removed a newer marker";
    EXPECT_EQ(orphan->meta.condemn_round, c + 1);
}

TEST(CASGcMetaWriter, SparedThenRecondemnedBlobGraduates)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x3303);
    const BlobRef x = gateRef(hash);
    Gc gc(store, kGateLeaderA);
    uint64_t r1 = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, r1));

    /// A new owner spares the entry; GC never clears a marker, so `Condemned(r1)` stays.
    const ManifestRef again = publishBlobs(*backend, layout, "tbl2", 2, {hash});
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    ASSERT_FALSE(retiredEntryFor(*backend, layout, x).has_value());
    ASSERT_EQ(markerOf(*backend, layout, x)->meta.condemn_round, r1);

    dropBlobs(*backend, layout, "tbl2", again);
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    const auto recondemned = retiredEntryFor(*backend, layout, x);
    ASSERT_TRUE(recondemned.has_value());
    ASSERT_GT(recondemned->condemn_round, r1);

    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&]
    {
        const auto entry = retiredEntryFor(*backend, layout, x);
        return entry && entry->delete_pending;
    }, /*max_rounds*/ 2)) << "a spared, re-condemned blob never graduates";
}
