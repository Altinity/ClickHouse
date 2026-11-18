#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>
#include <base/scope_guard.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

namespace ProfileEvents
{
extern const Event CASGCRetiredRedeleteFailed;
}

namespace DB::ErrorCodes
{
extern const int CANNOT_SCHEDULE_TASK;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");
constexpr uint64_t kBlobs = 6;
constexpr uint64_t kFaultedBlob = 3;
constexpr uint64_t kReplacedBlob = 5;
constexpr uint64_t kAbsentBlob = 4;
constexpr uint64_t kShards = 4;

DB::UInt128 blobHash(uint64_t blob)
{
    const DB::UInt128 value(blob);
    return (value << 64) | value;
}

class WorkerFaultBackend : public InMemoryBackend
{
public:
    void armRemoveFaults(std::set<String> keys, bool workers_only = true)
    {
        arm(remove_keys, std::move(keys), workers_only);
    }

    void armHeadFaults(std::set<String> keys, bool workers_only = true)
    {
        arm(head_keys, std::move(keys), workers_only);
    }

    bool allFired() const
    {
        std::lock_guard lock(mutex);
        return remove_keys.empty() && head_keys.empty();
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (takeFault(remove_keys, key))
            throw std::runtime_error("injected worker remove fault for key " + key);
        return InMemoryBackend::remove(key, expected_value, access);
    }

    std::vector<RawRemoval> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        for (const RawConditionalRemove & removal : removals)
            if (takeFault(remove_keys, removal.key))
                throw std::runtime_error("injected worker remove fault for key " + removal.key);
        return InMemoryBackend::removeManyByConditional(removals, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (takeFault(head_keys, key))
            throw std::runtime_error("injected worker head fault for key " + key);
        return InMemoryBackend::head(key, access);
    }

private:
    void arm(std::set<String> & target, std::set<String> keys, bool workers_only)
    {
        std::lock_guard lock(mutex);
        owner = std::this_thread::get_id();
        only_workers = workers_only;
        target = std::move(keys);
    }

    bool takeFault(std::set<String> & keys, const String & key)
    {
        std::lock_guard lock(mutex);
        return (!only_workers || std::this_thread::get_id() != owner) && keys.erase(key) > 0;
    }

    mutable std::mutex mutex;
    std::thread::id owner;
    bool only_workers = true;
    std::set<String> remove_keys;
    std::set<String> head_keys;
};

class RedeleteDiagnosticsBackend : public InMemoryBackend
{
public:
    std::set<String> blob_keys;
    std::function<void(const String &)> read_blob_hook;
    std::function<void(const std::vector<RawConditionalRemove> &, TransportAccess &)> delete_chunk_hook;

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (!read_blob_hook || !blob_keys.contains(key))
            return InMemoryBackend::head(key, access);

        SCOPE_EXIT({ LOG_WARNING(logger, "Test blob read finished"); });
        read_blob_hook(key);
        return InMemoryBackend::head(key, access);
    }

    std::vector<RawRemoval> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        if (!delete_chunk_hook)
            return InMemoryBackend::removeManyByConditional(removals, access);

        LOG_WARNING(logger, "Test delete chunk started");
        SCOPE_EXIT({ LOG_WARNING(logger, "Test delete chunk finished"); });
        delete_chunk_hook(removals, access);
        return InMemoryBackend::removeManyByConditional(removals, access);
    }

private:
    LoggerPtr logger = getLogger("CasGc");
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
                    gate.wait_for(lock, std::chrono::milliseconds(250), [&] { return in_flight >= k_overlap || saw_overlap.load(); });
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

    std::vector<RawRemoval> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        std::vector<RawRemoval> results;
        results.reserve(removals.size());
        for (const RawConditionalRemove & removal : removals)
            results.push_back(remove(removal.key, removal.expected_token, access));
        return results;
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
PoolPtr openPoolWithIoConcurrency(
    std::shared_ptr<BackendT> backend,
    uint64_t concurrency,
    uint64_t gc_shards = 1,
    uint64_t outcome_entry_budget = 0,
    CasEventSink event_sink = {},
    std::optional<size_t> refuse_at_for_test = std::nullopt,
    uint64_t gc_bulk_delete_chunk_keys = kBulkDeleteMaxKeys)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = concurrency;
    config.gc_shards = gc_shards;
    config.gc_round_outcome_entry_budget = outcome_entry_budget;
    config.event_sink = std::move(event_sink);
    config.gc_io_pool_refuse_at_for_test = refuse_at_for_test;
    config.gc_bulk_delete_chunk_keys = gc_bulk_delete_chunk_keys;
    config.skip_access_check = true;
    return Pool::open(std::move(backend), std::move(config));
}

String blobKeyOf(const Pool & store, uint64_t blob)
{
    return store.layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(blobHash(blob))});
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
        if (!blobAbsent(backend, store.layout(), blobHash(b)))
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
        writeBlobBody(backend, store->layout(), blobHash(b));
        entries.push_back(blobEntryFor("f" + std::to_string(b), blobHash(b)));
    }
    writeManifestRaw(backend, store->layout(), ns, r, entries);
    publishCommittedTransition(backend, store->layout(), ns, "tbl", std::nullopt, r);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_FALSE(blobAbsent(backend, store->layout(), blobHash(1)));

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

void runRedeleteScenario(uint64_t concurrency, RedeleteRun & out, uint64_t outcome_entry_budget = 0)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, concurrency, 1, outcome_entry_budget);
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

struct PendingDeletesRows
{
    std::vector<std::map<String, UInt64>> rows;

    UInt64 total(const String & key) const
    {
        UInt64 sum = 0;
        for (const auto & row : rows)
            if (const auto it = row.find(key); it != row.end())
                sum += it->second;
        return sum;
    }

    bool lastRowHas(const String & key) const { return !rows.empty() && rows.back().contains(key); }
};

GcPhaseSink recordPendingDeletes(std::shared_ptr<PendingDeletesRows> rows, GcPhaseSink next_sink = {})
{
    return [captured_rows = std::move(rows), captured_next_sink = std::move(next_sink)](const GcPhaseRecord & rec)
    {
        if (rec.phase == "pending_deletes")
            captured_rows->rows.push_back(rec.metrics);
        if (captured_next_sink)
            captured_next_sink(rec);
    };
}

