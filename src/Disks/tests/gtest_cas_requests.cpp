#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include "cas_test_helpers.h"

#include "config.h"

#include <Poco/Exception.h>
#include <base/defines.h>

#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int CAS_DELETE_MARKER;
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int S3_ERROR;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::FakeClock;
using DB::Cas::tests::expectThrowsCode;

namespace
{

/// Every engine test drives `CasRequests` on an injected clock, so a ninety-second policy is exercised
/// in no wall-clock time and the retry schedule itself becomes an assertion.
CasRequests makeRequests(BackendPtr backend, FakeClock & clock, Fence fence = Fence::open())
{
    return CasRequests(std::move(backend), std::move(fence), clock.nowFn(), clock.sleepFn());
}

}

static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
static_assert(!std::is_constructible_v<Incarnation, PersistedIncarnation>);
static_assert(!std::is_default_constructible_v<TransportAccess>);
static_assert(!std::is_copy_constructible_v<TransportAccess>);

TEST(CASIncarnation, GrammarRefusesTheNineWays)
{
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, ""));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, "*"));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, " * "));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, "\"a\",\"b\""));
    EXPECT_TRUE(isIncarnationValue(Dialect::ETag, "\"abc\""));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "0"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "00123"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "\"123\""));
    EXPECT_FALSE(isIncarnationValue(Dialect::Generation, "123 "));   /// the ninth: decimal is not "decimal, trimmed"
    EXPECT_TRUE(isIncarnationValue(Dialect::Generation, "123"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Emulated, ""));
}

TEST(CASRetry, BackoffIsFullJitterUnderTheCap)
{
    for (uint32_t attempt = 1; attempt <= 12; ++attempt)
    {
        const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << (attempt - 1));
        uint64_t sum = 0;
        std::set<uint64_t> seen;
        bool low = false;
        bool high = false;
        for (int i = 0; i < 1000; ++i)
        {
            const uint64_t s = Retry::backoff(attempt);
            ASSERT_LE(s, ceiling);
            sum += s;
            seen.insert(s);
            low = low || s < ceiling / 4;
            high = high || s > ceiling * 3 / 4;
        }
        const double mean = static_cast<double>(sum) / 1000.0;
        EXPECT_GT(mean, static_cast<double>(ceiling) * 0.35) << "attempt " << attempt;
        EXPECT_LT(mean, static_cast<double>(ceiling) * 0.65) << "attempt " << attempt;
        /// The mean alone cannot tell full jitter from a constant half the ceiling, so the SPREAD is
        /// asserted too: many distinct values, reaching into both the bottom and the top quarter.
        EXPECT_GE(seen.size(), 3u) << "attempt " << attempt;
        EXPECT_TRUE(low) << "attempt " << attempt;
        EXPECT_TRUE(high) << "attempt " << attempt;
    }
}

