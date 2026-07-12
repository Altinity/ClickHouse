#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h>
#include <Common/ProfileEvents.h>

#if USE_AWS_S3
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/S3Common.h>
#include <Poco/Net/NetException.h>
#endif

using namespace DB::Cas;

namespace ProfileEvents
{
    extern const Event CasConditionalWriteAttempts;
    extern const Event CasConditionalWriteCommitted;
    extern const Event CasConditionalWriteDefiniteFailure;
    extern const Event CasConditionalWriteUnresolved;
    extern const Event CasConditionalWriteSdkRetries;
}

#if USE_AWS_S3
namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
#endif

/// The success path (buf.finalize() returned without throwing) is always Committed. No exception
/// object is needed — the caller distinguishes success from failure before calling either overload.
TEST(CasRequestControl, SuccessIsAlwaysCommitted)
{
    EXPECT_EQ(classifyConditionalWriteResult(), CasWriteOutcome::Committed);
}

#if USE_AWS_S3

/// One row per RFC cas-s3-timeout-retry-control §operation-classes classification. PreconditionFailed
/// is NEVER DefiniteFailure — it means the key exists, not that the request was rejected — and every
/// unrecognized/ambiguous error also falls to Unresolved, never to a false DefiniteFailure.
TEST(CasRequestControl, ClassifiesPreconditionFailedAsUnresolved)
{
    DB::S3Exception e("412 from backend", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::Unresolved);
}

TEST(CasRequestControl, ClassifiesTimeoutAsUnresolved)
{
    Poco::TimeoutException e("simulated client-side receive timeout");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::Unresolved);
}

TEST(CasRequestControl, ClassifiesConnectionResetAsUnresolved)
{
    Poco::Net::ConnectionResetException e("simulated connection reset");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::Unresolved);
}

TEST(CasRequestControl, Classifies5xxAsUnresolved)
{
    DB::S3Exception e("simulated internal error", Aws::S3::S3Errors::INTERNAL_FAILURE, "InternalError");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::Unresolved);
    /// SlowDown / ServiceUnavailable are also 5xx-class and equally Unresolved.
    DB::S3Exception slow_down("simulated throttle", Aws::S3::S3Errors::SLOW_DOWN, "SlowDown");
    EXPECT_EQ(classifyConditionalWriteResult(slow_down), CasWriteOutcome::Unresolved);
}

TEST(CasRequestControl, ClassifiesMalformedRequestAsDefiniteFailure)
{
    DB::S3Exception e("bad xml", Aws::S3::S3Errors::UNKNOWN, "MalformedXML");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::DefiniteFailure);
    /// The modeled-enum path (no canonical name attached) must classify identically.
    DB::S3Exception by_code("bad argument", Aws::S3::S3Errors::INVALID_REQUEST);
    EXPECT_EQ(classifyConditionalWriteResult(by_code), CasWriteOutcome::DefiniteFailure);
}

TEST(CasRequestControl, ClassifiesEntityTooLargeAsDefiniteFailure)
{
    DB::S3Exception e("body exceeds the maximum object size", Aws::S3::S3Errors::UNKNOWN, "EntityTooLarge");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::DefiniteFailure);
}

TEST(CasRequestControl, ClassifiesAccessDeniedAsDefiniteFailure)
{
    DB::S3Exception e("simulated 403", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied");
    EXPECT_EQ(classifyConditionalWriteResult(e), CasWriteOutcome::DefiniteFailure);
    /// The modeled-enum path (no canonical name attached) must classify identically.
    DB::S3Exception by_code("simulated 403, no name", Aws::S3::S3Errors::ACCESS_DENIED);
    EXPECT_EQ(classifyConditionalWriteResult(by_code), CasWriteOutcome::DefiniteFailure);
}

/// Anything the classifier does not recognize (an unmodeled/unnamed S3 error, or an entirely
/// unrelated exception type) must fail toward Unresolved — never toward a false DefiniteFailure or a
/// false Committed (RFC §resolve-before-reissuing: ambiguity always resolves toward "resolve before
/// reissuing").
TEST(CasRequestControl, UnrecognizedErrorsFailSafeToUnresolved)
{
    DB::S3Exception unknown_named("weird service error", Aws::S3::S3Errors::UNKNOWN, "SomeFutureErrorCode");
    EXPECT_EQ(classifyConditionalWriteResult(unknown_named), CasWriteOutcome::Unresolved);

    DB::Exception unrelated(DB::ErrorCodes::LOGICAL_ERROR, "not an S3 error at all");
    EXPECT_EQ(classifyConditionalWriteResult(unrelated), CasWriteOutcome::Unresolved);
}

/// recordConditionalWriteAttemptStarted / recordConditionalWriteOutcome bump the per-class counters
/// (RFC §observability): attempts, and exactly one of Committed/DefiniteFailure/Unresolved per call.
TEST(CasRequestControl, CountersHookupIncrementsPerClass)
{
    using ProfileEvents::global_counters;
    const auto attempts_before = global_counters[ProfileEvents::CasConditionalWriteAttempts].load();
    const auto committed_before = global_counters[ProfileEvents::CasConditionalWriteCommitted].load();
    const auto definite_before = global_counters[ProfileEvents::CasConditionalWriteDefiniteFailure].load();
    const auto unresolved_before = global_counters[ProfileEvents::CasConditionalWriteUnresolved].load();

    recordConditionalWriteAttemptStarted();
    recordConditionalWriteOutcome(CasWriteOutcome::Committed);
    recordConditionalWriteAttemptStarted();
    recordConditionalWriteOutcome(CasWriteOutcome::DefiniteFailure);
    recordConditionalWriteAttemptStarted();
    recordConditionalWriteOutcome(CasWriteOutcome::Unresolved);

#if !WITH_COVERAGE
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteAttempts].load() - attempts_before, 3u);
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteCommitted].load() - committed_before, 1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteDefiniteFailure].load() - definite_before, 1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteUnresolved].load() - unresolved_before, 1u);
#else
    (void)attempts_before; (void)committed_before; (void)definite_before; (void)unresolved_before;