std::vector<Gc::PreviewEntry> previewPendingDeletes(Gc & gc)
{
    std::vector<Gc::PreviewEntry> result;
    for (Gc::PreviewEntry entry : gc.previewDeletes())
        if (entry.reason == "delete_pending")
            result.push_back(std::move(entry));
    return result;
}

}

namespace
{

void driveUntilAllGraduated(Backend & backend, Gc & gc, const PoolPtr & store, uint64_t marked_blob);
void expectBodiesPresentOnlyFor(Backend & backend, const Pool & store, const std::set<uint64_t> & present);
uint64_t failedCounter();

}

TEST(CASGCRedeleteConcurrency, ParallelRedeleteReclaimsEveryBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));

    size_t redeleted = 0;
    size_t deleted = 0;
    size_t redelete_failed = 0;
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        redeleted += rep.redeleted;
        deleted += rep.deleted;
        redelete_failed += rep.redelete_failed;
    }

    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(redeleted, kBlobs);
    EXPECT_EQ(deleted, kBlobs);
    EXPECT_EQ(redelete_failed, 0u);
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
}

TEST(CASGCRedeleteConcurrency, ConditionalDeleteBatchesAreRecorded)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, {}, std::nullopt, 2);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kReplacedBlob));

    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_EQ(rep.deleted, kBlobs);
    ASSERT_EQ(rows->rows.size(), 1u);
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), 3u);
    EXPECT_EQ(rows->total("delete_chunks"), 3u);
    ASSERT_TRUE(rows->lastRowHas("read_blob_jobs_failed"));
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_failed"));
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
    ASSERT_TRUE(rows->lastRowHas("read_blob_job_schedule_failures"));
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_job_schedule_failures"));
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
}

TEST(CASGCRedeleteConcurrency, DeleteWaveEnqueueRefusalWaitsForSubmittedChunks)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, {}, kBlobs + 1, 2);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kReplacedBlob));

    RoundReport progress;
    EXPECT_THROW(
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress),
        DB::Exception);
    store->renewWatermarkOnce();

    EXPECT_EQ(progress.redeleted, 2u);
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_EQ(runRegularRoundReclaiming(gc).redeleted, kBlobs);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
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
    EXPECT_TRUE(backend->sawOverlap()) << "no two blob deletes were ever in the backend at the same time; peak in flight was "
                                       << backend->peakInFlight();
    EXPECT_GT(backend->peakInFlight(), 1u);
}

TEST(CASGCRedeleteConcurrency, ConcurrencyOneDeletesSequentially)
{
    auto backend = std::make_shared<BlobRemoveWitnessBackend>();
    auto store = openPoolWithIoConcurrency(backend, 1);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));

    backend->arm(allBlobKeys(*store), /*k_overlap*/ 1);
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(backend->blobRemoves(), kBlobs);
    EXPECT_EQ(backend->peakInFlight(), 1u);
    ASSERT_TRUE(rows->lastRowHas("read_blob_jobs_scheduled"));
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_scheduled"));
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), 0u) << "concurrency 1 must not use the pool";
    EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), 0u);
    EXPECT_EQ(rows->total("delete_chunks"), kBlobs);
    ASSERT_TRUE(rows->lastRowHas("read_blob_jobs_failed"));
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_failed"));
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
    ASSERT_TRUE(rows->lastRowHas("read_blob_job_schedule_failures"));
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_job_schedule_failures"));
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
}

TEST(CASGCRedeleteConcurrency, WorkerRemoveFaultKeepsSiblingOutcomesAndPoolAlive)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    backend->armRemoveFaults({blobKeyOf(*store, kFaultedBlob)});
    const auto failed_before = ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed];
    std::map<String, std::vector<OutcomeRow>> outcome_logs_before;
    collectOutcomeLogs(*backend, outcome_logs_before);
    RoundReport progress;
    EXPECT_ANY_THROW(gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress));
    store->renewWatermarkOnce();

    std::map<String, std::vector<OutcomeRow>> outcome_logs_after;
    collectOutcomeLogs(*backend, outcome_logs_after);
    EXPECT_EQ(outcome_logs_after.size(), outcome_logs_before.size());
    for (const auto & log : outcome_logs_after)
        EXPECT_TRUE(outcome_logs_before.contains(log.first)) << "the failed round wrote outcome log " << log.first;
    EXPECT_EQ(progress.redeleted, kBlobs - 1);
    expectBodiesPresentOnlyFor(*backend, *store, {kFaultedBlob});

    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_TRUE(backend->allFired()) << "the faulted blob delete never ran on a pool worker";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed] - failed_before, 1u);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_EQ(next.absent, kBlobs - 1);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, SequentialWorkerFailureStillAttemptsLaterEntries)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 1);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    const auto preview = previewPendingDeletes(gc);
    ASSERT_GE(preview.size(), 2u);
    const String first_preview_key = preview.front().key;
    backend->armRemoveFaults({first_preview_key}, false);
    const auto failed_before = failedCounter();
    RoundReport progress;
    String message;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const std::exception & e)
    {
        message = e.what();
    }
    store->renewWatermarkOnce();

    EXPECT_NE(message.find(first_preview_key), String::npos);
    EXPECT_TRUE(backend->allFired());
    EXPECT_EQ(progress.redeleted, preview.size() - 1);
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_EQ(failedCounter() - failed_before, 1u);
    for (size_t i = 1; i < preview.size(); ++i)
    {
        OperationForTest op(*backend);
        EXPECT_FALSE((*op).head(preview[i].key, Retry::standard()).has_value()) << preview[i].key;
    }
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

namespace
{

BlobRef blobRefOf(uint64_t blob)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(blobHash(blob))};
}

void driveUntilAllGraduated(Backend & backend, Gc & gc, const PoolPtr & store, uint64_t marked_blob)
{
    bool graduated = false;
    for (int i = 0; i < 8 && !graduated; ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        ASSERT_EQ(rep.redeleted, 0u) << "no delete may run before the replacement is in place";
        graduated = rep.graduated == kBlobs;
    }
    ASSERT_TRUE(graduated) << "the six blobs never became delete_pending together";

    OperationForTest op(backend);
    ASSERT_TRUE((*op).head(store->layout().blobMetaKey(blobRefOf(marked_blob)), Retry::standard()).has_value())
        << "the condemn marker must exist before the redelete round, or the meta assertion is vacuous";
}

