#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/tests/cas_time_taking_backend.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <functional>
#include <latch>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

/// Each request of the engine binds its own window when it is sent. In the first two sections a first step
/// takes all but one millisecond of a window and succeeds, and the test checks that the next request
/// is still sent and the operation completes; its failure text carries what the first step used and
/// what a window shared with the next request would have left. The other sections check a lost
/// write's settlement and the caps on decisions and submissions.

namespace ProfileEvents
{
extern const Event CASRefRecoveryRetries;
}

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::ManualClock;
using DB::Cas::tests::TimeTakingBackend;
using DB::Cas::tests::idOf;

namespace
{

using Verb = TimeTakingBackend::Verb;
using Ending = TimeTakingBackend::Ending;

constexpr uint64_t kWindowMs = kDefaultRequestWindowMs;
/// What one attempt reserves on a disk with default settings: `cas_attempt_timeout_ms` 5000 and two
/// connect caps of 1000. A read reserves one, a write two.
constexpr uint64_t kEnvelopeMs = 7'000;
constexpr uint64_t kFirstStepMs = kWindowMs - 1;
/// The longest pause `Retry::backoff` draws.
constexpr uint64_t kMaxBackoffMs = 5'000;

std::function<bool(const String &)> exactly(const String & wanted)
{
    return [wanted](const String & key) { return key == wanted; };
}

/// Runs `step` and returns the text of whatever it threw, or nothing.
std::optional<String> failureOf(const std::function<void()> & step)
{
    try
    {
        step();
    }
    catch (...)
    {
        return DB::getCurrentExceptionMessage(/*with_stacktrace=*/false);
    }
    return std::nullopt;
}

String orNone(const std::optional<String> & failure)
{
    return failure ? *failure : String("none");
}

/// The two numbers of a case, as text for a failure message. `sent` counts the requests of the next
/// step the store received after the first step ended.
String sharedWindow(uint64_t window_ms, uint64_t started_ms, uint64_t first_step_ended_ms, const String & next, size_t sent)
{
    const uint64_t used_ms = first_step_ended_ms - started_ms;
    const uint64_t left_ms = window_ms > used_ms ? window_ms - used_ms : 0;
    return fmt::format(
        "\n  first step: {} ms of the shared {} ms\n  next ({}): {} ms of {} left, {}\n  ",
        used_ms, window_ms, next, left_ms, window_ms,
        sent == 0 ? String("refused unsent, 0 attempt(s)") : fmt::format("sent {} time(s)", sent));
}

/// Requests of `verb` to `key` that reached the store at or after `from_ms`.
size_t sentSince(const TimeTakingBackend & backend, uint64_t from_ms, Verb verb, const String & key)
{
    size_t count = 0;
    for (const TimeTakingBackend::Seen & request : backend.seen())
        if (request.verb == verb && request.key == key && request.at_ms >= from_ms)
            ++count;
    return count;
}

String whyOf(GaveUp::Why why)
{
    switch (why)
    {
        case GaveUp::Why::Deadline:   return "deadline";
        case GaveUp::Why::FenceLost:  return "fence lost";
        case GaveUp::Why::Unresolved: return "unresolved";
        case GaveUp::Why::Attempts:   return "attempts";
    }
    return "unknown";
}

String outcomeOf(const WriteResult & result)
{
    if (std::holds_alternative<Committed>(result))
        return "committed";
    if (std::holds_alternative<Conflict>(result))
        return "conflict";
    if (std::holds_alternative<Declined>(result))
        return "declined";
    if (const auto * refused = std::get_if<Refused>(&result))
        return "refused by the store: " + refused->message;
    const GaveUp & gave_up = std::get<GaveUp>(result);
    return fmt::format("gave up ({}, {} bound) after {} attempt(s)", whyOf(gave_up.why),
                       gave_up.deadline_source == GaveUp::Source::Lease ? "lease" : "policy", gave_up.attempts_sent);
}

/// A request plane over an open fence, with the reservation of a real disk and a clock the test moves.
struct Plane
{
    std::shared_ptr<TimeTakingBackend> backend = std::make_shared<TimeTakingBackend>();
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    CasRequests requests;
    CasOperation op;

    explicit Plane(CasHotKeys * lane = nullptr, Fence fence = Fence::open())
        : requests(backend, std::move(fence), ManualClock::nowFnOf(clock), ManualClock::sleepFnOf(clock), lane, kWindowMs)
        , op(requests.admit())
    {
        requests.setAttemptReservationForTest(kEnvelopeMs);
        backend->stampWith(ManualClock::nowFnOf(clock));
    }

    void seed(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::standard())));
    }

    String bytesOf(const String & key)
    {
        const std::optional<Object> object = op.read(key, Retry::standard());
        return object ? object->bytes : String("<absent>");
    }

    /// The next `verb` of `key` takes all but one millisecond of a window.
    void slowDown(Verb verb, const String & key, Ending ending = Ending::Answered)
    {
        backend->armWith(verb, exactly(key), [clock_ = clock] { clock_->advance(kFirstStepMs); }, /*times=*/1, ending);
    }

    /// The next `times` requests of `verb` to `key` each take `ms` and end as `ending`.
    void takeTime(Verb verb, const String & key, uint64_t ms, size_t times, Ending ending = Ending::Answered)
    {
        backend->armWith(verb, exactly(key), [clock_ = clock, ms] { clock_->advance(ms); }, times, ending);
    }

    /// Counts the pauses of the plane's own operation; the clock still moves as before.
    void countPauses(size_t & pauses)
    {
        requests.setSleepFnForTest([clock_ = clock, &pauses](uint64_t ms)
        {
            ++pauses;
            clock_->advance(ms + 1);
        });
    }
};

/// A second writer on the plane's store. Inside each of the plane's next `times` requests of `verb` to
/// `key` it replaces the object, so that request meets a moved precondition.
struct Rival
{
    Rival(Plane & plane, Verb verb, const String & key_, size_t times)
        : key(key_)
        , requests(plane.backend, Fence::open(), ManualClock::nowFnOf(plane.clock), ManualClock::sleepFnOf(plane.clock),
                   nullptr, kWindowMs)
        , op(requests.admit())
    {
        requests.setAttemptReservationForTest(kEnvelopeMs);
        plane.backend->armWith(verb, [this](const String & k) { return k == key && !inside; }, [this] { move(); }, times);
    }

    void move()
    {
        inside = true;
        if (const auto current = op.read(key, Retry::once()))
            (void)op.replace(key, current->bytes + "r", current->etag, Retry::once());
        else
            (void)op.create(key, "r", Retry::once());
        inside = false;
        ++moves;
        if (after_move)
            after_move();
    }

