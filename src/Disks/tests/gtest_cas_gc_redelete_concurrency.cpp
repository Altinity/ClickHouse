#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <Common/ProfileEvents.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

namespace ProfileEvents
{
extern const Event CASGCRetiredRedeleteFailed;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");
constexpr uint64_t kBlobs = 6;
constexpr uint64_t kFaultedBlob = 3;

class WorkerRemoveFaultBackend : public InMemoryBackend
{
public:
    void armAgainstOtherThreads(String key)
    {
        faulted_key = std::move(key);
        owner = std::this_thread::get_id();
        armed.store(true);
    }

    bool fired() const { return !armed.load(); }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (armed.load() && key == faulted_key && std::this_thread::get_id() != owner && armed.exchange(false))
            throw std::runtime_error("injected worker remove fault");
        return InMemoryBackend::remove(key, expected_value, access);
    }

private:
    String faulted_key;
    std::thread::id owner;
    std::atomic<bool> armed{false};
};

class BlobRemoveWitnessBackend : public InMemoryBackend
{
public:
    void arm(std::set<String> blob_keys_, size_t k_overlap_)
    {
        blob_keys = std::move(blob_keys_);
        k_overlap = k_overlap_;
        armed.store(true);
    }

    bool sawOverlap() const { return saw_overlap.load(); }

    size_t peakInFlight() const
    {
        std::lock_guard lock(mutex);
        return peak;
    }

    size_t blobRemoves() const
    {
        std::lock_guard lock(mutex);
        return total;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!armed.load() || !blob_keys.contains(key))
            return InMemoryBackend::remove(key, expected_value, access);
        {
            std::unique_lock lock(mutex);
            ++total;
            ++in_flight;
            peak = std::max(peak, in_flight);
            if (k_overlap > 1)
            {
                if (in_flight >= k_overlap)
                {
                    saw_overlap.store(true);
                    gate.notify_all();
                }
                else
                {
                    gate.wait_for(lock, std::chrono::milliseconds(250),
                                  [&] { return in_flight >= k_overlap || saw_overlap.load(); });
                }
            }
        }
        const RawRemoval result = InMemoryBackend::remove(key, expected_value, access);
        {
            std::lock_guard lock(mutex);
            --in_flight;
        }
        return result;
    }

private:
    std::set<String> blob_keys;
    size_t k_overlap = 1;
    std::atomic<bool> armed{false};
    mutable std::mutex mutex;
    std::condition_variable gate;
    size_t in_flight = 0;
    size_t peak = 0;
    size_t total = 0;
    std::atomic<bool> saw_overlap{false};
};

template <typename BackendT>
PoolPtr openPoolWithIoConcurrency(std::shared_ptr<BackendT> backend, uint64_t concurrency)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = concurrency;
    return Pool::open(std::move(backend), std::move(config));
}

String blobKeyOf(const Pool & store, uint64_t blob)
{
    return store.layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(blob))});
}

std::set<String> allBlobKeys(const Pool & store)
{
    std::set<String> keys;
    for (uint64_t b = 1; b <= kBlobs; ++b)
        keys.insert(blobKeyOf(store, b));
    return keys;
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

using OutcomeRow = std::pair<BlobRef, OutcomeKind>;

struct RedeleteRun
{
    std::vector<std::vector<uint64_t>> reports;
    std::map<String, std::vector<OutcomeRow>> outcome_logs;
};

void collectOutcomeLogs(Backend & backend, std::map<String, std::vector<OutcomeRow>> & out)
{
    OperationForTest op(backend);
    String cursor;
    while (true)
    {
        const ListPage page = (*op).list("", cursor, 1000, Retry::standard());
        for (const ListedKey & listed : page.keys)
        {
            if (listed.key.find("/outcomes/") == String::npos || out.contains(listed.key))
                continue;
            const auto object = (*op).read(listed.key, Retry::standard());
            if (!object)
                continue;
            std::vector<OutcomeRow> rows;
            for (const OutcomeEntry & entry : decodeOutcomeLog(openObject(FormatId::GcOutcomes, object->bytes)).entries)
                rows.emplace_back(entry.ref, entry.outcome);
            out.emplace(listed.key, std::move(rows));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

void runRedeleteScenario(uint64_t concurrency, RedeleteRun & out)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, concurrency);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(publishThenDrop(*backend, store, gc));

    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        out.reports.push_back({rep.redeleted, rep.deleted, rep.absent, rep.replaced, rep.spared});
        collectOutcomeLogs(*backend, out.outcome_logs);
    }
    ASSERT_TRUE(allBlobsAbsent(*backend, *store));
}

}

TEST(CASGCRedeleteConcurrency, ParallelRedeleteReclaimsEveryBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
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

TEST(CASGCRedeleteConcurrency, BlobDeletesActuallyOverlap)
{
    auto backend = std::make_shared<BlobRemoveWitnessBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    backend->arm(allBlobKeys(*store), /*k_overlap*/ 2);
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(backend->blobRemoves(), kBlobs);
    EXPECT_TRUE(backend->sawOverlap())
        << "no two blob deletes were ever in the backend at the same time; peak in flight was "
        << backend->peakInFlight();
    EXPECT_GT(backend->peakInFlight(), 1u);
}

TEST(CASGCRedeleteConcurrency, ConcurrencyOneDeletesSequentially)
{
    auto backend = std::make_shared<BlobRemoveWitnessBackend>();
    auto store = openPoolWithIoConcurrency(backend, 1);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    backend->arm(allBlobKeys(*store), /*k_overlap*/ 1);
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(backend->blobRemoves(), kBlobs);
    EXPECT_EQ(backend->peakInFlight(), 1u);
}

TEST(CASGCRedeleteConcurrency, WorkerRemoveFaultKeepsSiblingOutcomesAndPoolAlive)
{
    auto backend = std::make_shared<WorkerRemoveFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    backend->armAgainstOtherThreads(blobKeyOf(*store, kFaultedBlob));
    const auto failed_before = ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load();
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

    EXPECT_TRUE(backend->fired()) << "the faulted blob delete never ran on a pool worker";
    EXPECT_EQ(failed_rounds, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load() - failed_before, 1u);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, SameOutcomesAtConcurrencyOneAndFour)
{
    RedeleteRun one;
    RedeleteRun four;
    ASSERT_NO_FATAL_FAILURE(runRedeleteScenario(1, one));
    ASSERT_NO_FATAL_FAILURE(runRedeleteScenario(4, four));

    EXPECT_EQ(one.reports, four.reports);

    std::vector<std::vector<OutcomeRow>> logs_one;
    for (const auto & [key, rows] : one.outcome_logs)
        logs_one.push_back(rows);
    std::vector<std::vector<OutcomeRow>> logs_four;
    for (const auto & [key, rows] : four.outcome_logs)
        logs_four.push_back(rows);
    EXPECT_EQ(logs_one, logs_four);

    size_t deleted_rows = 0;
    for (const auto & rows : logs_one)
        deleted_rows += static_cast<size_t>(
            std::count_if(rows.begin(), rows.end(), [](const OutcomeRow & row) { return row.second == OutcomeKind::Deleted; }));
    EXPECT_EQ(deleted_rows, kBlobs) << "the scenario must delete every blob through the outcome log, or ordering is untested";
}
