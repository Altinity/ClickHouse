#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include "cas_test_helpers.h"

#include "config.h"

#include <type_traits>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int CORRUPTED_DATA;
extern const int S3_ERROR;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

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
    EXPECT_TRUE(isIncarnationValue(Dialect::Generation, "123"));
    EXPECT_FALSE(isIncarnationValue(Dialect::Emulated, ""));
}

TEST(CASRetry, BackoffIsFullJitterUnderTheCap)
{
    for (uint32_t attempt = 1; attempt <= 12; ++attempt)
    {
        const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << (attempt - 1));
        uint64_t sum = 0;
        for (int i = 0; i < 1000; ++i)
        {
            const uint64_t s = Retry::backoff(attempt);
            ASSERT_LE(s, ceiling);
            sum += s;
        }
        const double mean = static_cast<double>(sum) / 1000.0;
        EXPECT_GT(mean, static_cast<double>(ceiling) * 0.35) << "attempt " << attempt;   /// a mean near half the ceiling: jitter is real
        EXPECT_LT(mean, static_cast<double>(ceiling) * 0.65) << "attempt " << attempt;
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
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { orThrow(WriteResult{Conflict{ProvenAbsent{}}}, "t"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { orThrow(WriteResult{Refused{DB::ErrorCodes::S3_ERROR, "denied"}}, "t"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Deadline, GaveUp::Source::Policy, true, NotObserved{}}}, "t"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Unresolved, GaveUp::Source::Policy, true, ProvenAbsent{}}}, "t"); });
    EXPECT_ANY_THROW(orThrow(WriteResult{GaveUp{GaveUp::Why::FenceLost, GaveUp::Source::Lease, false, NotObserved{}}}, "t"));   /// throwCasTransientUnavailable's code
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

TEST(CASBackendPrimitives, LegacyOverridesStillFireWhenTheNewPrimitiveIsCalled)
{
    /// The migration rule: new methods are the primitives; a fault-injection override written against
    /// the NEW signature intercepts a legacy caller too, because legacy forwards through the virtual.
    struct Counting : InMemoryBackend
    {
        size_t writes = 0;
        std::expected<String, RawConflict> write(const String & k, const String & v, const std::optional<String> & e, TransportAccess & a) override
        {
            ++writes;
            return InMemoryBackend::write(k, v, e, a);
        }
    };
    auto b = std::make_shared<Counting>();
    b->putIfAbsent("k", "v");                 /// legacy call
    EXPECT_EQ(b->writes, 1u);
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