TEST(CASRetry, PoliciesAreShapedAsSpecified)
{
    const uint64_t now = 1'000'000;
    EXPECT_EQ(Retry::standard().bind(now).deadline_ms, now + 90'000);
    EXPECT_FALSE(Retry::standard().bind(now).lease_bound);
    EXPECT_FALSE(Retry::standard().single_attempt);
    EXPECT_TRUE(Retry::once().single_attempt);
    const Retry::Bound lease = Retry::untilLeaseSafe(now + 10'000, 2'000).bind(now);
    EXPECT_EQ(lease.deadline_ms, now + 8'000);
    EXPECT_TRUE(lease.lease_bound);
    EXPECT_EQ(Retry::within(1'000).bind(now).deadline_ms, now + 1'000);
}

TEST(CASWriteResult, OrThrowMapsEveryAlternative)
{
    /// The two that are not failures: a commit hands back its incarnation, a decline hands back
    /// nothing, and neither throws. Minting one needs a real write, since nothing else may mint.
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    WriteResult committed = op.create("k", "v", Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Committed>(committed));
    const Incarnation landed = std::get<Committed>(committed).incarnation;
    const auto returned = orThrow(std::move(committed), "create");
    ASSERT_TRUE(returned.has_value());
    EXPECT_EQ(*returned, landed);
    EXPECT_FALSE(orThrow(WriteResult{Declined{ProvenAbsent{}}}, "declined").has_value());

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { orThrow(WriteResult{Conflict{ProvenAbsent{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { orThrow(WriteResult{Refused{DB::ErrorCodes::S3_ERROR, "denied"}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Deadline, GaveUp::Source::Policy, true, NotObserved{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Unresolved, GaveUp::Source::Policy, true, ProvenAbsent{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::FenceLost, GaveUp::Source::Lease, false, NotObserved{}}}, "t"); });
}

TEST(CASFence, OpenFenceAdmitsEverythingAndNeverMoves)
{
    Fence f = Fence::open();
    EXPECT_EQ(f.generation(), 0u);
    EXPECT_EQ(f.admit(0, 1'000'000), Fence::Admit::Ok);
    EXPECT_NO_THROW(f.check_or_throw(0));
}

/// ================================================================================================
/// The backend's keyed string primitives
/// ================================================================================================

/// The door the primitives are reachable through until `CasRequests` lands: `Backend` is a friend of
/// `TransportAccess`, so a `Backend` subclass can hand out the migration key. Both go at the lock.
struct RawDoor : DB::Cas::Backend
{
    static DB::Cas::TransportAccess key() { return migrationAccess(); }
};

TEST(CASBackendPrimitives, InMemoryWriteReadRemoveRoundTripInStrings)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto key = RawDoor::key();
    auto w1 = b->write("k", "v1", std::nullopt, key);
    ASSERT_TRUE(w1.has_value());
    auto r = b->read("k", key);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->bytes, "v1");
    EXPECT_EQ(r->value, *w1);

    auto h = b->head("k", key);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->size, 2u);
    EXPECT_EQ(h->value, *w1);

    auto w2 = b->write("k", "v2", std::nullopt, key);     /// must be absent → refused
    EXPECT_FALSE(w2.has_value());
    auto w3 = b->write("k", "v2", *w1, key);
    ASSERT_TRUE(w3.has_value());
    EXPECT_NE(*w3, *w1);                                  /// values never repeat

    EXPECT_EQ(b->remove("k", *w1, key), Backend::RawRemoval::Mismatch);
    EXPECT_EQ(b->remove("k", *w3, key), Backend::RawRemoval::Removed);
    EXPECT_EQ(b->remove("k", *w3, key), Backend::RawRemoval::Gone);
    EXPECT_FALSE(b->read("k", key).has_value());
}

TEST(CASBackendPrimitives, ListSurfacesTheIncarnationValueAndPaginates)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto key = RawDoor::key();
    const String a = *b->write("p/a", "0123456789", std::nullopt, key);
    b->write("p/b", "xy", std::nullopt, key);
    b->write("q/c", "z", std::nullopt, key);

    const auto page = b->list("p/", "", 10, key);
    ASSERT_EQ(page.keys.size(), 2u);                      /// sorted, prefix-scoped
    EXPECT_EQ(page.keys[0].key, "p/a");
    EXPECT_EQ(page.keys[0].size, 10u);
    ASSERT_TRUE(page.keys[0].value.has_value());
    EXPECT_EQ(*page.keys[0].value, a);
    EXPECT_TRUE(page.next_cursor.empty());

    const auto first = b->list("p/", "", 1, key);
    ASSERT_EQ(first.keys.size(), 1u);
    EXPECT_EQ(first.next_cursor, "p/a");
    const auto second = b->list("p/", first.next_cursor, 1, key);
    ASSERT_EQ(second.keys.size(), 1u);
    EXPECT_EQ(second.keys[0].key, "p/b");
}

TEST(CASBackendPrimitives, EveryBackendInstanceHasItsOwnId)
{
    auto a = std::make_shared<InMemoryBackend>();
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_NE(a->backendId(), b->backendId());
    EXPECT_NE(a->backendId(), 0u);
    EXPECT_EQ(a->dialect(), Dialect::Emulated);
}

namespace
{

/// Counts every call that reaches the primitive `write`, whichever surface it entered through.
struct WriteCountingBackend : InMemoryBackend
{
    size_t writes = 0;

    std::expected<String, RawConflict> write(const String & k, const String & v, const std::optional<String> & e,
                                             TransportAccess & a) override
    {
        ++writes;
        return InMemoryBackend::write(k, v, e, a);
    }
};

}

TEST(CASBackendPrimitives, ALegacyCallReachesAnOverrideOfThePrimitiveItForwardsTo)
{
    /// The migration rule: the new methods are the primitives, and a fault injection written against a
    /// NEW signature intercepts a legacy caller too, because the legacy verb forwards through the
    /// virtual. `putIfAbsent` and `casPut` are this backend's two documented exceptions -- they route
    /// around the primitive to keep their knobs' verb identity -- so the rule is asserted on a verb
    /// that does forward.
    auto b = std::make_shared<WriteCountingBackend>();
    auto door = RawDoor::key();
    const String first = *b->write("k", "v", std::nullopt, door);
    b->writes = 0;

    b->putOverwrite("k", "w", Token{first, Dialect::Emulated});     /// legacy call
    EXPECT_EQ(b->writes, 1u);

    /// And the negative half of the same ruling: the two exceptions really do route around the
    /// primitive. Both writes LAND, so this cannot pass by the calls having done nothing.
    b->writes = 0;
    EXPECT_EQ(b->putIfAbsent("k2", "v").outcome, PutOutcome::Done);
    EXPECT_EQ(b->casPut("k3", "v", std::nullopt).outcome, CasOutcome::Committed);
    EXPECT_EQ(b->writes, 0u);
}

TEST(CASBackendPrimitives, EachWriteKnobFiresOnlyForTheVerbItNames)
{
    /// A knob is armed against a VERB. The keyed `write` cannot see which verb its caller used, so
    /// consuming both knobs there is right and consuming the other one from a legacy verb is not: a
    /// test that arms an ambiguity for `putIfAbsent` must not have it fire on a `casPut`.
    auto b = std::make_shared<InMemoryBackend>();
    auto door = RawDoor::key();

    b->failNextCasPut("k");
    EXPECT_EQ(b->putIfAbsent("k", "v").outcome, PutOutcome::Done);   /// not casPut's knob to consume
    const Token present = b->head("k").token;
    EXPECT_EQ(b->casPut("k", "w", present).outcome, CasOutcome::Conflict);
    EXPECT_EQ(b->get("k")->bytes, "v");                              /// the refusal changed nothing

    b->injectAmbiguousPutIfAbsent("k2");
    EXPECT_EQ(b->casPut("k2", "v", std::nullopt).outcome, CasOutcome::Committed);   /// not casPut's knob either
    EXPECT_THROW(b->putIfAbsent("k2", "w"), std::runtime_error);

    /// The keyed primitive is the one caller every knob is armed against, and each is still one-shot.
    b->injectAmbiguousPutIfAbsent("k3");
    b->failNextCasPut("k3");
    EXPECT_THROW((void)b->write("k3", "v", std::nullopt, door), std::runtime_error);
    EXPECT_FALSE(b->write("k3", "v", std::nullopt, door).has_value());
    EXPECT_TRUE(b->write("k3", "v", std::nullopt, door).has_value());
}

TEST(CASBackendPrimitives, LegacyGetRefusesAValueThatIsNotAnIncarnation)
{
    /// `read` hands back whatever the store said, malformed included -- settling that is the caller's.
    /// The legacy forwarder has no caller to settle it: it would hand the value on as a `Token` that
    /// the next conditional operation refuses as a caller bug, one layer too late to name the key.
    struct EmptyValueBackend : InMemoryBackend
    {
        std::optional<Raw> read(const String &, TransportAccess &) override { return Raw{"body", ""}; }
    };
    auto b = std::make_shared<EmptyValueBackend>();
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { b->get("k"); });
}

namespace
{

/// A double whose fault injection is written against the LEGACY surface, the way almost every one in
/// this suite is. A `Pool` hands its callers a decorator, so a legacy call reaches the decorator
/// first: if the decorator converted it to a primitive before forwarding, this override would never
/// run and the injection would be silently dead.
struct LegacyCasPutFlaggingBackend : DB::Cas::tests::CountingBackend
{
    bool legacy_cas_put_ran = false;

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        legacy_cas_put_ran = true;
        return CountingBackend::casPut(key, bytes, expected, meta);
    }
};

}

TEST(CASBackendPrimitives, InstrumentedBackendPassesALegacyCallThroughAsLegacy)
{
    auto inner = std::make_shared<LegacyCasPutFlaggingBackend>();
    InstrumentedBackend instrumented(inner);
    EXPECT_EQ(instrumented.casPut("k", "v", std::nullopt).outcome, CasOutcome::Committed);
    EXPECT_TRUE(inner->legacy_cas_put_ran);
    EXPECT_EQ(inner->casPutCount("k"), 1u);
}

TEST(CASBackendPrimitives, RefreshCredentialsIsOffUntilAskedFor)
{
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_FALSE(b->refreshCredentials());
    b->setRefreshCredentialsResult(true);
    EXPECT_TRUE(b->refreshCredentials());
}

#if USE_AWS_S3

TEST(CASThrottlingBackend, FirstPerKeyRefusesOnceThenForwards)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 0, 429);
    auto key = RawDoor::key();
    EXPECT_THROW(t->read("k", key), DB::S3Exception);
    EXPECT_NO_THROW(t->read("k", key));
    EXPECT_EQ(t->refusals("k"), 1u);
    EXPECT_THROW(t->write("k2", "v", std::nullopt, key), DB::S3Exception);
    EXPECT_TRUE(t->write("k2", "v", std::nullopt, key).has_value());
}