    String key;
    bool inside = false;
    size_t moves = 0;
    /// Runs inside the plane's request, after the move.
    std::function<void()> after_move;
    CasRequests requests;
    CasOperation op;
};

/// A mount fence the test trips: by re-arming its generation, or by letting the lease run out.
struct TrippableFence
{
    enum class Trip : uint8_t { Rearm, LeaseExpiry };

    uint64_t generation = 0;
    bool lease_expired = false;

    Fence fence()
    {
        return Fence{
            [this] { return generation; },
            [this](uint64_t admitted_generation, uint64_t)
            {
                if (admitted_generation != generation)
                    return Fence::Admit::LostOrRearmed;
                return lease_expired ? Fence::Admit::NoBudget : Fence::Admit::Ok;
            },
            [](uint64_t) {}};
    }

    void trip(Trip how)
    {
        if (how == Trip::Rearm)
            ++generation;
        else
            lease_expired = true;
    }
};

String tripName(TrippableFence::Trip how)
{
    return how == TrippableFence::Trip::Rearm ? "re-armed generation" : "expired lease";
}

CatalogEntry liveEntry(const String & ns, uint64_t incarnation)
{
    return CatalogEntry{.ns = RootNamespace{ns}, .state = NsState::Live, .incarnation = UInt128(incarnation)};
}

/// Another server writing the ref catalog. Inside each of the plane's next `times` writes of the catalog
/// it admits a new row, so that write meets a moved precondition.
struct CatalogRival
{
    CatalogRival(Plane & plane, const Layout & layout_, size_t times)
        : layout(layout_)
        , requests(plane.backend, Fence::open(), ManualClock::nowFnOf(plane.clock), ManualClock::sleepFnOf(plane.clock),
                   nullptr, kWindowMs)
        , op(requests.admit())
    {
        requests.setAttemptReservationForTest(kEnvelopeMs);
        plane.backend->armWith(Verb::Write, [this](const String & k) { return k == layout.refCatalogKey() && !inside; },
                               [this] { move(); }, times);
    }

    void move()
    {
        inside = true;
        CasRefCatalog::casAdmitEntry(op, layout, 1, liveEntry(fmt::format("rival{}", moves), 1000 + moves));
        inside = false;
        ++moves;
    }

    Layout layout;
    bool inside = false;
    size_t moves = 0;
    CasRequests requests;
    CasOperation op;
};

/// A mounted pool whose request planes read a clock the test moves and reserve what a real disk does.
struct PoolOnManualClock
{
    std::shared_ptr<TimeTakingBackend> backend = std::make_shared<TimeTakingBackend>();
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    PoolPtr store;

    PoolOnManualClock()
    {
        store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
        store->setCasRequestNowFnForTest(ManualClock::nowFnOf(clock));
        store->setCasRetrySleepForTest(ManualClock::sleepFnOf(clock));
        store->mountRequests().setAttemptReservationForTest(kEnvelopeMs);
        backend->stampWith(ManualClock::nowFnOf(clock));
    }

    void slowDown(Verb verb, const String & key)
    {
        backend->armWith(verb, exactly(key), [clock_ = clock] { clock_->advance(kFirstStepMs); });
    }
};

/// A pool whose LEASE clock is the test's too, for the bounds that are kept on it. The lease is an
/// hour long, so the request window and not the lease is what ends a wait.
PoolPtr openOnBootClock(const std::shared_ptr<TimeTakingBackend> & backend, const std::shared_ptr<ManualClock> & clock)
{
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.mount_lease_ttl_ms = std::chrono::hours(1);
    config.unsafe_remount_no_delay = true;
    config.boot_ms_fn = ManualClock::nowFnOf(clock);
    PoolPtr store = Pool::open(backend, config);
    store->setCasRetrySleepForTest(ManualClock::sleepFnOf(clock));
    store->mountRequests().setAttemptReservationForTest(kEnvelopeMs);
    backend->stampWith(ManualClock::nowFnOf(clock));
    return store;
}

ManifestEntry blobManifestEntry(const String & path, const String & payload)
{
    ManifestEntry entry;
    entry.path = path;
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    return entry;
}

ManifestId publishOnePart(const PoolPtr & store, const RootNamespace & ns, const String & ref, const String & payload)
{
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = store->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

}

/// ---------------------------------------------------------------------------------------------
/// The request engine: every request binds its own window when it is sent.
/// ---------------------------------------------------------------------------------------------

TEST(CASSharedDeadline, ReadModifyWriteSendsItsWriteAfterASlowRead)
{
    Plane plane;
    plane.seed("k", "v1");
    plane.slowDown(Verb::Read, "k");

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.readModifyWrite(
        "k", [](const std::optional<Object> &) -> std::optional<String> { return "v2"; }, Retry::standard());
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_TRUE(std::holds_alternative<Committed>(result))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the conditional write", sentSince(*plane.backend, first_ended_ms, Verb::Write, "k"))
        << "result: " << outcomeOf(result);
    EXPECT_EQ(plane.bytesOf("k"), "v2");
}

/// The same verb when the caller's `decide` takes the time: a decision may read other keys under
/// windows of their own.
TEST(CASSharedDeadline, ReadModifyWriteSendsItsWriteAfterASlowDecide)
{
    Plane plane;
    plane.seed("k", "v1");

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.readModifyWrite("k", [&](const std::optional<Object> &) -> std::optional<String>
    {
        plane.clock->advance(kFirstStepMs);
        return "v2";
    }, Retry::standard());
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_TRUE(std::holds_alternative<Committed>(result))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the conditional write", sentSince(*plane.backend, first_ended_ms, Verb::Write, "k"))
        << "result: " << outcomeOf(result);
    EXPECT_EQ(plane.bytesOf("k"), "v2");
}

TEST(CASSharedDeadline, ReadModifyWriteOnPresenceSendsItsWriteAfterASlowHead)
{
    Plane plane;
    plane.slowDown(Verb::Head, "k");

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.readModifyWriteOnPresence(
        "k", [](const std::optional<Meta> &) -> std::optional<String> { return "created"; }, Retry::standard());
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_TRUE(std::holds_alternative<Committed>(result))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the create", sentSince(*plane.backend, first_ended_ms, Verb::Write, "k"))
        << "result: " << outcomeOf(result);
    EXPECT_EQ(plane.bytesOf("k"), "created");
}

TEST(CASSharedDeadline, RemoveCurrentSendsItsRemoveAfterASlowHead)
{
    Plane plane;
    plane.seed("k", "v1");
    plane.slowDown(Verb::Head, "k");

    const uint64_t started_ms = plane.clock->now();
    std::optional<Removal> removed;
    const auto failure = failureOf([&] { removed = plane.op.removeCurrent("k", Retry::standard()); });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_EQ(removed, std::optional<Removal>(Removal::Removed))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the conditional delete", sentSince(*plane.backend, first_ended_ms, Verb::Remove, "k"))
        << "error: " << orNone(failure);
}

