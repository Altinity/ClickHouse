#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <base/getThreadId.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
    extern const int CORRUPTED_DATA;
    extern const int READONLY;
}

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::idOf;
using DB::Cas::tests::openRequestsForTest;
using DB::Cas::tests::u128Of;
using namespace DB::Cas::tests;

namespace
{

/// ============================ the class, on its own ============================

struct ReadAheadRig
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};

    void put(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::once()))) << key;
    }
};

}

TEST(CASGCReadAhead, HitReturnsTheHintedBytesWithOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    const auto got = reads.takeRead("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "one");
    EXPECT_EQ(reads.pending(), 0u);
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, MissReadsInlineOnTheCallersOperation)
{
    ReadAheadRig rig;
    rig.put("k2", "two");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    const auto got = reads.takeRead("k2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "two");
    EXPECT_EQ(rig.backend->getCount("k2"), 1u);
}

TEST(CASGCReadAhead, AbsentKeyIsNulloptHintedOrNot)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);

    reads.hintRead("absent-hinted");
    EXPECT_FALSE(reads.takeRead("absent-hinted").has_value());
    EXPECT_FALSE(reads.takeRead("absent-inline").has_value());
}

TEST(CASGCReadAhead, DuplicateHintIsOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintRead("k1");
    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, WorkerExceptionRethrowsAtTheTakeSiteAndDoesNotPoisonTheKey)
{
    ReadAheadRig rig;
    rig.put("k3", "three");
    /// A non-Poco exception is a deterministic local failure to the engine, so it is thrown on the
    /// first attempt rather than reissued.
    rig.backend->failNextReadWith("k3", std::make_exception_ptr(std::runtime_error("injected read fault")));
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);

    reads.hintRead("k3");
    EXPECT_THROW(reads.takeRead("k3"), std::runtime_error);
    EXPECT_EQ(reads.pending(), 0u);

    const auto again = reads.takeRead("k3");   /// the fault was consumed; an inline read now answers
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->bytes, "three");
}

TEST(CASGCReadAhead, ConcurrencyOneNeverHints)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 1);
    rig.backend->resetCounts();

    EXPECT_EQ(reads.window(), 0u);
    reads.hintRead("k1");
    reads.hintHead("k1");
    EXPECT_EQ(reads.pending(), 0u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    ASSERT_TRUE(reads.takeHead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, DestructorWaitsForOutstandingRequests)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    rig.backend->resetCounts();
    {
        GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
        reads.hintRead("k1");
        reads.hintHead("k1");
    }
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, HeadHitCarriesSizeAndAbsentIsNullopt)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintHead("k1");
    const auto meta = reads.takeHead("k1");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->size, 3u);
    EXPECT_FALSE(reads.takeHead("absent").has_value());
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, WindowIsFourTimesConcurrency)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 8);
    EXPECT_EQ(reads.window(), 32u);
}

/// A create's state is released when its answer is taken, so a site that keeps a window of creates in flight holds
/// at most that window, however many creates it sends.
TEST(CASGCReadAhead, TakenCreateIsReleased)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    std::deque<std::function<IntentCreateResult()>> outstanding;
    size_t answered = 0;
    size_t most_held = 0;
    const auto takeOldest = [&]
    {
        EXPECT_FALSE(outstanding.front()().confirmed());
        outstanding.pop_front();
        ++answered;
    };
    for (size_t i = 0; i < 64; ++i)
    {
        outstanding.push_back(reads.startCreate([](CasOperation &) { return IntentCreateResult{}; }));
        if (outstanding.size() == 4)
            takeOldest();
        most_held = std::max(most_held, reads.untakenCreates());
    }
    while (!outstanding.empty())
        takeOldest();
    EXPECT_EQ(answered, 64u) << "positive control: every create was answered";
    EXPECT_LE(most_held, 4u);
    EXPECT_EQ(reads.untakenCreates(), 0u);
}

/// Once a create fails, a create that starts later sends nothing and answers with that first failure, whichever
/// answer is taken first. One pool thread runs the two in the order they were started.
TEST(CASGCReadAhead, CreateStartedAfterAFailureAnswersWithThatFailure)
{
    ReadAheadRig rig;
    ThreadPool one_thread{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
                          /*max_threads*/ 1, /*max_free_threads*/ 1, /*queue_size*/ 0};
    GcReadAhead reads(rig.op, rig.requests, one_thread, 4);
    std::atomic<bool> second_sent{false};
    auto first = reads.startCreate([](CasOperation &) -> IntentCreateResult
    {
        throw DB::Exception(DB::ErrorCodes::READONLY, "the store is read-only");
    });
    auto second = reads.startCreate([&](CasOperation &)
    {
        second_sent = true;
        return IntentCreateResult{.outcome = IntentCreate::Created, .detail = {}};
    });

    for (auto * answer : {&second, &first})
    {
        try
        {
            (*answer)();
            ADD_FAILURE() << "a create answered after the first create of the run failed";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::READONLY) << e.message();
            EXPECT_EQ(e.message(), "the store is read-only");
        }
    }
    EXPECT_FALSE(second_sent.load());
}

/// A create that cannot be scheduled leaves nothing for the destructor to wait on, and the next create runs.
TEST(CASGCReadAhead, CreateThatCannotBeScheduledLeavesNoCreateBehind)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    std::atomic<bool> sent{false};
    const auto request = [&](CasOperation &)
    {
        sent = true;
        return IntentCreateResult{.outcome = IntentCreate::Created, .detail = {}};
    };

    SCOPE_EXIT({ CannotAllocateThreadFaultInjector::setFaultProbability(0); });
    CannotAllocateThreadFaultInjector::setFaultProbability(1.0);
    int code = 0;
    try
    {
        reads.startCreate(request);
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
    }
    CannotAllocateThreadFaultInjector::setFaultProbability(0);
    EXPECT_EQ(code, DB::ErrorCodes::CANNOT_SCHEDULE_TASK);
    EXPECT_EQ(reads.untakenCreates(), 0u);
    EXPECT_FALSE(sent.load());

    /// Positive control: with scheduling back, a create runs and answers.
    auto answer = reads.startCreate(request);
    EXPECT_EQ(reads.untakenCreates(), 1u);
    EXPECT_EQ(answer().outcome, IntentCreate::Created);
    EXPECT_TRUE(sent.load());
    EXPECT_EQ(reads.untakenCreates(), 0u);
}

/// ============================ per-family accounting ============================

namespace
{

/// A clock that moves only when a test or a backend advances it, so every duration below is exact.
std::atomic<UInt64> fake_now_ns{0};

UInt64 fakeClockNs() noexcept
{
    return fake_now_ns.load();
}

/// Advances `fake_now_ns` by a fixed amount inside the read of every key containing a given substring,
/// while armed: the time a read of that key costs.
class ClockAdvancingBackend : public CountingBackend
{
public:
    void delayKeysContaining(const String & substr, UInt64 ns)
    {
        std::lock_guard lock(mutex);
        delays.emplace_back(substr, ns);
    }

    void arm() { armed.store(true); }
    void disarm() { armed.store(false); }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (armed.load())
        {
            std::lock_guard lock(mutex);
            for (const auto & [substr, ns] : delays)
                if (key.find(substr) != String::npos)
                    fake_now_ns.fetch_add(ns);
        }
        return CountingBackend::read(key, access);
    }

private:
    std::mutex mutex;
    std::vector<std::pair<String, UInt64>> delays;
    std::atomic<bool> armed{true};
};

struct FamilyRig
{
    std::shared_ptr<ClockAdvancingBackend> backend = std::make_shared<ClockAdvancingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};

    FamilyRig() { fake_now_ns.store(0); }

    void put(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::once()))) << key;
    }
};

/// The calls of `FamilyCountersFollowTheCalls`, shared with the test that runs them without accounting.
void runFamilyCalls(FamilyRig & rig, GcReadAhead & reads)
{
    reads.hintRead("k1", ReadFamily::RefLog);
    reads.hintRead("k1", ReadFamily::RefLog);
    rig.pool.wait();
    ASSERT_TRUE(reads.takeRead("k1", ReadFamily::RefLog).has_value());
    ASSERT_TRUE(reads.takeRead("k2", ReadFamily::Manifest).has_value());
    reads.hintRead("k4", ReadFamily::Manifest);
    rig.pool.wait();
    ASSERT_TRUE(reads.takeRead("k4", ReadFamily::Manifest).has_value());
    ASSERT_TRUE(reads.takeRead("k3").has_value());
}

void putFamilyKeys(FamilyRig & rig)
{
    for (const char * key : {"k1", "k2", "k3", "k4"})
        rig.put(key, key);
}

}

TEST(CASGCReadAhead, FamilyCountersFollowTheCalls)
{
    for (const size_t concurrency : {4, 1})
    {
        SCOPED_TRACE(concurrency);
        FamilyRig rig;
        putFamilyKeys(rig);
        GcReadAhead reads(rig.op, rig.requests, rig.pool, concurrency);
        reads.enableFamilyAccounting(&fakeClockNs);
        ASSERT_NO_FATAL_FAILURE(runFamilyCalls(rig, reads));

        const bool hinting = concurrency > 1;
        EXPECT_EQ(reads.familyStats(ReadFamily::RefLog), (ReadFamilyStats{.issued = 1, .takes = 1, .awaited = hinting ? 0u : 1u}));
        EXPECT_EQ(reads.familyStats(ReadFamily::Manifest), (ReadFamilyStats{.issued = 2, .takes = 2, .awaited = hinting ? 1u : 2u}));
        EXPECT_EQ(reads.familyStats(ReadFamily::RefCheckpoint), ReadFamilyStats{});
        EXPECT_EQ(reads.familyStats(ReadFamily::Other), ReadFamilyStats{});
    }
}

namespace
{

std::atomic<UInt64> scripted_clock_calls{0};

/// 0 at the first read, 10 at every later one: time passes between a take's entry and the rest of the take.
UInt64 scriptedClockNs() noexcept
{
    return scripted_clock_calls.fetch_add(1) == 0 ? 0 : 10;
}

}

TEST(CASGCReadAhead, AHintedTakesWaitStartsAtItsEntry)
{
    FamilyRig rig;
    putFamilyKeys(rig);
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.hintRead("k1", ReadFamily::RefLog);
    rig.pool.wait();
    scripted_clock_calls.store(0);
    reads.enableFamilyAccounting(&scriptedClockNs);

    ASSERT_TRUE(reads.takeRead("k1", ReadFamily::RefLog).has_value());
    EXPECT_EQ(reads.familyStats(ReadFamily::RefLog).wait_ns, 10u);
}

