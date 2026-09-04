#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include "cas_test_helpers.h"

#include <IO/ReadHelpers.h>

#include "config.h"

#include <Poco/Exception.h>
#include <base/defines.h>

#include <atomic>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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
using DB::Cas::tests::expectBytes;
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

static_assert(!std::is_default_constructible_v<Etag>);
static_assert(!std::is_constructible_v<Etag, String>);
static_assert(!std::is_constructible_v<Etag, PersistedEtag>);
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

/// A frozen policy is ONE absolute deadline: time passing does not buy a later one, freezing again
/// cannot extend it, and the lease bound still wins when it is the smaller of the two -- which is what
/// keeps `GaveUp::Source` able to say which bound refused.
TEST(CASRetry, AFrozenPolicyIsOneDeadlineAndTheLeaseStillWins)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const uint64_t start = clock.now;
    const Retry frozen = op.freeze(Retry::standard());
    ASSERT_TRUE(frozen.policy_deadline_ms.has_value());
    EXPECT_EQ(frozen.bind(start).deadline_ms, start + 90'000);
    EXPECT_EQ(frozen.bind(start + 50'000).deadline_ms, start + 90'000);
    EXPECT_FALSE(frozen.bind(start + 50'000).lease_bound);
    /// The single-attempt view of a frozen policy keeps the deadline rather than starting a window.
    EXPECT_EQ(frozen.asSingleAttempt().policy_deadline_ms, frozen.policy_deadline_ms);
    EXPECT_TRUE(frozen.asSingleAttempt().single_attempt);

    clock.now += 50'000;
    EXPECT_EQ(op.freeze(frozen).policy_deadline_ms, frozen.policy_deadline_ms);

    const Retry::Bound leashed = op.freeze(Retry::untilLeaseSafe(start + 10'000, 2'000)).bind(clock.now);
    EXPECT_EQ(leashed.deadline_ms, start + 8'000);
    EXPECT_TRUE(leashed.lease_bound);
}

/// Freezing belongs to a loop. A single verb still gets a full window from where it is called, however
/// long its caller has already been running.
TEST(CASRequests, ALoneReadUnderTheStandardPolicyStillGetsItsFullWindow)
{
    FakeClock clock;
    auto throttled = std::make_shared<ThrottlingBackend>(
        std::make_shared<InMemoryBackend>(), ThrottlingBackend::Mode::EveryNth, 1, 429);
    auto requests = makeRequests(throttled, clock);
    auto op = requests.admit();

    clock.now += 10 * 90'000;
    const uint64_t start = clock.now;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_GE(clock.now - start, 85'000u);
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
    const Etag landed = std::get<Committed>(committed).etag;
    const auto returned = orThrow(std::move(committed), "create");
    ASSERT_TRUE(returned.has_value());
    EXPECT_EQ(*returned, landed);
    EXPECT_FALSE(orThrow(WriteResult{Declined{ProvenAbsent{}}}, "declined").has_value());

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { orThrow(WriteResult{Conflict{ProvenAbsent{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { orThrow(WriteResult{Refused{DB::ErrorCodes::S3_ERROR, "denied"}}, "t"); });
    /// Designated rather than positional: `GaveUp` grows fields at its end, and a positional list is
    /// the form a field inserted anywhere else would silently re-interpret.
    const GaveUp deadline{
        .why = GaveUp::Why::Deadline, .deadline_source = GaveUp::Source::Policy,
        .sent_any = true, .last_seen = NotObserved{}};
    const GaveUp unresolved{
        .why = GaveUp::Why::Unresolved, .deadline_source = GaveUp::Source::Policy,
        .sent_any = true, .last_seen = ProvenAbsent{}};
    const GaveUp fence_lost{
        .why = GaveUp::Why::FenceLost, .deadline_source = GaveUp::Source::Lease,
        .sent_any = false, .last_seen = NotObserved{}};
    for (const GaveUp & gave_up : {deadline, unresolved, fence_lost})
        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{gave_up}, "t"); });
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

TEST(CASBackendPrimitives, InMemoryWriteReadRemoveRoundTripThroughOneOperation)
{
    FakeClock clock;
    auto b = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(b, clock);
    auto op = requests.admit();

    const std::optional<Etag> w1 = orThrow(op.create("k", "v1", Retry::once()), "create");
    ASSERT_TRUE(w1);
    const std::optional<Object> r = op.read("k", Retry::once());
    ASSERT_TRUE(r);
    EXPECT_EQ(r->bytes, "v1");
    EXPECT_EQ(r->etag, *w1);

    const std::optional<Meta> h = op.head("k", Retry::once());
    ASSERT_TRUE(h);
    EXPECT_EQ(h->size, 2u);
    EXPECT_EQ(h->etag, *w1);

    EXPECT_TRUE(std::holds_alternative<Conflict>(op.create("k", "v2", Retry::once())));   /// must be absent
    const std::optional<Etag> w3 = orThrow(op.replace("k", "v2", *w1, Retry::once()), "replace");
    ASSERT_TRUE(w3);
    EXPECT_NE(*w3, *w1);                                  /// incarnations never repeat

    EXPECT_EQ(op.remove("k", *w1, Retry::once()), Removal::Mismatch);
    EXPECT_EQ(op.remove("k", *w3, Retry::once()), Removal::Removed);
    EXPECT_EQ(op.remove("k", *w3, Retry::once()), Removal::Gone);
    EXPECT_FALSE(op.read("k", Retry::once()).has_value());
}

TEST(CASBackendPrimitives, ListSurfacesTheIncarnationAndPaginates)
{
    FakeClock clock;
    auto b = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(b, clock);
    auto op = requests.admit();

    const std::optional<Etag> a = orThrow(op.create("p/a", "0123456789", Retry::once()), "create");
    ASSERT_TRUE(a);
    orThrow(op.create("p/b", "xy", Retry::once()), "create");
    orThrow(op.create("q/c", "z", Retry::once()), "create");

    const ListPage page = op.list("p/", "", 10, Retry::once());
    ASSERT_EQ(page.keys.size(), 2u);                      /// sorted, prefix-scoped
    EXPECT_EQ(page.keys[0].key, "p/a");
    EXPECT_EQ(page.keys[0].size, 10u);
    ASSERT_TRUE(page.keys[0].etag.has_value());
    EXPECT_EQ(*page.keys[0].etag, *a);
    EXPECT_TRUE(page.next_cursor.empty());

    const ListPage first = op.list("p/", "", 1, Retry::once());
    ASSERT_EQ(first.keys.size(), 1u);
    EXPECT_EQ(first.next_cursor, "p/a");
    const ListPage second = op.list("p/", first.next_cursor, 1, Retry::once());
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

/// The legacy verbs (`putIfAbsent`/`casPut`/`putOverwrite`) that used to forward through the primitive
/// `write` are gone -- `CasOperation` is the only caller of `Backend` now -- so that forwarding is a
/// type-level guarantee rather than a runtime check. What remains to prove is that every fault double
/// in this file that overrides `write` sees an ATTEMPT under either shape `CasOperation` can send:
/// unconditional (`create`) and Etag-conditioned (`replace`).
/// `EachWriteKnobIsKeyedAndOneShotOnThePrimitiveWrite` below covers both.

TEST(CASBackendPrimitives, EachWriteKnobIsKeyedAndOneShotOnThePrimitiveWrite)
{
    /// A knob names a KEY, not a call site: the keyed `write` every write reaches, whichever
    /// `CasOperation` verb (`create`/`replace`) issued it.
    auto b = std::make_shared<InMemoryBackend>();
    FakeClock clock;
    auto requests = makeRequests(b, clock);
    auto op = requests.admit();

    b->refuseNextWrite("k");
    EXPECT_TRUE(std::holds_alternative<Conflict>(op.create("k", "v", Retry::once())));   /// consumed here
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create("k", "v", Retry::once())));  /// and only once
    expectBytes(b, "k", "v");

    b->refuseNextWrite("k2");
    EXPECT_TRUE(std::holds_alternative<Conflict>(op.create("k2", "v", Retry::once())));
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create("k2", "v", Retry::once())));

    b->injectAmbiguousWrite("k3");
    EXPECT_TRUE(std::holds_alternative<GaveUp>(op.create("k3", "v", Retry::once())));
    EXPECT_FALSE(op.read("k3", Retry::once()).has_value()) << "an ambiguous write leaves the store untouched";
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create("k3", "v", Retry::once())));

    /// Both knobs on one key, each consumed by the next write in turn.
    b->injectAmbiguousWrite("k4");
    b->refuseNextWrite("k4");
    EXPECT_TRUE(std::holds_alternative<GaveUp>(op.create("k4", "v", Retry::once())));
    EXPECT_TRUE(std::holds_alternative<Conflict>(op.create("k4", "v", Retry::once())));
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create("k4", "v", Retry::once())));

    /// The Etag-conditioned shape: every write above was unconditional (`create`), so none of them
    /// could have caught a fault double that only intercepts `write` when it carries an
    /// `expected_value` -- the shape `replace` alone sends.
    const std::optional<Etag> k5_first = orThrow(op.create("k5", "v", Retry::once()), "create");
    ASSERT_TRUE(k5_first);
    b->refuseNextWrite("k5");
    EXPECT_TRUE(std::holds_alternative<Conflict>(op.replace("k5", "v2", *k5_first, Retry::once())))
        << "consumed here";
    const std::optional<Etag> k5_second
        = orThrow(op.replace("k5", "v2", *k5_first, Retry::once()), "replace");   /// and only once
    ASSERT_TRUE(k5_second);
    expectBytes(b, "k5", "v2");
}