/// Verbs called one after another with `Retry::standard` are bound at their own call, so nothing is shared.
TEST(CASSharedDeadline, EachVerbCalledWithTheStandardPolicyStartsItsOwnWindow)
{
    Plane plane;
    plane.seed("k", "v1");
    plane.slowDown(Verb::Head, "k");
    plane.slowDown(Verb::Read, "k");

    const uint64_t started_ms = plane.clock->now();
    const std::optional<Meta> meta = plane.op.head("k", Retry::standard());
    const std::optional<Object> object = plane.op.read("k", Retry::standard());
    ASSERT_TRUE(meta.has_value());
    ASSERT_TRUE(object.has_value());
    const WriteResult replaced = plane.op.replace("k", "v2", object->etag, Retry::standard());

    EXPECT_GT(plane.clock->now() - started_ms, kWindowMs);
    EXPECT_TRUE(std::holds_alternative<Committed>(replaced)) << outcomeOf(replaced);
}

/// A slow page costs the next one nothing.
TEST(CASSharedDeadline, ForEachListedKeyGivesEachPageItsOwnWindow)
{
    Plane plane;
    for (int i = 0; i < 5; ++i)
        plane.seed("list/" + std::to_string(i), "x");
    plane.backend->armWith(Verb::List, exactly("list/"), [clock = plane.clock] { clock->advance(kFirstStepMs); }, /*times=*/3);

    const uint64_t started_ms = plane.clock->now();
    std::vector<String> keys;
    plane.op.forEachListedKey("list/", [&](const ListedKey & listed)
    {
        keys.push_back(listed.key);
        return true;
    }, Retry::standard(), /*page_limit=*/2);

    EXPECT_EQ(keys.size(), 5u);
    EXPECT_GT(plane.clock->now() - started_ms, 2 * kWindowMs) << "the pages were not slow";
}

/// A stream's window bounds only its open; the body is read under no deadline.
TEST(CASSharedDeadline, StreamBodyIsNotMeasuredAgainstTheWindowOfItsOpen)
{
    Plane plane;
    const String body(4096, 'b');
    plane.seed("big", body);
    plane.backend->setStreamChunkForTest(1024);
    plane.slowDown(Verb::Stream, "big");

    std::unique_ptr<DB::ReadBuffer> stream = plane.op.stream("big", Retry::standard());
    ASSERT_TRUE(stream);
    plane.clock->advance(10 * kWindowMs);
    String read;
    DB::readStringUntilEOF(read, *stream);
    EXPECT_EQ(read, body);
}

/// ---------------------------------------------------------------------------------------------
/// The hot-key lane and the ref catalog loops over it.
/// ---------------------------------------------------------------------------------------------

/// Nobody else is in the lane: the base read alone uses a window.
TEST(CASSharedDeadline, HotKeyLaneSendsItsWriteAfterASlowBaseRead)
{
    CasHotKeys lane(0);
    Plane plane(&lane);
    plane.seed("k", "1");
    plane.slowDown(Verb::Read, "k");

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = lane.submit("k", plane.op, Retry::standard(),
        [](const std::optional<Object> & current) -> std::optional<String> { return current->bytes + ",2"; });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_TRUE(std::holds_alternative<Committed>(result))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the conditional write", sentSince(*plane.backend, first_ended_ms, Verb::Write, "k"))
        << "result: " << outcomeOf(result);
    EXPECT_EQ(plane.bytesOf("k"), "1,2");
}

/// Another writer holds the key while all but one millisecond of the waiter's window passes. No
/// request of the waiter was slow: the wait is the lane's own.
TEST(CASSharedDeadline, HotKeyLaneServesAWriterThatWaitedBehindAnother)
{
    CasHotKeys lane(0);
    Plane plane(&lane);
    std::latch parked{1};
    std::latch release{1};
    plane.backend->armWith(Verb::Write, exactly("k"), [&]
    {
        parked.count_down();
        release.wait();
    });

    std::optional<WriteResult> held;
    std::optional<WriteResult> waited;
    std::thread holder([&]
    {
        auto op = plane.requests.admit();
        held = lane.submit("k", op, Retry::standard(), [](const std::optional<Object> &) -> std::optional<String> { return "1"; });
    });
    parked.wait();
    const uint64_t started_ms = plane.clock->now();
    std::thread waiter([&]
    {
        auto op = plane.requests.admit();
        waited = lane.submit("k", op, Retry::standard(), [](const std::optional<Object> & current) -> std::optional<String>
        {
            return (current ? current->bytes : String()) + ",2";
        });
    });
    while (lane.queueDepthForTest("k") < 2)
        std::this_thread::yield();
    plane.clock->advance(kFirstStepMs);
    release.count_down();
    holder.join();
    waiter.join();
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_TRUE(held.has_value());
    ASSERT_TRUE(waited.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(*held)) << "the holder itself failed: " << outcomeOf(*held);
    EXPECT_TRUE(std::holds_alternative<Committed>(*waited))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the waiter's base read", sentSince(*plane.backend, first_ended_ms, Verb::Read, "k"))
        << "result: " << outcomeOf(*waited);
    EXPECT_EQ(plane.bytesOf("k"), "1,2");
}

/// Another server wins the first race late. The update decides again on what the lost write's
/// settling read saw, and its own change lands next to the other server's.
TEST(CASSharedDeadline, CasUpdateDecidesAgainAfterALostRaceThatTookTheWindow)
{
    CasHotKeys lane(0);
    Plane plane(&lane);
    CasRequests other_server = DB::Cas::tests::openRequestsForTest(plane.backend);
    CasOperation other_op = other_server.admit();
    const Layout layout("p");
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    CasRefCatalog::casAdmitEntry(plane.op, layout, 1, liveEntry("a", 1));

    const String key = layout.refCatalogKey();
    size_t after_the_other_server = 0;
    plane.backend->armWith(Verb::Write, exactly(key), [&]
    {
        plane.clock->advance(kFirstStepMs);
        CasRefCatalog::casAdmitEntry(other_op, layout, 1, liveEntry("b", 2));
        after_the_other_server = plane.backend->seenCount();
    });

    const uint64_t started_ms = plane.clock->now();
    size_t decisions = 0;
    const auto failure = failureOf([&]
    {
        CasRefCatalog::casUpdate(plane.op, layout, [&](const RefCatalog & current)
        {
            ++decisions;
            RefCatalog next = current;
            for (CatalogEntry & entry : next.entries)
            {
                if (entry.ns == RootNamespace{"a"})
                {
                    entry.state = NsState::Removing;
                    entry.removal_started_round = 1;
                }
            }
            return next;
        });
    });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_FALSE(failure.has_value())
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the read that settles the lost race",
                        plane.backend->seenSince(after_the_other_server, Verb::Read, key))
        << "decisions made: " << decisions << "; error: " << orNone(failure);
    EXPECT_EQ(decisions, 2u);
    const RefCatalog durable = CasRefCatalog::read(other_op, layout).catalog;
    ASSERT_EQ(durable.entries.size(), 2u);
    EXPECT_EQ(durable.entries[0].ns, RootNamespace{"a"});
    EXPECT_EQ(durable.entries[0].state, NsState::Removing) << "the update's own change did not land";
    EXPECT_EQ(durable.entries[1].ns, RootNamespace{"b"});
    EXPECT_EQ(durable.entries[1].state, NsState::Live);
}