void expectOnlyReplacedBlobSurvives(Backend & backend, const Pool & store, const RoundReport & rep)
{
    EXPECT_EQ(rep.redeleted, kBlobs);
    EXPECT_EQ(rep.deleted, kBlobs - 1);
    EXPECT_EQ(rep.replaced, 1u);

    for (uint64_t b = 1; b <= kBlobs; ++b)
    {
        OperationForTest op(backend);
        const bool body_present = (*op).head(blobKeyOf(store, b), Retry::standard()).has_value();
        const bool meta_present = (*op).head(store.layout().blobMetaKey(blobRefOf(b)), Retry::standard()).has_value();
        EXPECT_EQ(body_present, b == kReplacedBlob) << "blob " << b;
        EXPECT_EQ(meta_present, b == kReplacedBlob) << "meta of blob " << b;
    }

    std::map<String, std::vector<OutcomeRow>> logs;
    collectOutcomeLogs(backend, logs);
    size_t replaced_rows = 0;
    size_t deleted_rows = 0;
    for (const auto & [key, rows] : logs)
    {
        for (const auto & [ref, outcome] : rows)
        {
            if (outcome == OutcomeKind::Replaced)
            {
                EXPECT_EQ(ref, blobRefOf(kReplacedBlob));
                ++replaced_rows;
            }
            else if (outcome == OutcomeKind::Deleted)
            {
                EXPECT_NE(ref, blobRefOf(kReplacedBlob));
                ++deleted_rows;
            }
        }
    }
    EXPECT_EQ(replaced_rows, 1u);
    EXPECT_EQ(deleted_rows, kBlobs - 1);
}

}

TEST(CASGCRedeleteConcurrency, ReplacedBlobInsideParallelBatchSurvivesWhileSiblingsDelete)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kReplacedBlob));

    {
        OperationForTest op(*backend);
        const auto current = (*op).read(blobKeyOf(*store, kReplacedBlob), Retry::standard());
        ASSERT_TRUE(current);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(blobKeyOf(*store, kReplacedBlob), current->bytes, current->etag, Retry::standard())));
    }

    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    expectOnlyReplacedBlobSurvives(*backend, *store, rep);
}

TEST(CASGCRedeleteConcurrency, BlobReplacedBetweenHeadAndDeleteSurvivesWhileSiblingsDelete)
{
    auto backend = std::make_shared<RedeleteDiagnosticsBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, {}, std::nullopt, 2);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kReplacedBlob));

    const String raced_key = blobKeyOf(*store, kReplacedBlob);
    const auto blob_keys = allBlobKeys(*store);
    const auto caller = std::this_thread::get_id();
    std::optional<String> replacement_token;
    backend->delete_chunk_hook = [&](const std::vector<Backend::RawConditionalRemove> & removals, TransportAccess & access)
    {
        for (const auto & removal : removals)
        {
            if (removal.key != raced_key)
                continue;
            EXPECT_NE(std::this_thread::get_id(), caller);
            ASSERT_EQ(removals.size(), 2u);
            EXPECT_TRUE(std::any_of(removals.begin(), removals.end(), [&](const auto & sibling)
            {
                return sibling.key != raced_key && blob_keys.contains(sibling.key);
            }));
            const auto original = backend->read(raced_key, access);
            ASSERT_TRUE(original);
            const auto written = backend->write(raced_key, original->bytes, removal.expected_token, access);
            ASSERT_TRUE(written.has_value());
            replacement_token = *written;
        }
    };
    const RoundReport rep = runRegularRoundReclaiming(gc);
    backend->delete_chunk_hook = {};
    store->renewWatermarkOnce();

    ASSERT_TRUE(replacement_token);
    OperationForTest op(*backend);
    const auto survivor = (*op).head(raced_key, Retry::once());
    ASSERT_TRUE(survivor);
    EXPECT_EQ(PersistedEtag::capture(survivor->etag).value, *replacement_token);
    expectOnlyReplacedBlobSurvives(*backend, *store, rep);
}

TEST(CASGCRedeleteConcurrency, BlobAlreadyAbsentInsideParallelBatchIsRecordedAbsent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kAbsentBlob));

    {
        OperationForTest op(*backend);
        const auto current = (*op).head(blobKeyOf(*store, kAbsentBlob), Retry::standard());
        ASSERT_TRUE(current);
        ASSERT_EQ((*op).remove(blobKeyOf(*store, kAbsentBlob), current->etag, Retry::standard()), Removal::Removed);
    }

    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_EQ(rep.redeleted, kBlobs);
    EXPECT_EQ(rep.deleted, kBlobs - 1);
    EXPECT_EQ(rep.absent, 1u);
    EXPECT_EQ(rep.replaced, 0u);

    for (uint64_t b = 1; b <= kBlobs; ++b)
    {
        OperationForTest op(*backend);
        EXPECT_FALSE((*op).head(blobKeyOf(*store, b), Retry::standard()).has_value()) << "blob " << b;
        EXPECT_FALSE((*op).head(store->layout().blobMetaKey(blobRefOf(b)), Retry::standard()).has_value()) << "meta of blob " << b;
    }

    std::map<String, std::vector<OutcomeRow>> logs;
    collectOutcomeLogs(*backend, logs);
    size_t absent_rows = 0;
    size_t deleted_rows = 0;
    for (const auto & [key, rows] : logs)
    {
        for (const auto & [ref, outcome] : rows)
        {
            if (outcome == OutcomeKind::Absent)
            {
                EXPECT_EQ(ref, blobRefOf(kAbsentBlob));
                ++absent_rows;
            }
            else if (outcome == OutcomeKind::Deleted)
            {
                EXPECT_NE(ref, blobRefOf(kAbsentBlob));
                ++deleted_rows;
            }
        }
    }
    EXPECT_EQ(absent_rows, 1u);
    EXPECT_EQ(deleted_rows, kBlobs - 1);
}

namespace
{

uint64_t failedCounter()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed];
}