TEST(CASBackendPrimitives, ReadRefusesAValueThatIsNotAnIncarnation)
{
    /// `read` hands back whatever the store said, malformed included -- `CasRequests::mint` is what
    /// refuses it, naming the key, before any caller can see it as an `Etag`.
    struct EmptyValueBackend : InMemoryBackend
    {
        std::optional<Raw> read(const String &, TransportAccess &) override { return Raw{"body", ""}; }
    };
    auto b = std::make_shared<EmptyValueBackend>();
    FakeClock clock;
    auto requests = makeRequests(b, clock);
    auto op = requests.admit();
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { op.read("k", Retry::once()); });
}

/// `InstrumentedBackendPassesALegacyCallThroughAsLegacy` pinned `InstrumentedBackend` delegating the
/// legacy `casPut` verb to its inner backend unconverted. `Backend` has no legacy verbs left --
/// `InstrumentedBackend` is a pure primitive decorator now -- and its primitive delegation (`write` and
/// every other primitive, classified and counted) is what `CASInstrumentedBackend.ClassifierAndPerNamespaceOpEvents`
/// (gtest_cas_backend.cpp) pins.

TEST(CASBackendPrimitives, RefreshCredentialsIsOffUntilAskedFor)
{
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_FALSE(b->refreshCredentials());
    b->setRefreshCredentialsResult(true);
    EXPECT_TRUE(b->refreshCredentials());
}