/// The erase landed slowly; the read that resolves it is a request of its own.
TEST(CASSharedDeadline, DeleteCompletedRemovingSendsItsResolutionReadAfterASlowErase)
{
    Plane plane;
    const Layout layout("p");
    const CatalogEntry removing{
        .ns = RootNamespace{"a"}, .state = NsState::Removing, .incarnation = UInt128{7}, .removal_started_round = 13};
    plane.seed(layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {removing}}));
    CasFoldSeal ready_parent;
    ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});

    const String key = layout.refCatalogKey();
    plane.slowDown(Verb::Write, key);
    uint64_t started_ms = 0;
    std::optional<CasRefCatalog::CompletedRemovingDeleteOutcome> outcome;
    const auto failure = failureOf([&]
    {
        /// The helper reads the catalog first; the erase loop starts at the first authority refresh.
        outcome = DB::Cas::tests::deleteCompletedRemovingForTest(plane.op, layout, removing, ready_parent,
            [&] { started_ms = started_ms ? started_ms : plane.clock->now(); }).outcome;
    });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_EQ(outcome, std::optional(CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the resolution read", sentSince(*plane.backend, first_ended_ms, Verb::Read, key))
        << "error: " << orNone(failure);
    /// The erase's own acknowledgment is not the answer; the resolution read is.
    EXPECT_EQ(sentSince(*plane.backend, first_ended_ms, Verb::Read, key), 1u);
}

/// The leader's authority check in the lane passes late; the cancel it authorised is a request of its own.
TEST(CASSharedDeadline, CancelStalledCreatingSendsItsWriteAfterASlowAuthorityCheck)
{
    CasHotKeys lane(0);
    Plane plane(&lane);
    const Layout layout("p");
    const CatalogEntry creating{
        .ns = RootNamespace{"c"}, .state = NsState::Creating, .incarnation = UInt128{9},
        .creator = CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1}};
    const String key = layout.refCatalogKey();
    plane.seed(key, encodeRefCatalog(RefCatalog{.entries = {creating}}));

    const uint64_t started_ms = plane.clock->now();
    bool checked = false;
    std::optional<CasRefCatalog::StalledCreatingCancelOutcome> outcome;
    const auto failure = failureOf([&]
    {
        outcome = CasRefCatalog::cancelStalledCreating(
            plane.op, layout, creating, [](const CreatorFence &) { return true; },
            [&](const Retry &)
            {
                if (!std::exchange(checked, true))
                    plane.clock->advance(kFirstStepMs);
            });
    });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    EXPECT_EQ(outcome, std::optional(CasRefCatalog::StalledCreatingCancelOutcome::Cancelled))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the cancelling write", sentSince(*plane.backend, first_ended_ms, Verb::Write, key))
        << "error: " << orNone(failure);
    EXPECT_TRUE(CasRefCatalog::read(plane.op, layout).catalog.entries.empty());
}

/// ---------------------------------------------------------------------------------------------
/// A mounted pool: the loops of the ref ledger.
/// ---------------------------------------------------------------------------------------------

/// The first catalog read of `resolveNamespaceLife` is slow; the next request of the loop is a request
/// of its own, so the first insert into a new table succeeds.
TEST(CASSharedDeadline, ResolveNamespaceLifeSendsItsNextRequestAfterASlowCatalogRead)
{
    PoolOnManualClock pool;
    const RootNamespace ns{"srv/new_table"};
    const String key = pool.store->layout().refCatalogKey();
    /// The admission reads the catalog once before the loop; the read after it is the loop's first.
    pool.backend->armWith(Verb::Read, [key, reads = std::make_shared<size_t>(0)](const String & read_key)
    {
        return read_key == key && ++*reads == 2;
    }, [clock = pool.clock] { clock->advance(kFirstStepMs); });

    const uint64_t started_ms = pool.clock->now();
    const auto failure = failureOf([&] { publishOnePart(pool.store, ns, "part", "payload"); });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_EQ(pool.backend->slowedKeys(), std::vector<String>{key}) << "no catalog read ran slowly";
    EXPECT_FALSE(failure.has_value())
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the next catalog read", sentSince(*pool.backend, first_ended_ms, Verb::Read, key))
        << "error: " << orNone(failure);
    EXPECT_TRUE(pool.store->resolveRef(ns, "part").has_value());
}

/// The first cancel of `cancelIdleOwnCreatingRows` is slow and lands; the second row's cancel is a
/// request of its own.
TEST(CASSharedDeadline, CancelIdleOwnCreatingRowsCancelsTheSecondRowAfterASlowFirstCancel)
{
    PoolOnManualClock pool;
    const Layout & layout = pool.store->layout();
    CasRequests other_requests = DB::Cas::tests::openRequestsForTest(pool.backend);
    CasOperation other_op = other_requests.admit();
    const CreatorFence ours{
        .server_root_id = "test", .writer_epoch = pool.store->liveWriterEpoch(), .fence_generation = pool.store->fenceGeneration()};
    for (uint64_t i = 1; i <= 2; ++i)
        CasRefCatalog::casAdmitEntry(other_op, layout, 1, CatalogEntry{
            .ns = RootNamespace{fmt::format("srv/own_row_{}", i)}, .state = NsState::Creating, .incarnation = UInt128{i},
            .creator = ours});
    ASSERT_EQ(CasRefCatalog::read(other_op, layout).catalog.entries.size(), 2u);

    const String key = layout.refCatalogKey();
    pool.slowDown(Verb::Write, key);
    const uint64_t started_ms = pool.clock->now();
    const auto failure = failureOf([&] { pool.store->cancelIdleOwnCreatingRows(); });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_EQ(pool.backend->slowedKeys(), std::vector<String>{key}) << "no cancel ran slowly";
    const size_t rows_left = CasRefCatalog::read(other_op, layout).catalog.entries.size();
    EXPECT_EQ(rows_left, 0u)
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the second row's cancel", sentSince(*pool.backend, first_ended_ms, Verb::Write, key))
        << "rows left: " << rows_left << "; error: " << orNone(failure);
}