void expectBodiesPresentOnlyFor(Backend & backend, const Pool & store, const std::set<uint64_t> & present)
{
    for (uint64_t b = 1; b <= kBlobs; ++b)
        EXPECT_EQ(blobAbsent(backend, store.layout(), blobHash(b)), !present.contains(b)) << "blob " << b;
}

struct RecordHookState
{
    std::atomic<bool> armed{false};
    BlobRef target{};
    size_t recorded = 0;
};

}

TEST(CASGCRedeleteConcurrency, TwoWorkerFailuresAreEachCountedAndTheRoundThrowsOnce)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    const auto preview = previewPendingDeletes(gc);
    const String faulted_key = blobKeyOf(*store, kFaultedBlob);
    const String replaced_key = blobKeyOf(*store, kReplacedBlob);
    const auto faulted_it = std::find_if(preview.begin(), preview.end(), [&](const Gc::PreviewEntry & entry)
    {
        return entry.key == faulted_key;
    });
    const auto replaced_it = std::find_if(preview.begin(), preview.end(), [&](const Gc::PreviewEntry & entry)
    {
        return entry.key == replaced_key;
    });
    ASSERT_NE(faulted_it, preview.end());
    ASSERT_NE(replaced_it, preview.end());
    const String first_fault_key = faulted_it < replaced_it ? faulted_key : replaced_key;

    backend->armRemoveFaults({faulted_key, replaced_key});
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const auto failed_before = failedCounter();
    RoundReport progress;
    String message;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const std::exception & e)
    {
        message = e.what();
    }
    store->renewWatermarkOnce();

    EXPECT_NE(message.find(first_fault_key), String::npos);
    EXPECT_TRUE(backend->allFired()) << "both faulted deletes must have run on pool workers";
    EXPECT_EQ(failedCounter() - failed_before, 2u);
    EXPECT_EQ(progress.redeleted, kBlobs - 2);
    EXPECT_EQ(progress.redelete_failed, 2u);
    ASSERT_EQ(rows->rows.size(), 1u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_failed")) << "the phase metrics must survive the round's exception";
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 2u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
    expectBodiesPresentOnlyFor(*backend, *store, {kFaultedBlob, kReplacedBlob});

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.deleted, 2u);
    EXPECT_EQ(next.absent, kBlobs - 2);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, FailuresAreReportedAfterTheirWaveAndRethrowLowestBlobIndex)
{
    for (const uint64_t concurrency : {1, 4})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<RedeleteDiagnosticsBackend>();
        backend->pinConditionalBatchDeleteSupport(true);
        auto store = openPoolWithIoConcurrency(backend, concurrency, 1, 0, {}, std::nullopt, 2);
        Gc gc(store, kGc);
        publishThenDrop(*backend, store, gc);
        ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));
        const auto preview = previewPendingDeletes(gc);
        ASSERT_EQ(preview.size(), kBlobs);

        /// Identical messages require the GC diagnostic to identify the operation itself.
        const auto read_error = std::make_exception_ptr(std::runtime_error("injected job fault"));
        const auto delete_error = std::make_exception_ptr(std::runtime_error("injected job fault"));
        backend->blob_keys = allBlobKeys(*store);
        backend->read_blob_hook = [&](const String & key)
        {
            if (key == preview.back().key)
                std::rethrow_exception(read_error);
        };
        backend->delete_chunk_hook = [&](const std::vector<Backend::RawConditionalRemove> & removals, TransportAccess &)
        {
            if (removals.front().key == preview.front().key)
            {
                EXPECT_EQ(removals.size(), 2u);
                std::rethrow_exception(delete_error);
            }
        };

        ScopedLogCapture capture("CasGc", "warning");
        auto rows = std::make_shared<PendingDeletesRows>();
        gc.setPhaseSink(recordPendingDeletes(rows));
        const auto failed_before = failedCounter();
        RoundReport progress;
        std::exception_ptr error;
        try
        {
            gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
        }
        catch (...)
        {
            error = std::current_exception();
        }
        backend->read_blob_hook = {};
        backend->delete_chunk_hook = {};

        const String log = capture.captured();
        const size_t read_warning = log.find("CAS gc: read blob job failed");
        const size_t delete_warning = log.find("CAS gc: delete chunk job failed");
        ASSERT_NE(read_warning, String::npos);
        ASSERT_NE(delete_warning, String::npos);
        EXPECT_LT(log.rfind("Test blob read finished"), read_warning);
        EXPECT_LT(read_warning, log.find("Test delete chunk started"));
        EXPECT_LT(log.rfind("Test delete chunk finished"), delete_warning);
        EXPECT_EQ(capture.count("CAS gc: read blob job failed"), 1u);
        EXPECT_EQ(capture.count("CAS gc: delete chunk job failed"), 2u);
        EXPECT_EQ(error, delete_error) << "the lower blob index wins even though its wave is logged later";
        EXPECT_EQ(progress.redeleted, 3u);
        EXPECT_EQ(progress.redelete_failed, 3u);
        EXPECT_EQ(failedCounter() - failed_before, 3u);
        EXPECT_EQ(rows->total("read_blob_jobs_failed"), 1u);
        EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 1u);
        EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
        EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
        EXPECT_EQ(rows->total("delete_chunks"), 3u);
        EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), concurrency == 1 ? 0u : kBlobs);
        EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), concurrency == 1 ? 0u : 3u);
        EXPECT_EQ(previewPendingDeletes(gc).size(), kBlobs);
        for (size_t i = 0; i < preview.size(); ++i)
        {
            OperationForTest op(*backend);
            EXPECT_EQ((*op).head(preview[i].key, Retry::once()).has_value(), i < 2 || i == 5);
        }
        store->renewWatermarkOnce();
        EXPECT_EQ(runRegularRoundReclaiming(gc).redeleted, kBlobs);
        EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    }
}