TEST(CASThrottlingBackend, RefusalsAreRetryableUnderBothStatuses)
{
    auto key = RawDoor::key();
    for (const int status : {429, 503})
    {
        auto t = std::make_shared<ThrottlingBackend>(
            std::make_shared<InMemoryBackend>(), ThrottlingBackend::Mode::FirstPerKey, 0, status);
        try
        {
            t->head("k", key);
            FAIL() << "expected a refusal for status " << status;
        }
        catch (const DB::S3Exception & e)
        {
            /// The property the seam exists for: the engine must see an AMBIGUOUS attempt, which is
            /// what a retryable store error means, not a definite failure.
            EXPECT_TRUE(e.isRetryableError()) << "status " << status;
        }
    }
}

TEST(CASThrottlingBackend, PassesALegacyCallThroughAsLegacy)
{
    auto inner = std::make_shared<LegacyCasPutFlaggingBackend>();
    /// A period no call here reaches, so nothing is refused: what this pins is the pass-through.
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 1000, 503);
    EXPECT_EQ(t->casPut("k", "v", std::nullopt).outcome, CasOutcome::Committed);
    EXPECT_TRUE(inner->legacy_cas_put_ran);
    EXPECT_EQ(inner->casPutCount("k"), 1u);
}

TEST(CASThrottlingBackend, EveryNthRefusesOnThePeriodAcrossKeys)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 3, 503);
    auto key = RawDoor::key();
    EXPECT_NO_THROW(t->read("a", key));
    EXPECT_NO_THROW(t->read("b", key));
    EXPECT_THROW(t->read("c", key), DB::S3Exception);     /// the third request, whatever it names
    EXPECT_EQ(t->refusals("c"), 1u);
    EXPECT_EQ(t->refusals("a"), 0u);
    EXPECT_NO_THROW(t->read("c", key));
}

#endif

/// ================================================================================================
/// The request engine
/// ================================================================================================

namespace
{

/// A type nothing in the engine catches, so a `decide` that throws it can only reach the caller by
/// propagating unchanged.
struct DecideMarker
{
};

/// Answers the FIRST remove with a mismatch without reaching the store, so `removeCurrent` has to
/// re-observe. Counts its own requests: an answer given here never reaches the counting base.
struct MismatchOnceOnRemoveBackend : InMemoryBackend
{
    using InMemoryBackend::head;

    size_t heads = 0;
    size_t removes = 0;
    bool refuse_next_remove = true;

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        ++heads;
        return InMemoryBackend::head(key, access);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        ++removes;
        if (std::exchange(refuse_next_remove, false))
            return RawRemoval::Mismatch;
        return InMemoryBackend::remove(key, expected_value, access);
    }
};

/// Answers `Indeterminate` for its first `indeterminate_answers` probes, then delegates -- a store
/// briefly out of reach, whose absence was never established.
struct IndeterminateProbeBackend : InMemoryBackend
{
    using Backend::probeSentinelRaw;

    size_t probes = 0;
    size_t indeterminate_answers = 2;

    SentinelProbeResult probeSentinelRaw(const String & key, TransportAccess & access) override
    {
        if (++probes <= indeterminate_answers)
            return {ProbeOutcome::Indeterminate, std::nullopt};
        return InMemoryBackend::probeSentinelRaw(key, access);
    }
};

/// Refuses the FIRST `list` naming each distinct cursor -- one refusal per page -- and charges every
/// list a fixed slice of the caller's clock, so what a page costs is a fact rather than a jitter draw.
/// `always_refuse_cursor` keeps one page refused for good.
struct PagedThrottleBackend : InMemoryBackend
{
    using InMemoryBackend::list;

    std::function<void()> charge_latency;
    std::set<String> refused_cursors;
    std::optional<String> always_refuse_cursor;
    size_t list_calls = 0;

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        ++list_calls;
        if (charge_latency)
            charge_latency();
        if ((always_refuse_cursor && *always_refuse_cursor == cursor) || refused_cursors.insert(cursor).second)
            throw Poco::TimeoutException("the list resuming after '" + cursor + "' timed out");
        return InMemoryBackend::list(prefix, cursor, limit, access);
    }
};

