#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
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
