#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h>
#include <Common/Exception.h>

#include <functional>
#include <limits>
#include <optional>

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

namespace
{

/// A `CasMountRuntime` with nothing running on it: no renewer, no workers, an injected boot clock and a
/// fence the test arms by hand. Enough to exercise admission, which reads only the fence's own state.
class RuntimeFixture
{
public:
    explicit RuntimeFixture(uint64_t lease_safety_margin_ms, uint64_t attempt_timeout_ms = 10,
                            std::optional<uint64_t> connect_timeout_cap_ms = std::nullopt)
        : backend(std::make_shared<InMemoryBackend>())
        , farewell(backend, Fence::open())
        , lease(backend, Fence::open())
        , runtime(
              backend, farewell, lease, layout,
              MountConfig{.boot_ms_fn = [this] { return boot_ms; }},
              "test", sink,
              CasRequestBudget{.attempt_timeout_ms = attempt_timeout_ms,
                               .lease_safety_margin_ms = lease_safety_margin_ms,
                               .connect_timeout_cap_ms = connect_timeout_cap_ms},
              [] { return false; })
    {
    }

    CasMountRuntime * operator->() { return &runtime; }

    uint64_t boot_ms = 1'000;

private:
    std::shared_ptr<InMemoryBackend> backend;
    Layout layout{"mount-runtime-admit"};
    CasEventSink sink;
    CasRequests farewell;
    CasRequests lease;
    CasMountRuntime runtime;
};

/// Named verdicts, so a failing expectation reads as the answer rather than as a raw byte.
const char * admitName(Fence::Admit verdict)
{
    switch (verdict)
    {
        case Fence::Admit::Ok: return "Ok";
        case Fence::Admit::LostOrRearmed: return "LostOrRearmed";
        case Fence::Admit::NoBudget: return "NoBudget";
    }
    return "unknown";
}

/// The message of the refusal `refuse` throws; a failure when it does not throw the transient class.
String refusalText(const std::function<void()> & refuse)
{
    try
    {
        refuse();
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
        return e.message();
    }
    ADD_FAILURE() << "the fence check did not refuse";
    return {};
}

constexpr DB::UInt128 kUuid{7};

}

/// The boundary is STRICT on both terms: a request that would only just finish as the lease runs out
/// is one that may land after this node's fence is already gone.
TEST(CASMountRuntime, AdmitRefusesAtTheExactBudgetBoundary)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/20);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);   /// 100 ms of lease left
    const uint64_t generation = f->fenceGeneration();

    EXPECT_STREQ(admitName(f->admit(generation, 80)), "NoBudget") << "needed + margin == remaining must refuse";
    EXPECT_STREQ(admitName(f->admit(generation, 79)), "Ok") << "one millisecond of slack is enough";
    EXPECT_STREQ(admitName(f->admit(generation, 100)), "NoBudget") << "needed == remaining must refuse";
}

/// The subtraction in `admit` exists for this: `needed_ms + margin` would wrap and read as room.
TEST(CASMountRuntime, AdmitDoesNotWrapOnAnAbsurdNeed)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/20);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);

    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), std::numeric_limits<uint64_t>::max())), "NoBudget");
}

TEST(CASMountRuntime, AdmitRefusesAnExpiredLease)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    const uint64_t generation = f->fenceGeneration();

    f.boot_ms = 1'099;
    EXPECT_STREQ(admitName(f->admit(generation, 0)), "Ok");
    f.boot_ms = 1'100;
    EXPECT_STREQ(admitName(f->admit(generation, 0)), "NoBudget") << "the deadline instant is already past";
    /// One millisecond further is what the `now >= deadline` guard actually earns: without it
    /// `deadline - now` underflows to a huge remaining and the budget test reads it as room.
    f.boot_ms = 1'101;
    EXPECT_STREQ(admitName(f->admit(generation, 0)), "NoBudget")
        << "a deadline already past must not underflow into room";
}

/// A re-arm is a fresh lease incarnation. A caller admitted under the previous one is stale even though
/// the fence is live again, which is the whole point of carrying a generation.
TEST(CASMountRuntime, AdmitRefusesAGenerationTheFenceMovedPast)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/100'000);
    const uint64_t stale = f->fenceGeneration();
    f->armMountFence(kUuid, 2, /*deadline_boot_ms=*/100'000);

    EXPECT_STREQ(admitName(f->admit(stale, 0)), "LostOrRearmed");
    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 0)), "Ok");
}

/// The latch, isolated from the generation bump that accompanies it: the generation presented here is
/// the one the trip itself produced, so only `lost` can be refusing.
TEST(CASMountRuntime, AdmitRefusesALostFenceWhateverTheBudget)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/100'000);
    f->tripMountLost();

    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 0)), "LostOrRearmed");
}