/// Runs `on_read` after every read. The resolve read is where a caller's own facts can change
/// between an attempt and the pause that would precede the next one.
struct FlipOnReadBackend : CountingBackend
{
    std::function<void()> on_read;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = CountingBackend::read(key, access);
        if (on_read)
            on_read();
        return raw;
    }
};

}

TEST(CASIncarnation, RenderAndPersistedCompare)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const Incarnation first = *orThrow(op.create("k", "v", Retry::standard()), "create");
    EXPECT_EQ(first.render(), "emulated:1");
    EXPECT_EQ(first.key(), "k");
    EXPECT_EQ(first.dialect(), Dialect::Emulated);

    const PersistedIncarnation persisted = PersistedIncarnation::capture(first);
    EXPECT_EQ(persisted.dialect, "emulated");
    EXPECT_EQ(persisted.value, "1");
    EXPECT_TRUE(persisted.matches(first));

    const Incarnation second = *orThrow(op.replace("k", "w", first, Retry::standard()), "replace");
    EXPECT_EQ(second.render(), "emulated:2");
    EXPECT_FALSE(persisted.matches(second));   /// a captured record never re-matches a later incarnation
    EXPECT_TRUE(PersistedIncarnation::capture(second).matches(second));
}

TEST(CASRetry, BindSaturatesAndLeavesAnEqualLeaseOffTheLeaseSource)
{
    constexpr uint64_t largest = std::numeric_limits<uint64_t>::max();
    /// A window one short of the whole range, so any `now` above 1 overflows a naive addition.
    EXPECT_EQ(Retry::within(largest - 1).bind(2).deadline_ms, largest);
    EXPECT_EQ(Retry::within(largest - 1).bind(1).deadline_ms, largest);
    EXPECT_FALSE(Retry::within(largest - 1).bind(2).lease_bound);

    const uint64_t now = 1'000'000;
    /// The lease bound lands exactly on the policy deadline. The lease is taken only when it is
    /// STRICTLY smaller, so the tie belongs to the policy and `GaveUp` will not name the lease.
    const Retry::Bound tie = Retry::untilLeaseSafe(now + 92'000, 2'000).bind(now);
    EXPECT_EQ(tie.deadline_ms, now + 90'000);
    EXPECT_FALSE(tie.lease_bound);

    const Retry::Bound lease = Retry::untilLeaseSafe(now + 91'999, 2'000).bind(now);
    EXPECT_EQ(lease.deadline_ms, now + 89'999);
    EXPECT_TRUE(lease.lease_bound);
}

TEST(CASRequests, CreateThenReplaceThenRemove)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const Incarnation first = *orThrow(op.create("k", "v1", Retry::standard()), "create");
    const auto seen = op.read("k", Retry::standard());
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->bytes, "v1");
    EXPECT_EQ(seen->incarnation, first);

    const Incarnation second = *orThrow(op.replace("k", "v2", first, Retry::standard()), "replace");
    EXPECT_NE(second, first);

    EXPECT_EQ(op.remove("k", first, Retry::standard()), Removal::Mismatch);    /// the incarnation is stale
    EXPECT_EQ(op.remove("k", second, Retry::standard()), Removal::Removed);
    EXPECT_EQ(op.remove("k", second, Retry::standard()), Removal::Gone);
    EXPECT_FALSE(op.read("k", Retry::standard()).has_value());
}

/// An incarnation observed for one key is refused as the precondition for another, before the write
/// loop starts anything. Constructing a `LOGICAL_ERROR` exception ABORTS under a debug or sanitizer
/// build, so the same contract is asserted there as a death expectation; both forms pin that the
/// refusal happens, and the non-death form additionally pins that it costs no request.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRequests, KeyBindingThrowsBeforeAnyRequest)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Incarnation of_a = *orThrow(op.create("a", "v", Retry::standard()), "create");
    backend->resetCounts();

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { (void)op.replace("b", "w", of_a, Retry::standard()); });
    EXPECT_EQ(backend->writeRequests(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}
#else
TEST(CASRequestsDeathTest, KeyBindingThrowsBeforeAnyRequest)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Incarnation of_a = *orThrow(op.create("a", "v", Retry::standard()), "create");

    EXPECT_DEATH({ (void)op.replace("b", "w", of_a, Retry::standard()); }, "");
}
#endif

TEST(CASRequests, EveryConflictIsSettledByOneReadAndCarriesTheOccupant)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "theirs", Retry::standard()), "create");
    backend->resetCounts();

    WriteResult result = op.create("k", "mine", Retry::once());
    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    const auto * occupant = std::get_if<Object>(&conflict->seen);
    ASSERT_NE(occupant, nullptr);
    EXPECT_EQ(occupant->bytes, "theirs");

    /// The refused precondition says only that the key is taken; ONE exact read says by whom.
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 1u);
}

TEST(CASRequests, AmbiguousCreateThatLandedIsCommittedByTheResolveRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    /// The object becomes durable and THEN the response is lost, so the store holds bytes the caller
    /// never learned it wrote -- the only ambiguity a resolve read can settle as a commit.
    backend->injectAmbiguousLandedWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_TRUE(committed->resolved_by_read);
    EXPECT_EQ(committed->attempts_sent, 1u);
    /// Settled by reading, never by writing again: a second create would have conflicted with the
    /// first one's own object.
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, AmbiguousCreateThatNeverLandedIsReissued)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousPutIfAbsent("k");   /// the attempt's outcome is lost and the store is untouched
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_FALSE(committed->resolved_by_read);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_EQ(backend->readRequests(), 1u);     /// the resolve proved absence, and only then did a reissue follow
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