#if USE_AWS_S3

TEST(CASThrottlingBackend, FirstPerKeyRefusesOnceAndTheCallStillSucceeds)
{
    FakeClock clock;
    auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::FirstPerKey, 0, 429);
    auto requests = makeRequests(t, clock);
    auto op = requests.admit();

    orThrow(op.create("k2", "v", Retry::standard()), "create");
    EXPECT_EQ(t->refusals("k2"), 1u);
    EXPECT_TRUE(op.read("k2", Retry::standard()).has_value());
    EXPECT_EQ(t->refusals("k2"), 1u) << "only the FIRST request naming a key is refused";
}

TEST(CASThrottlingBackend, RefusalsAreRetryableUnderBothStatuses)
{
    /// The property the seam exists for: a refusal must reach the engine as an AMBIGUOUS attempt, not
    /// a definite failure. What proves it is that the engine REISSUES -- a definite failure would
    /// surface unchanged, with the refusal still the only request the store ever saw.
    for (const int status : {429, 503})
    {
        FakeClock clock;
        auto t = std::make_shared<ThrottlingBackend>(
            std::make_shared<InMemoryBackend>(), ThrottlingBackend::Mode::FirstPerKey, 0, status);
        auto requests = makeRequests(t, clock);
        auto op = requests.admit();

        EXPECT_FALSE(op.head("k", Retry::standard()).has_value()) << "status " << status;
        EXPECT_EQ(t->refusals("k"), 1u) << "status " << status;
    }
}

/// `PassesALegacyCallThroughAsLegacy` pinned `ThrottlingBackend` delegating the legacy `casPut` verb
/// unconverted. `Backend` has no legacy verbs left; `ThrottlingBackend`'s primitive pass-through is
/// pinned by `FirstPerKeyRefusesOnceAndTheCallStillSucceeds` above and `EveryNthRefusesOnThePeriodAcrossKeys`
/// below, both of which drive it through `CasOperation`.