/// The first catalog read of `dropNamespaceImpl` is slow; the table is in memory and nothing else takes
/// time. The removal's next requests are requests of their own.
TEST(CASSharedDeadline, DropNamespaceSendsItsNextRequestAfterASlowFirstRead)
{
    PoolOnManualClock pool;
    const RootNamespace ns{"srv/tbl"};
    publishOnePart(pool.store, ns, "part", "payload");
    const String key = pool.store->layout().refCatalogKey();
    pool.slowDown(Verb::Read, key);

    const uint64_t started_ms = pool.clock->now();
    const auto failure = failureOf([&] { pool.store->dropNamespace(ns); });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_EQ(pool.backend->slowedKeys(), std::vector<String>{key}) << "no catalog read ran slowly";
    EXPECT_FALSE(failure.has_value())
        << "catalog reads after the slow read: " << sentSince(*pool.backend, first_ended_ms, Verb::Read, key)
        << "; error: " << orNone(failure);
    EXPECT_FALSE(pool.store->resolveRef(ns, "part").has_value());
}

/// The body `HEAD` is slow and finds nothing; the publication that follows is a request of its own and
/// must still be sent.
TEST(CASSharedDeadline, BlobLoopStartsItsPublicationAfterASlowHead)
{
    PoolOnManualClock pool;
    const RootNamespace ns{"srv/tbl"};
    const String payload = "body-behind-a-slow-head";
    const String key = pool.store->layout().blobKey(idOf(payload));

    PartWriteInfo info;
    info.intended_ref = ns.string() + "/part";
    auto build = pool.store->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "part", id);

    pool.slowDown(Verb::Head, key);
    const uint64_t started_ms = pool.clock->now();
    const auto failure = failureOf([&]
    {
        build->putBlob(idOf(payload), BlobSource::fromString(payload));
        build->promote(ns, "part", build->buildId(), id);
    });
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_EQ(pool.backend->slowedKeys(), std::vector<String>{key}) << "the body `HEAD` never ran slowly";
    EXPECT_FALSE(failure.has_value())
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the body publication", sentSince(*pool.backend, first_ended_ms, Verb::Publish, key))
        << "error: " << orNone(failure);
    EXPECT_EQ(pool.backend->publicationsOf(key), 1u);
}

/// ---------------------------------------------------------------------------------------------
/// A write whose response is lost: the settling read is a request with its own window.
/// ---------------------------------------------------------------------------------------------

TEST(CASSharedDeadline, ALostWriteThatLandedIsProvenAfterItsWindow)
{
    Plane plane;
    plane.takeTime(Verb::Write, "k", kWindowMs + 1, /*times=*/1, Ending::LostAfterLanding);

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.create("k", "landed", Retry::standard());
    const uint64_t write_ended_ms = started_ms + kWindowMs + 1;

    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Write, "k"), 1u);
    EXPECT_EQ(sentSince(*plane.backend, write_ended_ms, Verb::Read, "k"), 1u) << "result: " << outcomeOf(result);
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr) << outcomeOf(result);
    EXPECT_TRUE(committed->resolved_by_read);
    EXPECT_EQ(plane.bytesOf("k"), "landed");
}

/// The settling read proves the write absent; resending the same bytes is still governed by the
/// write's own window, which is spent.
TEST(CASSharedDeadline, ALostWriteThatDidNotLandIsNotSentAgain)
{
    Plane plane;
    plane.takeTime(Verb::Write, "k", kWindowMs + 1, /*times=*/1, Ending::LostBeforeLanding);

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.create("k", "lost", Retry::standard());
    const uint64_t write_ended_ms = started_ms + kWindowMs + 1;

    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Write, "k"), 1u);
    EXPECT_EQ(sentSince(*plane.backend, write_ended_ms, Verb::Read, "k"), 1u) << "result: " << outcomeOf(result);
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr) << outcomeOf(result);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_TRUE(std::holds_alternative<ProvenAbsent>(gave_up->last_seen));
    EXPECT_EQ(plane.bytesOf("k"), "<absent>");
}

/// The settling read fails until its own window ends, and the give-up names the bound of that read:
/// the policy's window, or an explicit lease that ends before it.
TEST(CASSharedDeadline, AFailingSettlementReportsItsOwnStop)
{
    const auto settle = [](std::optional<uint64_t> lease_after_ms, GaveUp::Source expected)
    {
        SCOPED_TRACE(lease_after_ms ? "with an explicit lease" : "under the window");
        Plane plane;
        const uint64_t started_ms = plane.clock->now();
        const Retry policy = lease_after_ms ? Retry::untilLeaseSafe(started_ms + *lease_after_ms, 0) : Retry::standard();
        plane.takeTime(Verb::Write, "k", kFirstStepMs, /*times=*/1, Ending::LostAfterLanding);
        plane.takeTime(Verb::Read, "k", 1'000, /*times=*/1'000, Ending::LostBeforeLanding);

        const WriteResult result = plane.op.create("k", "landed", policy);
        const uint64_t write_ended_ms = started_ms + kFirstStepMs;
        const uint64_t read_end_ms = lease_after_ms ? started_ms + *lease_after_ms : write_ended_ms + kWindowMs;

        EXPECT_GT(sentSince(*plane.backend, write_ended_ms, Verb::Read, "k"), 1u) << outcomeOf(result);
        EXPECT_GT(plane.clock->now(), read_end_ms - kEnvelopeMs - kMaxBackoffMs);
        EXPECT_LE(plane.clock->now(), read_end_ms);
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr) << outcomeOf(result);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
        EXPECT_EQ(gave_up->deadline_source, expected);
        EXPECT_EQ(gave_up->attempts_sent, 1u);
    };
    settle(std::nullopt, GaveUp::Source::Policy);
    /// The write is bound by the window, the settling read by the lease.
    settle(kWindowMs + kWindowMs / 2, GaveUp::Source::Lease);
}

