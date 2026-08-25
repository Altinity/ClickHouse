#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace DB::Cas;

namespace
{

/// Open a pool, arm one teardown phase to throw, destroy it, and report whether the clean-release
/// marker was written. Runs inside the subprocess of each exit test below.
[[noreturn]] void tearDownWithThrowingPhase(int phase)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    auto thrower = [] { throw std::runtime_error("injected teardown phase failure"); };
    if (phase == 1)
        config.teardown_phase1_throw_for_test = thrower;
    else if (phase == 2)
        config.teardown_phase2_throw_for_test = thrower;
    else
        config.teardown_phase3_throw_for_test = thrower;

    {
        auto store = Pool::open(backend, config);
        (void)store;
    }   /// `~Pool` runs here.

    /// A failed ref-lane drain must not leave a clean-release marker behind. That marker lets a
    /// successor skip the observation window, so a phase-2 failure must leave it absent.
    const auto mount = backend->get(Layout(config.pool_prefix).mountKey(config.server_root_id));
    const bool clean_release = mount
        && decodeMountLease(mount->bytes).min_active == std::numeric_limits<uint64_t>::max();
    const bool marker_must_be_absent = phase == 2;
    std::exit(marker_must_be_absent && clean_release ? 1 : 0);
}

}

TEST(CASShutdownExitTest, TeardownPhase1ThrowExitsCleanly)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(1), ::testing::ExitedWithCode(0), "");
}

TEST(CASShutdownExitTest, TeardownPhase2ThrowExitsCleanlyAndSkipsTheMarker)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(2), ::testing::ExitedWithCode(0), "");
}

TEST(CASShutdownExitTest, TeardownPhase3ThrowExitsCleanly)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(3), ::testing::ExitedWithCode(0), "");
}