TEST(CASGCReadAhead, AccountingIsOffUnlessEnabled)
{
    for (const size_t concurrency : {4, 1})
    {
        SCOPED_TRACE(concurrency);
        FamilyRig rig;
        putFamilyKeys(rig);
        GcReadAhead reads(rig.op, rig.requests, rig.pool, concurrency);
        ASSERT_NO_FATAL_FAILURE(runFamilyCalls(rig, reads));
        reads.closeApply();

        for (const ReadFamily family : {ReadFamily::RefCheckpoint, ReadFamily::RefLog, ReadFamily::Manifest, ReadFamily::Other})
            EXPECT_EQ(reads.familyStats(family), ReadFamilyStats{}) << static_cast<int>(family);
    }
}

/// The instrumented path issues exactly the requests the plain path issues, checked against a literal
/// rather than against the other rig, so a defect shared by both paths cannot hide.
TEST(CASGCReadAhead, TaggedAccountingIssuesTheSameRequests)
{
    struct Outcome
    {
        std::vector<String> keys;
        std::optional<String> a;
        bool c_absent = false;
        std::optional<String> d;
    };

    const auto run = [](size_t concurrency, bool accounted)
    {
        FamilyRig rig;
        for (const char * key : {"a", "b", "d"})
            rig.put(key, String("bytes-") + key);
        GcReadAhead reads(rig.op, rig.requests, rig.pool, concurrency);
        if (accounted)
            reads.enableFamilyAccounting(&fakeClockNs);
        const ReadFamily family = accounted ? ReadFamily::RefLog : ReadFamily::Other;
        rig.backend->clearJournal();

        Outcome out;
        reads.hintRead("a", family);
        reads.hintRead("b", family);
        if (const auto got = reads.takeRead("a", family))
            out.a = got->bytes;
        reads.discardRead("b");
        out.c_absent = !reads.takeRead("c", family).has_value();
        if (const auto got = reads.takeRead("d", family))
            out.d = got->bytes;

        for (const JournalEntry & entry : rig.backend->journal())
        {
            EXPECT_EQ(entry.verb, JournalVerb::Read);
            out.keys.insert(out.keys.end(), entry.keys.begin(), entry.keys.end());
        }
        return out;
    };

    for (const bool accounted : {true, false})
    {
        SCOPED_TRACE(accounted);
        const Outcome one = run(1, accounted);
        EXPECT_EQ(one.keys, (std::vector<String>{"a", "c", "d"}));
        EXPECT_EQ(one.a, "bytes-a");
        EXPECT_TRUE(one.c_absent);
        EXPECT_EQ(one.d, "bytes-d");

        Outcome four = run(4, accounted);
        std::sort(four.keys.begin(), four.keys.end());
        EXPECT_EQ(four.keys, (std::vector<String>{"a", "b", "c", "d"}));
        EXPECT_EQ(four.a, "bytes-a");
        EXPECT_TRUE(four.c_absent);
        EXPECT_EQ(four.d, "bytes-d");
    }
}

TEST(CASGCReadAhead, ApplyBelongsToThePrecedingTake)
{
    FamilyRig rig;
    putFamilyKeys(rig);
    rig.backend->delayKeysContaining("k2", 7);
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 1);
    reads.enableFamilyAccounting(&fakeClockNs);

    ASSERT_TRUE(reads.takeRead("k1", ReadFamily::RefLog).has_value());
    fake_now_ns.fetch_add(10);
    ASSERT_TRUE(reads.takeRead("k2", ReadFamily::Manifest).has_value());
    fake_now_ns.fetch_add(20);
    reads.closeApply();
    fake_now_ns.fetch_add(50);
    ASSERT_TRUE(reads.takeRead("k3").has_value());

    EXPECT_EQ(reads.familyStats(ReadFamily::RefLog).apply_ns, 10u);
    EXPECT_EQ(reads.familyStats(ReadFamily::RefLog).wait_ns, 0u);
    EXPECT_EQ(reads.familyStats(ReadFamily::Manifest).wait_ns, 7u);
    EXPECT_EQ(reads.familyStats(ReadFamily::Manifest).apply_ns, 20u);
    EXPECT_EQ(reads.familyStats(ReadFamily::RefCheckpoint), ReadFamilyStats{});
    EXPECT_EQ(reads.familyStats(ReadFamily::Other), ReadFamilyStats{});
}

namespace
{

/// Holds the read of one key inside the backend until `open`, bounded so a defect cannot hang the suite.
class GatedReadBackend : public CountingBackend
{
public:
    explicit GatedReadBackend(String gated_key_) : gated_key(std::move(gated_key_)) {}

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (key == gated_key)
        {
            std::unique_lock lock(mutex);
            inside = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return opened; });
        }
        return CountingBackend::read(key, access);
    }

    bool waitUntilInside()
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return inside; });
    }

    void open()
    {
        std::lock_guard lock(mutex);
        opened = true;
        cv.notify_all();
    }

private:
    const String gated_key;
    std::mutex mutex;
    std::condition_variable cv;
    bool inside = false;
    bool opened = false;
};

/// Moves by one per call; its second call after `reset` is the one after the readiness poll, and it
/// signals the thread that opens the gate.
std::atomic<UInt64> ticking_calls{0};
std::atomic<bool> poll_passed{false};

UInt64 tickingClockNs() noexcept
{
    const UInt64 call = ticking_calls.fetch_add(1);
    if (call == 1)
    {
        poll_passed.store(true);
        poll_passed.notify_all();
    }
    return call;
}

}

TEST(CASGCReadAhead, AHintedUnreadyTakeIsAwaited)
{
    auto backend = std::make_shared<GatedReadBackend>("gated");
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create("gated", "bytes", Retry::once())));
    backend->resetCounts();

    ticking_calls.store(0);
    poll_passed.store(false);
    std::thread releaser([&]
    {
        poll_passed.wait(false);
        backend->open();
    });
    SCOPE_EXIT({
        poll_passed.store(true);
        poll_passed.notify_all();
        releaser.join();
    });

    GcReadAhead reads(op, requests, pool, 4);
    reads.enableFamilyAccounting(&tickingClockNs);
    reads.hintRead("gated", ReadFamily::Manifest);
    ASSERT_TRUE(backend->waitUntilInside());
    const auto got = reads.takeRead("gated", ReadFamily::Manifest);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "bytes");

    const ReadFamilyStats & stats = reads.familyStats(ReadFamily::Manifest);
    EXPECT_EQ(stats.awaited, 1u);
    EXPECT_EQ(stats.takes, 1u);
    EXPECT_EQ(stats.wait_ns, 1u);
    EXPECT_EQ(backend->getCount("gated"), 1u);
}

TEST(CASGCReadAhead, AWorkerFaultOnATaggedTakeStillThrows)
{
    FamilyRig rig;
    rig.put("k3", "three");
    rig.backend->failNextReadWith("k3", std::make_exception_ptr(std::runtime_error("injected read fault")));
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    reads.enableFamilyAccounting(&fakeClockNs);

    reads.hintRead("k3", ReadFamily::RefLog);
    EXPECT_THROW(reads.takeRead("k3", ReadFamily::RefLog), std::runtime_error);
    EXPECT_EQ(reads.familyStats(ReadFamily::RefLog).takes, 1u);
    EXPECT_EQ(reads.pending(), 0u);

    const auto again = reads.takeRead("k3", ReadFamily::RefLog);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->bytes, "three");
    EXPECT_EQ(reads.familyStats(ReadFamily::RefLog).takes, 2u);
}

/// ============================ the fold, at 1 against 8 ============================

namespace
{

const UInt128 kGc = u128Of("gc-read-ahead");

ManifestId publishPart(const PoolPtr & s, const String & ns, const String & ref, const String & payload)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);

    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of(payload))};
    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

/// Three namespaces. `wide` carries a ref-log backlog longer than any window this file uses, so the
/// same-epoch lookahead is genuinely exercised; `quiet` publishes once and never drops, so its
/// frontier is proved by the checkpoint ceiling with no read at all; `gone` is emptied entirely, so
/// its blobs reach in-degree zero and the reduce phase HEADs them. Every blob is unique to its part.
void populate(const PoolPtr & store)
{
    for (int i = 0; i < 30; ++i)
        publishPart(store, "srv1/wide", fmt::format("part_{}", i), fmt::format("wide-payload-{}", i));
    for (int i = 0; i < 15; ++i)
        store->dropRef(RootNamespace{"srv1/wide"}, fmt::format("part_{}", i));

    publishPart(store, "srv1/quiet", "only", "quiet-payload");

    publishPart(store, "srv1/gone", "a", "gone-payload-a");
    publishPart(store, "srv1/gone", "b", "gone-payload-b");
    store->dropRef(RootNamespace{"srv1/gone"}, "a");
    store->dropRef(RootNamespace{"srv1/gone"}, "b");

    store->renewWatermarkOnce();
}

/// TWO POOLS ARE NOT BYTE-COMPARABLE UNTIL THEIR IDENTITIES ARE MAPPED. A namespace's catalog
/// incarnation is minted from the process RNG at creation, and it appears BOTH inside every one of that
/// namespace's object keys and inside the fold seal's `life` rows -- so two independently created pools
/// running the identical workload produce identical decisions under different names, and the seal's
/// `ref_life` rows come out in a different order because they are keyed by that random id.
///
/// Neither fact has anything to do with the read-ahead, and hiding them by weakening the comparison
/// would hide the read-ahead's own defects too. So the identities are MAPPED instead of dropped: each
/// run reports its own incarnation-hex -> namespace-name table, every 32-hex id in a key or a seal is
/// rewritten to the namespace it names, and the `ref_life` rows are sorted once their names are stable.
/// What survives the rewrite is everything the fold decided; what it removes is only the naming.
using IdNames = std::map<String, String>;

String normalizeIds(const String & text, const IdNames & names)
{
    String out = text;
    for (const auto & [hex, name] : names)
    {
        size_t at = 0;
        while ((at = out.find(hex, at)) != String::npos)
        {
            out.replace(at, hex.size(), name);
            at += name.size();
        }
    }
    return out;
}