/// The unarmed default (no lease deadline yet) permits work: the bootstrap-control writes that claim a
/// lease run before there is one to be gated on.
TEST(CASMountRuntime, AdmitAllowsAnUnarmedFence)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/2'000);
    f.boot_ms = 1'000;

    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 5'000)), "Ok");
}

/// `refAppendFenceOk` is `admit` at one attempt's worth of budget under the live generation.
TEST(CASMountRuntime, RefAppendFenceOkIsAdmitAtTwoEnvelopes)
{
    /// connect_timeout_cap_ms is nullopt (see RuntimeFixture), so the envelope equals the bare attempt
    /// timeout (10 ms); refAppendFenceOk asks for TWO of them (a write and its settlement read).
    RuntimeFixture f(/*lease_safety_margin_ms=*/20, /*attempt_timeout_ms=*/10);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'041);   /// 41 ms left: one more than 2*10 + 20
    EXPECT_TRUE(f->refAppendFenceOk());
    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 20)), "Ok");

    f->setMountDeadline(1'040);   /// exactly 2*10 + 20 left
    EXPECT_FALSE(f->refAppendFenceOk());
    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 20)), "NoBudget");
}

/// Same boundary, with a nonzero connect cap so the envelope's connect contribution (not just the
/// doubling) is pinned: attempt 100, cap 50 -> envelope 200, refAppendFenceOk asks for 2*200 = 400.
TEST(CASMountRuntime, RefAppendFenceOkIsAdmitAtTwoEnvelopesWithANonzeroCap)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/20, /*attempt_timeout_ms=*/100, /*connect_timeout_cap_ms=*/50);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'421);   /// 421 ms left: one more than 2*200 + 20
    EXPECT_TRUE(f->refAppendFenceOk());
    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 400)), "Ok");

    f->setMountDeadline(1'420);   /// exactly 2*200 + 20 left
    EXPECT_FALSE(f->refAppendFenceOk());
    EXPECT_STREQ(admitName(f->admit(f->fenceGeneration(), 400)), "NoBudget");
}

/// An expiry is this server's own confirmed deadline passing while nothing else is wrong. A lost fence or
/// a lifecycle that left `Live` is a different state and reports as itself.
TEST(CASMountRuntime, LeaseExpiredOnlyWhileLiveAndNotLost)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "an unarmed fence has no deadline to pass";

    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    f.boot_ms = 1'099;
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value());
    f.boot_ms = 1'100;
    ASSERT_TRUE(f->leaseExpiredSinceBootMs().has_value()) << "the deadline instant is already past, as `admit` reads it";
    EXPECT_EQ(*f->leaseExpiredSinceBootMs(), 1'100u);
    EXPECT_TRUE(f->lastRenewFailure().empty()) << "no renewal request has failed";

    f->setLifecycleForTest(PoolLifecycle::IdentityLost);
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "only a `Live` pool reports an expiry";
    f->setLifecycleForTest(PoolLifecycle::Live);
    ASSERT_TRUE(f->leaseExpiredSinceBootMs().has_value());

    f->tripMountLost();
    EXPECT_FALSE(f->leaseExpiredSinceBootMs().has_value()) << "a lost fence is a lease loss, not an expiry";
}

/// Only a refusal caused by the expiry itself names it: an operator can wait for a renewal then, and for
/// nothing else.
TEST(CASMountRuntime, ExpiredLeaseRefusalSaysWritesResume)
{
    RuntimeFixture f(/*lease_safety_margin_ms=*/0);
    f.boot_ms = 1'000;
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    const uint64_t generation = f->fenceGeneration();
    f.boot_ms = 1'100;

    const String expired = refusalText([&] { f->checkFenceOrThrow(generation); });
    EXPECT_NE(expired.find("lease expired"), String::npos) << expired;
    EXPECT_NE(expired.find("writes resume when a renewal restores it"), String::npos) << expired;

    /// A re-arm moves the generation while the lease stays expired: the caller's incarnation is gone.
    f->armMountFence(kUuid, 1, /*deadline_boot_ms=*/1'100);
    const String moved = refusalText([&] { f->checkFenceOrThrow(generation); });
    EXPECT_EQ(moved.find("lease expired"), String::npos) << moved;
    EXPECT_NE(moved.find("mount fence tripped"), String::npos) << moved;

    f->tripMountLost();
    const String lost = refusalText([&] { f->checkFenceOrThrow(f->fenceGeneration()); });
    EXPECT_EQ(lost.find("lease expired"), String::npos) << lost;
    EXPECT_NE(lost.find("mount fence tripped"), String::npos) << lost;
}