TEST(CASGCRedeleteConcurrency, EnqueueFailuresIdentifyWaveAndUnscheduledBlobCount)
{
    struct Case
    {
        bool delete_wave;
        size_t refused_job;
        size_t deleted;
        size_t unscheduled;
        size_t read_blob_jobs_scheduled;
        size_t delete_chunk_jobs_scheduled;
    };
    for (const Case test : {Case{false, 0, 0, 6, 0, 0}, Case{false, 3, 3, 3, 3, 0}, Case{true, 0, 0, 6, 6, 0}, Case{true, 1, 4, 2, 6, 1}})
    {
        SCOPED_TRACE(test.delete_wave);
        SCOPED_TRACE(test.refused_job);
        auto backend = std::make_shared<InMemoryBackend>();
        backend->pinConditionalBatchDeleteSupport(true);
        const size_t refused_index = (test.delete_wave ? kBlobs : 0) + test.refused_job;
        auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, {}, refused_index, 4);
        Gc gc(store, kGc);
        publishThenDrop(*backend, store, gc);
        ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

        ScopedLogCapture capture("CasGc", "warning");
        auto rows = std::make_shared<PendingDeletesRows>();
        gc.setPhaseSink(recordPendingDeletes(rows));
        const auto failed_before = failedCounter();
        RoundReport progress;
        expectThrowsCode(DB::ErrorCodes::CANNOT_SCHEDULE_TASK, [&]
        {
            gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
        });
        EXPECT_EQ(capture.count(test.delete_wave
            ? "CAS gc: scheduling delete chunk job failed" : "CAS gc: scheduling read blob job failed"), 1u);
        EXPECT_NE(capture.captured().find(std::to_string(test.unscheduled) + (test.delete_wave
            ? " blob deletions were not submitted" : " blob reads were not submitted")), String::npos);
        EXPECT_EQ(progress.redeleted, test.deleted);
        EXPECT_EQ(progress.redelete_failed, 1u);
        EXPECT_EQ(failedCounter() - failed_before, 1u);
        ASSERT_TRUE(rows->lastRowHas("read_blob_jobs_scheduled"));
        ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_scheduled"));
        EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), test.read_blob_jobs_scheduled);
        EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), test.delete_chunk_jobs_scheduled);
        EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
        EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
        EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), test.delete_wave ? 0u : 1u);
        EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), test.delete_wave ? 1u : 0u);
        EXPECT_EQ(previewPendingDeletes(gc).size(), kBlobs);
        store->renewWatermarkOnce();
        EXPECT_EQ(runRegularRoundReclaiming(gc).redeleted, kBlobs);
        EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    }
}

TEST(CASGCRedeleteConcurrency, WorkerHeadFaultIsCountedAndRetriedNextRound)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    backend->armHeadFaults({blobKeyOf(*store, kFaultedBlob)});
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const auto failed_before = failedCounter();
    RoundReport progress;
    EXPECT_ANY_THROW(gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress));
    store->renewWatermarkOnce();

    EXPECT_TRUE(backend->allFired()) << "the faulted HEAD must have run on a pool worker";
    EXPECT_EQ(failedCounter() - failed_before, 1u);
    EXPECT_EQ(progress.redeleted, kBlobs - 1);
    EXPECT_EQ(progress.redelete_failed, 1u);
    ASSERT_EQ(rows->rows.size(), 1u);
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 1u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
    expectBodiesPresentOnlyFor(*backend, *store, {kFaultedBlob});

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_EQ(next.absent, kBlobs - 1);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, SchedulingFailureAttemptsNothingAndTheNextRoundReclaims)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    SCOPE_EXIT({ CannotAllocateThreadFaultInjector::setFaultProbability(0); });
    auto injected = std::make_shared<std::atomic<bool>>(false);
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows, [injected](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_seal_write")
        {
            CannotAllocateThreadFaultInjector::setFaultProbability(1.0);
            injected->store(true);
        }
        else if (rec.phase == "pending_deletes")
        {
            CannotAllocateThreadFaultInjector::setFaultProbability(0);
        }
    }));

    const auto failed_before = failedCounter();
    RoundReport progress;
    int code = 0;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
    }
    gc.setPhaseSink({});
    CannotAllocateThreadFaultInjector::setFaultProbability(0);
    store->renewWatermarkOnce();

    ASSERT_TRUE(injected->load()) << "the fault was never armed before pending_deletes";
    EXPECT_EQ(code, DB::ErrorCodes::CANNOT_SCHEDULE_TASK);
    EXPECT_EQ(progress.redeleted, 0u);
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_EQ(failedCounter() - failed_before, 1u);
    ASSERT_EQ(rows->rows.size(), 1u);
    ASSERT_TRUE(rows->lastRowHas("read_blob_jobs_scheduled"));
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), 0u);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 1u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
    expectBodiesPresentOnlyFor(*backend, *store, {1, 2, 3, 4, 5, 6});

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.deleted, kBlobs);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, EnqueueRefusalYieldsToLowerIndexWorkerFailure)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, {}, 3);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    const auto preview = previewPendingDeletes(gc);
    ASSERT_EQ(preview.size(), kBlobs);
    const String first_preview_key = preview.front().key;
    backend->armRemoveFaults({first_preview_key});
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const auto failed_before = failedCounter();
    RoundReport progress;
    String message;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const std::exception & e)
    {
        message = e.what();
    }
    store->renewWatermarkOnce();

    EXPECT_EQ(progress.redelete_failed, 2u);
    EXPECT_EQ(failedCounter() - failed_before, 2u);
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), 3u);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 1u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 1u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
    EXPECT_NE(message.find(first_preview_key), String::npos);
    EXPECT_EQ(message.find("Injected CAS GC re-delete enqueue refusal"), String::npos);
    EXPECT_TRUE(backend->allFired());
    for (size_t i = 0; i < preview.size(); ++i)
    {
        OperationForTest op(*backend);
        const bool body_present = (*op).head(preview[i].key, Retry::standard()).has_value();
        EXPECT_EQ(body_present, i == 0 || i >= 3) << preview[i].key;
    }

    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_EQ(next.redelete_failed, 0u);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
}