TEST(CASThrottlingBackend, EveryNthRefusesOnThePeriodAcrossKeys)
{
    FakeClock clock;
    auto inner = std::make_shared<InMemoryBackend>();
    auto t = std::make_shared<ThrottlingBackend>(inner, ThrottlingBackend::Mode::EveryNth, 3, 503);
    auto requests = makeRequests(t, clock);
    auto op = requests.admit();

    EXPECT_FALSE(op.read("a", Retry::standard()).has_value());
    EXPECT_FALSE(op.read("b", Retry::standard()).has_value());
    /// The THIRD request is refused whatever it names; the engine reissues it as the fourth.
    EXPECT_FALSE(op.read("c", Retry::standard()).has_value());
    EXPECT_EQ(t->refusals("c"), 1u);
    EXPECT_EQ(t->refusals("a"), 0u);
    EXPECT_EQ(t->refusals("b"), 0u);
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

    const Etag first = *orThrow(op.create("k", "v", Retry::standard()), "create");
    EXPECT_EQ(first.render(), "emulated:1");
    EXPECT_EQ(first.key(), "k");
    EXPECT_EQ(first.dialect(), Dialect::Emulated);

    const PersistedEtag persisted = PersistedEtag::capture(first);
    EXPECT_EQ(persisted.dialect, "emulated");
    EXPECT_EQ(persisted.value, "1");
    EXPECT_TRUE(persisted.matches(first));

    const Etag second = *orThrow(op.replace("k", "w", first, Retry::standard()), "replace");
    EXPECT_EQ(second.render(), "emulated:2");
    EXPECT_FALSE(persisted.matches(second));   /// a captured record never re-matches a later incarnation
    EXPECT_TRUE(PersistedEtag::capture(second).matches(second));
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

    const Etag first = *orThrow(op.create("k", "v1", Retry::standard()), "create");
    const auto seen = op.read("k", Retry::standard());
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->bytes, "v1");
    EXPECT_EQ(seen->etag, first);

    const Etag second = *orThrow(op.replace("k", "v2", first, Retry::standard()), "replace");
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
    const Etag of_a = *orThrow(op.create("a", "v", Retry::standard()), "create");
    backend->resetCounts();

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { (void)op.replace("b", "w", of_a, Retry::standard()); });
    EXPECT_EQ(backend->writeTotal(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}
#else
TEST(CASRequestsDeathTest, KeyBindingThrowsBeforeAnyRequest)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag of_a = *orThrow(op.create("a", "v", Retry::standard()), "create");

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
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 1u);
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
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, AmbiguousCreateThatNeverLandedIsReissued)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->injectAmbiguousWrite("k");   /// the attempt's outcome is lost and the store is untouched
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_FALSE(committed->resolved_by_read);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_EQ(backend->getTotal(), 1u);     /// the resolve proved absence, and only then did a reissue follow
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
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 1u);
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
    EXPECT_EQ(backend->writeTotal(), 0u);
    EXPECT_EQ(backend->getTotal(), 1u);   /// the key was read, and nothing was decided about it
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
    EXPECT_EQ(backend->getTotal(), 0u);
    /// One HEAD decided it and one write landed it: the loop issues no request it does not need.
    EXPECT_EQ(backend->headTotal(), 1u);
    EXPECT_EQ(backend->writeTotal(), 1u);
}

TEST(CASRequests, OnPresenceSettlesARefusedPreconditionWithAHead)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    backend->refuseNextWrite("k");   /// the store refuses the precondition, writing nothing
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
    EXPECT_EQ(backend->getTotal(), 0u);
    EXPECT_EQ(backend->headTotal(), 2u);
    EXPECT_EQ(backend->writeTotal(), 2u);
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
    op.forEachListedKey("p/", [&](const ListedKey &) { return ++seen < 3; }, Retry::standard(),
                        /*page_limit=*/10, [&] { ++pages; });
    EXPECT_EQ(seen, 3u);
    /// The walk stops where the caller stops it: the remaining two pages are never fetched.
    EXPECT_EQ(pages, 1u);
    EXPECT_EQ(backend->listTotal(), 1u);
}

TEST(CASRequests, DeleteMarkerIsANamedException)
{
    FakeClock clock;
    auto backend = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag inc = *orThrow(op.create("k", "v", Retry::standard()), "create");

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
    /// The store is observed through an OPEN fence: these checks run while the subject's own fence is
    /// closed, and a fenced read would report the fence rather than the store.
    auto observer_requests = makeRequests(backend, clock);
    auto observer = observer_requests.admit();

    /// (1) before the first attempt, on a handle resumed under a generation the fence has moved past
    {
        auto op = requests.resume(0);
        WriteResult result = op.create("k", "v", Retry::standard());
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
        EXPECT_FALSE(gave_up->sent_any);
        EXPECT_FALSE(observer.read("k", Retry::once()).has_value());
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
        EXPECT_FALSE(observer.read("k", Retry::once()).has_value());
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
        EXPECT_TRUE(observer.read("k2", Retry::once()).has_value());
    }
}

TEST(CASRequests, TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite)
{
    FakeClock clock;
    auto backend = std::make_shared<FlipOnReadBackend>();
    bool alive = true;
    backend->on_read = [&] { alive = false; };
    backend->injectAmbiguousWrite("k");
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
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 1u);
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
    const Etag seen = *orThrow(op.create("k", "v", Retry::standard()), "create");

    /// The store refuses the precondition, and the lease budget is gone by the time the read that
    /// would say WHO holds the key is due. The call learned nothing about the key, so what it reports
    /// is the bound that stopped it -- not a conflict it never observed.
    backend->refuseNextWrite("k");
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
    EXPECT_EQ(backend->writeTotal(), 2u);   /// the create and the one refused replace
    EXPECT_EQ(backend->getTotal(), 0u);    /// the resolve read never started
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
    EXPECT_EQ(backend->writeTotal(), 0u);
    EXPECT_EQ(backend->getTotal(), 0u);
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
    EXPECT_EQ(backend->writeTotal(), 0u);
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
    EXPECT_EQ(backend->writeTotal(), 0u);
    EXPECT_EQ(backend->getTotal(), 0u);   /// the presence loop does not fall back to a body read
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
    backend->injectAmbiguousWrite("k");
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit([&] { return alive; });

    WriteResult result = op.create("k", "v", Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    /// A lost fence is not an observation. Reporting it as an ordinary conflict would tell the caller
    /// somebody else holds the key, when what happened is that this node stopped being allowed to ask.
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 0u);   /// refused before the resolve read was issued
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
    EXPECT_EQ(backend->getTotal(), 1u);
}

