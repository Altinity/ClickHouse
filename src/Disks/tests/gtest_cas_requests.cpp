#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include "cas_test_helpers.h"

#include <type_traits>

namespace DB::ErrorCodes
{
extern const int ABORTED;
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
        EXPECT_GT(mean, ceiling * 0.35) << "attempt " << attempt;   /// a mean near half the ceiling: jitter is real
        EXPECT_LT(mean, ceiling * 0.65) << "attempt " << attempt;
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
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { orThrow(WriteResult{Conflict{ProvenAbsent{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::S3_ERROR, [&] { orThrow(WriteResult{Refused{DB::ErrorCodes::S3_ERROR, "denied"}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Deadline, GaveUp::Source::Policy, true, NotObserved{}}}, "t"); });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { orThrow(WriteResult{GaveUp{GaveUp::Why::Unresolved, GaveUp::Source::Policy, true, ProvenAbsent{}}}, "t"); });
    EXPECT_ANY_THROW(orThrow(WriteResult{GaveUp{GaveUp::Why::FenceLost, GaveUp::Source::Lease, false, NotObserved{}}}, "t"));   /// throwCasTransientUnavailable's code
}

TEST(CASFence, OpenFenceAdmitsEverythingAndNeverMoves)
{
    Fence f = Fence::open();
    EXPECT_EQ(f.generation(), 0u);
    EXPECT_EQ(f.admit(0, 1'000'000), Fence::Admit::Ok);
    EXPECT_NO_THROW(f.check_or_throw(0));
}