TEST(CASGCRedeleteConcurrency, SingletonWorkerFailureIsCountedWithoutScheduling)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = 4;
    config.gc_round_redelete_budget = 1;
    auto store = Pool::open(backend, std::move(config));
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    const auto preview = previewPendingDeletes(gc);
    ASSERT_FALSE(preview.empty());
    const String first_preview_key = preview.front().key;
    backend->armRemoveFaults({first_preview_key}, false);
    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const auto failed_before = failedCounter();
    RoundReport progress;
    String message;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const std::exception & e)
    {
        message = e.what();
    }
    store->renewWatermarkOnce();

    EXPECT_NE(message.find(first_preview_key), String::npos);
    EXPECT_TRUE(backend->allFired());
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_EQ(failedCounter() - failed_before, 1u);
    EXPECT_EQ(rows->total("read_blob_jobs_scheduled"), 0u);
    ASSERT_TRUE(rows->lastRowHas("delete_chunk_jobs_scheduled"));
    EXPECT_EQ(rows->total("delete_chunk_jobs_scheduled"), 0u);
    EXPECT_EQ(rows->total("read_blob_jobs_failed"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 1u);
    EXPECT_EQ(rows->total("read_blob_job_schedule_failures"), 0u);
    EXPECT_EQ(rows->total("delete_chunk_job_schedule_failures"), 0u);
}

TEST(CASGCRedeleteConcurrency, ShardedPoolDeletesEveryShard)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4, kShards);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    std::set<uint64_t> expected_shards;
    for (uint64_t b = 1; b <= kBlobs; ++b)
        expected_shards.insert(blobShard(blobRefOf(b), kShards));
    ASSERT_GT(expected_shards.size(), 1u) << "the blobs must spread over several shards, or sharding is untested";

    std::map<String, std::vector<OutcomeRow>> logs;
    size_t deleted = 0;
    for (int i = 0; i < 8 && !allBlobsAbsent(*backend, *store); ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        deleted += rep.deleted;
        collectOutcomeLogs(*backend, logs);
    }
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(deleted, kBlobs);

    std::set<uint64_t> shards_with_rows;
    size_t deleted_rows = 0;
    for (const auto & [key, rows] : logs)
    {
        const size_t slash = key.rfind('/');
        const uint64_t log_shard = std::stoull(key.substr(slash + 1, key.find('.', slash) - slash - 1));
        for (const auto & [ref, outcome] : rows)
        {
            if (outcome != OutcomeKind::Deleted)
                continue;
            EXPECT_EQ(blobShard(ref, kShards), log_shard) << key;
            shards_with_rows.insert(log_shard);
            ++deleted_rows;
        }
    }
    EXPECT_EQ(deleted_rows, kBlobs);
    EXPECT_EQ(shards_with_rows, expected_shards);
}

TEST(CASGCRedeleteConcurrency, RecordPhaseFaultLeavesEveryEntryForTheNextRound)
{
    auto state = std::make_shared<RecordHookState>();
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = 4;
    config.gc_redelete_record_hook_for_test = [state](const BlobRef & ref)
    {
        if (!state->armed.load())
            return;
        if (ref == state->target)
        {
            state->armed.store(false);
            throw std::runtime_error("injected record fault");
        }
        ++state->recorded;
    };
    auto store = Pool::open(backend, std::move(config));
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    state->target = blobRefOf(kFaultedBlob);
    state->armed.store(true);
    const auto failed_before = failedCounter();
    RoundReport progress;
    String message;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const std::exception & e)
    {
        message = e.what();
    }
    store->renewWatermarkOnce();

    EXPECT_NE(message.find("injected record fault"), String::npos) << message;
    EXPECT_EQ(progress.redeleted, state->recorded);
    EXPECT_EQ(failedCounter() - failed_before, 0u) << "a failed record is not a failed delete";
    EXPECT_TRUE(allBlobsAbsent(*backend, *store)) << "every delete finished before the record walk started";

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.absent, kBlobs);
    EXPECT_EQ(next.deleted, 0u);
    for (uint64_t b = 1; b <= kBlobs; ++b)
    {
        OperationForTest op(*backend);
        EXPECT_FALSE((*op).head(store->layout().blobMetaKey(blobRefOf(b)), Retry::standard()).has_value()) << "meta of blob " << b;
    }
}

namespace
{

struct BlobDeleteEvents
{
    std::mutex mutex;
    std::map<String, String> outcome_by_key;
    size_t count = 0;

    CasEventSink sink()
    {
        return [this](CasEvent event)
        {
            if (event.type != CasEventType::BlobDelete)
                return;
            std::lock_guard lock(mutex);
            outcome_by_key[event.detail["key"]] = event.outcome;
            ++count;
        };
    }

    std::pair<std::map<String, String>, size_t> take()
    {
        std::lock_guard lock(mutex);
        auto result = std::make_pair(std::move(outcome_by_key), count);
        outcome_by_key.clear();
        count = 0;
        return result;
    }
};

}

TEST(CASGCRedeleteConcurrency, BlobDeleteEventsMatchAppliedEntriesInAFailedRound)
{
    auto events = std::make_shared<BlobDeleteEvents>();
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4, 1, 0, events->sink());
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));
    ASSERT_EQ(events->take().second, 0u) << "no blob delete may be reported before the redelete round";

    backend->armRemoveFaults({blobKeyOf(*store, kFaultedBlob)});
    EXPECT_ANY_THROW(runRegularRoundReclaiming(gc));
    store->renewWatermarkOnce();
    ASSERT_TRUE(backend->allFired());

    auto [failed_round, failed_round_count] = events->take();
    EXPECT_EQ(failed_round_count, kBlobs - 1);
    EXPECT_FALSE(failed_round.contains(blobKeyOf(*store, kFaultedBlob))) << "a failed delete must not be reported";
    for (uint64_t b = 1; b <= kBlobs; ++b)
        if (b != kFaultedBlob)
            EXPECT_EQ(failed_round[blobKeyOf(*store, b)], "deleted") << "blob " << b;

    runRegularRoundReclaiming(gc);
    auto [next_round, next_round_count] = events->take();
    EXPECT_EQ(next_round_count, kBlobs);
    for (uint64_t b = 1; b <= kBlobs; ++b)
        EXPECT_EQ(next_round[blobKeyOf(*store, b)], b == kFaultedBlob ? "deleted" : "absent") << "blob " << b;
}