#endif
}

/// Wiring smoke test: a real conditional write through ObjectStorageBackend (Native mode) counts one
/// attempt and one Committed outcome via the SAME instrumented call site nativeConditionalPut uses —
/// see finalizeConditionalWriteInstrumented in CasObjectStorageBackend.cpp.
TEST(CasRequestControl, NativeConditionalPutCountsOneAttemptAndCommitted)
{
    using ProfileEvents::global_counters;
    const auto attempts_before = global_counters[ProfileEvents::CasConditionalWriteAttempts].load();
    const auto committed_before = global_counters[ProfileEvents::CasConditionalWriteCommitted].load();

    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    EXPECT_EQ(b->putIfAbsent("p/rc/one", "v1").outcome, PutOutcome::Done);

#if !WITH_COVERAGE
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteAttempts].load() - attempts_before, 1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteCommitted].load() - committed_before, 1u);
#else
    (void)attempts_before; (void)committed_before;
#endif
}

/// Mechanism property (RFC §disable-transparent-conditional-write-retries), tested at the layer
/// actually reachable from a unit-test binary: NO live/fake S3 endpoint is available here (the Native
/// conditional-write path is exercised end-to-end only at M-W against RustFS — see the HONEST NOTE in
/// CasObjectStorageBackend.cpp), so driving a real socket-level retry against a real client is not
/// reachable from this binary. What IS reachable and asserted here: the single-attempt client override
/// is populated ONLY when the underlying object storage actually exposes an S3 client
/// (IObjectStorage::tryGetS3StorageClient), and stays null — leaving the disk's own client and retry
/// policy untouched — for a non-S3 backend such as LocalObjectStorage. This is the same property
/// BackupIO_S3.cpp's per-operation client override relies on (Client::cloneWithConfigurationOverride
/// reusing the base client's connection pool/credentials).
TEST(CasRequestControl, ClientOverrideAbsentOverNonS3ObjectStorage)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    const auto ws = b->conditionalWriteSettingsForTest();
    EXPECT_EQ(ws.s3_client_override, nullptr);
}

/// The SDK-retry tripwire itself (RFC §observability: "SDK-level retries, which must remain zero for
/// conditional writes"): detail::SingleAttemptRetryStrategy is exactly what the single-attempt
/// client's `retryStrategy` is set to (CasObjectStorageBackend.cpp ctor). Simulating a retryable 5xx
/// the AWS SDK would normally retry proves BOTH halves of the property directly at the seam that
/// decides it — refusal (ShouldRetry returns false, GetMaxAttempts is 1) AND that
/// CasConditionalWriteSdkRetries is a LIVE tripwire, not a counter nothing ever touches.
TEST(CasRequestControl, SingleAttemptRetryStrategyRefusesAndCountsEveryConsultation)
{
    using ProfileEvents::global_counters;
    const auto before = global_counters[ProfileEvents::CasConditionalWriteSdkRetries].load();

    DB::Cas::detail::SingleAttemptRetryStrategy strategy;
    EXPECT_EQ(strategy.GetMaxAttempts(), 1);

    Aws::Client::AWSError<Aws::Client::CoreErrors> retryable_error(
        Aws::Client::CoreErrors::INTERNAL_FAILURE, /*isRetryable=*/true);
    EXPECT_FALSE(strategy.ShouldRetry(retryable_error, /*attemptedRetries=*/0));
    EXPECT_FALSE(strategy.ShouldRetry(retryable_error, /*attemptedRetries=*/1));

#if !WITH_COVERAGE
    EXPECT_EQ(global_counters[ProfileEvents::CasConditionalWriteSdkRetries].load() - before, 2u);
#else
    (void)before;
#endif
}

#endif
