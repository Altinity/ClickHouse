#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include "cas_test_helpers.h"

#include <Common/ProfileEvents.h>
#include <IO/S3/Client.h>
#include <Poco/Exception.h>

#include <atomic>
#include <deque>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ProfileEvents
{
    extern const Event CASHotKeyQueueWaitMicroseconds;
    extern const Event CASHotKeyCacheStarts;
    extern const Event CASHotKeyReadStarts;
    extern const Event CASHotKeyCacheVerdictsReread;
    extern const Event CASRequestGaveUp;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;

namespace
{

/// The harness's `FakeClock` is single-threaded. The lane is not: its holders sleep on the engine's
/// clock from their own threads while the test thread advances it, so every access goes through one
/// mutex. A sleep still advances the clock by what it slept, so a holder's transport backoff is real
/// time to every waiter's deadline.
struct SyncClock
{
    std::mutex mutex;
    uint64_t now = 1'000'000;
    std::vector<uint64_t> sleeps;

    std::function<uint64_t()> nowFn()
    {
        return [this] { std::lock_guard lock(mutex); return now; };
    }
    std::function<void(uint64_t)> sleepFn()
    {
        return [this](uint64_t ms) { std::lock_guard lock(mutex); sleeps.push_back(ms); now += ms; };
    }
    void advance(uint64_t ms) { std::lock_guard lock(mutex); now += ms; }
    size_t sleepCount() { std::lock_guard lock(mutex); return sleeps.size(); }
};

/// The object under test lists the tickets that wrote it, comma-separated, so order is visible.
CasHotKeys::Decide appendTicket(int ticket)
{
    return [ticket](const std::optional<Object> & current) -> std::optional<String>
    {
        if (!current)
            return std::to_string(ticket);
        return current->bytes + "," + std::to_string(ticket);
    };
}

uint64_t counter(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

/// A one-shot gate a write hook parks on: the first write of the key waits here until the test
/// releases it; every later write passes.
struct ParkFirstWrite
{
    std::latch parked{1};
    std::latch release{1};
    std::atomic<int> seen{0};

    void install(CountingBackend & backend, const String & key)
    {
        backend.onBeforeWrite(key, [this]
        {
            if (seen.fetch_add(1) != 0)
                return;
            parked.count_down();
            release.wait();
        });
    }
};

#if USE_AWS_S3
std::exception_ptr s3Error(Aws::S3::S3Errors code, const String & name)
{
    return std::make_exception_ptr(DB::S3Exception("the store answered " + name, code, name));
}
#endif

}

TEST(CASHotKeys, SubmissionsOfOneKeyAreSerializedInArrivalOrder)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    constexpr int N = 4;
    ParkFirstWrite park;
    park.install(*backend, "k");

    std::vector<std::thread> threads;
    std::vector<std::optional<WriteResult>> results(N);
    std::deque<std::latch> go;   /// a deque: `std::latch` is neither copyable nor movable
    for (int i = 0; i < N; ++i)
        go.emplace_back(1);
    for (int i = 0; i < N; ++i)
    {
        threads.emplace_back([&, i]
        {
            go[i].wait();
            auto op = requests.admit();
            results[i] = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(i + 1));
        });
    }
    /// The first holder is released into its write and parked there; every later thread is released
    /// only after its item is seen queued, so arrival order is the release order.
    go[0].count_down();
    park.parked.wait();
    for (int i = 1; i < N; ++i)
    {
        go[i].count_down();
        while (hot_keys.queueDepthForTest("k") < static_cast<size_t>(i + 1))
            std::this_thread::yield();
    }
    park.release.count_down();
    for (auto & t : threads)
        t.join();

    EXPECT_EQ(backend->writeCount("k"), static_cast<uint64_t>(N));
    EXPECT_EQ(backend->getCount("k"), static_cast<uint64_t>(N));   /// no cache in this task: a read per hold
    std::vector<Etag> etags;
    for (const auto & result : results)
    {
        ASSERT_TRUE(result.has_value());
        const auto * committed = std::get_if<Committed>(&*result);
        ASSERT_NE(committed, nullptr);
        etags.push_back(committed->etag);
    }
    for (size_t i = 1; i < etags.size(); ++i)
        EXPECT_FALSE(etags[i] == etags[i - 1]);
    DB::Cas::tests::expectBytes(*backend, "k", "1,2,3,4");
    auto reader = requests.admit();
    EXPECT_EQ(reader.read("k", Retry::standard())->etag, etags.back());
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_EQ(hot_keys.queueDepthForTest("k"), 0u);
}

