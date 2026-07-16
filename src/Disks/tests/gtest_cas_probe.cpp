#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}
}

using namespace DB::Cas;

TEST(CasProbe, PassesOnEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());   // probe cleans up after itself
}

/// AWS S3 answers 400 InvalidArgument to a conditional DELETE with an EMPTY If-Match, and the
/// probe's exit cleanup used to issue exactly that (deleteExact with the absent HeadResult's empty
/// token) after step 8 had already deleted the probe keys — two scary AWSClient <Error> log lines
/// on every real-S3 mount. The cleanup must HEAD-gate the delete instead of firing blindly.
class EmptyTokenDeleteRecorder : public InMemoryBackend
{
public:
    size_t empty_token_deletes = 0;

    DeleteOutcome deleteExact(const String & key, const Token & token) override
    {
        if (token.empty())
            ++empty_token_deletes;
        return InMemoryBackend::deleteExact(key, token);
    }
};

TEST(CasProbe, CleanupNeverDeletesWithEmptyToken)
{
    auto b = std::make_shared<EmptyTokenDeleteRecorder>();
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
    EXPECT_EQ(b->empty_token_deletes, 0u);
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
/// `<pool>/_probe/token` over a shared backend, then opening the Pool. With the OLD fixed-key probe
/// the open's `putIfAbsent("<pool>/_probe/token", …)` returns PreconditionFailed and `Pool::open`
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
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// And two genuinely-concurrent mounts (distinct unique prefixes) both succeed over one backend.
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// The seeded fixed-key artifact is untouched (the probe never collided with it).
    EXPECT_TRUE(b->get("p/_probe/token").has_value());
}

/// The probe must consult the backend's store-preconditions hook BEFORE the op battery: a
/// generation-dialect store on a VERSIONED bucket passes every conditional-op check, but its
/// token-exact DELETEs archive noncurrent generations instead of reclaiming storage — only the
/// hook can see that, so a throwing hook must fail the probe closed.
class PreconditionRefusingBackend : public InMemoryBackend
{
public:
    void checkPoolPreconditions() override
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
            "test: store precondition violated (e.g. bucket versioning enabled)");
    }
};

TEST(CasProbe, FailsClosedOnPoolPreconditions)
{
    auto b = std::make_shared<PreconditionRefusingBackend>();
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
    /// The hook fires FIRST: no probe keys may have been written.
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());
}

/// `Pool::open` wraps the pool backend in `InstrumentedBackend` BEFORE calling `runCapabilityProbe`
/// (see CasPool.cpp), so the hook must actually fire THROUGH the wrapper on the real mount path —
/// not just on a raw backend, which `FailsClosedOnPoolPreconditions` above already covers.
TEST(CasProbe, PoolPreconditionsFireThroughInstrumentedWrapper)
{
    auto inner = std::make_shared<PreconditionRefusingBackend>();
    InstrumentedBackend wrapped(inner);
    EXPECT_THROW(runCapabilityProbe(wrapped, "p/.cas_probe"), DB::Exception);
    /// The hook fires FIRST: no probe keys may have been written to the inner backend.
    EXPECT_TRUE(inner->list("p/.cas_probe", "", 10).keys.empty());
}

/// RFC cas-s3-timeout-retry-control: a Native-mode mount without a working single-attempt S3 client
/// (ObjectStorageBackend::single_attempt_s3_client) must never silently proceed under the disk's
/// default (~500-attempt) transparent retry policy — see Backend::checkConditionalWriteSingleAttemptSupport.
/// LocalObjectStorage never exposes an S3 client (IObjectStorage::tryGetS3StorageClient returns null),
/// so Native mode over it is exactly the case this must refuse. EmulatedSingleProcess is exempt: it
/// never claims single-attempt S3 semantics in the first place (PassesOnEmulatedLocal above).
TEST(CasProbe, FailsClosedOnMissingSingleAttemptClient)
{
    auto native = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    EXPECT_THROW(native->checkConditionalWriteSingleAttemptSupport(), DB::Exception);

    auto emulated = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    EXPECT_NO_THROW(emulated->checkConditionalWriteSingleAttemptSupport());
}

/// The same fail-closed refusal through the actual capability probe (Step 0b) — the real gate a
/// writable Pool::open goes through, not just the hook in isolation above.
TEST(CasProbe, MissingSingleAttemptClientFailsCapabilityProbe)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
    /// The hook fires before the op battery: no probe keys may have been written.
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());
}

/// Mirrors PoolPreconditionsFireThroughInstrumentedWrapper: the real mount path wraps the backend in
/// InstrumentedBackend BEFORE calling runCapabilityProbe, so this check must fire through it too.
TEST(CasProbe, MissingSingleAttemptClientFiresThroughInstrumentedWrapper)
{
    auto inner = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    InstrumentedBackend wrapped(inner);
    EXPECT_THROW(runCapabilityProbe(wrapped, "p/.cas_probe"), DB::Exception);
}