/// A write that meets nothing but transport failures ends at its own window, measured from when it
/// was sent rather than from when its operation started.
TEST(CASSharedDeadline, ATransportStormEndsInsideTheRequestsOwnWindow)
{
    Plane plane;
    plane.takeTime(Verb::Write, "k", 1'000, /*times=*/1'000, Ending::LostBeforeLanding);

    const uint64_t started_ms = plane.clock->now();
    const WriteResult result = plane.op.readModifyWrite("k", [&](const std::optional<Object> &) -> std::optional<String>
    {
        plane.clock->advance(kWindowMs + 1);
        return "v";
    }, Retry::standard());
    const uint64_t write_started_ms = started_ms + kWindowMs + 1;

    EXPECT_GT(sentSince(*plane.backend, write_started_ms, Verb::Write, "k"), 1u) << outcomeOf(result);
    /// Every attempt, the resends included, starts only with room in the write's own window for itself and its
    /// settling read.
    for (const TimeTakingBackend::Seen & request : plane.backend->seen())
        if (request.verb == Verb::Write && request.key == "k" && request.at_ms >= write_started_ms)
            EXPECT_LE(request.at_ms + 2 * kEnvelopeMs, write_started_ms + kWindowMs)
                << "an attempt started " << request.at_ms - write_started_ms << " ms into the write's window";
    /// The write stops only when the next attempt, its settling read and the pause before it no longer fit.
    EXPECT_GT(plane.clock->now() - write_started_ms, kWindowMs - 2 * kEnvelopeMs - kMaxBackoffMs);
    EXPECT_LE(plane.clock->now() - write_started_ms, kWindowMs + kEnvelopeMs + kMaxBackoffMs);
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr) << outcomeOf(result);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_TRUE(gave_up->sent_any);
}

/// ---------------------------------------------------------------------------------------------
/// Loops that decide again end on a count of decisions.
/// ---------------------------------------------------------------------------------------------

TEST(CASSharedDeadline, ReadModifyWriteSucceedsOnTheHundredthDecision)
{
    Plane plane;
    plane.seed("k", "v");
    Rival rival(plane, Verb::Write, "k", kCasMaxDecisions - 1);
    size_t pauses = 0;
    plane.countPauses(pauses);

    size_t decisions = 0;
    const WriteResult result = plane.op.readModifyWrite("k", [&](const std::optional<Object> & current) -> std::optional<String>
    {
        ++decisions;
        return current->bytes + "x";
    }, Retry::standard());

    ASSERT_TRUE(std::holds_alternative<Committed>(result)) << outcomeOf(result);
    EXPECT_EQ(decisions, kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
    EXPECT_EQ(rival.moves, kCasMaxDecisions - 1);
    EXPECT_EQ(std::get<Committed>(result).attempts_sent, kCasMaxDecisions);
}

TEST(CASSharedDeadline, ReadModifyWriteEndsAfterAHundredDecisions)
{
    Plane plane;
    plane.seed("k", "v");
    Rival rival(plane, Verb::Write, "k", 10 * kCasMaxDecisions);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const uint64_t started_ms = plane.clock->now();

    size_t decisions = 0;
    const WriteResult result = plane.op.readModifyWrite("k", [&](const std::optional<Object> & current) -> std::optional<String>
    {
        ++decisions;
        return current->bytes + "x";
    }, Retry::standard());

    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr) << outcomeOf(result);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Attempts);
    EXPECT_EQ(gave_up->attempts_sent, kCasMaxDecisions);
    EXPECT_EQ(decisions, kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
    /// Every loss but the last is followed by a read, after a pause from the second, and only the last refusal, which
    /// no decision follows, is settled: the plane's own reads are the first one, one per later decision and the last
    /// settlement.
    EXPECT_EQ(rival.moves, kCasMaxDecisions);
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Read, "k") - rival.moves, 1 + (kCasMaxDecisions - 1) + 1);
    const auto * seen = std::get_if<Object>(&gave_up->last_seen);
    ASSERT_NE(seen, nullptr);
    EXPECT_EQ(seen->bytes, plane.bytesOf("k"));
}

TEST(CASSharedDeadline, RemoveCurrentSucceedsOnTheHundredthDecision)
{
    Plane plane;
    plane.seed("k", "v");
    Rival rival(plane, Verb::Remove, "k", kCasMaxDecisions - 1);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const uint64_t started_ms = plane.clock->now();

    std::optional<Removal> removed;
    const auto failure = failureOf([&] { removed = plane.op.removeCurrent("k", Retry::standard()); });

    EXPECT_EQ(removed, std::optional<Removal>(Removal::Removed)) << "error: " << orNone(failure);
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Head, "k"), kCasMaxDecisions);
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Remove, "k"), kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
}

TEST(CASSharedDeadline, RemoveCurrentEndsAfterAHundredDecisions)
{
    Plane plane;
    plane.seed("k", "v");
    Rival rival(plane, Verb::Remove, "k", 10 * kCasMaxDecisions);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const uint64_t started_ms = plane.clock->now();

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, "CAS remove of 'k': did not converge after 100 decisions",
        [&] { (void)plane.op.removeCurrent("k", Retry::standard()); });
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Head, "k"), kCasMaxDecisions);
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Remove, "k"), kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
    EXPECT_NE(plane.bytesOf("k"), "<absent>");
}

/// The fence trips while the read that settles the hundredth lost race is in flight. That read still
/// answers, and the call reports the trip, as the pause before a next decision would have, not the count.
TEST(CASSharedDeadline, ReadModifyWriteReportsAFenceTripDuringItsLastSettlementNotTheCount)
{
    const auto run = [](TrippableFence::Trip how, GaveUp::Why why, GaveUp::Source source)
    {
        SCOPED_TRACE(tripName(how));
        TrippableFence mount;
        Plane plane(nullptr, mount.fence());
        plane.seed("k", "v");
        Rival rival(plane, Verb::Write, "k", 10 * kCasMaxDecisions);
        bool tripped = false;
        plane.backend->armWith(Verb::Read,
            [&](const String & key) { return key == "k" && !rival.inside && rival.moves == kCasMaxDecisions; },
            [&]
            {
                mount.trip(how);
                tripped = true;
            });
        const size_t journal_from = plane.backend->seenCount();

        size_t decisions = 0;
        const WriteResult result = plane.op.readModifyWrite("k", [&](const std::optional<Object> & current) -> std::optional<String>
        {
            ++decisions;
            return current->bytes + "x";
        }, Retry::standard());

        ASSERT_TRUE(tripped) << "the hundredth settling read was never sent; " << outcomeOf(result);
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr) << outcomeOf(result);
        EXPECT_EQ(gave_up->why, why) << outcomeOf(result);
        EXPECT_EQ(gave_up->deadline_source, source) << outcomeOf(result);
        EXPECT_EQ(gave_up->attempts_sent, kCasMaxDecisions);
        EXPECT_EQ(decisions, kCasMaxDecisions);
        EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, "k") - rival.moves, kCasMaxDecisions);
    };
    run(TrippableFence::Trip::Rearm, GaveUp::Why::FenceLost, GaveUp::Source::Policy);
    run(TrippableFence::Trip::LeaseExpiry, GaveUp::Why::Deadline, GaveUp::Source::Lease);
}

