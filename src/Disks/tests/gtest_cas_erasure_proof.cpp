#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

/// Task 6 (spec §2 [C2][C3][D1]): the `Vanished(erased)` proof — the ONLY mechanism that may ever
/// conclude a pool's data is gone. These tests open a real writable `Pool` over an in-memory backend that
/// can be TOLD (test-only override) whether it declares the strong-prefix-LIST capability, drive the
/// lifecycle observer synchronously through `tryRemountOnce` (following gtest_cas_lifecycle_condition.cpp),
/// and advance a virtual `CLOCK_BOOTTIME` (injected `boot_ms_fn`) so the minimum-grace and sample-spacing
/// gates are exercised WITHOUT any wall-clock sleep. Correctness of a NEGATIVE (never falsely conclude
/// erased) is asserted at least as hard as the positive.

namespace DB::ErrorCodes
{
extern const int INVALID_STATE;
}

using namespace DB::Cas;

namespace
{

const String kSrid = "test";

/// The test-only capability override (the Emulated harness's licence to reach `Vanished(erased)`): an
/// in-memory backend whose `supportsErasureProof()` returns a settable flag. Clearly test-only — no
/// production `InMemoryBackend`/`ObjectStorageBackend` behavior changes. The Pool wraps this in its
/// `InstrumentedBackend`, which forwards `supportsErasureProof()` to it.
class ErasureProofBackend final : public InMemoryBackend
{
public:
    /// Unhide the base convenience overloads shadowed by the head/get/list overrides below.
    using Backend::get;
    using Backend::getStream;
    using Backend::putIfAbsent;
    using Backend::putIfAbsentStream;
    using Backend::putOverwrite;
    using Backend::casPut;

    bool supportsErasureProof() const override { return capable.load(); }

    /// While `fail_probes` is armed, head/get/list throw an untyped transport error, so the gate's
    /// authoritative probes classify `Indeterminate` (absence never proven) — the sharp error-reset case.
    HeadResult head(const String & key) override
    {
        if (fail_probes.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::head(key);
    }
    std::optional<GetResult> get(const String & key, Range range) override
    {
        if (fail_probes.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::get(key, range);
    }
    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (fail_probes.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::list(prefix, cursor, limit);
    }

    std::atomic<bool> capable{true};
    std::atomic<bool> fail_probes{false};
};

struct ClockedPool
{
    std::shared_ptr<std::atomic<uint64_t>> clock;
    std::shared_ptr<ErasureProofBackend> backend;
    PoolPtr store;
    uint64_t grace = 0;   /// erasure-proof minimum grace (bootMs), derived from the pool config
    uint64_t renew = 0;   /// mount renewal period (bootMs), the required sample spacing
};

/// Open a writable Pool over an `ErasureProofBackend` with an injected virtual boot clock. `wait_sleep_fn`
/// is a no-op so no open/remount wait ever blocks these synchronous tests.
ClockedPool openClockedPool(bool capable)
{
    auto clock = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    auto backend = std::make_shared<ErasureProofBackend>();
    backend->capable.store(capable);

    PoolConfig cfg;
    cfg.pool_prefix = "p";
    cfg.server_root_id = kSrid;
    cfg.boot_ms_fn = [clock] { return clock->load(); };
    cfg.wait_sleep_fn = [](uint64_t) {};

    ClockedPool cp;
    cp.clock = clock;
    cp.backend = backend;
    cp.store = Pool::open(backend, cfg);
    /// Read the grace + renewal period straight off the Pool so the test never re-derives (and drifts
    /// from) the production retry-policy arithmetic.
    cp.grace = cp.store->erasureProofGraceMsForTest();
    cp.renew = static_cast<uint64_t>(cp.store->poolConfig().mount_renew_period.count());
    return cp;
}

void advance(std::atomic<uint64_t> & clock, uint64_t ms)
{
    clock.fetch_add(ms);
}

/// Exact-token delete of one key (no-op when absent).
void deleteKey(Backend & backend, const String & key)
{
    const HeadResult h = backend.head(key);
    if (h.exists)
        backend.deleteExact(key, h.token);
}

/// Erase EVERY object under the pool prefix the gate probes (`poolPrefix()+"/"`), so a subsequent gate
/// evaluation sees an authoritatively empty prefix.
void eraseAllUnderPool(Backend & backend, const Layout & layout)
{
    const String prefix = layout.poolPrefix() + "/";
    std::vector<String> keys;
    forEachListedKey(backend, prefix, [&](const ListedKey & k) { keys.push_back(k.key); });
    for (const String & key : keys)
        deleteKey(backend, key);
}

/// Assert `fn` throws a DB::Exception with `expected_code` AND a message containing `needle`.
template <typename F>
void expectThrowsCodeContaining(int expected_code, const String & needle, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code);
        EXPECT_NE(e.message().find(needle), String::npos)
            << "message did not contain '" << needle << "': " << e.message();
    }
}

}