TEST(CASRequests, OnceSendsOneWriteAndAtMostOneResolveRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("the read timed out")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::once());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    /// One attempt is one attempt, but the read that would have settled it is still owed and sent.
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, DecideMayThrowAndTheExceptionPropagatesUnchanged)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    EXPECT_THROW(
        op.readModifyWrite("k", [](const std::optional<Object> &) -> std::optional<String> { throw DecideMarker{}; },
                           Retry::standard()),
        DecideMarker);
    EXPECT_EQ(backend->writeRequests(), 0u);
    EXPECT_EQ(backend->readRequests(), 1u);   /// the key was read, and nothing was decided about it
}

TEST(CASRequests, OnPresenceIssuesHeadsAndNoGet)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> & current) -> std::optional<String>
        {
            return current ? std::nullopt : std::optional<String>("v");
        },
        Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_GE(backend->headRequests(), 1u);
}

TEST(CASRequests, OnPresenceSettlesARefusedPreconditionWithAHead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextCasPut("k");   /// the store refuses the precondition, writing nothing
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> & current) -> std::optional<String>
        {
            return current ? std::nullopt : std::optional<String>("v");
        },
        Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Committed>(result));
    /// A refused precondition needs only to know WHAT is at the key, so this loop never fetches a body.
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_GE(backend->headRequests(), 2u);
    EXPECT_EQ(backend->writeRequests(), 2u);
}

TEST(CASRequests, ForEachListedKeyStopsEarlyAndBudgetsPerPage)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    for (int i = 0; i < 25; ++i)
        orThrow(op.create("p/" + std::to_string(i), "v", Retry::standard()), "create");
    backend->resetCounts();

    size_t seen = 0;
    size_t pages = 0;
    op.forEachListedKey("p/", [&](const KeyEntry &) { return ++seen < 3; }, Retry::standard(),
                        /*page_limit=*/10, [&] { ++pages; });
    EXPECT_EQ(seen, 3u);
    /// The walk stops where the caller stops it: the remaining two pages are never fetched.
    EXPECT_EQ(pages, 1u);
    EXPECT_EQ(backend->listRequests(), 1u);
}

TEST(CASRequests, DeleteMarkerIsANamedException)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Incarnation inc = *orThrow(op.create("k", "v", Retry::standard()), "create");

    backend->setSimulateDeleteMarkers(true);
    expectThrowsCode(DB::ErrorCodes::CAS_DELETE_MARKER, [&] { (void)op.remove("k", inc, Retry::standard()); });
    EXPECT_TRUE(clock.sleeps.empty());   /// a versioned bucket answers this way every time
}

TEST(CASRequests, RemoveCurrentReObservesAMismatchAndRefusesUnderOnce)
{
    {
        FakeClock clock;
        auto backend = std::make_shared<MismatchOnceOnRemoveBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        orThrow(op.create("k", "v", Retry::standard()), "create");

        EXPECT_EQ(op.removeCurrent("k", Retry::standard()), Removal::Removed);
        /// Another incarnation became current between the observation and the delete: observe again,
        /// paced like every other reissue, and delete what the second look saw.
        EXPECT_EQ(backend->heads, 2u);
        EXPECT_EQ(backend->removes, 2u);
        EXPECT_EQ(clock.sleeps.size(), 1u);
    }
    {
        FakeClock clock;
        auto backend = std::make_shared<MismatchOnceOnRemoveBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        orThrow(op.create("k", "v", Retry::standard()), "create");

        /// `once` has no reissue with which to settle a mismatch, and this verb never hands one back.
        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.removeCurrent("k", Retry::once()); });
        EXPECT_EQ(backend->heads, 1u);
        EXPECT_EQ(backend->removes, 1u);
        EXPECT_TRUE(clock.sleeps.empty());
    }
}

TEST(CASRequests, ProbeSentinelRetriesOnlyTheIndeterminateOutcome)
{
    {
        FakeClock clock;
        auto backend = std::make_shared<IndeterminateProbeBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        orThrow(op.create("k", "v", Retry::standard()), "create");

        const SentinelProbeResult result = op.probeSentinel("k", Retry::standard());
        EXPECT_EQ(result.outcome, ProbeOutcome::Present);
        ASSERT_TRUE(result.body.has_value());
        EXPECT_EQ(*result.body, "v");
        EXPECT_EQ(backend->probes, 3u);          /// inconclusive twice, then an authoritative answer
        EXPECT_EQ(clock.sleeps.size(), 2u);
    }
    {
        FakeClock clock;
        auto backend = std::make_shared<IndeterminateProbeBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        orThrow(op.create("k", "v", Retry::standard()), "create");

        /// With no reissue left, the inconclusive outcome IS the answer: reported, never thrown.
        const SentinelProbeResult result = op.probeSentinel("k", Retry::once());
        EXPECT_EQ(result.outcome, ProbeOutcome::Indeterminate);
        EXPECT_EQ(backend->probes, 1u);
        EXPECT_TRUE(clock.sleeps.empty());
    }
}

TEST(CASRequests, AdmissionIsCheckedAtThreePoints)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto door = RawDoor::key();
    uint64_t generation = 1;
    bool lost = false;
    Fence fence{
        [&] { return generation; },
        [&](uint64_t admitted, uint64_t)
        {
            return (lost || admitted != generation) ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok;
        },
        [&](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);

    /// (1) before the first attempt, on a handle resumed under a generation the fence has moved past
    {
        auto op = requests.resume(0);
        WriteResult result = op.create("k", "v", Retry::standard());
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
        EXPECT_FALSE(backend->read("k", door).has_value());
    }
    /// (2) before the next verb of an admitted handle, after a re-arm between two verbs
    {
        auto op = requests.admit();
        EXPECT_FALSE(op.head("k", Retry::standard()).has_value());
        generation = 2;
        WriteResult result = op.create("k", "v", Retry::standard());
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
        EXPECT_FALSE(backend->read("k", door).has_value());
    }
    /// (3) after a proven commit: the write landed, then the fence tripped before the call returned
    {
        auto op = requests.admit();
        backend->onWriteCommitted("k2", [&] { lost = true; });
        WriteResult result = op.create("k2", "v", Retry::standard());
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_TRUE(gave_up->sent_any);
        /// The object IS durable. This call refuses to CLAIM it; it does not undo it.
        EXPECT_TRUE(backend->read("k2", door).has_value());
    }
}