/// The seal with its ids named and its `ref_life` rows sorted; every other row keeps its position.
String canonicalSeal(const String & seal, const IdNames & names)
{
    const String named = normalizeIds(seal, names);
    std::vector<String> out;
    std::vector<String> lives;
    size_t pos = 0;
    while (pos <= named.size())
    {
        const size_t nl = named.find('\n', pos);
        const String line = named.substr(pos, nl == String::npos ? String::npos : nl - pos);
        if (line.find("\"kind\":\"ref_life\"") != String::npos)
        {
            lives.push_back(line);
        }
        else
        {
            if (!lives.empty())
            {
                std::sort(lives.begin(), lives.end());
                out.insert(out.end(), lives.begin(), lives.end());
                lives.clear();
            }
            out.push_back(line);
        }
        if (nl == String::npos)
            break;
        pos = nl + 1;
    }
    std::sort(lives.begin(), lives.end());
    out.insert(out.end(), lives.begin(), lives.end());

    String joined;
    for (const String & line : out)
    {
        joined += line;
        joined += '\n';
    }
    return joined;
}

struct FoldRun
{
    std::vector<String> seals;                        /// the fold seal's bytes after each round
    std::vector<std::map<String, UInt64>> intake;     /// `fold_ref_intake` metrics, per round
    std::vector<std::map<String, UInt64>> reduce;     /// `fold_reduce` metrics, per round
    std::map<String, uint64_t> gets;                  /// key -> GETs over the whole run
    std::map<String, uint64_t> heads;                 /// key -> HEADs over the whole run
    std::vector<size_t> condemned;
    std::vector<uint64_t> deleted;
    IdNames id_names;                                 /// incarnation hex -> namespace, for the comparison
};

void runFolds(uint64_t concurrency, size_t rounds, FoldRun & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = concurrency});
    populate(store);

    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    /// Read the id table BEFORE the rounds: a namespace the fold reclaims loses its catalog row, and its
    /// keys still have to be nameable when the two runs are compared.
    for (const CatalogEntry & entry : CasRefCatalog::read(op, layout).catalog.entries)
        out.id_names.emplace(u128ToHex(entry.incarnation), "<" + entry.ns.string() + ">");

    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            out.intake.push_back(rec.metrics);
        else if (rec.phase == "fold_reduce")
            out.reduce.push_back(rec.metrics);
    });

    /// Only the rounds' own I/O is compared; the identical population above is not part of the claim.
    backend->resetCounts();

    for (size_t round = 0; round < rounds; ++round)
    {
        const RoundReport report = gc.runRegularRound();
        ASSERT_TRUE(report.acquired_lease) << "round " << round;
        out.condemned.push_back(report.condemned);
        out.deleted.push_back(report.deleted);
        store->renewWatermarkOnce();

        const GcState st = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
        const auto seal = op.read(layout.foldSealKey(st.snap_generation, st.snap_attempt), Retry::once());
        out.seals.push_back(seal ? canonicalSeal(seal->bytes, out.id_names) : String{});
    }

    for (const String & key : backend->touchedKeys())
    {
        const String named = normalizeIds(key, out.id_names);
        if (const uint64_t n = backend->getCount(key); n != 0)
            out.gets[named] += n;
        if (const uint64_t n = backend->headCount(key); n != 0)
            out.heads[named] += n;
    }
}

}

TEST(CASGCReadAhead, FoldIsIdenticalAtConcurrencyOneAndEight)
{
    constexpr size_t kRounds = 6;
    FoldRun one;
    FoldRun eight;
    ASSERT_NO_FATAL_FAILURE(runFolds(1, kRounds, one));
    ASSERT_NO_FATAL_FAILURE(runFolds(8, kRounds, eight));

    ASSERT_EQ(one.seals.size(), kRounds);
    ASSERT_EQ(eight.seals.size(), kRounds);
    for (size_t i = 0; i < kRounds; ++i)
        EXPECT_EQ(one.seals[i], eight.seals[i]) << "fold seal differs at round " << i;

    /// Times differ between any two runs; the counts must not.
    const auto withoutTimes = [](std::vector<std::map<String, UInt64>> rounds)
    {
        for (std::map<String, UInt64> & metrics : rounds)
            std::erase_if(metrics, [](const auto & entry)
            {
                const String & key = entry.first;
                return key.ends_with("_awaited") || key.ends_with("_wait_us") || key.ends_with("_apply_us")
                    || key == "cursor_resolve_us";
            });
        return rounds;
    };
    EXPECT_EQ(withoutTimes(one.intake), withoutTimes(eight.intake));
    /// `populate` hints nothing the walk does not take, so every hint is matched by its take.
    for (const FoldRun * run : {&one, &eight})
        for (size_t i = 0; i < run->intake.size(); ++i)
            for (const char * family : {"ckpt", "reflog", "manifest"})
                EXPECT_EQ(run->intake[i].at(fmt::format("{}_reads_issued", family)),
                          run->intake[i].at(fmt::format("{}_takes", family)))
                    << family << " at round " << i << (run == &one ? " (concurrency 1)" : " (concurrency 8)");
    EXPECT_EQ(one.reduce, eight.reduce);
    EXPECT_EQ(one.condemned, eight.condemned);
    EXPECT_EQ(one.deleted, eight.deleted);

    /// THE REQUEST-SET CLAIM. Every namespace here is healthy, so nothing is hinted that the walk
    /// does not go on to read: the hints stop at the listed tail the walk reaches, a walk paused after
    /// pass 1 hinted only keys it read, and every decoded log's manifest edges are all folded. So the
    /// read-ahead must issue the SAME GETs against the SAME keys, not merely produce the same answer.
    EXPECT_EQ(one.gets, eight.gets);

    ASSERT_FALSE(one.intake.empty());
    EXPECT_GT(one.intake[0].at("logs_applied"), 32u)
        << "the wide namespace must carry more logs than the window, or the lookahead is untested";
}

TEST(CASGCReadAhead, ReduceCondemnsTheSameBlobsWithTheSameHeadsAtConcurrencyOneAndEight)
{
    /// `populate` gives every part its own blob and drops whole parts, so a dropped blob loses its only
    /// edge and no surviving blob has a removal: the hinted set equals the set `head_blob` takes, and the
    /// per-key HEAD counts must match exactly rather than merely producing the same verdict.
    constexpr size_t kRounds = 6;
    FoldRun one;
    FoldRun eight;
    ASSERT_NO_FATAL_FAILURE(runFolds(1, kRounds, one));
    ASSERT_NO_FATAL_FAILURE(runFolds(8, kRounds, eight));

    EXPECT_EQ(one.condemned, eight.condemned);
    EXPECT_EQ(one.heads, eight.heads);

    uint64_t condemned_total = 0;
    for (const size_t n : one.condemned)
        condemned_total += n;
    EXPECT_GT(condemned_total, 0u) << "the scenario must condemn, or the reduce read-ahead is untested";
}

namespace
{

/// Throws once on the first read issued from a thread other than the one that armed it: exactly a
/// read-ahead worker's request, never the round thread's own.
class WorkerReadFaultBackend : public CountingBackend
{
public:
    void armAgainstOtherThreads()
    {
        owner = std::this_thread::get_id();
        armed.store(true);
    }

    bool fired() const { return !armed.load(); }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (armed.load() && std::this_thread::get_id() != owner)
        {
            armed.store(false);
            throw std::runtime_error("injected worker read fault");
        }
        return CountingBackend::read(key, access);
    }

private:
    std::atomic<bool> armed{false};
    std::thread::id owner;
};

}

namespace
{

/// Proves OVERLAP, which no equality test can: it releases a read only once `k_overlap` reads are
/// inside the backend at the same time. If the fold's reads were still strictly one after another the
/// count could never reach two, so the round would block until the bounded wait expires and the flag
/// below would stay false. The wait is bounded and the last arrival wakes everyone, so nothing here can
/// hang the suite: a fold with no overlap finishes late, it does not finish never.
class OverlapWitnessBackend : public CountingBackend
{
public:
    explicit OverlapWitnessBackend(size_t k_overlap_) : k_overlap(k_overlap_) {}

    /// ARMED ONLY FOR THE ROUND. Holding reads is fatal to a WRITER: its checkpoint publication is a
    /// CAS with a bounded retry budget, and a latch on every read exhausts it long before the round
    /// under test ever starts.
    void arm() { armed.store(true); }

    bool sawOverlap() const { return saw_overlap.load(); }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (!armed.load())
            return CountingBackend::read(key, access);
        {
            std::unique_lock lock(mutex);
            ++in_flight;
            peak = std::max(peak, in_flight);
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
        /// The count stays raised ACROSS the read, so what it measures is requests genuinely in the
        /// backend together. Releasing it before the read would leave a window of a few instructions
        /// that two threads would have to hit simultaneously to be seen -- which is a measurement of
        /// luck, not of overlap.
        std::optional<DB::Cas::Backend::Raw> raw = CountingBackend::read(key, access);
        {
            std::lock_guard lock(mutex);
            --in_flight;
        }
        return raw;
    }

    size_t peakInFlight() const
    {
        std::lock_guard lock(mutex);
        return peak;
    }

private:
    const size_t k_overlap;
    std::atomic<bool> armed{false};
    mutable std::mutex mutex;
    std::condition_variable gate;
    size_t in_flight = 0;
    size_t peak = 0;
    std::atomic<bool> saw_overlap{false};
};

}

TEST(CASGCReadAhead, TheFoldsReadsActuallyOverlap)
{
    auto backend = std::make_shared<OverlapWitnessBackend>(/*k_overlap*/ 2);
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 8});
    populate(store);

    Gc gc(store, kGc);
    backend->arm();
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_TRUE(backend->sawOverlap())
        << "no two of the fold's reads were ever in the backend at the same time; peak in flight was "
        << backend->peakInFlight();
    EXPECT_GT(backend->peakInFlight(), 1u);
}

TEST(CASGCReadAhead, WorkerReadFaultFailsTheRoundAndTheNextRoundRecovers)
{
    auto backend = std::make_shared<WorkerReadFaultBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 8});
    populate(store);

    Gc gc(store, kGc);
    backend->armAgainstOtherThreads();
    EXPECT_ANY_THROW(gc.runRegularRound());
    EXPECT_TRUE(backend->fired()) << "no read-ahead worker ever issued a request";

    store->renewWatermarkOnce();
    const RoundReport recovered = gc.runRegularRound();
    EXPECT_TRUE(recovered.acquired_lease);
}

/// ============================ the fold's intake telemetry ============================