TEST(CASGCRedeleteConcurrency, ExhaustedMultiKeyRetryKeepsPublishedPendingState)
{
    constexpr size_t chunk_size = 3;
    auto events = std::make_shared<BlobDeleteEvents>();
    auto backend = std::make_shared<RedeleteDiagnosticsBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    /// Run chunks inline so expiring one request's virtual deadline cannot expire a sibling in flight.
    auto store = openPoolWithIoConcurrency(backend, 1, 1, 0, events->sink(), std::nullopt, chunk_size);
    const auto clock = VirtualRetryClock::installOn(store);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(publishThenDrop(*backend, store, gc));
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));
    const auto pending = previewPendingDeletes(gc);
    ASSERT_EQ(pending.size(), 2 * chunk_size);
    OperationForTest op(*backend);
    const auto & layout = store->layout();
    const auto read_state = [&] { return decodeGcState((*op).read(layout.gcStateKey(), Retry::once()).value().bytes); };
    const auto before = read_state();
    const auto runs_before = runsForShard(*backend, layout, 0);
    ASSERT_FALSE(runs_before.empty());

    size_t physical_attempts = 0;
    std::optional<String> replacement_token;
    backend->delete_chunk_hook = [&](const std::vector<Backend::RawConditionalRemove> & removals, TransportAccess & access)
    {
        ++physical_attempts;
        ASSERT_EQ(removals.size(), chunk_size);
        if (removals.front().key != pending[0].key)
            return;
        for (size_t i = 0; i < chunk_size; ++i)
        {
            EXPECT_EQ(removals[i].key, pending[i].key);
            EXPECT_EQ(removals[i].expected_token, pending[i].token.value);
        }
        if (access.attemptNo() == 1)
        {
            /// A was deleted when the response was lost. A writer replaced B, and C is still eligible.
            ASSERT_EQ(backend->remove(removals[0].key, removals[0].expected_token, access), Backend::RawRemoval::Removed);
            const auto original = backend->read(removals[1].key, access);
            ASSERT_TRUE(original);
            const auto written = backend->write(removals[1].key, original->bytes, removals[1].expected_token, access);
            ASSERT_TRUE(written.has_value());
            replacement_token = *written;
        }
        else
        {
            clock->advance(store->poolConfig().cas_request_budget.request_window_ms);
        }
        throw Poco::TimeoutException("lost conditional delete response");
    };

    auto rows = std::make_shared<PendingDeletesRows>();
    gc.setPhaseSink(recordPendingDeletes(rows));
    const auto failed_before = failedCounter();
    RoundReport progress;
    expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, "policy deadline after 2 attempt(s)", [&]
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    });
    backend->delete_chunk_hook = {};

    ASSERT_TRUE(replacement_token);
    EXPECT_EQ(physical_attempts, 3u);
    ASSERT_EQ(rows->rows.size(), 1u);
    EXPECT_EQ(rows->total("delete_chunks"), 2u);
    EXPECT_EQ(rows->total("delete_chunk_jobs_failed"), 1u);
    EXPECT_EQ(progress.redelete_failed, chunk_size);
    EXPECT_EQ(failedCounter() - failed_before, chunk_size);
    EXPECT_EQ(progress.redeleted, chunk_size);
    /// GC copies per-outcome totals into the report after writing the outcome logs. This round threw before that step.
    EXPECT_EQ(progress.deleted, 0u);
    EXPECT_EQ(progress.absent, 0u);
    EXPECT_EQ(progress.replaced, 0u);

    const auto after = read_state();
    EXPECT_EQ(after.round, before.round);
    EXPECT_EQ(after.snap_generation, before.snap_generation);
    EXPECT_EQ(after.snap_attempt, before.snap_attempt);
    EXPECT_EQ(runsForShard(*backend, layout, 0), runs_before);
    const auto still_pending = previewPendingDeletes(gc);
    ASSERT_EQ(still_pending.size(), pending.size());
    const auto [reported, count] = events->take();
    EXPECT_EQ(count, chunk_size);
    std::map<String, String> expected_events;
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const auto & entry = pending[i];
        EXPECT_EQ(still_pending[i].key, entry.key);
        EXPECT_EQ(still_pending[i].token.value, entry.token.value);
        const auto body = (*op).head(entry.key, Retry::once());
        EXPECT_EQ(body.has_value(), i == 1 || i == 2) << entry.key;
        EXPECT_EQ((*op).head(layout.blobMetaKey(entry.ref), Retry::once()).has_value(), i < chunk_size) << entry.key;
        if (i >= chunk_size)
            expected_events.emplace(entry.key, "deleted");
    }
    EXPECT_EQ(reported, expected_events);

    rows->rows.clear();
    store->renewWatermarkOnce();
    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, pending.size());
    EXPECT_EQ(next.absent, chunk_size + 1);
    EXPECT_EQ(next.replaced, 1u);
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_EQ(next.redelete_failed, 0u);
    EXPECT_EQ(rows->total("delete_chunks"), 1u);
    EXPECT_EQ(read_state().round, before.round + 1);
    EXPECT_TRUE(previewPendingDeletes(gc).empty());
    expected_events.clear();
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const auto & entry = pending[i];
        const auto body = (*op).head(entry.key, Retry::once());
        if (i == 1)
        {
            ASSERT_TRUE(body);
            EXPECT_EQ(PersistedEtag::capture(body->etag).value, *replacement_token);
        }
        else
        {
            EXPECT_FALSE(body) << entry.key;
        }
        EXPECT_EQ((*op).head(layout.blobMetaKey(entry.ref), Retry::once()).has_value(), i == 1) << entry.key;
        expected_events.emplace(entry.key, "absent");
    }
    expected_events.at(pending[1].key) = "replaced";
    expected_events.at(pending[2].key) = "deleted";
    const auto [recovered, recovered_count] = events->take();
    EXPECT_EQ(recovered_count, pending.size());
    EXPECT_EQ(recovered, expected_events);
}

