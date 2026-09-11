#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");
constexpr uint64_t kBlobs = 6;
constexpr uint64_t kFaultedBlob = 3;

class RemoveFaultBackend : public InMemoryBackend
{
public:
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (key == faulted_key && armed.exchange(false))
            throw std::runtime_error("injected remove fault");
        return InMemoryBackend::remove(key, expected_value, access);
    }

    String faulted_key;
    std::atomic<bool> armed{false};
};

template <typename BackendT>
PoolPtr openPoolWithRedeleteConcurrency(std::shared_ptr<BackendT> backend, uint64_t concurrency)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_redelete_concurrency = concurrency;
    return Pool::open(std::move(backend), std::move(config));
}

String blobKeyOf(const Pool & store, uint64_t blob)
{
    return store.layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(blob))});
}

bool allBlobsAbsent(Backend & backend, const Pool & store)
{
    for (uint64_t b = 1; b <= kBlobs; ++b)
        if (!blobAbsent(backend, store.layout(), DB::UInt128(b)))
            return false;
    return true;
}

void publishThenDrop(Backend & backend, const PoolPtr & store, Gc & gc)
{
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    std::vector<ManifestEntry> entries;
    for (uint64_t b = 1; b <= kBlobs; ++b)
    {
        writeBlobBody(backend, store->layout(), DB::UInt128(b));
        entries.push_back(blobEntryFor("f" + std::to_string(b), DB::UInt128(b)));
    }
    writeManifestRaw(backend, store->layout(), ns, r, entries);
    publishCommittedTransition(backend, store->layout(), ns, "tbl", std::nullopt, r);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_FALSE(blobAbsent(backend, store->layout(), DB::UInt128(1)));

    dropRefTransition(backend, store->layout(), ns, "tbl", r);
}

}

TEST(CASGCRedeleteConcurrency, ParallelRedeleteReclaimsEveryBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithRedeleteConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    size_t redeleted = 0;
    size_t deleted = 0;
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        redeleted += rep.redeleted;
        deleted += rep.deleted;
    }

    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(redeleted, kBlobs);
    EXPECT_EQ(deleted, kBlobs);
}

TEST(CASGCRedeleteConcurrency, FailedRemoveKeepsSiblingOutcomesAndPoolAlive)
{
    auto backend = std::make_shared<RemoveFaultBackend>();
    auto store = openPoolWithRedeleteConcurrency(backend, 4);
    backend->faulted_key = blobKeyOf(*store, kFaultedBlob);
    backend->armed = true;

    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    size_t failed_rounds = 0;
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        RoundReport progress;
        try
        {
            gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
        }
        catch (const std::exception &)
        {
            ++failed_rounds;
            EXPECT_EQ(progress.redeleted, kBlobs - 1);
            for (uint64_t b = 1; b <= kBlobs; ++b)
                EXPECT_EQ(blobAbsent(*backend, store->layout(), DB::UInt128(b)), b != kFaultedBlob) << "blob " << b;
        }
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(failed_rounds, 1u);
    EXPECT_FALSE(backend->armed.load());
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}
