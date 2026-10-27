#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <Poco/Exception.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

namespace ProfileEvents
{
extern const Event CASMountRenewalAttempts;
}

using namespace DB::Cas;

namespace
{

uint64_t attemptsCounter()
{
    return ProfileEvents::global_counters[ProfileEvents::CASMountRenewalAttempts].load();
}

/// Fixed boot clock and no-op sleep: every armed fault is finite, so retries end without time passing.
struct Fixture
{
    std::shared_ptr<InMemoryBackend> backend = std::make_shared<InMemoryBackend>();
    PoolPtr store;
    String mount_key;

    explicit Fixture(const String & prefix)
    {
        backend->setAttemptTimeoutMs(10);
        store = Pool::open(backend, PoolConfig{
            .pool_prefix = prefix,
            .server_root_id = "test",
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 10, .lease_safety_margin_ms = 20, .connect_timeout_cap_ms = std::nullopt},
            .boot_ms_fn = [] { return uint64_t{100}; },
            .retry_sleep_fn = [](uint64_t) {},
        });
        mount_key = store->layout().mountKey("test");
    }
};

}

TEST(CASMountRenewalProgress, AttemptsAndLastErrorAreVisibleWhileTheRenewalRuns)
{
    Fixture f("renewal-progress-in-flight");
    const auto timeout = std::make_exception_ptr(Poco::TimeoutException("injected renewal timeout"));
    f.backend->failNextWriteWith(f.mount_key, timeout);
    f.backend->failNextWriteWith(f.mount_key, timeout);
    const uint64_t attempts_before = attemptsCounter();
    /// Read at the third attempt, the first that reaches the store. Heap state and a raw pointer: the
    /// hook must not keep the pool alive, and it is removed before the pool's farewell write.
    auto in_flight = std::make_shared<std::optional<std::pair<RenewalProgress, uint64_t>>>();
    Pool * pool = f.store.get();
    f.backend->onBeforeWrite(f.mount_key, [in_flight, pool]
    {
        if (!*in_flight)
            *in_flight = std::make_pair(pool->renewalProgress(), attemptsCounter());
    });

    f.store->renewWatermarkOnce();
    f.backend->onBeforeWrite(f.mount_key, {});

    ASSERT_TRUE(in_flight->has_value());
    const auto & [seen, attempts_counted] = **in_flight;
    EXPECT_TRUE(seen.in_progress);
    EXPECT_EQ(seen.attempts_sent, 3u);
    EXPECT_EQ(seen.last_error, "PUT " + f.mount_key + ": transport Timeout injected renewal timeout");
    EXPECT_EQ(attempts_counted - attempts_before, 3u) << "attempts are counted as they are sent";

    const RenewalProgress ended = f.store->renewalProgress();
    EXPECT_FALSE(ended.in_progress);
    EXPECT_EQ(ended.attempts_sent, 3u);
    EXPECT_EQ(ended.last_error, seen.last_error) << "an ended renewal keeps its progress";

    f.store->renewWatermarkOnce();
    const RenewalProgress next = f.store->renewalProgress();
    EXPECT_EQ(next.attempts_sent, 1u);
    EXPECT_FALSE(next.last_error.has_value()) << "the next renewal starts clean";
}

#if USE_AWS_S3
TEST(CASMountRenewalProgress, AStoreRefusalIsClassedRefused)
{
    Fixture f("renewal-progress-refused");
    f.backend->failNextWriteWith(f.mount_key, std::make_exception_ptr(
        DB::S3Exception("the store refused the renewal", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied")));

    EXPECT_THROW(f.store->renewWatermarkOnce(), DB::Exception);

    const RenewalProgress ended = f.store->renewalProgress();
    EXPECT_FALSE(ended.in_progress) << "a renewal that threw has still ended";
    EXPECT_EQ(ended.attempts_sent, 1u);
    EXPECT_EQ(ended.last_error, "PUT " + f.mount_key + ": refused AccessDenied the store refused the renewal");
}
#endif