TEST(CASRequests, OnPresenceReportsMetaEvenWhenItHadToFetchTheBody)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto competitor = makeRequests(backend, clock);
    auto rival = competitor.admit();

    /// A competitor takes the key while our own create is in flight, and that create's own fate is
    /// lost. The ambiguity is armed from inside the hook so the competitor's write cannot consume it.
    bool staged = false;
    std::optional<Etag> rival_etag;
    backend->onBeforeWrite("k", [&]
    {
        if (staged)
            return;
        staged = true;
        const WriteResult rival_result = rival.create("k", "theirs", Retry::once());
        const auto * rival_committed = std::get_if<Committed>(&rival_result);
        ASSERT_NE(rival_committed, nullptr);
        rival_etag = rival_committed->etag;
        backend->injectAmbiguousWrite("k");
    });

    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    WriteResult result = op.readModifyWriteOnPresence("k",
        [](const std::optional<Meta> &) -> std::optional<String> { return String("mine"); }, Retry::once());
    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    /// The ambiguity forced a body read, and the body stops at this boundary: a caller of the
    /// presence loop can never come to depend on bytes the loop does not promise. `get_if<Meta>` plus
    /// its field checks, not a bare `holds_alternative`: a variant that already proved it holds `Meta`
    /// cannot also hold `Object`, so the field checks are what a regression could actually fail --
    /// proving the observed Meta is the RIVAL's own committed incarnation, not some other object.
    ASSERT_TRUE(rival_etag.has_value());
    const auto * meta_seen = std::get_if<Meta>(&conflict->seen);
    ASSERT_NE(meta_seen, nullptr);
    EXPECT_EQ(meta_seen->etag, *rival_etag);
    EXPECT_EQ(meta_seen->size, String("theirs").size());
    EXPECT_EQ(backend->getTotal(), 1u);
}

TEST(CASRequests, AmbiguousReplaceWhoseResolveShowsThePreconditionUnchangedIsReissued)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag seen = *orThrow(op.create("k", "v1", Retry::standard()), "create");
    backend->resetCounts();

    /// The attempt's fate is lost and the store is untouched. The incarnation it named is still the
    /// current one -- which proves nothing landed, and leaves a precondition a reissue can still meet.
    backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
    WriteResult result = op.replace("k", "v2", seen, Retry::standard());
    const auto * committed = std::get_if<Committed>(&result);
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->attempts_sent, 2u);
    EXPECT_FALSE(committed->resolved_by_read);
    EXPECT_EQ(backend->writeTotal(), 2u);
    EXPECT_EQ(backend->getTotal(), 1u);   /// exactly one resolve read, and it settled the ambiguity
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