/// The fence trips while the hundredth conditional delete is in flight and meets a replaced object.
TEST(CASSharedDeadline, RemoveCurrentReportsAFenceTripDuringItsLastDeleteNotTheCount)
{
    const auto run = [](TrippableFence::Trip how, const String & expected)
    {
        SCOPED_TRACE(tripName(how));
        TrippableFence mount;
        Plane plane(nullptr, mount.fence());
        plane.seed("k", "v");
        Rival rival(plane, Verb::Remove, "k", 10 * kCasMaxDecisions);
        rival.after_move = [&]
        {
            if (rival.moves == kCasMaxDecisions)
                mount.trip(how);
        };
        const size_t journal_from = plane.backend->seenCount();

        DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, expected,
            [&] { (void)plane.op.removeCurrent("k", Retry::standard()); });
        EXPECT_EQ(rival.moves, kCasMaxDecisions);
        EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Head, "k"), kCasMaxDecisions);
        EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Remove, "k"), kCasMaxDecisions);
    };
    run(TrippableFence::Trip::Rearm, "mount fence tripped");
    run(TrippableFence::Trip::LeaseExpiry, "no lease budget");
}

/// The fence trips while the hundredth submission's write is in flight, and that write is refused: the call reports
/// the lost fence, not the count. The first submission writes twice, on the lane's cached catalog and then on its
/// resolve read, so the hundredth submission sends write `kCasMaxDecisions + 1`.
TEST(CASSharedDeadline, CasUpdateReportsAFenceTripDuringItsLastWriteNotTheCount)
{
    TrippableFence mount;
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane, mount.fence());
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    CasRefCatalog::casAdmitEntry(plane.op, layout, 1, liveEntry("a", 1));
    CatalogRival rival(plane, layout, 10 * kCasMaxDecisions);
    bool tripped = false;
    /// The rival's own read, inside the move that refuses write `kCasMaxDecisions + 1`.
    plane.backend->armWith(Verb::Read,
        [&](const String & k) { return k == key && rival.inside && rival.moves == kCasMaxDecisions; },
        [&]
        {
            mount.trip(TrippableFence::Trip::Rearm);
            tripped = true;
        });
    const size_t journal_from = plane.backend->seenCount();

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, "mount fence tripped", [&]
        {
            CasRefCatalog::casUpdate(plane.op, layout, [](const RefCatalog & current) { return current; });
        });
    EXPECT_TRUE(tripped) << "the hundredth submission's write was never sent";
    EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, key) - rival.moves, kCasMaxDecisions + 1);
}

/// The fence trips while the read that settles the hundredth submission's refused write is in flight: no
/// submission follows it, so that write is settled, and the call reports the lost fence, not the count.
TEST(CASSharedDeadline, CasUpdateReportsAFenceTripDuringItsLastSettlementNotTheCount)
{
    TrippableFence mount;
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane, mount.fence());
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    CasRefCatalog::casAdmitEntry(plane.op, layout, 1, liveEntry("a", 1));
    CatalogRival rival(plane, layout, 10 * kCasMaxDecisions);
    bool tripped = false;
    /// The plane's own read after write `kCasMaxDecisions + 1`, the hundredth submission's.
    plane.backend->armWith(Verb::Read,
        [&](const String & k) { return k == key && !rival.inside && rival.moves == kCasMaxDecisions + 1; },
        [&]
        {
            mount.trip(TrippableFence::Trip::Rearm);
            tripped = true;
        });
    const size_t journal_from = plane.backend->seenCount();

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, "mount fence tripped", [&]
        {
            CasRefCatalog::casUpdate(plane.op, layout, [](const RefCatalog & current) { return current; });
        });
    EXPECT_TRUE(tripped) << "the hundredth submission's settling read was never sent";
    EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, key) - rival.moves, kCasMaxDecisions + 1);
}

/// `casUpdate` counts submissions to the hot-key lane: one submission can run its `decide` twice on a
/// cached catalog, so the tests count the plane's writes of the catalog, not the calls of `mutate`. The
/// first submission starts from the cached catalog, loses, and writes again at once on the resolve read,
/// so `kCasMaxDecisions` submissions send one write more than that.
TEST(CASSharedDeadline, CasUpdateSucceedsOnTheHundredthSubmission)
{
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane);
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    CasRefCatalog::casAdmitEntry(plane.op, layout, 1, liveEntry("a", 1));
    CatalogRival rival(plane, layout, kCasMaxDecisions);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const size_t journal_from = plane.backend->seenCount();

    const auto failure = failureOf([&]
    {
        CasRefCatalog::casUpdate(plane.op, layout, [](const RefCatalog & current)
        {
            RefCatalog next = current;
            next.entries.front().state = NsState::Removing;
            next.entries.front().removal_started_round = 1;
            return next;
        });
    });

    EXPECT_FALSE(failure.has_value()) << orNone(failure);
    EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, key) - rival.moves, kCasMaxDecisions + 1);
    EXPECT_EQ(rival.moves, kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
    const RefCatalog durable = CasRefCatalog::read(plane.op, layout).catalog;
    EXPECT_EQ(durable.entries.size(), 1 + rival.moves);
    EXPECT_EQ(durable.entries.front().state, NsState::Removing);
}

/// Every write meets a moved precondition. The hundredth refused submission ends the call without a
/// pause after it, and the error names a clean conflict. The first submission writes twice.
TEST(CASSharedDeadline, CasUpdateEndsAfterAHundredSubmissionsOnACleanConflict)
{
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane);
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    CasRefCatalog::casAdmitEntry(plane.op, layout, 1, liveEntry("a", 1));
    CatalogRival rival(plane, layout, 10 * kCasMaxDecisions);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const size_t journal_from = plane.backend->seenCount();

    /// The closing parenthesis is the end of the reason: nothing names an unresolved write.
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR,
        fmt::format("CAS ref catalog '{}' update: did not converge after 100 submissions)", key), [&]
        {
            CasRefCatalog::casUpdate(plane.op, layout, [](const RefCatalog & current) { return current; });
        });
    EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, key) - rival.moves, kCasMaxDecisions + 1);
    EXPECT_EQ(rival.moves, kCasMaxDecisions + 1);
    EXPECT_EQ(pauses, kCasMaxDecisions - 2) << "the first lost race is retried at once";
}

