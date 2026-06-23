#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;

TEST(CasProbe, PassesOnEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());   // probe cleans up after itself
}

TEST(CasProbe, FailsClosedOnNonEnforcingDelete)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);                                  // the MinIO-OSS failure mode
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
}

TEST(CasProbe, FailsClosedOnDeleteMarkers)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setSimulateDeleteMarkers(true);                           // versioning enabled on the prefix
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
}

TEST(CasProbe, PassesOnEmulatedLocal)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
}

/// B135: two servers mounting the SAME shared CA pool concurrently must not race on the probe keys.
/// We simulate "a concurrent mounter's probe is in flight" by PRE-SEEDING the fixed-name probe key
/// `<pool>/_probe/token` over a shared backend, then opening the Store. With the OLD fixed-key probe
/// the open's `putIfAbsent("<pool>/_probe/token", …)` returns PreconditionFailed and `Store::open`
/// throws NOT_IMPLEMENTED ("putIfAbsent on a fresh key returned PreconditionFailed"). With the
/// per-mount unique probe prefix `<pool>/_probe/<rand>/token`, the seeded key does not collide and
/// the open succeeds — exactly the concurrent-shared-pool-mount behaviour we need.
TEST(CasProbe, ConcurrentMountsDoNotCollide)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Simulate a concurrent mounter whose probe object under the legacy fixed key is still present.
    ASSERT_EQ(b->putIfAbsent("p/_probe/token", "concurrent-mounter-in-flight").outcome, PutOutcome::Done);

    /// A real (second) mount over the same shared pool must still succeed — its probe runs under a
    /// fresh per-mount-unique prefix and never touches the seeded fixed key.
    EXPECT_NO_THROW(Store::open(b, PoolConfig{.pool_prefix = "p"}));

    /// And two genuinely-concurrent mounts (distinct unique prefixes) both succeed over one backend.
    EXPECT_NO_THROW(Store::open(b, PoolConfig{.pool_prefix = "p"}));

    /// The seeded fixed-key artifact is untouched (the probe never collided with it).
    EXPECT_TRUE(b->get("p/_probe/token").has_value());
}