namespace
{

struct IntakeCapture
{
    std::map<String, UInt64> metrics;
    UInt64 duration_us = 0;
    size_t rows = 0;
};

/// One sink for both jobs: `markCutComplete` would replace a second one. Nothing between the
/// `fold_seal_read` row and the start of intake reads, so the marks bracket the intake's requests.
void captureIntake(Gc & gc, CountingBackend & backend, IntakeCapture & out)
{
    gc.setPhaseSink([&backend, &out](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_seal_read")
        {
            backend.mark("intake_begin");
        }
        else if (rec.phase == "fold_ref_intake")
        {
            backend.mark("intake_end");
            out.metrics = rec.metrics;
            out.duration_us = rec.duration_us;
            ++out.rows;
        }
    });
}

const std::vector<String> kFamilies{"ckpt", "reflog", "manifest"};
const std::vector<String> kFamilySuffixes{"reads_issued", "takes", "awaited", "wait_us", "apply_us"};

/// The key substring each family's objects carry in the layout.
String familyKeyPart(const String & family)
{
    if (family == "ckpt")
        return "_ckpt";
    if (family == "reflog")
        return "/_log/";
    return "/cas/manifests/";
}

UInt64 intakeMetric(const std::map<String, UInt64> & metrics, const String & key)
{
    const auto it = metrics.find(key);
    EXPECT_NE(it, metrics.end()) << "missing intake metric " << key;
    return it == metrics.end() ? 0 : it->second;
}

size_t intakeReadsContaining(const std::vector<JournalEntry> & journal, const String & part)
{
    size_t n = 0;
    bool inside = false;
    for (const JournalEntry & entry : journal)
    {
        if (entry.verb == JournalVerb::Mark)
            inside = entry.detail == "intake_begin";
        else if (inside && entry.verb == JournalVerb::Read && entry.keys.front().find(part) != String::npos)
            ++n;
    }
    return n;
}

PoolPtr openPopulated(const std::shared_ptr<CountingBackend> & backend, uint64_t concurrency)
{
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = concurrency});
    populate(store);
    return store;
}

}

TEST(CASGCReadAhead, IntakeCountersMatchTheJournalAtConcurrencyOne)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPopulated(backend, 1);
    Gc gc(store, kGc);
    IntakeCapture intake;
    captureIntake(gc, *backend, intake);
    backend->clearJournal();
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(intake.rows, 1u);

    for (const String & family : kFamilies)
        for (const String & suffix : kFamilySuffixes)
            ASSERT_TRUE(intake.metrics.contains(family + "_" + suffix)) << family << "_" << suffix;
    ASSERT_TRUE(intake.metrics.contains("cursor_resolves"));
    ASSERT_TRUE(intake.metrics.contains("cursor_resolve_us"));

    const std::vector<JournalEntry> journal = backend->journal();
    UInt64 timed_us = 0;
    for (const String & family : kFamilies)
    {
        SCOPED_TRACE(family);
        const size_t reads = intakeReadsContaining(journal, familyKeyPart(family));
        EXPECT_GT(reads, 0u);
        EXPECT_EQ(intakeMetric(intake.metrics, family + "_reads_issued"), reads);
        EXPECT_EQ(intakeMetric(intake.metrics, family + "_takes"), reads);
        EXPECT_EQ(intakeMetric(intake.metrics, family + "_awaited"), reads);
        timed_us += intakeMetric(intake.metrics, family + "_wait_us") + intakeMetric(intake.metrics, family + "_apply_us");
    }
    EXPECT_EQ(intakeMetric(intake.metrics, "cursor_resolves"), 0u);
    EXPECT_LE(timed_us, intake.duration_us);
}

TEST(CASGCReadAhead, ADelayedFamilyOwnsTheWait)
{
    constexpr UInt64 kDelayUs = 7;
    for (const String & delayed : kFamilies)
    {
        SCOPED_TRACE(delayed);
        auto backend = std::make_shared<ClockAdvancingBackend>();
        backend->disarm();
        auto store = openPopulated(backend, 1);
        Gc gc(store, kGc);
        gc.setIntakeClockForTest(&fakeClockNs);
        IntakeCapture intake;
        captureIntake(gc, *backend, intake);
        backend->delayKeysContaining(familyKeyPart(delayed), kDelayUs * 1000);
        backend->arm();
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        ASSERT_EQ(intake.rows, 1u);

        const UInt64 takes = intakeMetric(intake.metrics, delayed + "_takes");
        EXPECT_GT(takes, 0u);
        EXPECT_EQ(intakeMetric(intake.metrics, delayed + "_wait_us"), takes * kDelayUs);
        for (const String & other : kFamilies)
            if (other != delayed)
                EXPECT_EQ(intakeMetric(intake.metrics, other + "_wait_us"), 0u) << other;
    }
}

TEST(CASGCReadAhead, ColdCursorResolveIsCounted)
{
    constexpr UInt64 kDelayUs = 7;
    auto backend = std::make_shared<ClockAdvancingBackend>();
    backend->disarm();
    auto store = openPopulated(backend, 1);
    {
        Gc warm(store, kGc);
        ASSERT_TRUE(warm.runRegularRound().acquired_lease);
    }
    store->renewWatermarkOnce();

    Gc cold(store, kGc);
    cold.setIntakeClockForTest(&fakeClockNs);
    IntakeCapture intake;
    captureIntake(cold, *backend, intake);
    backend->clearJournal();
    backend->delayKeysContaining(familyKeyPart("reflog"), kDelayUs * 1000);
    backend->arm();
    ASSERT_TRUE(cold.runRegularRound().acquired_lease);
    ASSERT_EQ(intake.rows, 1u);

    const UInt64 resolves = intakeMetric(intake.metrics, "cursor_resolves");
    EXPECT_GE(resolves, 1u);
    EXPECT_LE(resolves, intakeMetric(intake.metrics, "tables_scanned"));
    const size_t reflog_reads = intakeReadsContaining(backend->journal(), familyKeyPart("reflog"));
    const UInt64 resolve_reads = reflog_reads - intakeMetric(intake.metrics, "reflog_reads_issued");
    EXPECT_GT(resolve_reads, 0u);
    EXPECT_EQ(intakeMetric(intake.metrics, "cursor_resolve_us"), resolve_reads * kDelayUs);
}

/// ============================ manifest read-ahead across logs ============================

namespace
{

bool isManifestKey(const String & key)
{
    return key.find("/cas/manifests/") != String::npos;
}

/// Observes and steers the fold's reads. A read made off the thread that built this backend is a read-ahead worker's.
/// Every wait is bounded, so a missing overlap fails an assertion instead of hanging the suite.
class ManifestReadAheadBackend : public CountingBackend
{
public:
    /// Holds worker manifest reads until `width` distinct keys are held at once. A wait that expires abandons the
    /// barrier for good.
    void armManifestBarrier(size_t width)
    {
        std::lock_guard lock(mutex);
        barrier_width = width;
    }

    bool manifestBarrierFilled() const
    {
        std::lock_guard lock(mutex);
        return barrier_filled;
    }

    /// Every worker read of `key` throws.
    void failWorkerReadsOf(String key)
    {
        std::lock_guard lock(mutex);
        fault_key = std::move(key);
    }

    size_t faultsServed() const
    {
        std::lock_guard lock(mutex);
        return faults;
    }

    /// Runs `callback` once, on the reading thread, right after a read of `key` returns; it is told whether the key
    /// was present.
    void afterFirstRead(String key, std::function<void(bool)> callback)
    {
        std::lock_guard lock(mutex);
        after_key = std::move(key);
        after_callback = std::move(callback);
    }

    bool afterFirstReadFired() const
    {
        std::lock_guard lock(mutex);
        return after_fired;
    }

    /// Holds every read of a key of `keys` until `releaseHeld`.
    void holdReadsOf(std::set<String> keys)
    {
        std::lock_guard lock(mutex);
        held_keys = std::move(keys);
        held_released = false;
    }

    void releaseHeld()
    {
        std::lock_guard lock(mutex);
        held_released = true;
        cv.notify_all();
    }

    /// Waits until every key of `keys` was read to completion, on any thread.
    bool waitUntilRead(const std::set<String> & keys)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&]
        {
            return std::all_of(keys.begin(), keys.end(), [&](const String & key) { return completed.contains(key); });
        });
    }

    /// Waits until every key of `keys` was read, by this thread or by a worker that has since started another read.
    /// A worker starts another read only after it published the result of the previous one, so the walk can then see
    /// each of these results without waiting.
    bool waitUntilVisible(const std::set<String> & keys)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&]
        {
            return std::all_of(keys.begin(), keys.end(), [&](const String & key)
            {
                const auto it = completed_by.find(key);
                return it != completed_by.end()
                    && (it->second.first == owner || reads_started.at(it->second.first) > it->second.second);
            });
        });
    }

    bool readCompleted(const String & key) const
    {
        std::lock_guard lock(mutex);
        return completed.contains(key);
    }

    size_t readsInFlight() const
    {
        std::lock_guard lock(mutex);
        return in_flight;
    }

    /// A `mono_ms_fn` that jumps by one intake slice right after the first read of the key given to
    /// `stepClockAfterRead`.
    std::function<uint64_t()> monoMsFn()
    {
        return [this] { return now_ms.load(); };
    }

    void stepClockAfterRead(String key)
    {
        std::lock_guard lock(mutex);
        step_key = std::move(key);
    }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        const bool worker = std::this_thread::get_id() != owner;
        {
            std::unique_lock lock(mutex);
            if (worker && key == fault_key)
            {
                ++faults;
                throw std::runtime_error("injected manifest read fault");
            }
            ++in_flight;
            ++reads_started[std::this_thread::get_id()];
            cv.notify_all();
            if (held_keys.contains(key))
                cv.wait_for(lock, std::chrono::seconds(10), [&] { return held_released; });
            if (worker && isManifestKey(key))
                holdForBarrier(lock, key);
        }
        std::optional<DB::Cas::Backend::Raw> got;
        try
        {
            got = CountingBackend::read(key, access);
        }
        catch (...)
        {
            std::lock_guard lock(mutex);
            --in_flight;
            cv.notify_all();
            throw;
        }
        std::function<void(bool)> fire;
        {
            std::lock_guard lock(mutex);
            if (after_callback && key == after_key)
            {
                fire = std::exchange(after_callback, nullptr);
                after_fired = true;
            }
            if (!step_key.empty() && key == step_key)
            {
                step_key.clear();
                now_ms.fetch_add(Gc::kRefIntakeBudgetMs);
            }
        }
        if (fire)
            fire(got.has_value());
        {
            std::lock_guard lock(mutex);
            --in_flight;
            completed.insert(key);
            completed_by[key] = {std::this_thread::get_id(), reads_started[std::this_thread::get_id()]};
            cv.notify_all();
        }
        return got;
    }