TEST(CASHotKeys, ADecideRunsWithTheLaneMutexReleased)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    /// `queueDepthForTest` takes the lane's mutex; a `decide` run under it would deadlock this test,
    /// which the runner's per-test timeout reports. The call is the assertion.
    WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()),
        [&](const std::optional<Object> &) -> std::optional<String>
        {
            EXPECT_EQ(hot_keys.queueDepthForTest("k"), 1u);
            return String("1");
        });
    EXPECT_TRUE(std::holds_alternative<Committed>(result));
}

TEST(CASHotKeys, AFailedEnqueueLeavesNoEmptyLaneBehind)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    hot_keys.enter_after_lane_hook_for_test = [] { throw std::bad_alloc(); };
    EXPECT_THROW(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1)), std::bad_alloc);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_EQ(backend->writeCount("k"), 0u);
    hot_keys.enter_after_lane_hook_for_test = {};
    EXPECT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));
}

TEST(CASHotKeys, ResultsAreTheEnginesOwn)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(false);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1))));

#if USE_AWS_S3
    /// The store refuses the bytes: the caller gets that `Refused`, at once.
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
        ASSERT_TRUE(std::holds_alternative<Refused>(result));
    }
#endif
    /// A clean refused precondition with the store unchanged: `Conflict` carrying the occupant.
    backend->refuseNextWrite("k");
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3));
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_TRUE(std::holds_alternative<Object>(conflict->seen));
        EXPECT_FALSE(conflict->any_ambiguous);
    }
    /// The resolve read fails at the transport under `once`: nothing observed. The failure is armed
    /// from a one-shot write hook, not up front, so it lands on the write's own resolve read rather
    /// than on the hold's base read, which must succeed for this sub-case to reach the write at all.
    backend->refuseNextWrite("k");
    backend->onBeforeWrite("k", [&] { backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("resolve"))); });
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(4));
        const auto * conflict = std::get_if<Conflict>(&result);
        ASSERT_NE(conflict, nullptr);
        EXPECT_TRUE(std::holds_alternative<NotObserved>(conflict->seen));
    }
    backend->onBeforeWrite("k", [] {});
    /// An ambiguous attempt whose resolve read fails at the transport under `once`: unresolved. Same
    /// one-shot arming as above, for the same reason.
    backend->injectAmbiguousWrite("k");
    backend->onBeforeWrite("k", [&] { backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("resolve"))); });
    {
        WriteResult result = hot_keys.submit("k", op, op.freeze(Retry::once()), appendTicket(5));
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
        EXPECT_TRUE(gave_up->sent_any);
    }
    backend->onBeforeWrite("k", [] {});
    /// The fence trips inside the hold, before the write: nothing sent.
    bool alive = true;
    auto fenced = requests.admit([&] { return alive; });
    {
        WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()),
            [&](const std::optional<Object> & current) -> std::optional<String>
            {
                alive = false;
                return current->bytes + ",6";
            });
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
    }
    /// The fence trips after the landed write: the object carries the ticket, the caller is told so.
    alive = true;
    backend->onWriteCommitted("k", [&] { alive = false; });
    {
        WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()), appendTicket(7));
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_TRUE(gave_up->sent_any);
        DB::Cas::tests::expectBytes(*backend, "k", "1,7");
    }
    backend->onWriteCommitted("k", [] {});
    /// The engine call throws a local fault: it reaches the caller and the key is handed over.
    backend->failNextWriteWith("k", std::make_exception_ptr(std::logic_error("local")));
    EXPECT_THROW(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(8)), std::logic_error);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
    EXPECT_TRUE(std::holds_alternative<Committed>(hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(9))));
}

TEST(CASHotKeys, ABaseReadThatFailsGivesUpAsReadModifyWriteDoes)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setAttemptTimeoutMs(1000);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto op = requests.admit();
    (void)orThrow(op.create("k", "1", Retry::standard()), "seed");

    /// Enough armed failures to outlast a standard window: the read loop gives up at its deadline.
    for (int i = 0; i < 64; ++i)
        backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("read")));
    WriteResult lane = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(2));
    for (int i = 0; i < 64; ++i)
        backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("read")));
    WriteResult verb = op.readModifyWrite("k", appendTicket(2), Retry::standard());

    const auto * a = std::get_if<GaveUp>(&lane);
    const auto * b = std::get_if<GaveUp>(&verb);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->why, b->why);
    EXPECT_EQ(a->deadline_source, b->deadline_source);
    EXPECT_EQ(a->sent_any, b->sent_any);
    EXPECT_FALSE(a->sent_any);
    EXPECT_EQ(a->last_seen.index(), b->last_seen.index());

    /// A fence that refuses the read's own reservation, and nothing smaller: the wait step passes
    /// (it asks for zero), the base read is refused before its first attempt.
    bool refuse_reservations = false;
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t needed) { return refuse_reservations && needed > 0 ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
                [](uint64_t) {}};
    CasRequests fenced_requests(backend, fence, clock.nowFn(), clock.sleepFn(), &hot_keys);
    auto fenced = fenced_requests.admit();
    refuse_reservations = true;
    WriteResult result = hot_keys.submit("k", fenced, fenced.freeze(Retry::standard()), appendTicket(3));
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_FALSE(gave_up->sent_any);
}