/// (a) Progressive erase (the spec headline: `rm -rf` deletes the sentinels first) on a proof-CAPABLE
/// backend: sentinels gone → `IdentityLost`; then the data drains → two authoritative empty samples,
/// spaced >= the renewal period, taken after the grace, with the durable counter at zero → the one-way
/// promotion to `Vanished(erased)`, whose typed error carries the verified reason [D5].
TEST(CasErasureProof, ProgressiveEraseOnCapableBackendReachesVanishedErased)
{
    auto cp = openClockedPool(/*capable=*/true);
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::Live);

    /// Sentinels first (data remains) → IdentityLost; this first tick records the fence-trip instant.
    deleteKey(*cp.backend, cp.store->layout().poolMetaKey());
    deleteKey(*cp.backend, cp.store->layout().ownerKey(kSrid));
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u);

    /// The rest drains → prefix authoritatively empty. Wait out the grace, then take the first sample.
    eraseAllUnderPool(*cp.backend, cp.store->layout());
    advance(*cp.clock, cp.grace + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost) << "one sample must not conclude erased";

    /// A second sample, correctly spaced → promote to Vanished(erased).
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
    EXPECT_TRUE(cp.store->isVanished());
    expectThrowsCodeContaining(DB::ErrorCodes::INVALID_STATE, "verified: pool prefix empty",
                               [&] { cp.store->throwIfLifecycleTerminal(); });
}

/// (a') The direct path: a FULL live erase (sentinels AND data gone together) is concluded erased straight
/// from `TransientNotLive` through the SAME full proof (spec §2: "erased can be concluded from
/// TransientNotLive"), never passing through `IdentityLost`.
TEST(CasErasureProof, FullEraseFromTransientReachesVanishedErasedDirectly)
{
    auto cp = openClockedPool(/*capable=*/true);
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::Live);

    eraseAllUnderPool(*cp.backend, cp.store->layout());

    /// First tick: records the fence trip; the grace has not elapsed yet, so no sample counts.
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::TransientNotLive) << "direct path never enters IdentityLost";
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u);

    advance(*cp.clock, cp.grace + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::TransientNotLive);

    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
    EXPECT_TRUE(cp.store->isVanished());
}

/// (b) One empty sample, then an object re-appears → the streak resets and the disk stays `IdentityLost`
/// (never falsely erased): any non-empty observation invalidates an in-progress proof.
TEST(CasErasureProof, ObjectReappearsResetsStreakAndStaysIdentityLost)
{
    auto cp = openClockedPool(/*capable=*/true);

    deleteKey(*cp.backend, cp.store->layout().poolMetaKey());
    deleteKey(*cp.backend, cp.store->layout().ownerKey(kSrid));
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);

    eraseAllUnderPool(*cp.backend, cp.store->layout());
    advance(*cp.clock, cp.grace + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);

    /// An object re-appears under the prefix (the sentinels stay gone → the gate verdicts IdentityLost).
    ASSERT_EQ(cp.backend->putIfAbsent(cp.store->layout().poolPrefix() + "/roots/test/late", "x").outcome,
              PutOutcome::Done);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u) << "a non-empty prefix must reset the streak";
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
    EXPECT_FALSE(cp.store->isVanished());
}

/// (c) A live `DurableRequestGuard` held → FULL durable-lane quiescence [D1] is violated, so the proof
/// never starts (streak stays 0) no matter how many correctly-spaced empty samples are taken; releasing
/// the guard lets the very same proof complete — proving the guard, not timing, is what blocked it.
TEST(CasErasureProof, DurableRequestGuardBlocksProofUntilReleased)
{
    auto cp = openClockedPool(/*capable=*/true);

    eraseAllUnderPool(*cp.backend, cp.store->layout());
    EXPECT_FALSE(cp.store->tryRemountOnce());   /// record fence trip
    advance(*cp.clock, cp.grace + 1);

    {
        /// Admit one durable-effect operation and hold it across several spaced samples.
        auto guard = cp.store->beginDurableRequest();
        ASSERT_EQ(cp.store->outstandingDurableRequestsForTest(), 1u);
        for (int i = 0; i < 4; ++i)
        {
            EXPECT_FALSE(cp.store->tryRemountOnce());
            EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u) << "the proof must not start while a durable request is in flight";
            EXPECT_FALSE(cp.store->isVanished());
            advance(*cp.clock, cp.renew + 1);
        }
    }
    ASSERT_EQ(cp.store->outstandingDurableRequestsForTest(), 0u);

    /// With the guard released the proof completes over two correctly-spaced empty samples.
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
}