private:
    void holdForBarrier(std::unique_lock<std::mutex> & lock, const String & key)
    {
        if (barrier_width == 0 || barrier_filled || barrier_abandoned)
            return;
        barrier_held.insert(key);
        if (barrier_held.size() >= barrier_width)
        {
            barrier_filled = true;
            cv.notify_all();
            return;
        }
        if (!cv.wait_for(lock, std::chrono::seconds(10), [&] { return barrier_filled; }))
            barrier_abandoned = true;
    }

    const std::thread::id owner = std::this_thread::get_id();
    mutable std::mutex mutex;
    std::condition_variable cv;
    size_t in_flight = 0;
    std::set<String> completed;
    size_t barrier_width = 0;
    std::set<String> barrier_held;
    bool barrier_filled = false;
    bool barrier_abandoned = false;
    String fault_key;
    size_t faults = 0;
    String after_key;
    std::function<void(bool)> after_callback;
    bool after_fired = false;
    std::set<String> held_keys;
    bool held_released = false;
    /// Per thread, the reads it started; per key, the thread that completed it and that thread's count then.
    std::map<std::thread::id, size_t> reads_started;
    std::map<String, std::pair<std::thread::id, size_t>> completed_by;
    String step_key;
    std::atomic<uint64_t> now_ms{1'000'000};
};

using ManifestReadAheadHintHoleBackend = HintHoleBackendOn<ManifestReadAheadBackend>;

const RootNamespace kLife{"00/aa@cas@"};

template <typename BackendT>
PoolPtr openAtWidth(const std::shared_ptr<BackendT> & backend, uint64_t width)
{
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                          .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = width});
}

ManifestRef manifestRefOf(uint64_t epoch, uint64_t build)
{
    return ManifestRef{.writer_epoch = epoch, .build_sequence = build, .manifest_ordinal = 1};
}

/// The manifest `publishAt(..., {epoch, i}, ..., build, ...)` writes.
String manifestKeyOf(const Layout & layout, uint64_t epoch, uint64_t build)
{
    return layout.manifestKey(ManifestId{kLife, manifestRefOf(epoch, build)});
}

String refLogKeyOf(const Layout & layout, const RefTxnId & id)
{
    return layout.refLogKey(fixture::fixtureLife(kLife), id);
}

std::set<String> refLogKeysOf(const Layout & layout, uint64_t first, uint64_t last)
{
    std::set<String> keys;
    for (uint64_t i = first; i <= last; ++i)
        keys.insert(refLogKeyOf(layout, RefTxnId{1, i}));
    return keys;
}

/// Publishes `{1,first} .. {1,last}`; record `{1,i}` names its own manifest `{1, i}` and blob `i`.
void publishRecords(Backend & backend, const Layout & layout, uint64_t first, uint64_t last)
{
    for (uint64_t i = first; i <= last; ++i)
        publishAt(backend, layout, kLife, RefTxnId{1, i}, fmt::format("ref_{}", i), i, DB::UInt128(i), /*birth=*/i == 1);
}

/// A live life whose records `{1,1}` and `{1,2}` a first round of `gc` folds.
void foldFirstTwo(Backend & backend, const Layout & layout, Gc & gc)
{
    fixture::admitLive(backend, layout, kLife);
    publishRecords(backend, layout, 1, 2);
    writeRecoverableCkptForRawFixture(backend, layout, kLife, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
}

std::optional<RefCoverage> lifeCoverage(Backend & backend, const Layout & layout)
{
    const uint64_t gen = currentGenerationOf(backend, layout);
    const uint64_t attempt = currentAttemptOf(backend, layout);
    const UInt128 life_id = catalogLifeIdForTest(backend, layout, kLife);
    for (uint64_t g = gen; ; --g)
    {
        OperationForTest op(backend);
        if (const auto got = (*op).read(layout.foldSealKey(g, attempt), Retry::once()))
        {
            const CasFoldSeal seal = decodeFoldSeal(got->bytes);
            const auto it = seal.ref_lives.find(life_id);
            if (it == seal.ref_lives.end())
                return std::nullopt;
            return it->second.coverage;
        }
        if (g == 0)
            return std::nullopt;
    }
}

RefTxnId lifeCursor(Backend & backend, const Layout & layout)
{
    const auto coverage = lifeCoverage(backend, layout);
    return coverage ? coverage->last_folded_ref_id : RefTxnId{};
}

std::optional<std::pair<HoldReason, RefTxnId>> lifeHold(Backend & backend, const Layout & layout)
{
    const auto coverage = lifeCoverage(backend, layout);
    if (!coverage || !coverage->hold)
        return std::nullopt;
    return std::make_pair(coverage->hold->reason, coverage->hold->offending_position);
}

/// Requests of `verb` on `key` inside the ref intakes `runRoundReadingRows` marked.
size_t intakeRequestsOf(const CountingBackend & backend, JournalVerb verb, const String & key)
{
    size_t n = 0;
    bool inside = false;
    for (const JournalEntry & entry : backend.journal())
    {
        if (entry.verb == JournalVerb::Mark && (entry.detail == "fold_seal_read" || entry.detail == "fold_ref_intake"))
            inside = entry.detail == "fold_seal_read";
        else if (entry.verb != JournalVerb::Mark)
            n += inside && entry.verb == verb && entry.keys.front() == key;
    }
    return n;
}

size_t intakeReadsOf(const CountingBackend & backend, const String & key)
{
    return intakeRequestsOf(backend, JournalVerb::Read, key);
}

UInt64 rowMetric(const std::map<String, UInt64> & metrics, const String & key)
{
    const auto it = metrics.find(key);
    EXPECT_NE(it, metrics.end()) << "missing metric " << key;
    return it == metrics.end() ? 0 : it->second;
}

struct RoundRows
{
    RoundReport report;
    std::map<String, UInt64> intake;
    std::map<String, UInt64> reduce;
};

/// One round of `gc`. Marks the end of each phase in the journal of `backend`, so the requests between the
/// `fold_seal_read` and `fold_ref_intake` marks are the ref intake's.
RoundRows runRoundReadingRows(Gc & gc, CountingBackend & backend)
{
    RoundRows rows;
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        backend.mark(record.phase);
        if (record.phase == "fold_ref_intake")
            rows.intake = record.metrics;
        else if (record.phase == "fold_reduce")
            rows.reduce = record.metrics;
    });
    SCOPE_EXIT({ gc.setPhaseSink({}); });
    rows.report = gc.runRegularRound();
    EXPECT_TRUE(rows.report.acquired_lease);
    return rows;
}

/// Whether thread `tid` of this process is sleeping, from its `/proc` stat line.
bool threadSleeps(uint64_t tid)
{
    std::ifstream stat(fmt::format("/proc/self/task/{}/stat", tid));
    std::string line;
    if (!std::getline(stat, line))
        return false;
    const size_t name_end = line.rfind(')');
    return name_end != std::string::npos && name_end + 2 < line.size() && line[name_end + 2] == 'S';
}

/// Installs a walk decode probe for its lifetime.
class ScopedWalkDecodeProbe
{
public:
    explicit ScopedWalkDecodeProbe(std::function<void()> probe) { setWalkRecordDecodeProbeForTest(std::move(probe)); }
    ~ScopedWalkDecodeProbe() { setWalkRecordDecodeProbeForTest({}); }

    ScopedWalkDecodeProbe(const ScopedWalkDecodeProbe &) = delete;
    ScopedWalkDecodeProbe & operator=(const ScopedWalkDecodeProbe &) = delete;
};

/// Holds the store reads of `later` now, and returns a probe for the walk's first decode that makes the results of
/// `needed` visible to the walk before it looks past its current record. The probe waits until every key of `needed`
/// is visible (see `waitUntilVisible`), releases `later` and waits until those were read too; `ready` records whether
/// both happened. `later` must be hinted after `needed` and hold at least as many keys as there are workers: a worker
/// done with a key of `needed` then always finds a key of `later` to start, and blocks on it. The probe runs a copy of
/// the function, so the call count lives outside it.
std::function<void()> readyAtFirstDecode(ManifestReadAheadBackend & backend, std::set<String> needed, std::set<String> later,
                                         bool & ready)
{
    backend.holdReadsOf(later);
    return [&backend, wanted = std::move(needed), held = std::move(later), &ready, calls = std::make_shared<size_t>(0)]
    {
        if (++*calls != 1)
            return;
        const bool visible = backend.waitUntilVisible(wanted);
        backend.releaseHeld();
        ready = visible && backend.waitUntilRead(held);
    };
}

/// A seal at `id` whose body names `next_epoch` as the epoch the chain continues in.
void writeSealNaming(Backend & backend, const Layout & layout, const RefTxnId & id, uint64_t next_epoch)
{
    RefLogTxn txn;
    txn.ns = kLife.string();
    txn.txn_id = id;
    txn.ops = {epochSealOp()};
    txn.next_epoch = next_epoch;
    writeRefLogTxnRaw(backend, layout, txn);
}

}