TEST(CASRequests, TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite)
{
    FakeClock clock;
    auto backend = std::make_shared<FlipOnReadBackend>();
    bool alive = true;
    backend->on_read = [&] { alive = false; };
    backend->injectAmbiguousPutIfAbsent("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit([&] { return alive; });

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_TRUE(gave_up->sent_any);
    /// The ambiguous attempt was resolved, and the pause before the reissue was refused rather than
    /// served: no sleep, and no second attempt after it.
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 1u);
}

TEST(CASRequests, AResolveReadRefusedForLeaseBudgetIsReportedAsTheLeaseDeadline)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    bool lease_spent = false;
    Fence fence{
        [] { return uint64_t{0}; },
        [&](uint64_t, uint64_t) { return lease_spent ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
        [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    auto op = requests.admit();
    const Incarnation seen = *orThrow(op.create("k", "v", Retry::standard()), "create");

    /// The store refuses the precondition, and the lease budget is gone by the time the read that
    /// would say WHO holds the key is due. The call learned nothing about the key, so what it reports
    /// is the bound that stopped it -- not a conflict it never observed.
    backend->failNextCasPut("k");
    backend->onBeforeWrite("k", [&] { lease_spent = true; });
    const uint64_t lease_deadline = clock.now + 10'000;
    WriteResult result = op.replace("k", "w", seen, Retry::untilLeaseSafe(lease_deadline, 2'000));
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_EQ(backend->writeRequests(), 2u);   /// the create and the one refused replace
    EXPECT_EQ(backend->readRequests(), 0u);    /// the resolve read never started
}

TEST(CASRequests, AFenceWithNoBudgetForTheRequestSendsNothingAndNamesTheLease)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    const uint64_t budget_ms = 500;
    Fence fence{
        [] { return uint64_t{0}; },
        [&](uint64_t, uint64_t needed_ms) { return needed_ms > budget_ms ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
        [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    /// One attempt reserves more than the lease has left, so nothing may be started under it.
    requests.setAttemptReservationForTest(1'000);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    /// The policy's own window is untouched; what ran out is the fence's budget, which IS the lease.
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    EXPECT_FALSE(gave_up->sent_any);

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->writeRequests(), 0u);
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, AnRmwWhoseFirstReadFailsGivesUpUnresolvedWithoutWriting)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("the read timed out")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.readModifyWrite("k",
        [](const std::optional<Object> &) -> std::optional<String> { return String("v"); }, Retry::once());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    /// No BOUND refused this read; the read itself failed. Claiming a deadline the clock never reached
    /// would send its reader to widen the wrong thing.
    EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
    EXPECT_FALSE(gave_up->sent_any);
    EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    EXPECT_EQ(backend->writeRequests(), 0u);
}

TEST(CASRequests, AnOnPresenceRmwWhoseFirstHeadFailsGivesUpUnresolvedWithoutWriting)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextHeadWith("k", std::make_exception_ptr(Poco::TimeoutException("the head timed out")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> &) -> std::optional<String> { return String("v"); }, Retry::once());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
    EXPECT_FALSE(gave_up->sent_any);
    EXPECT_EQ(backend->writeRequests(), 0u);
    EXPECT_EQ(backend->readRequests(), 0u);   /// the presence loop does not fall back to a body read
}

TEST(CASRequests, AConflictWhoseResolveReadFailsIsReportedWithNothingObserved)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "theirs", Retry::standard()), "create");

    backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("the read timed out")));
    WriteResult result = op.create("k", "mine", Retry::once());
    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    /// The precondition was refused, so the key IS taken; the read that would have said by whom failed,
    /// and the caller is told exactly that rather than handed a guess about the occupant.
    EXPECT_TRUE(std::holds_alternative<NotObserved>(conflict->seen));
}

TEST(CASRequests, AFenceLostDuringTheResolveReadIsAFenceLossNotAConflict)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    bool alive = true;
    /// The fence trips while the ambiguous attempt is in flight: the write's own hook runs before the
    /// store is touched, so the resolve read is the first request to meet the closed gate.
    backend->onBeforeWrite("k", [&] { alive = false; });
    backend->injectAmbiguousPutIfAbsent("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit([&] { return alive; });

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    /// A lost fence is not an observation. Reporting it as an ordinary conflict would tell the caller
    /// somebody else holds the key, when what happened is that this node stopped being allowed to ask.
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 0u);   /// refused before the resolve read was issued
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, OnPresenceFetchesTheBodyToProveAnAmbiguousAttemptLanded)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousLandedWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> & current) -> std::optional<String>
        {
            return current ? std::nullopt : std::optional<String>("v");
        },
        Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_TRUE(committed->resolved_by_read);
    /// Presence-only is what this loop REPORTS, not a promise about what it may read: only the bytes
    /// can prove the ambiguous attempt was this call's own.
    EXPECT_EQ(backend->readRequests(), 1u);
}

TEST(CASRequests, OnPresenceReportsMetaEvenWhenItHadToFetchTheBody)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "theirs", Retry::standard()), "create");

    backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> &) -> std::optional<String> { return String("mine"); }, Retry::once());
    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    /// The ambiguity forced a body read, and the body stops at this boundary: a caller of the
    /// presence loop can never come to depend on bytes the loop does not promise.
    EXPECT_TRUE(std::holds_alternative<Meta>(conflict->seen));
    EXPECT_FALSE(std::holds_alternative<Object>(conflict->seen));
    EXPECT_GE(backend->readRequests(), 1u);
}