TEST(CASRequests, AmbiguousReplaceOfIdenticalBytesIsReissuedNotClaimedByByteEquality)
{
    /// The key already holds exactly the bytes we are about to write, so byte equality alone can never
    /// say whether the ambiguous attempt landed. The incarnation can: an attempt that applied would
    /// have moved it. Under a policy with a reissue that means re-sending; under `once` it means saying
    /// the write is unresolved rather than claiming somebody else's identical object.
    {
        FakeClock clock;
        auto backend = std::make_shared<CountingBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        const Etag seen = *orThrow(op.create("k", "B", Retry::standard()), "create");
        backend->resetCounts();

        backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
        WriteResult result = op.replace("k", "B", seen, Retry::standard());
        const auto * committed = std::get_if<Committed>(&result);
        ASSERT_NE(committed, nullptr);
        /// Claiming the resolve read's object would have reported one attempt and a commit this call
        /// never made; the reissue is what actually put these bytes there under a new incarnation.
        EXPECT_EQ(committed->attempts_sent, 2u);
        EXPECT_FALSE(committed->resolved_by_read);
        EXPECT_NE(committed->etag, seen);
        EXPECT_EQ(backend->writeTotal(), 2u);
        EXPECT_EQ(backend->getTotal(), 1u);
        EXPECT_EQ(clock.sleeps.size(), 1u);
    }
    {
        FakeClock clock;
        auto backend = std::make_shared<CountingBackend>();
        auto requests = makeRequests(backend, clock);
        auto op = requests.admit();
        const Etag seen = *orThrow(op.create("k", "B", Retry::standard()), "create");
        backend->resetCounts();

        backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
        WriteResult result = op.replace("k", "B", seen, Retry::once());
        const auto * gave_up = std::get_if<GaveUp>(&result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
        EXPECT_TRUE(gave_up->sent_any);
        EXPECT_EQ(backend->writeTotal(), 1u);
        EXPECT_EQ(backend->getTotal(), 1u);
        EXPECT_TRUE(clock.sleeps.empty());
    }
}

TEST(CASRequests, AmbiguousReplaceWhoseResolveShowsAnotherIncarnationIsAConflict)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    const Etag stale = *orThrow(op.create("k", "v1", Retry::standard()), "create");
    orThrow(op.replace("k", "theirs", stale, Retry::standard()), "the competitor's replace");
    backend->resetCounts();

    /// The attempt's fate is lost, and the key has moved past the incarnation it named: no reissue of
    /// it could ever apply, so the ambiguity is settled and the occupant is the answer.
    backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("the write timed out")));
    WriteResult result = op.replace("k", "mine", stale, Retry::standard());
    const auto * conflict = std::get_if<Conflict>(&result);
    ASSERT_NE(conflict, nullptr);
    const auto * occupant = std::get_if<Object>(&conflict->seen);
    ASSERT_NE(occupant, nullptr);
    EXPECT_EQ(occupant->bytes, "theirs");
    EXPECT_NE(occupant->etag, stale);
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 1u);
    /// The count the conflict reports is the count the transport saw, not a constant that happens to
    /// match here: a caller totalling attempts across endings has to be able to add this one.
    EXPECT_EQ(conflict->attempts_sent, backend->writeTotal());
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ReadModifyWriteDoesNotClaimACompetitorsIdenticalBytesAfterAnEarlierAmbiguity)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto competitor = makeRequests(backend, clock);
    auto rival = competitor.admit();

    /// The competitor moves the key once before each of our two attempts, and its own writes re-enter
    /// this hook. The ambiguity is armed here rather than up front so the competitor's create cannot
    /// consume the arming meant for ours.
    bool inside = false;
    int staged = 0;
    backend->onBeforeWrite("k", [&]
    {
        if (inside)
            return;
        inside = true;
        if (staged == 0)
        {
            (void)rival.create("k", "X", Retry::once());
            backend->injectAmbiguousWrite("k");
        }
        else if (staged == 1)
        {
            /// The bytes we are about to send, under an incarnation that is not ours.
            if (const auto current = rival.read("k", Retry::once()))
                (void)rival.replace("k", "B", current->etag, Retry::once());
        }
        ++staged;
        inside = false;
    });

    std::vector<String> decided_on;
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    WriteResult result = op.readModifyWrite("k",
        [&](const std::optional<Object> & current) -> std::optional<String>
        {
            decided_on.push_back(current ? current->bytes : String("<absent>"));
            if (!current)
                return String("A");
            if (current->bytes == "X")
                return String("B");
            return std::nullopt;
        },
        Retry::standard());

    /// The only ambiguity this call had belonged to "A", and the competitor's "X" already proved it
    /// dead. "B" at the key is the competitor's, so the loop re-decides on it instead of claiming it.
    const auto * declined = std::get_if<Declined>(&result);
    ASSERT_NE(declined, nullptr);
    const auto * seen = std::get_if<Object>(&declined->seen);
    ASSERT_NE(seen, nullptr);
    EXPECT_EQ(seen->bytes, "B");
    EXPECT_EQ(decided_on, (std::vector<String>{"<absent>", "X", "B"}));
}