/// Single-edge logs of different records have their manifest reads in flight together, within what planning observed.
/// The walk applies what it applies at width 1. No manifest of a record past a seal, a terminal record or a head with
/// a bad backlink is read.
TEST(CASGCReadAhead, SingleEdgeLogsOverlapWithinPlan)
{
    constexpr uint64_t tail = 60;
    struct Outcome
    {
        RefTxnId cursor;
        std::vector<int64_t> degrees;
        bool filled = false;
        bool refs_ready = false;
        UInt64 manifest_issued = 0;
        UInt64 manifest_takes = 0;
    };
    const auto run = [&](uint64_t width)
    {
        Outcome out;
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, width);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        foldFirstTwo(*backend, layout, gc);
        publishRecords(*backend, layout, 3, tail);

        std::optional<ScopedWalkDecodeProbe> probe;
        ScopedRoundCallback arm(gc, RoundInterleavePoint::AfterDiscovery, {}, [&]
        {
            if (width == 1)
                return;
            backend->armManifestBarrier(9);
            probe.emplace(readyAtFirstDecode(*backend, refLogKeysOf(layout, 4, 30), refLogKeysOf(layout, 31, tail - 1),
                                             out.refs_ready));
        });
        const RoundRows rows = runRoundReadingRows(gc, *backend);
        probe.reset();
        EXPECT_EQ(arm.fired(), 1u);
        out.filled = backend->manifestBarrierFilled();
        out.cursor = lifeCursor(*backend, layout);
        for (uint64_t i = 3; i <= tail; ++i)
            out.degrees.push_back(inDegreeOf(*backend, layout, DB::UInt128(i)));
        out.manifest_issued = rowMetric(rows.intake, "manifest_reads_issued");
        out.manifest_takes = rowMetric(rows.intake, "manifest_takes");
        return out;
    };
    const Outcome sequential = run(1);
    const Outcome wide = run(16);
    EXPECT_TRUE(wide.refs_ready) << "the ref logs were never read ahead";
    EXPECT_TRUE(wide.filled) << "nine distinct manifest reads were never in flight at once";
    EXPECT_EQ(wide.cursor, sequential.cursor);
    EXPECT_EQ(wide.degrees, sequential.degrees);
    EXPECT_EQ(sequential.cursor, (RefTxnId{1, tail}));
    EXPECT_EQ(sequential.degrees, std::vector<int64_t>(tail - 2, 1));
    EXPECT_EQ(wide.manifest_issued, wide.manifest_takes) << "a manifest was read ahead and never taken";

    const auto stray_reads = [&](const std::function<void(ManifestReadAheadBackend &, const Layout &)> & seed,
                                 const std::function<std::vector<String>(const Layout &)> & strays,
                                 const RefTxnId & stray_record)
    {
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        foldFirstTwo(*backend, layout, gc);
        publishRecords(*backend, layout, 3, 10);
        seed(*backend, layout);
        backend->clearJournal();
        runRoundReadingRows(gc, *backend);
        EXPECT_GT(intakeReadsOf(*backend, refLogKeyOf(layout, stray_record)), 0u) << "positive control: the stray was read";
        size_t n = 0;
        for (const String & key : strays(layout))
            n += intakeReadsOf(*backend, key);
        return std::make_pair(n, lifeHold(*backend, layout));
    };
    {
        SCOPED_TRACE("past a seal");
        const auto [reads, hold] = stray_reads([](ManifestReadAheadBackend & backend, const Layout & layout)
        {
            writeSealNaming(backend, layout, RefTxnId{1, 11}, 2);
            publishAt(backend, layout, kLife, RefTxnId{2, 1}, "ref_e2", 2001, DB::UInt128(2001),
                      /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 11});
            publishAt(backend, layout, kLife, RefTxnId{1, 12}, "ref_stray", 1012, DB::UInt128(1012));
        }, [](const Layout & layout) { return std::vector<String>{manifestKeyOf(layout, 1, 1012)}; }, RefTxnId{1, 12});
        (void)hold;
        EXPECT_EQ(reads, 0u);
    }
    {
        SCOPED_TRACE("past a terminal record");
        const auto [reads, hold] = stray_reads([](ManifestReadAheadBackend & backend, const Layout & layout)
        {
            RefOp remove_op;
            remove_op.kind = RefOpKind::RemoveNamespace;
            writeTxnAt(backend, layout, kLife, RefTxnId{1, 11}, {remove_op});
            publishAt(backend, layout, kLife, RefTxnId{1, 12}, "ref_stray", 1012, DB::UInt128(1012));
        }, [](const Layout & layout) { return std::vector<String>{manifestKeyOf(layout, 1, 1012)}; }, RefTxnId{1, 12});
        (void)hold;
        EXPECT_EQ(reads, 0u);
    }
    {
        SCOPED_TRACE("past a bad backlink at the named head");
        const auto [reads, hold] = stray_reads([](ManifestReadAheadBackend & backend, const Layout & layout)
        {
            writeSealNaming(backend, layout, RefTxnId{1, 11}, 2);
            publishAt(backend, layout, kLife, RefTxnId{2, 1}, "ref_e2", 2001, DB::UInt128(2001),
                      /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 10});
            publishAt(backend, layout, kLife, RefTxnId{2, 2}, "ref_e2_2", 2002, DB::UInt128(2002));
            publishAt(backend, layout, kLife, RefTxnId{2, 3}, "ref_e2_3", 2003, DB::UInt128(2003));
        }, [](const Layout & layout)
        {
            return std::vector<String>{manifestKeyOf(layout, 2, 2001), manifestKeyOf(layout, 2, 2002),
                                       manifestKeyOf(layout, 2, 2003)};
        }, RefTxnId{2, 2});
        ASSERT_TRUE(hold.has_value()) << "positive control: the bad backlink holds";
        EXPECT_EQ(hold->second, (RefTxnId{2, 1}));
        EXPECT_EQ(reads, 0u);
    }
}

/// Pass 2 starts with the whole ref window read and not yet taken. Manifests of later records still go out: sixteen
/// distinct manifest reads are in flight at once.
TEST(CASGCReadAhead, ManifestsAreAdmittedWhileTheRefWindowIsFull)
{
    constexpr uint64_t width = 16;
    constexpr uint64_t window = 4 * width;
    constexpr uint64_t tail = 200;
    /// Pass 1 takes `window` records from `{1,3}`.
    constexpr uint64_t first_of_pass_two = 3 + window;
    auto backend = std::make_shared<ManifestReadAheadBackend>();
    auto store = openAtWidth(backend, width);
    const Layout & layout = store->layout();
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
    publishRecords(*backend, layout, 3, tail);

    bool refs_ready = false;
    std::optional<ScopedWalkDecodeProbe> probe;
    ScopedRoundCallback arm(gc, RoundInterleavePoint::AfterNamespaceWalk,
                            RoundInterleaveSelector{.first = kLife.string(), .second = kLife.string(), .pass = 1}, [&]
    {
        backend->armManifestBarrier(16);
        /// The window pass 2 hints before its first take is the `window` records right after it: the first 43 are made
        /// visible, the other 21 held until then, and all are read before the walk looks ahead.
        probe.emplace(readyAtFirstDecode(*backend, refLogKeysOf(layout, first_of_pass_two + 1, first_of_pass_two + 43),
                                         refLogKeysOf(layout, first_of_pass_two + 44, first_of_pass_two + window), refs_ready));
    });
    const RoundRows rows = runRoundReadingRows(gc, *backend);
    probe.reset();
    ASSERT_EQ(arm.fired(), 1u) << "the walk did not reach pass 2";
    EXPECT_TRUE(refs_ready) << "the ref window was not read before the first take of pass 2";
    EXPECT_TRUE(backend->manifestBarrierFilled()) << "sixteen distinct manifest reads were never in flight at once";
    EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
    EXPECT_EQ(rowMetric(rows.intake, "frontier_proven"), rowMetric(rows.intake, "frontier_namespaces"));
    for (uint64_t i = 3; i <= tail; ++i)
        ASSERT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1) << "record " << i << " was not applied";
}

namespace
{

/// At width 16 pass 1 takes 64 records from `{1,3}`, and pass 2 starts at `{1,67}`. The tests below hold the walk in
/// pass 2, which a held life does not leave in the same round.
constexpr uint64_t kFirstOfPassTwo = 3 + 4 * 16;

/// Calls `make_probe` when pass 1 of `kLife` ends and installs the probe it returns, so it runs at the first decode of
/// pass 2.
class PassTwoProbe
{
public:
    PassTwoProbe(Gc & gc, std::function<std::function<void()>()> make_probe)
        : arm(gc, RoundInterleavePoint::AfterNamespaceWalk,
              RoundInterleaveSelector{.first = kLife.string(), .second = kLife.string(), .pass = 1},
              [this, make = std::move(make_probe)] { probe.emplace(make()); })
    {
    }

    size_t fired() const { return arm.fired(); }

private:
    std::optional<ScopedWalkDecodeProbe> probe;
    ScopedRoundCallback arm;
};

/// Record `{1,at}` names two manifests, the second one absent, so the walk holds there with `ManifestBodyMissing`.
/// Blob 1010 is the first manifest's.
void writeHoldingRecord(Backend & backend, const Layout & layout, uint64_t at)
{
    writeBlobBody(backend, layout, DB::UInt128(1010));
    writeManifestRaw(backend, layout, kLife, manifestRefOf(1, 1010), {blobEntryFor("data.bin", DB::UInt128(1010))});
    writeBlobBody(backend, layout, DB::UInt128(1011));
    std::vector<RefOp> ops = publishCommittedOps("held_a", manifestRefOf(1, 1010));
    for (const RefOp & op : publishCommittedOps("held_b", manifestRefOf(1, 1011)))
        ops.push_back(op);
    writeTxnAt(backend, layout, kLife, RefTxnId{1, at}, std::move(ops));
}

void publishMissingManifestOfTheHold(Backend & backend, const Layout & layout)
{
    writeManifestRaw(backend, layout, kLife, manifestRefOf(1, 1011), {blobEntryFor("data.bin", DB::UInt128(1011))});
}

/// Record `{1,i}` as `publishRecords` writes it, without its manifest.
void publishRecordWithoutManifest(Backend & backend, const Layout & layout, uint64_t i)
{
    writeBlobBody(backend, layout, DB::UInt128(i));
    writeTxnAt(backend, layout, kLife, RefTxnId{1, i}, publishCommittedOps(fmt::format("ref_{}", i), manifestRefOf(1, i)));
}

void publishManifestOf(Backend & backend, const Layout & layout, uint64_t i)
{
    writeManifestRaw(backend, layout, kLife, manifestRefOf(1, i), {blobEntryFor("data.bin", DB::UInt128(i))});
}

}