TEST(CASRequests, ForEachListedKeyGivesEachPageItsOwnPolicyWindow)
{
    FakeClock clock;
    auto backend = std::make_shared<PagedThrottleBackend>();
    /// Every list costs the caller 300ms, so a page's cost is a fact and not a jitter draw.
    backend->charge_latency = [&clock] { clock.now += 300; };
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    for (int i = 0; i < 25; ++i)
        orThrow(op.create("p/" + std::to_string(i), "v", Retry::standard()), "create");

    size_t seen = 0;
    size_t pages = 0;
    const uint64_t start = clock.now;
    /// A window that comfortably covers ONE page's refusal and its reissue, and could not have covered
    /// the walk: the policy governs each page, because a walk is an unbounded number of requests.
    op.forEachListedKey("p/", [&](const KeyEntry &) { ++seen; return true; }, Retry::within(1'000),
                        /*page_limit=*/10, [&] { ++pages; });
    EXPECT_EQ(seen, 25u);
    EXPECT_EQ(pages, 3u);
    EXPECT_EQ(backend->list_calls, 6u);        /// each page refused once, then delivered
    EXPECT_GT(clock.now - start, 1'000u);      /// the walk outlived the window every page was given
}

TEST(CASRequests, ForEachListedKeyThrowsRatherThanTruncateWhenAPageNeverArrives)
{
    FakeClock clock;
    auto backend = std::make_shared<PagedThrottleBackend>();
    backend->charge_latency = [&clock] { clock.now += 300; };
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    for (int i = 0; i < 25; ++i)
        orThrow(op.create("p/" + std::to_string(i), "v", Retry::standard()), "create");

    const KeyPage first = op.list("p/", "", 10, Retry::within(1'000));
    ASSERT_FALSE(first.next_cursor.empty());
    backend->always_refuse_cursor = first.next_cursor;   /// the second page never arrives
    backend->refused_cursors.clear();

    size_t seen = 0;
    size_t pages = 0;
    /// A silently truncated enumeration is the error a coverage record exists to prevent, so the walk
    /// reports the page it could not fetch instead of returning what it managed to read.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        op.forEachListedKey("p/", [&](const KeyEntry &) { ++seen; return true; }, Retry::within(1'000),
                            /*page_limit=*/10, [&] { ++pages; });
    });
    EXPECT_EQ(pages, 1u);
    EXPECT_EQ(seen, 10u);
}

TEST(CASRequests, LivenessPredicateEndsTheOperationLikeAFenceLoss)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    bool alive = true;
    auto op = requests.admit([&] { return alive; });
    EXPECT_TRUE(op.admitted());

    alive = false;
    EXPECT_FALSE(op.admitted());

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_FALSE(gave_up->sent_any);
    EXPECT_EQ(backend->writeRequests(), 0u);

    /// The read surface reports the same refusal the only way it can: by exception.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->readRequests(), 0u);
}

TEST(CASRequests, ReadModifyWriteLosesNoIncrementUnderContentionAndBoundsAHotKey)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const auto increment = [](const std::optional<Object> & current) -> std::optional<String>
    {
        return std::to_string(std::stoi(current ? current->bytes : "0") + 1);
    };

    /// The real clock and the real sleep: two threads share this engine, and a `FakeClock` would be a
    /// data race on both of its fields.
    CasRequests contended(backend, Fence::open());
    {
        auto seed = contended.admit();
        orThrow(seed.create("ctr", "0", Retry::standard()), "create");
    }
    const auto fifty_increments = [&]
    {
        auto op = contended.admit();
        for (int i = 0; i < 50; ++i)
            orThrow(op.readModifyWrite("ctr", increment, Retry::standard()), "increment");
    };
    std::thread first(fifty_increments);
    std::thread second(fifty_increments);
    first.join();
    second.join();

    auto reader = contended.admit();
    const auto counted = reader.read("ctr", Retry::standard());
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted->bytes, "100");   /// every conflict re-decided against what the resolve read saw

    /// A key rewritten under EVERY attempt is bounded by the deadline instead of looping forever.
    FakeClock clock;
    auto hot = makeRequests(backend, clock);
    bool inside_hook = false;
    backend->onBeforeWrite("ctr", [&]
    {
        if (inside_hook)   /// the hook's own write re-enters this callback
            return;
        inside_hook = true;
        auto door = RawDoor::key();
        if (auto raw = backend->read("ctr", door))
            (void)backend->write("ctr", "999", raw->value, door);
        inside_hook = false;
    });

    auto op = hot.admit();
    WriteResult result = op.readModifyWrite("ctr", increment, Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_FALSE(clock.sleeps.empty());   /// it paced its retries rather than spinning
}