TEST(CASRequests, AnUnmodeledLocalExceptionOnAWritePropagatesUnchanged)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    /// Not a `Poco::Exception`, so it did not come from the transport and cannot have landed anything.
    /// Settling it by a read would report a store answer the store never gave.
    backend->failNextWriteWith("k", std::make_exception_ptr(std::logic_error("a local bug")));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    EXPECT_THROW((void)op.create("k", "v", Retry::standard()), std::logic_error);
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
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
    op.forEachListedKey("p/", [&](const ListedKey &) { ++seen; return true; }, Retry::within(1'000),
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

    const ListPage first = op.list("p/", "", 10, Retry::within(1'000));
    ASSERT_FALSE(first.next_cursor.empty());
    backend->always_refuse_cursor = first.next_cursor;   /// the second page never arrives
    backend->refused_cursors.clear();

    size_t seen = 0;
    size_t pages = 0;
    /// A silently truncated enumeration is the error a coverage record exists to prevent, so the walk
    /// reports the page it could not fetch instead of returning what it managed to read.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        op.forEachListedKey("p/", [&](const ListedKey &) { ++seen; return true; }, Retry::within(1'000),
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
    EXPECT_EQ(backend->writeTotal(), 0u);

    /// The read surface reports the same refusal the only way it can: by exception.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->getTotal(), 0u);
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
    auto competitor = makeRequests(backend, clock);
    auto rival = competitor.admit();
    bool inside_hook = false;
    backend->onBeforeWrite("ctr", [&]
    {
        if (inside_hook)   /// the hook's own write re-enters this callback
            return;
        inside_hook = true;
        if (const auto current = rival.read("ctr", Retry::once()))
            (void)rival.replace("ctr", "999", current->etag, Retry::once());
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
    EXPECT_EQ(backend->getTotal(), 1u);
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
        EXPECT_EQ(backend->getTotal(), 2u);
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
        EXPECT_EQ(backend->getTotal(), 1u);
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

/// Answers EVERY read with the same store error. A classification that terminates on an error is then
/// visible as a single attempt, and one that keeps the error ambiguous as a policy spent to its
/// deadline -- which a one-shot arming could never tell apart.
class AlwaysFailingReadBackend final : public CountingBackend
{
public:
    explicit AlwaysFailingReadBackend(std::exception_ptr error_) : error(std::move(error_)) {}

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        (void)CountingBackend::read(key, access);
        std::rethrow_exception(error);
    }

private:
    std::exception_ptr error;
};

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
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 0u);
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
    EXPECT_EQ(backend->writeTotal(), 1u);
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
    const auto * refused = std::get_if<Refused>(&result);
    ASSERT_NE(refused, nullptr);
    EXPECT_EQ(backend->refreshCredentialsCalls(), 1u);
    EXPECT_EQ(backend->writeTotal(), 2u);
    EXPECT_EQ(backend->getTotal(), 0u);
    /// BOTH attempts are counted, not just the one that produced the answer -- which is why this is
    /// asserted on the refusal that took two rather than on one of the single-attempt refusals.
    EXPECT_EQ(refused->attempts_sent, backend->writeTotal());
    EXPECT_EQ(clock.sleeps.size(), 1u);   /// the one paced re-send under the credentials it installed
}

TEST(CASRequests, UnderOnceACredentialAnswerIsRefusedWithoutARefresh)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    /// A refresh that WOULD have installed credentials, so the zero below is the gate and not the
    /// storage refusing to hand any back.
    backend->setRefreshCredentialsResult(true);
    backend->failNextWriteWith("k", s3Error(Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    /// Fresh credentials only help a reissue, and `once` has none to sign, so none are asked for --
    /// which is what keeps `Refused` meaning "no refresh installed credentials and no earlier
    /// ambiguity" rather than "a refresh helped and the answer stood anyway".
    WriteResult result = op.create("k", "v", Retry::once());
    ASSERT_TRUE(std::holds_alternative<Refused>(result));
    EXPECT_EQ(backend->refreshCredentialsCalls(), 0u);
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 0u);
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
    EXPECT_EQ(backend->getTotal(), 2u);
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
    EXPECT_EQ(backend->getTotal(), 0u);
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
    EXPECT_EQ(backend->writeTotal(), 1u);
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
    EXPECT_EQ(backend->getTotal(), 1u);
    EXPECT_TRUE(clock.sleeps.empty());
}

TEST(CASRequests, ReadModifyWriteWhoseResolveAndFreshObservationBothFailGivesUpUnresolved)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "v0", Retry::standard()), "create");
    backend->resetCounts();

    /// The store refuses the precondition, and both reads that would settle what happened answer with
    /// a store refusal the read loop surfaces at once rather than reissuing. They are armed from inside
    /// the write so the loop's OWN first read still succeeds and `decide` sees the object.
    backend->refuseNextWrite("k");
    bool armed = false;
    backend->onBeforeWrite("k", [&]
    {
        if (armed)
            return;
        armed = true;
        backend->failNextReadWith("k", s3Error(Aws::S3::S3Errors::UNKNOWN, "MalformedXML"));
        backend->failNextReadWith("k", s3Error(Aws::S3::S3Errors::UNKNOWN, "MalformedXML"));
    });

    WriteResult result = op.readModifyWrite("k",
        [](const std::optional<Object> &) -> std::optional<String> { return String("v1"); }, Retry::standard());
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    /// No BOUND refused either read -- the reads themselves failed -- so naming a deadline the clock
    /// never reached would send its reader to widen the wrong thing, and nothing was observed to
    /// report as a conflict.
    EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
    /// The write count is unchanged after the first attempt: nothing ever said another one was safe.
    EXPECT_EQ(backend->writeTotal(), 1u);
    EXPECT_EQ(backend->getTotal(), 3u);
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

/// A missing bucket is an ANSWER the store gave, but not an answer about the object: an S3-compatible
/// store that transiently misroutes a bucket says exactly this, and a read that ended on it would turn
/// an availability blip into a hard failure. It stays in the ambiguous class -- reissued until the
/// policy's deadline -- like a throttle or a 5xx.
TEST(CASRequests, AMissingBucketOnAReadIsReissuedToTheDeadline)
{
    FakeClock clock;
    auto backend = std::make_shared<AlwaysFailingReadBackend>(
        s3Error(Aws::S3::S3Errors::NO_SUCH_BUCKET, "NoSuchBucket"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    const uint64_t start = clock.now;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_GT(backend->getTotal(), 1u) << "the read ended on its first attempt instead of reissuing";
    EXPECT_GE(clock.now - start, 85'000u) << "the policy's own deadline is what must end this read";
}

/// The kept half of the same classification: a key miss IS an answer about the object, so reissuing it
/// only replays the same authoritative absence until the deadline. One attempt, no pause.
TEST(CASRequests, AnAuthoritativeKeyMissOnAReadEndsTheCallAtOnce)
{
    FakeClock clock;
    auto backend = std::make_shared<AlwaysFailingReadBackend>(
        s3Error(Aws::S3::S3Errors::NO_SUCH_KEY, "NoSuchKey"));
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();

    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { (void)op.read("k", Retry::standard()); });
    EXPECT_EQ(backend->getTotal(), 1u);
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
    EXPECT_EQ(backend->getTotal(), 2u);
    EXPECT_EQ(clock.sleeps.size(), 1u);
}

#endif

/// A write reserves TWO request envelopes, not one: the attempt, and the read that settles it if the
/// attempt comes back ambiguous. At exactly one reservation of surplus before lease minus margin there
/// is room for the attempt alone, and an engine that reserved only the attempt would start one it
/// could not settle inside the bound. Nothing may be sent.
TEST(CASRequests, AWriteReservesTwoEnvelopesSoOneOfSurplusStartsNothing)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    requests.setAttemptReservationForTest(1'000);

    const uint64_t lease_deadline = clock.now + 2'000 + 1'000;
    auto op = requests.admit();
    WriteResult result = op.create("k", "v", Retry::untilLeaseSafe(lease_deadline, 2'000));
    const auto * gave_up = std::get_if<GaveUp>(&result);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::Deadline);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Lease);
    EXPECT_FALSE(gave_up->sent_any);
    EXPECT_EQ(backend->writeTotal(), 0u);
    EXPECT_EQ(backend->getTotal(), 0u);
    EXPECT_TRUE(clock.sleeps.empty());
}


/// The body of a streamed object is read at the consumer's pace, long after the opening attempt
/// returned; the wrapper re-admits it at every refill. The window the open already loaded is served
/// first -- the SDK buffer arrives with pending data -- and the check first fires on advancing past it.
TEST(CASRequests, StreamBodyKeepsThePreloadedWindowAndRefusesOnTheNextRefill)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    std::atomic<bool> torn_down{false};
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return torn_down.load() ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
                [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);   /// the body arrives as "0123", "4567", "89"

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    EXPECT_EQ(first, "0123") << "the window the open already loaded is served, not skipped";

    torn_down.store(true);
    char c;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { body->readStrict(&c, 1); });
    EXPECT_TRUE(body->isCanceled()) << "a refused refill leaves the buffer the consumer holds unusable";
}

TEST(CASRequests, StreamBodyServesEveryWindowThenEof)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String all;
    DB::readStringUntilEOF(all, *body);
    EXPECT_EQ(all, "0123456789");
    EXPECT_TRUE(body->eof());
    EXPECT_FALSE(op.stream("absent", Retry::once())) << "an absent object is still the open's answer";
}

/// The mount plane's fence can answer `NoBudget`; a body refused for that reason must read like a
/// refused open on the same plane -- the retry-later class, not a tripped fence.
TEST(CASRequests, StreamBodyRefusalKeepsTheNoBudgetMapping)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    std::atomic<bool> out_of_budget{false};
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return out_of_budget.load() ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
                [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    out_of_budget.store(true);
    char c;
    try
    {
        body->readStrict(&c, 1);
        FAIL() << "the refill must be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_NE(e.message().find("no lease budget"), String::npos) << e.message();
    }
}

/// The caller's liveness is the second half of admission for the body too, in the gate's order.
TEST(CASRequests, StreamBodyHonoursTheCallersLiveness)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    std::atomic<bool> alive{true};
    auto op = requests.admit([&] { return alive.load(); });
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    alive.store(false);
    char c;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { body->readStrict(&c, 1); });
}