TEST(CASHotKeys, WaitersLeaveOnTheirOwnFenceLeaseAndDeadline)
{
    SyncClock clock;
    auto backend = std::make_shared<CountingBackend>();
    CasHotKeys hot_keys(0);
    bool lease_spent = false;
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return lease_spent ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
                [](uint64_t) {}};
    CasRequests requests(backend, fence, clock.nowFn(), clock.sleepFn(), &hot_keys);
    ParkFirstWrite park;
    park.install(*backend, "k");

    std::thread holder([&]
    {
        auto op = requests.admit();
        (void)hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(1));
    });
    park.parked.wait();

    const auto gave_up_before = counter(ProfileEvents::CASRequestGaveUp);
    /// A waiter whose own window ends while the holder is parked.
    std::optional<WriteResult> by_deadline;
    std::thread deadline_waiter([&]
    {
        auto op = requests.admit();
        by_deadline = hot_keys.submit("k", op, op.freeze(Retry::within(500)), appendTicket(2));
    });
    /// A waiter whose task stops.
    std::atomic<bool> alive{true};
    std::optional<WriteResult> by_liveness;
    std::thread liveness_waiter([&]
    {
        auto op = requests.admit([&] { return alive.load(); });
        by_liveness = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(3));
    });
    while (hot_keys.queueDepthForTest("k") < 3)
        std::this_thread::yield();

    clock.advance(600);
    deadline_waiter.join();
    alive = false;
    liveness_waiter.join();
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_deadline);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
        EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
        EXPECT_FALSE(gave_up->sent_any);
        EXPECT_EQ(gave_up->attempts_sent, 0u);
        EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    }
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_liveness);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
    }
    /// A waiter whose lease budget is gone, and whose task has stopped at the same slice: the lease
    /// speaks first, as the engine's own gate orders it. Both refusals are already in place before
    /// this thread is even spawned, so its first admission check sees both at once.
    lease_spent = true;
    std::optional<WriteResult> by_lease;
    std::thread lease_waiter([&]
    {
        auto op = requests.admit([&] { return alive.load(); });
        by_lease = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(4));
    });
    lease_waiter.join();
    {
        const auto * gave_up = std::get_if<GaveUp>(&*by_lease);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
        EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    }
    EXPECT_EQ(counter(ProfileEvents::CASRequestGaveUp) - gave_up_before, 3u);
    EXPECT_EQ(hot_keys.queueDepthForTest("k"), 1u) << "only the parked holder remains";
    EXPECT_EQ(backend->writeCount("k"), 1u) << "no second write started";

    lease_spent = false;
    park.release.count_down();
    holder.join();
    DB::Cas::tests::expectBytes(*backend, "k", "1");
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
}

TEST(CASHotKeys, AThrottledHolderKeepsTheWaitersQueuedThroughItsBackoff)
{
    SyncClock clock;
    auto inner = std::make_shared<CountingBackend>();
    /// The first request naming each key is refused with a 429, so the holder's first `PUT` is
    /// ambiguous and reissued after the growing schedule; the waiters' windows outlast it.
    auto backend = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 1, 429);
    CasHotKeys hot_keys(0);
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), &hot_keys);

    std::vector<std::thread> threads;
    std::vector<std::optional<WriteResult>> results(3);
    for (int i = 0; i < 3; ++i)
        threads.emplace_back([&, i]
        {
            auto op = requests.admit();
            results[i] = hot_keys.submit("k", op, op.freeze(Retry::standard()), appendTicket(i + 1));
        });
    for (auto & t : threads)
        t.join();

    for (const auto & result : results)
        EXPECT_TRUE(std::holds_alternative<Committed>(*result));
    EXPECT_GE(clock.sleepCount(), 1u) << "the throttled write slept its backoff inside the hold";
    EXPECT_EQ(inner->writeCount("k"), 3u);
    EXPECT_EQ(hot_keys.laneCountForTest(), 0u);
}
