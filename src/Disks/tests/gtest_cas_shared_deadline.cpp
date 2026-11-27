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
    EXPECT_EQ(pauses, kCasMaxDecisions - 1);
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
    EXPECT_EQ(pauses, kCasMaxDecisions - 1);
    /// Every write was settled: the plane's own reads are the first one and one per write.
    EXPECT_EQ(rival.moves, kCasMaxDecisions);
    EXPECT_EQ(sentSince(*plane.backend, started_ms, Verb::Read, "k") - rival.moves, 1 + kCasMaxDecisions);
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
    EXPECT_EQ(pauses, kCasMaxDecisions - 1);
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
    EXPECT_EQ(pauses, kCasMaxDecisions - 1);
    EXPECT_NE(plane.bytesOf("k"), "<absent>");
}