TEST(CASRequests, ADeterministicLocalFailureSurfacesUnchangedWithoutAReissue)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextReadWith("k", std::make_exception_ptr(
        DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "the object at 'k' is not decodable")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    /// Reissuing would replay the same bug and bury it behind a retryable exception at the deadline.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->readRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ATransportTimeoutIsReissuedAndALocalFailureIsNot)
{
    {
        FakeClock clock;
        auto backend = std::make_shared<CountingBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        orThrow(op.create("k", "v", Retry::standard()), "create");
        backend->resetCounts();

        backend->failNextReadWith("k", std::make_exception_ptr(Poco::TimeoutException("the read timed out")));
        const auto seen = op.read("k", Retry::standard());
        ASSERT_TRUE(seen.has_value());
        EXPECT_EQ(seen->bytes, "v");
        EXPECT_EQ(backend->readRequests(), 2u);
        EXPECT_EQ(clock.sleeps.size(), 1u);
    }
    {
        FakeClock clock;
        auto backend = std::make_shared<CountingBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        /// Not a `Poco::Exception`, so it did not come from the transport: reissuing it would spend the
        /// whole deadline replaying a local bug.
        backend->failNextReadWith("k", std::make_exception_ptr(std::logic_error("a local bug")));
        EXPECT_THROW((void)op.read("k", Retry::standard()), std::logic_error);
        EXPECT_EQ(backend->readRequests(), 1u);
        EXPECT_TRUE(clock.sleeps.empty());
    }
}

#if USE_AWS_S3

namespace
{

/// An `S3Exception` carrying a canonical `<Code>` name. The name is how the request contract tells
/// one store answer from another: the SDK reports every error it does not model as `UNKNOWN`, so the
/// code alone can never stand for a particular failure.
std::exception_ptr s3Error(Aws::S3::S3Errors code, const String & name)
{
    return std::make_exception_ptr(DB::S3Exception("the store answered " + name, code, name));
}

}

TEST(CASRequests, DeadlineIsTheOnlyBoundUnderZeroLatencyThrottling)
{
    FakeClock clock;
    auto throttled = std::make_shared<ThrottlingBackend>(
        std::make_shared<InMemoryBackend>(), ThrottlingBackend::Mode::EveryNth, 1, 429);
    auto requests = makeRequests(throttled, clock);
    auto op = requests.admit();

    const uint64_t start = clock.now;
    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
    EXPECT_TRUE(gave_up->sent_any);
    /// What ends the call is the policy's own deadline, not a count of attempts: it kept issuing to
    /// within one backoff of that deadline, and paused many more times than a small fixed budget allows.
    EXPECT_GE(clock.now - start, 85'000u);
    EXPECT_GT(clock.sleeps.size(), 16u);
}

TEST(CASRequests, LeaseBoundPolicyIssuesNothingPastTheBoundary)
{
    FakeClock clock;
    auto throttled = std::make_shared<ThrottlingBackend>(
        std::make_shared<InMemoryBackend>(), ThrottlingBackend::Mode::EveryNth, 1, 429);
    auto requests = makeRequests(throttled, clock);
    requests.setAttemptReservationForTest(1'000);

    const uint64_t lease_deadline = clock.now + 10'000;
    auto op = requests.admit();
    WriteResult result = op.create("k", "v", Retry::untilLeaseSafe(lease_deadline, 2'000));
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    /// The lease was the smaller of the two bounds, and the give-up names it rather than the policy.
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    /// Nothing is STARTED that could not finish inside the bound: the last request began at least one
    /// attempt reservation before lease minus margin.
    EXPECT_LE(clock.now, lease_deadline - 2'000 - 1'000);
}

TEST(CASRequests, AMalformedRequestIsRefusedWithoutAReissue)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::UNKNOWN, "MalformedXML"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * refused = std::get_if<Refused>(&result);
    ASSERT_NE(refused, nullptr);
    EXPECT_EQ(refused->store_error, DB::ErrorCodes::S3_ERROR);
    /// The store's own answer proves the request never applied: nothing to resolve, nothing to reissue,
    /// and no credential the refusal could be about.
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_EQ(backend->refreshCredentialsCalls(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, AnAccessDenialNoRefreshCanFixIsRefusedOnTheFirstAttempt)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(false);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    ASSERT_TRUE(std::holds_alternative<Refused>(result));
    /// A refresh is asked for once and installs nothing, and THAT is what makes the denial terminal.
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ASecondCredentialAnswerAfterTheOneRefreshIsRefused)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(true);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    /// The store answers a denial BEFORE it applies anything, so neither attempt landed and no read
    /// has anything to settle. A call gets one refresh, so the denial that survives it is the answer.
    ASSERT_TRUE(std::holds_alternative<Refused>(result));
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(backend->writeRequests(), 2u);
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_EQ(clock.sleeps.size(), 1u);   /// the one paced re-send under the credentials it installed
}

TEST(CASRequests, UnderOnceTheStoreAnswerStandsEvenWhenTheRefreshSucceeded)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(true);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    /// Fresh credentials only help a reissue, and `once` has none to sign. Reporting the attempt as
    /// unresolved instead would turn a policy that sends one request into one that sleeps.
    WriteResult result = op.create("k", "v", Retry::once());
    ASSERT_TRUE(std::holds_alternative<Refused>(result));
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ACredentialAnswerAfterAnAmbiguousAttemptStillOwesTheResolveRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(true);
    /// The first attempt's fate is unknown and it may yet land; the second is a proven non-application.
    backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 3u);
    /// The refresh does not license a direct re-send here: the OTHER attempt is still unresolved, so
    /// the read that settles it is still owed.
    EXPECT_EQ(backend->readRequests(), 2u);
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
}

TEST(CASRequests, AnExpiredTokenARefreshFixesIsResentWithoutAResolveRead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(true);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID, "ExpiredToken"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_FALSE(committed->resolved_by_read);
    /// The credential answer proves its OWN attempt never applied, and no earlier attempt of this call
    /// is unresolved, so the re-send under the fresh credentials owes no read.
    EXPECT_EQ(backend->readRequests(), 0u);
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

TEST(CASRequests, AnExpiredTokenNoRefreshCanFixIsRefusedRatherThanRiddenToTheDeadline)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID, "ExpiredToken"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    /// The refusal class CONTAINS the refresh class: an expired credential that no refresh installed
    /// would otherwise spend the whole deadline being reissued.
    ASSERT_TRUE(std::holds_alternative<Refused>(result));
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(backend->writeRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ANameOnlyAccessDenialOnAReadPropagatesWhenNoRefreshIsAvailable)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->setRefreshCredentialsResult(false);
    /// Matched by NAME alone: the SDK reports this store's denial under its catch-all code, so the
    /// name is the only thing that says a credential could explain it.
    backend->failNextReadWith("k", s3Error(Aws::S3::S3Errors::UNKNOWN, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    /// One refresh is asked for and installs nothing, so nothing would sign differently: the read
    /// propagates instead of spending its policy on a request that cannot start succeeding.
    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(backend->readRequests(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, AnUnmodeledStoreErrorOnAReadIsReissuedNotSurfaced)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "v", Retry::standard()), "create");
    backend->resetCounts();

    /// An S3-compatible store's own vendor code. The SDK models it as `UNKNOWN`, which is its code for
    /// EVERY error it does not know, so it can never stand for "this will not start succeeding".
    backend->failNextReadWith("k", s3Error(Aws::S3::S3Errors::UNKNOWN, "SomeVendorCode"));
    const auto seen = op.read("k", Retry::standard());
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->bytes, "v");
    EXPECT_EQ(backend->readRequests(), 2u);
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

#endif