/// Behind an earlier hold, what is read ahead decides nothing: a malformed record holds only when the walk reaches it,
/// a manifest read absent before it is published is read again by the next round, and a worker fault on a manifest is
/// dropped. A log the walk holds in the middle of applies none of its edges.
TEST(CASGCReadAhead, OrderedTakeOwnsTheOutcome)
{
    constexpr uint64_t tail = 100;
    constexpr uint64_t held = kFirstOfPassTwo + 3;
    constexpr uint64_t malformed = held + 10;
    constexpr uint64_t late = held + 5;
    enum class Case : uint8_t
    {
        MalformedRecord,
        LatePublishedManifest,
        ManifestWorkerFault,
    };
    for (const Case c : {Case::MalformedRecord, Case::LatePublishedManifest, Case::ManifestWorkerFault})
    {
        SCOPED_TRACE(static_cast<int>(c));
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        for (uint64_t i = 3; i <= tail; ++i)
        {
            if (i == held)
                writeHoldingRecord(*backend, layout, held);
            else if (c == Case::MalformedRecord && i == malformed)
            {
                OperationForTest op(*backend);
                (*op).create(refLogKeyOf(layout, RefTxnId{1, i}), "this is not a cas_ref_log object", Retry::once());
            }
            else if (c == Case::LatePublishedManifest && i == late)
                publishRecordWithoutManifest(*backend, layout, i);
            else
                publishRecords(*backend, layout, i, i);
        }
        const String late_key = manifestKeyOf(layout, 1, late);
        if (c == Case::LatePublishedManifest)
            backend->afterFirstRead(late_key, [&](bool present)
            {
                if (!present)
                    publishManifestOf(*backend, layout, late);
            });
        if (c == Case::ManifestWorkerFault)
            backend->failWorkerReadsOf(late_key);

        backend->clearJournal();
        bool refs_ready = false;
        {
            PassTwoProbe probe(gc, [&]
            {
                return readyAtFirstDecode(*backend, refLogKeysOf(layout, kFirstOfPassTwo + 1, malformed),
                                          refLogKeysOf(layout, malformed + 1, tail - 1), refs_ready);
            });
            runRoundReadingRows(gc, *backend);
            EXPECT_EQ(probe.fired(), 1u);
        }
        EXPECT_TRUE(refs_ready);
        EXPECT_EQ(lifeHold(*backend, layout), std::make_pair(HoldReason::ManifestBodyMissing, RefTxnId{1, held}));
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, held - 1}));
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1010)), 0) << "the held log applied an edge";
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(late)), 0);

        switch (c)
        {
            case Case::MalformedRecord:
            {
                EXPECT_EQ(intakeReadsOf(*backend, manifestKeyOf(layout, 1, malformed - 1)), 1u)
                    << "positive control: the manifest of the record before the malformed one was read ahead";
                for (uint64_t i = malformed + 1; i <= tail; ++i)
                    EXPECT_EQ(intakeReadsOf(*backend, manifestKeyOf(layout, 1, i)), 0u) << "a manifest past the malformed record";
                publishMissingManifestOfTheHold(*backend, layout);
                runRoundReadingRows(gc, *backend);
                EXPECT_EQ(lifeHold(*backend, layout), std::make_pair(HoldReason::BodyUndecodable, RefTxnId{1, malformed}));
                EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, malformed - 1}));
                break;
            }
            case Case::LatePublishedManifest:
            {
                EXPECT_TRUE(backend->afterFirstReadFired()) << "the manifest was not read ahead behind the hold";
                publishMissingManifestOfTheHold(*backend, layout);
                runRoundReadingRows(gc, *backend);
                EXPECT_EQ(lifeHold(*backend, layout), std::nullopt);
                EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
                EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(late)), 1);
                EXPECT_EQ(intakeReadsOf(*backend, late_key), 2u) << "the next round did not read the manifest again";
                break;
            }
            case Case::ManifestWorkerFault:
            {
                EXPECT_GT(backend->faultsServed(), 0u) << "the manifest was not read ahead behind the hold";
                break;
            }
        }
    }
}

/// Two edges naming one manifest each get their own read: the body removed after the first read is absent for the
/// second, which holds. Once both edges are ahead of the walk when it first looks ahead; once the first is the current
/// log's there, at the first record of pass 2, and the second is the record after it.
TEST(CASGCReadAhead, ARepeatedManifestIdIsObservedOncePerEdge)
{
    constexpr uint64_t tail = 100;
    constexpr uint64_t last_remover = kFirstOfPassTwo + 13;
    for (const uint64_t width : {1u, 16u})
    {
        for (const auto & [adder, remover] : {std::pair{kFirstOfPassTwo + 5, last_remover},
                                              std::pair{kFirstOfPassTwo, kFirstOfPassTwo + 1}})
        {
            SCOPED_TRACE(fmt::format("width {}, added at {}, removed at {}", width, adder, remover));
            auto backend = std::make_shared<ManifestReadAheadBackend>();
            auto store = openAtWidth(backend, width);
            const Layout & layout = store->layout();
            Gc gc(store, kGc);
            ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
            const ManifestRef shared = manifestRefOf(1, 500);
            for (uint64_t i = 3; i <= tail; ++i)
            {
                if (i == adder)
                    publishAt(*backend, layout, kLife, RefTxnId{1, i}, "shared", 500, DB::UInt128(500));
                else if (i == remover)
                    writeTxnAt(*backend, layout, kLife, RefTxnId{1, i},
                               {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "shared", shared}, std::nullopt)});
                else
                    publishRecords(*backend, layout, i, i);
            }
            const String key = layout.manifestKey(ManifestId{kLife, shared});
            backend->afterFirstRead(key, [&](bool present)
            {
                if (present)
                    deleteManifestBody(*backend, layout, ManifestId{kLife, shared});
            });

            backend->clearJournal();
            bool refs_ready = false;
            {
                std::optional<PassTwoProbe> probe;
                if (width > 1)
                    probe.emplace(gc, [&]
                    {
                        return readyAtFirstDecode(*backend, refLogKeysOf(layout, kFirstOfPassTwo + 1, last_remover),
                                                  refLogKeysOf(layout, last_remover + 1, tail - 1), refs_ready);
                    });
                runRoundReadingRows(gc, *backend);
            }
            EXPECT_TRUE(width == 1 || refs_ready);
            EXPECT_TRUE(backend->afterFirstReadFired());
            EXPECT_EQ(intakeReadsOf(*backend, key), 2u);
            EXPECT_EQ(lifeHold(*backend, layout), std::make_pair(HoldReason::ManifestBodyMissing, RefTxnId{1, remover}));
            EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, remover - 1}));
        }
    }
}

/// A manifest the query cache holds is still read by its exact `GET`, and never by a `HEAD`. The cleanup of a dropped
/// owner's manifest after the round is the same at width 1 and 16.
TEST(CASGCReadAhead, AWarmManifestStillGetsAnExactGet)
{
    {
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, 10);
        const ManifestId warm{kLife, manifestRefOf(1, 5)};
        ASSERT_NE(store->readManifestShared(warm), nullptr);
        backend->clearJournal();
        runRoundReadingRows(gc, *backend);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, 10}));
        EXPECT_EQ(intakeReadsOf(*backend, layout.manifestKey(warm)), 1u);
        EXPECT_EQ(intakeRequestsOf(*backend, JournalVerb::Head, layout.manifestKey(warm)), 0u);
    }

    const auto deleted_manifests = [&](uint64_t width)
    {
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, width);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        foldFirstTwo(*backend, layout, gc);
        publishRecords(*backend, layout, 3, 10);
        writeTxnAt(*backend, layout, kLife, RefTxnId{1, 11},
                   {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "ref_5", manifestRefOf(1, 5)}, std::nullopt)});
        backend->clearJournal();
        for (size_t round = 0; round < 3; ++round)
        {
            runRegularRoundReclaiming(gc);
            store->renewWatermarkOnce();
        }
        std::set<String> deleted;
        for (const JournalEntry & entry : backend->journal())
            if (entry.verb == JournalVerb::Remove || entry.verb == JournalVerb::RemoveMany)
                for (const String & key : entry.keys)
                    if (isManifestKey(key))
                        deleted.insert(key);
        EXPECT_TRUE(deleted.contains(manifestKeyOf(layout, 1, 5))) << "positive control: the dropped manifest is removed";
        return deleted;
    };
    EXPECT_EQ(deleted_manifests(1), deleted_manifests(16));
}

/// A future manifest hint the pool refuses fails the round at once. The reads already sent are awaited before the
/// round returns, and nothing is adopted.
TEST(CASGCReadAhead, SubmissionRefusalPropagatesAndTeardownDrains)
{
    constexpr uint64_t tail = 40;
    auto backend = std::make_shared<ManifestReadAheadHintHoleBackend>();
    auto store = openAtWidth(backend, 16);
    const Layout & layout = store->layout();
    Gc gc(store, kGc);

    /// `{1,2}` adds a precommit and `{1,3}` promotes it, so the walk's first record names no manifest. `{1,4}` is the
    /// last record listed, so planning keeps its body and the walk at `{1,3}` finds it without waiting.
    fixture::admitLive(*backend, layout, kLife);
    publishRecords(*backend, layout, 1, 1);
    const ManifestRef promoted = manifestRefOf(1, 700);
    writeBlobBody(*backend, layout, DB::UInt128(700));
    writeManifestRaw(*backend, layout, kLife, promoted, {blobEntryFor("data.bin", DB::UInt128(700))});
    writeTxnAt(*backend, layout, kLife, RefTxnId{1, 2},
               {ownerTransitionOp(std::nullopt, RefOwnerBinding{RefOwnerKind::Precommit, "promoted", promoted})});
    writeRecoverableCkptForRawFixture(*backend, layout, kLife, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, 2}));
    writeTxnAt(*backend, layout, kLife, RefTxnId{1, 3},
               {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Precommit, "promoted", promoted},
                                  RefOwnerBinding{RefOwnerKind::Committed, "promoted", promoted})});
    publishRecords(*backend, layout, 4, tail);
    for (uint64_t i = 5; i <= tail; ++i)
        backend->hide(refLogKeyOf(layout, RefTxnId{1, i}));

    /// Not a body planning keeps: planning probes `{1,4}`, then `{1,5}, {1,6}, {1,8}, ...` and bisects below `{1,68}`.
    const String outstanding = refLogKeyOf(layout, RefTxnId{1, tail - 1});
    backend->holdReadsOf({outstanding});
    const uint64_t generation = currentGenerationOf(*backend, layout);
    bool decoded_the_next_record = false;
    SCOPE_EXIT({
        CannotAllocateThreadFaultInjector::setFaultProbability(0);
        backend->releaseHeld();
    });
    int code = 0;
    bool returned_while_held = true;
    {
        size_t calls = 0;
        std::atomic<uint64_t> round_tid{0};
        std::atomic<bool> round_returned{false};
        ScopedWalkDecodeProbe probe([&]
        {
            ++calls;
            if (calls == 1)
            {
                /// From here every submission is refused; the next one is the first record's look past itself.
                CannotAllocateThreadFaultInjector::setFaultProbability(1.0);
                round_tid = getThreadId();
            }
            else
                decoded_the_next_record = true;
        });
        std::thread round([&]
        {
            try
            {
                gc.runRegularRound();
            }
            catch (const DB::Exception & e)
            {
                code = e.code();
            }
            round_returned = true;
        });
        /// The read stays held until the round, after the refusal, returned or went to sleep: in the join, unless
        /// something else made it wait first.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && !round_returned
               && (round_tid == 0 || !threadSleeps(round_tid)))
            std::this_thread::yield();
        returned_while_held = round_returned;
        backend->releaseHeld();
        round.join();
    }
    CannotAllocateThreadFaultInjector::setFaultProbability(0);
    EXPECT_GT(backend->holesServed(), 0u);
    EXPECT_EQ(code, DB::ErrorCodes::CANNOT_SCHEDULE_TASK);
    EXPECT_FALSE(decoded_the_next_record) << "the refusal did not fail the round where the future manifest was hinted";
    EXPECT_FALSE(returned_while_held) << "the round returned while a read it had sent was outstanding";
    EXPECT_TRUE(backend->readCompleted(outstanding)) << "the round returned before an outstanding read was answered";
    EXPECT_EQ(backend->readsInFlight(), 0u);
    EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, 2}));
    EXPECT_EQ(currentGenerationOf(*backend, layout), generation) << "the failed round adopted a generation";
}

