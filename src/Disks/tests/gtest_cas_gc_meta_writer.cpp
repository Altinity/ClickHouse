#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <thread>

using namespace DB::Cas;
using DB::Cas::tests::MetaWriteLatchBackend;
using DB::Cas::tests::awaitLatchEntered;

namespace
{
constexpr auto kGcId = "0000000000000000000000000000002a";
}

/// A real condemn-marker job may be in flight when its `Gc` is destroyed. The job holds everything it
/// touches, so the pool's join completes it correctly rather than racing member teardown -- and the
/// marker it was writing is durable afterwards.
///
/// This asserts function, not ordering: the release may land before, during or after destruction
/// begins, and all three are sound. Nothing here detects a job that wrongly captured its owner --
/// that is prevented by there being no API to write one.
TEST(CasGcMetaWriter, RealCondemnMarkerJobCompletesAcrossOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf("1");
    const Token token{"tok-1"};

    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    backend->arm();
    gc->metaWriterForTest().scheduleCondemnMarkerWrite(ref, token, /*condemn_round=*/1, /*size=*/128);

    awaitLatchEntered(*backend);

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();

    const auto meta = loadMeta(*backend, store->layout(), ref);
    ASSERT_TRUE(meta) << "the condemn marker was lost across owner destruction";
    EXPECT_EQ(meta->meta.state, MetaState::Condemned);
    EXPECT_EQ(meta->meta.condemn_round, 1u);
}

/// The confirmation registry is written by the pool thread and read by the graduation gate. Assert it
/// on a `Gc` that is still alive, so the read is possible at all: after destruction there is no
/// registry left to consult, which is the documented behaviour a fresh leader relies on.
TEST(CasGcMetaWriter, CondemnMarkerConfirmationIsVisibleAfterDrain)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf("1");
    const Token token{"tok-1"};

    Gc gc(store, DB::Cas::tests::u128Of(kGcId));
    EXPECT_FALSE(gc.metaWriterForTest().condemnMarkerConfirmedInProcess(ref, token));

    gc.metaWriterForTest().scheduleCondemnMarkerWrite(ref, token, /*condemn_round=*/1, /*size=*/128);
    gc.metaWriterForTest().drain();

    EXPECT_TRUE(gc.metaWriterForTest().condemnMarkerConfirmedInProcess(ref, token));
    EXPECT_EQ(gc.metaWriterForTest().scheduled(), gc.metaWriterForTest().completed());
}

/// Same lifetime property for the other production job. `deleteConfirmedMeta` RETURNS IMMEDIATELY when
/// no meta object exists (`Gc/CasGcMetaWriter.cpp`), so the meta must be seeded first -- otherwise the
/// job never reaches the latch and the wait above is waiting for something that will never happen.
TEST(CasGcMetaWriter, RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf("2");
    ASSERT_EQ(
        putMetaIfAbsent(*store, ref, BlobMeta{.state = MetaState::Condemned, .condemn_round = 1, .size = 64}).outcome,
        CasOverwriteOutcome::Committed);
    ASSERT_TRUE(loadMeta(*backend, store->layout(), ref));

    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    backend->arm();
    gc->metaWriterForTest().scheduleConfirmedMetaDelete(ref);

    awaitLatchEntered(*backend);

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();

    EXPECT_FALSE(loadMeta(*backend, store->layout(), ref))
        << "the confirmed-meta delete was lost across owner destruction";
}
