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
    std::optional<size_t> refuse_at_for_test = std::nullopt)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = concurrency;
    config.gc_shards = gc_shards;
    config.gc_round_outcome_entry_budget = outcome_entry_budget;
    config.event_sink = std::move(event_sink);
    config.gc_io_pool_refuse_at_for_test = refuse_at_for_test;
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
    EXPECT_EQ(rows->total("jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("jobs_failed"), 0u);
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
    ASSERT_TRUE(rows->lastRowHas("jobs_scheduled"));
    EXPECT_EQ(rows->total("jobs_scheduled"), 0u) << "concurrency 1 must not use the pool";
}

TEST(CASGCRedeleteConcurrency, WorkerRemoveFaultKeepsSiblingOutcomesAndPoolAlive)
{
    auto backend = std::make_shared<WorkerFaultBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kFaultedBlob));

    backend->armRemoveFaults({blobKeyOf(*store, kFaultedBlob)});
    const auto failed_before = ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load();
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
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load() - failed_before, 1u);
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

class RemoveRaceBackend : public InMemoryBackend
{
public:
    void armAgainstOtherThreads(String key)
    {
        raced_key = std::move(key);
        owner = std::this_thread::get_id();
        armed.store(true);
    }

    bool racedOnWorker() const { return raced_on_worker.load(); }

    bool replacementCommitted() const { return replacement_committed.load(); }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (armed.load() && key == raced_key && armed.exchange(false))
        {
            raced_on_worker.store(std::this_thread::get_id() != owner);
            const std::optional<Raw> current = InMemoryBackend::read(key, access);
            if (current)
                replacement_committed.store(InMemoryBackend::write(key, current->bytes, current->value, access).has_value());
        }
        return InMemoryBackend::remove(key, expected_value, access);
    }

private:
    String raced_key;
    std::thread::id owner;
    std::atomic<bool> armed{false};
    std::atomic<bool> raced_on_worker{false};
    std::atomic<bool> replacement_committed{false};
};

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
    auto backend = std::make_shared<RemoveRaceBackend>();
    auto store = openPoolWithIoConcurrency(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    ASSERT_NO_FATAL_FAILURE(driveUntilAllGraduated(*backend, gc, store, kReplacedBlob));

    backend->armAgainstOtherThreads(blobKeyOf(*store, kReplacedBlob));
    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_TRUE(backend->replacementCommitted()) << "the race never replaced the blob, so the 412 path is untested";
    EXPECT_TRUE(backend->racedOnWorker()) << "the raced delete must have run on a pool worker";
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
    return ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load();
}

void expectBodiesPresentOnlyFor(Backend & backend, const Pool & store, const std::set<uint64_t> & present)
{
    for (uint64_t b = 1; b <= kBlobs; ++b)
        EXPECT_EQ(blobAbsent(backend, store.layout(), blobHash(b)), !present.contains(b)) << "blob " << b;
}

struct ApplyHookState
{
    std::atomic<bool> armed{false};
    BlobRef target{};
    size_t applied = 0;
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
    ASSERT_TRUE(rows->lastRowHas("jobs_failed")) << "the phase metrics must survive the round's exception";
    EXPECT_EQ(rows->total("jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("jobs_failed"), 2u);
    expectBodiesPresentOnlyFor(*backend, *store, {kFaultedBlob, kReplacedBlob});

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.redeleted, kBlobs);
    EXPECT_EQ(next.deleted, 2u);
    EXPECT_EQ(next.absent, kBlobs - 2);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
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
    EXPECT_EQ(rows->total("jobs_scheduled"), kBlobs);
    EXPECT_EQ(rows->total("jobs_failed"), 1u);
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
    ASSERT_TRUE(rows->lastRowHas("jobs_scheduled"));
    EXPECT_EQ(rows->total("jobs_scheduled"), 0u);
    EXPECT_EQ(rows->total("jobs_failed"), 1u);
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
    EXPECT_EQ(rows->total("jobs_scheduled"), 3u);
    EXPECT_EQ(rows->total("jobs_failed"), 2u);
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
    EXPECT_EQ(rows->total("jobs_scheduled"), 0u);
    EXPECT_EQ(rows->total("jobs_failed"), 1u);
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

TEST(CASGCRedeleteConcurrency, ApplyPhaseFaultLeavesEveryEntryForTheNextRound)
{
    auto state = std::make_shared<ApplyHookState>();
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = 4;
    config.gc_redelete_apply_hook_for_test = [state](const BlobRef & ref)
    {
        if (!state->armed.load())
            return;
        if (ref == state->target)
        {
            state->armed.store(false);
            throw std::runtime_error("injected apply fault");
        }
        ++state->applied;
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

    EXPECT_NE(message.find("injected apply fault"), String::npos) << message;
    EXPECT_EQ(progress.redeleted, state->applied);
    EXPECT_EQ(failedCounter() - failed_before, 0u) << "a failed apply is not a failed delete";
    EXPECT_TRUE(allBlobsAbsent(*backend, *store)) << "every delete finished before the apply walk started";

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
    EXPECT_EQ(deleted_total, budget) << "`deleted` is tallied from the durable audit rows";
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