/// Namespace-create step 1 decides again after a write whose fate is unknown. When the hundredth
/// write is that one, the error says the write may have landed.
TEST(CASSharedDeadline, CasUpdateEndsAfterAHundredSubmissionsAfterAnUnresolvedLastWrite)
{
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane);
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    plane.takeTime(Verb::Write, key, 0, 10 * kCasMaxDecisions, Ending::LostBeforeLanding);
    size_t pauses = 0;
    plane.countPauses(pauses);
    const size_t journal_from = plane.backend->seenCount();

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR,
        fmt::format("CAS ref catalog '{0}' update: a sent write's outcome is unresolved; the call ended with: Code: 210. DB::Exception: "
                    "CAS write could not be committed (CAS ref catalog '{0}' update: did not converge after 100 submissions)", key),
        [&]
        {
            (void)CasRefCatalog::createNamespace(plane.op, layout, 1, RootNamespace{"a"},
                CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1});
        });
    EXPECT_EQ(plane.backend->seenSince(journal_from, Verb::Write, key), kCasMaxDecisions);
    EXPECT_EQ(pauses, kCasMaxDecisions - 1);
    EXPECT_TRUE(CasRefCatalog::read(plane.op, layout).catalog.entries.empty());
}

/// The lease expires while the read that settles the hundredth unresolved write is in flight. The error
/// names the lease and still says the last write may have landed.
TEST(CASSharedDeadline, CasUpdateReportsTheLeaseAndAnUnresolvedLastWriteAtTheHundredthSubmission)
{
    TrippableFence mount;
    CasHotKeys lane(16ULL << 20);
    Plane plane(&lane, mount.fence());
    const Layout layout("p");
    const String key = layout.refCatalogKey();
    CasRefCatalog::initializeEmptyForNewPool(plane.op, layout);
    size_t writes = 0;
    plane.backend->armWith(Verb::Write, exactly(key), [&] { ++writes; }, 10 * kCasMaxDecisions, Ending::LostBeforeLanding);
    bool tripped = false;
    plane.backend->armWith(Verb::Read, [&](const String & k) { return k == key && writes == kCasMaxDecisions; }, [&]
    {
        mount.trip(TrippableFence::Trip::LeaseExpiry);
        tripped = true;
    });

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR,
        fmt::format("CAS ref catalog '{0}' update: a sent write's outcome is unresolved; the call ended with: Code: 210. DB::Exception: "
                    "CAS write could not be committed (CAS ref catalog '{0}' update: gave up at the lease deadline after 100 submissions)", key),
        [&]
        {
            (void)CasRefCatalog::createNamespace(plane.op, layout, 1, RootNamespace{"a"},
                CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1});
        });
    EXPECT_TRUE(tripped) << "the hundredth settling read was never sent";
    EXPECT_EQ(writes, kCasMaxDecisions);
}

/// The slot read of `Pool::confirmPinnedRef` binds its own request window: a ledger check that took all but
/// one millisecond of the poll's window does not shorten it.
TEST(CASSharedDeadline, ConfirmPinnedRefSendsItsSlotReadAfterASlowLedgerCheck)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    auto clock = std::make_shared<ManualClock>();
    const PoolPtr store = openOnBootClock(backend, clock);
    const RootNamespace ns{"srv/source"};
    const ManifestId id = publishOnePart(store, ns, "x", "payload");
    const auto pinned = store->resolveRefPinned(ns, "x");
    ASSERT_TRUE(pinned.has_value());
    const String slot_key = store->layout().mountKey("test");

    const uint64_t started_ms = clock->now();
    bool waited = false;
    store->setConfirmAfterLedgerSnapshotHookForTest([&]
    {
        if (!std::exchange(waited, true))
            clock->advance(kFirstStepMs);
    });
    std::optional<ConfirmAnswer> answer;
    const auto failure = failureOf([&] { answer = store->confirmPinnedRef(pinned->table, ns, "x", id.ref); });
    store->setConfirmAfterLedgerSnapshotHookForTest(nullptr);
    const uint64_t first_ended_ms = started_ms + kFirstStepMs;

    ASSERT_TRUE(waited) << "the confirm never reached its ledger check";
    EXPECT_EQ(answer, std::optional(ConfirmAnswer::Yes))
        << sharedWindow(kWindowMs, started_ms, first_ended_ms, "the mount slot read", sentSince(*backend, first_ended_ms, Verb::Read, slot_key))
        << "error: " << orNone(failure);
}

/// A recovery walk whose reads were slow and whose last read then failed for one request window is
/// retried: no elapsed-time limit ends recovery, only the cap on the number of walks.
TEST(CASSharedDeadline, RecoveryRetriesAWalkThatFailedLateInItsBudget)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    auto clock = std::make_shared<ManualClock>();
    const RootNamespace ns{"srv/tbl"};
    {
        const PoolPtr first = openOnBootClock(backend, clock);
        publishOnePart(first, ns, "part_a", "payload-a");
        publishOnePart(first, ns, "part_b", "payload-b");
    }
    const PoolPtr store = openOnBootClock(backend, clock);
    ASSERT_FALSE(store->refTableRecoveredForTest(ns));
    const String catalog_key = store->layout().refCatalogKey();

    const uint64_t started_ms = clock->now();
    constexpr uint64_t slow_read_ms = kWindowMs + 20'000;
    /// Later reads fail for one request window, then the store answers again.
    const uint64_t fault_ends_ms = started_ms + slow_read_ms + kWindowMs;
    backend->armWith(Verb::Read, [catalog_key](const String & key) { return key != catalog_key; },
                     [clock] { clock->advance(slow_read_ms); });
    backend->armWith(Verb::Read,
                     [catalog_key, clock, fault_ends_ms](const String & key) { return key != catalog_key && clock->now() < fault_ends_ms; },
                     [] {}, /*times=*/std::numeric_limits<size_t>::max(), Ending::LostBeforeLanding);

    const uint64_t retries_before = ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryRetries];
    size_t refs = 0;
    uint64_t walk_ended_ms = 0;
    const auto failure = failureOf([&]
    {
        try
        {
            refs = store->listRefs(ns).size();
        }
        catch (...)
        {
            walk_ended_ms = clock->now();
            throw;
        }
    });
    const uint64_t retries = ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryRetries] - retries_before;

    ASSERT_GE(backend->slowedKeys().size(), 2u) << "the walk made fewer than two reads, so none failed after the slow one";
    const uint64_t used_ms = (walk_ended_ms ? walk_ended_ms : clock->now()) - started_ms;
    EXPECT_FALSE(failure.has_value())
        << fmt::format("\n  first walk took {} ms; {} retry(ies)\n  ", used_ms, retries)
        << "error: " << orNone(failure);
    EXPECT_EQ(refs, 2u);
    EXPECT_GE(retries, 1u) << "a second walk was started";
}