/// (d) The capability is FALSE → the proof never runs. Even with the identical setup that promotes a
/// capable backend (empty prefix, quiescent, grace elapsed, many spaced samples), the terminal natural
/// state stays `IdentityLost` forever; `FORGET`/restart is the only cure.
TEST(CasErasureProof, WithoutCapabilityStaysIdentityLostForever)
{
    auto cp = openClockedPool(/*capable=*/false);

    deleteKey(*cp.backend, cp.store->layout().poolMetaKey());
    deleteKey(*cp.backend, cp.store->layout().ownerKey(kSrid));
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);

    eraseAllUnderPool(*cp.backend, cp.store->layout());
    advance(*cp.clock, cp.grace + 1);
    for (int i = 0; i < 6; ++i)
    {
        EXPECT_FALSE(cp.store->tryRemountOnce());
        EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u) << "no capability ⇒ no sample ever counts";
        EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
        EXPECT_FALSE(cp.store->isVanished());
        advance(*cp.clock, cp.renew + 1);
    }
}

/// (e) Sample spacing is enforced from the observer's own bookkeeping (no wall-clock sleep): two empty
/// samples taken INSIDE one renewal period do not both count, so the disk is not promoted early; once the
/// clock crosses a full renewal period a second sample counts and the promotion fires.
TEST(CasErasureProof, TwoSamplesInsideOneRenewalPeriodDoNotBothCount)
{
    auto cp = openClockedPool(/*capable=*/true);

    eraseAllUnderPool(*cp.backend, cp.store->layout());
    EXPECT_FALSE(cp.store->tryRemountOnce());   /// record fence trip
    advance(*cp.clock, cp.grace + 1);

    EXPECT_FALSE(cp.store->tryRemountOnce());    /// sample 1
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);

    /// A second sample strictly inside one renewal period: must NOT advance the streak (and must NOT reset
    /// it either — the window is still clean, we simply keep waiting).
    ASSERT_GE(cp.renew, 2u);   // guard: a positive sub-period advance requires renew >= 2
    advance(*cp.clock, cp.renew - 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u) << "a too-soon sample must not count";
    EXPECT_FALSE(cp.store->isVanished());

    /// Cross a full renewal period from the last COUNTED sample → the second sample counts → promote.
    advance(*cp.clock, cp.renew + 2);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
}

/// (f) The sharpest [D1] reset case: an ALREADY-ESTABLISHED streak (one counted empty sample) is reset when
/// the NEXT correctly-spaced sample finds a durable-effect operation in flight (`DurableRequestGuard`
/// held). Unlike (c) — which holds the guard from the start so the streak never reaches 1 — this proves the
/// counter can invalidate an in-progress proof mid-window; and that the reset does not wedge the machinery
/// (the proof completes cleanly once the guard is released).
TEST(CasErasureProof, EstablishedStreakResetByDurableGuardAtNextSampleThenCompletes)
{
    auto cp = openClockedPool(/*capable=*/true);

    /// Progressive erase → IdentityLost, then an established streak of one empty sample.
    deleteKey(*cp.backend, cp.store->layout().poolMetaKey());
    deleteKey(*cp.backend, cp.store->layout().ownerKey(kSrid));
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
    eraseAllUnderPool(*cp.backend, cp.store->layout());
    advance(*cp.clock, cp.grace + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);

    /// The next correctly-spaced sample finds a durable request in flight → the ESTABLISHED streak resets.
    {
        auto guard = cp.store->beginDurableRequest();
        ASSERT_EQ(cp.store->outstandingDurableRequestsForTest(), 1u);
        advance(*cp.clock, cp.renew + 1);
        EXPECT_FALSE(cp.store->tryRemountOnce());
        EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u)
            << "a nonzero durable counter must reset the established streak";
        EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
        EXPECT_FALSE(cp.store->isVanished());
    }

    /// The reset did not wedge the machinery: two fresh spaced samples complete the proof.
    ASSERT_EQ(cp.store->outstandingDurableRequestsForTest(), 0u);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
}

/// (g) The same established-streak reset, driven instead by an UNDECIDABLE probe (Indeterminate) at the next
/// spaced sample: absence is no longer proven, so the gate stays transient and the streak resets. Once the
/// probe heals, the proof completes — proving an error mid-window neither concludes erased nor wedges.
TEST(CasErasureProof, EstablishedStreakResetByErrorProbeAtNextSampleThenCompletes)
{
    auto cp = openClockedPool(/*capable=*/true);

    deleteKey(*cp.backend, cp.store->layout().poolMetaKey());
    deleteKey(*cp.backend, cp.store->layout().ownerKey(kSrid));
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
    eraseAllUnderPool(*cp.backend, cp.store->layout());
    advance(*cp.clock, cp.grace + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);

    /// The next spaced sample's probe errors (Indeterminate) → the gate stays transient and resets.
    cp.backend->fail_probes.store(true);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->erasureProofEmptySamplesForTest(), 0u)
        << "an undecidable probe must reset the established streak";
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::IdentityLost);
    EXPECT_FALSE(cp.store->isVanished());

    /// The probe heals → the proof completes over two fresh spaced samples.
    cp.backend->fail_probes.store(false);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    ASSERT_EQ(cp.store->erasureProofEmptySamplesForTest(), 1u);
    advance(*cp.clock, cp.renew + 1);
    EXPECT_FALSE(cp.store->tryRemountOnce());
    EXPECT_EQ(cp.store->lifecycle(), PoolLifecycle::VanishedErased);
}