/// Manifests read ahead belong to the visit that read them. A stop request or the end of the slice leaves them unused
/// and the next round reads them again; the last turn of pass 1 reads none; a log with more distinct manifests than
/// the window still has all of them applied; a worker fault nobody takes is dropped, and a bad manifest the walk
/// reaches fails the round.
TEST(CASGCReadAhead, FutureManifestWorkEndsWithTheVisit)
{
    const auto expect_prefix_applied = [](ManifestReadAheadBackend & backend, const Layout & layout, uint64_t last,
                                          uint64_t reached)
    {
        for (uint64_t i = 3; i <= last; ++i)
            EXPECT_EQ(inDegreeOf(backend, layout, DB::UInt128(i)), i <= reached ? 1 : 0) << "record " << i;
    };
    {
        SCOPED_TRACE("stop request");
        constexpr uint64_t tail = 60;
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, tail);
        std::atomic<bool> stop{false};
        gc.setStopSignal([&stop] { return stop.load(); });
        backend->afterFirstRead(manifestKeyOf(layout, 1, 50), [&](bool) { stop = true; });
        RoundRows stopped;
        bool refs_ready = false;
        {
            ScopedWalkDecodeProbe probe(
                readyAtFirstDecode(*backend, refLogKeysOf(layout, 4, 20), refLogKeysOf(layout, 21, tail - 1), refs_ready));
            stopped = runRoundReadingRows(gc, *backend);
        }
        gc.setStopSignal({});
        EXPECT_TRUE(backend->afterFirstReadFired());
        EXPECT_EQ(backend->readsInFlight(), 0u);
        EXPECT_EQ(rowMetric(stopped.reduce, "suppress_destructive"), 1u);
        const RefTxnId reached = lifeCursor(*backend, layout);
        EXPECT_LE(reached.ref_sequence, 50u);
        expect_prefix_applied(*backend, layout, tail, reached.ref_sequence);

        backend->clearJournal();
        runRoundReadingRows(gc, *backend);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
        expect_prefix_applied(*backend, layout, tail, tail);
        for (uint64_t i = reached.ref_sequence + 1; i <= tail; ++i)
            EXPECT_EQ(intakeReadsOf(*backend, manifestKeyOf(layout, 1, i)), 1u) << "manifest " << i;
    }
    {
        SCOPED_TRACE("slice");
        constexpr uint64_t tail = 120;
        constexpr uint64_t cut_at = 90;
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc, {}, backend->monoMsFn());
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, tail);
        backend->stepClockAfterRead(manifestKeyOf(layout, 1, cut_at));
        const RoundRows cut = runRoundReadingRows(gc, *backend);
        EXPECT_EQ(backend->readsInFlight(), 0u);
        EXPECT_EQ(rowMetric(cut.intake, "frontier_proven"), 0u);
        EXPECT_EQ(rowMetric(cut.reduce, "frontier_complete"), 0u);
        const RefTxnId reached = lifeCursor(*backend, layout);
        EXPECT_GE(reached.ref_sequence, 2 + 64u) << "pass 1 is never cut";
        EXPECT_LE(reached.ref_sequence, cut_at);
        expect_prefix_applied(*backend, layout, tail, reached.ref_sequence);

        backend->clearJournal();
        runRoundReadingRows(gc, *backend);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
        expect_prefix_applied(*backend, layout, tail, tail);
        for (uint64_t i = reached.ref_sequence + 1; i <= tail; ++i)
            EXPECT_EQ(intakeReadsOf(*backend, manifestKeyOf(layout, 1, i)), 1u) << "manifest " << i;
    }
    {
        SCOPED_TRACE("last turn of pass 1");
        /// With nothing listed, planning probes `{1,3}, {1,4}, {1,5}, {1,7}, ...`: `{1,67}`, the first record of pass 2,
        /// is a body it keeps, which the last turn of pass 1 must not look into.
        constexpr uint64_t tail = 150;
        auto backend = std::make_shared<ManifestReadAheadHintHoleBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, tail);
        for (uint64_t i = 3; i <= tail; ++i)
            backend->hide(refLogKeyOf(layout, RefTxnId{1, i}));
        backend->clearJournal();
        const RoundRows rows = runRoundReadingRows(gc, *backend);
        EXPECT_GT(backend->holesServed(), 0u);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
        EXPECT_EQ(rowMetric(rows.intake, "manifest_reads_issued"), rowMetric(rows.intake, "manifest_takes"));
        for (uint64_t i = 3; i <= tail; ++i)
            ASSERT_EQ(intakeReadsOf(*backend, manifestKeyOf(layout, 1, i)), 1u) << "manifest " << i << " was read more than once";
    }
    {
        SCOPED_TRACE("more distinct manifests in one log than the window");
        /// Width 2: the window is 8, and pass 1 takes `{1,3} .. {1,10}`. `{1,11}` holds on its absent manifest;
        /// `{1,12}` names 20 manifests. It is the last record listed, so planning keeps its body and the walk at
        /// `{1,11}` finds it without waiting.
        constexpr uint64_t width = 2;
        constexpr uint64_t edges = 20;
        constexpr uint64_t held = 11;
        constexpr uint64_t wide = 12;
        constexpr uint64_t tail = 20;
        auto backend = std::make_shared<ManifestReadAheadHintHoleBackend>();
        auto store = openAtWidth(backend, width);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, held - 1);
        publishRecordWithoutManifest(*backend, layout, held);
        std::vector<RefOp> ops;
        std::vector<String> wide_keys;
        for (uint64_t e = 0; e < edges; ++e)
        {
            const uint64_t build = 4000 + e;
            writeBlobBody(*backend, layout, DB::UInt128(build));
            writeManifestRaw(*backend, layout, kLife, manifestRefOf(1, build), {blobEntryFor("data.bin", DB::UInt128(build))});
            for (const RefOp & op : publishCommittedOps(fmt::format("wide_{}", e), manifestRefOf(1, build)))
                ops.push_back(op);
            wide_keys.push_back(manifestKeyOf(layout, 1, build));
        }
        writeTxnAt(*backend, layout, kLife, RefTxnId{1, wide}, std::move(ops));
        publishRecords(*backend, layout, wide + 1, tail);
        for (uint64_t i = wide + 1; i <= tail; ++i)
            backend->hide(refLogKeyOf(layout, RefTxnId{1, i}));

        backend->clearJournal();
        runRoundReadingRows(gc, *backend);
        EXPECT_GT(backend->holesServed(), 0u);
        EXPECT_EQ(lifeHold(*backend, layout), std::make_pair(HoldReason::ManifestBodyMissing, RefTxnId{1, held}));
        size_t read_ahead = 0;
        for (const String & key : wide_keys)
            read_ahead += intakeReadsOf(*backend, key);
        EXPECT_EQ(read_ahead, 4 * width) << "the read-ahead of one future log's manifests is not the window";

        publishManifestOf(*backend, layout, held);
        runRoundReadingRows(gc, *backend);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, tail}));
        for (uint64_t e = 0; e < edges; ++e)
            EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(4000 + e)), 1) << "edge " << e;
    }
    {
        SCOPED_TRACE("an untaken worker fault");
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        constexpr uint64_t held = kFirstOfPassTwo + 3;
        publishRecords(*backend, layout, 3, held - 1);
        writeHoldingRecord(*backend, layout, held);
        publishRecords(*backend, layout, held + 1, 100);
        backend->failWorkerReadsOf(manifestKeyOf(layout, 1, held + 5));
        bool refs_ready = false;
        {
            PassTwoProbe probe(gc, [&]
            {
                return readyAtFirstDecode(*backend, refLogKeysOf(layout, kFirstOfPassTwo + 1, held + 5),
                                          refLogKeysOf(layout, held + 6, 99), refs_ready);
            });
            runRoundReadingRows(gc, *backend);
        }
        EXPECT_TRUE(refs_ready);
        EXPECT_GT(backend->faultsServed(), 0u) << "positive control: the faulty manifest was read ahead";
        EXPECT_EQ(lifeHold(*backend, layout), std::make_pair(HoldReason::ManifestBodyMissing, RefTxnId{1, held}));
    }
    for (const bool mismatched : {false, true})
    {
        SCOPED_TRACE(mismatched ? "a reached mismatched manifest" : "a reached malformed manifest");
        auto backend = std::make_shared<ManifestReadAheadBackend>();
        auto store = openAtWidth(backend, 16);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldFirstTwo(*backend, layout, gc));
        publishRecords(*backend, layout, 3, 14);
        publishRecordWithoutManifest(*backend, layout, 15);
        publishRecords(*backend, layout, 16, 30);
        if (mismatched)
        {
            /// The body of manifest `{1,16}` stored under the key of `{1,15}`.
            OperationForTest op(*backend);
            const auto body = (*op).read(manifestKeyOf(layout, 1, 16), Retry::once());
            ASSERT_TRUE(body.has_value());
            (*op).create(manifestKeyOf(layout, 1, 15), body->bytes, Retry::once());
        }
        else
        {
            OperationForTest op(*backend);
            (*op).create(manifestKeyOf(layout, 1, 15), "this is not a cas manifest", Retry::once());
        }
        int code = 0;
        try
        {
            gc.runRegularRound();
        }
        catch (const DB::Exception & e)
        {
            code = e.code();
        }
        EXPECT_EQ(code, DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_EQ(lifeCursor(*backend, layout), (RefTxnId{1, 2}));
    }
}