TEST(CASGCRedeleteConcurrency, OutcomeBudgetCapsAuditRowsButNotDeletes)
{
    constexpr uint64_t budget = 3;
    RedeleteRun one;
    RedeleteRun four;
    ASSERT_NO_FATAL_FAILURE(runRedeleteScenario(1, one, budget));
    ASSERT_NO_FATAL_FAILURE(runRedeleteScenario(4, four, budget));

    EXPECT_EQ(one.reports, four.reports);

    std::vector<std::vector<OutcomeRow>> logs_one;
    for (const auto & [key, rows] : one.outcome_logs)
        logs_one.push_back(rows);
    std::vector<std::vector<OutcomeRow>> logs_four;
    for (const auto & [key, rows] : four.outcome_logs)
        logs_four.push_back(rows);
    EXPECT_EQ(logs_one, logs_four) << "the capped rows must be the same entries in the same order at any concurrency";

    size_t rows_total = 0;
    for (const auto & rows : logs_four)
        rows_total += rows.size();
    EXPECT_EQ(rows_total, budget);

    uint64_t redeleted_total = 0;
    uint64_t deleted_total = 0;
    for (const auto & report : four.reports)
    {
        redeleted_total += report[0];
        deleted_total += report[1];
    }
    EXPECT_EQ(redeleted_total, kBlobs) << "the budget caps audit rows, never deletes";
    EXPECT_EQ(deleted_total, kBlobs) << "`deleted` counts every delete, not only those with an audit row";
}

TEST(CASGCRedeleteConcurrency, RoundsWithoutRedeletesWriteNoOutcomeLog)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4, kShards);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);

    std::map<String, std::vector<OutcomeRow>> logs;
    collectOutcomeLogs(*backend, logs);
    bool graduated = false;
    for (int i = 0; i < 8 && !graduated; ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        ASSERT_EQ(rep.redeleted, 0u);
        ASSERT_EQ(rep.spared, 0u);
        graduated = rep.graduated == kBlobs;
        collectOutcomeLogs(*backend, logs);
    }
    ASSERT_TRUE(graduated) << "the scenario must reach graduation, or the rounds under test did nothing";

    for (const auto & [key, rows] : logs)
        ADD_FAILURE() << "a round without redeletes or spares wrote the outcome log " << key << " with " << rows.size() << " row(s)";
    EXPECT_TRUE(logs.empty());
}

namespace
{

Gc::RedeleteBlobState liveRedeleteBlobState(CasOperation & op, const String & key)
{
    Gc::RedeleteBlobState redelete_blob_state;
    redelete_blob_state.blob_key = key;
    redelete_blob_state.live_token = std::get<Committed>(op.create(key, "v", Retry::once())).etag;
    return redelete_blob_state;
}

Gc::RedeleteBlobState settledRedeleteBlobState(const String & key, const Removal removal_result)
{
    Gc::RedeleteBlobState redelete_blob_state;
    redelete_blob_state.blob_key = key;
    redelete_blob_state.removal_result = removal_result;
    return redelete_blob_state;
}

std::vector<std::vector<size_t>> chunkBlobStateIndices(const std::vector<Gc::ConditionalDeleteChunk> & chunks)
{
    std::vector<std::vector<size_t>> blob_state_indices;
    for (const auto & chunk : chunks)
        blob_state_indices.push_back(chunk.blob_state_indices);
    return blob_state_indices;
}

}

TEST(CASGCConditionalDeleteChunks, OnlyLiveTokensWithoutErrorsAreChunked)
{
    auto backend = std::make_shared<InMemoryBackend>();
    OperationForTest op(backend);
    std::vector<Gc::RedeleteBlobState> redelete_blob_states;
    redelete_blob_states.push_back(liveRedeleteBlobState(*op, "a"));
    redelete_blob_states.push_back(settledRedeleteBlobState("gone", Removal::Gone));
    redelete_blob_states.push_back(liveRedeleteBlobState(*op, "b"));
    redelete_blob_states.push_back(settledRedeleteBlobState("replaced", Removal::Mismatch));
    redelete_blob_states.push_back(liveRedeleteBlobState(*op, "failed"));
    redelete_blob_states.back().error = std::make_exception_ptr(std::runtime_error("head failed"));
    redelete_blob_states.emplace_back();
    redelete_blob_states.push_back(liveRedeleteBlobState(*op, "c"));

    const auto chunks = Gc::prepareConditionalDeleteChunks(redelete_blob_states, 10);

    ASSERT_EQ(chunkBlobStateIndices(chunks), (std::vector<std::vector<size_t>>{{0, 2, 6}}));
    const auto & conditional_removals = chunks.front().conditional_removals;
    ASSERT_EQ(conditional_removals.size(), 3u);
    for (size_t pos = 0; pos < conditional_removals.size(); ++pos)
    {
        const auto & redelete_blob_state = redelete_blob_states[chunks.front().blob_state_indices[pos]];
        EXPECT_EQ(conditional_removals[pos].key, redelete_blob_state.blob_key);
        EXPECT_EQ(conditional_removals[pos].expected_token, *redelete_blob_state.live_token);
    }
}

TEST(CASGCConditionalDeleteChunks, ChunksAreBoundedByChunkKeys)
{
    auto backend = std::make_shared<InMemoryBackend>();
    OperationForTest op(backend);
    std::vector<Gc::RedeleteBlobState> redelete_blob_states;
    for (const String key : {"a", "b", "c", "d", "e"})
        redelete_blob_states.push_back(liveRedeleteBlobState(*op, key));

    EXPECT_EQ(
        chunkBlobStateIndices(Gc::prepareConditionalDeleteChunks(redelete_blob_states, 2)),
        (std::vector<std::vector<size_t>>{{0, 1}, {2, 3}, {4}}));
    EXPECT_EQ(
        chunkBlobStateIndices(Gc::prepareConditionalDeleteChunks(redelete_blob_states, 1)),
        (std::vector<std::vector<size_t>>{{0}, {1}, {2}, {3}, {4}}));
    EXPECT_EQ(
        chunkBlobStateIndices(Gc::prepareConditionalDeleteChunks(redelete_blob_states, 5)),
        (std::vector<std::vector<size_t>>{{0, 1, 2, 3, 4}}));
}

TEST(CASGCConditionalDeleteChunks, NothingEligibleGivesNoChunks)
{
    std::vector<Gc::RedeleteBlobState> redelete_blob_states;
    redelete_blob_states.push_back(settledRedeleteBlobState("gone", Removal::Gone));
    redelete_blob_states.push_back(settledRedeleteBlobState("replaced", Removal::Mismatch));
    redelete_blob_states.emplace_back();

    EXPECT_TRUE(Gc::prepareConditionalDeleteChunks(redelete_blob_states, 2).empty());
    EXPECT_TRUE(Gc::prepareConditionalDeleteChunks({}, 2).empty());
}
