#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h>
#include <Common/ProfileEvents.h>

#if USE_AWS_S3
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
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
    extern const int CORRUPTED_DATA;
    extern const int BAD_ARGUMENTS;
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

/// ================================================================================================
/// Task 5: CasRequestController — retry controller (deadlines, fence gating, exact-key resolution)
/// ================================================================================================

namespace
{

/// A per-call scripted Backend for CasRequestController tests: `putIfAbsent` optionally throws a
/// caller-supplied exception (models one classified HTTP-attempt outcome) or returns a forced
/// `PutOutcome` directly (models a `PreconditionFailed` observed WITHOUT an exception); with neither
/// set it delegates to the real in-memory conditional-write semantics. `get` optionally returns a
/// forced result, independent of what `putIfAbsent` actually did, so a test can drive exact-key
/// resolution (identical / different / absent) without the scripted put and the resolve GET needing to
/// agree on a shared, real backing store.
class ScriptedControllerBackend : public InMemoryBackend
{
public:
    std::function<void()> put_thrower;
    std::optional<PutOutcome> put_forced_outcome;
    std::atomic<uint64_t> put_attempts{0};

    bool get_overridden = false;
    std::optional<GetResult> get_override_value;   /// meaningful only when get_overridden

    void setGetOverride(std::optional<GetResult> value)
    {
        get_overridden = true;
        get_override_value = std::move(value);
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override
    {
        ++put_attempts;
        if (put_thrower)
            put_thrower();
        if (put_forced_outcome)
            return {*put_forced_outcome, {}};
        return InMemoryBackend::putIfAbsent(key, bytes, meta);
    }

    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        if (get_overridden)
            return get_override_value;
        return InMemoryBackend::get(key, range);
    }
};

GetResult resultWithBytes(const String & bytes)
{
    return GetResult{.bytes = bytes, .token = Token{"t", TokenType::Emulated}, .attributes = {}};
}

}

TEST(CasRequestController, UncertainResolvesIdenticalAsCommitted)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    backend->put_thrower = [] { throw Poco::TimeoutException("scripted: ambiguous"); };
    backend->setGetOverride(resultWithBytes("payload"));

    CasRequestController controller(backend, CasRequestBudget{});
    const auto outcome = controller.putIfAbsentControlled("k", "payload", [] { return true; });
    EXPECT_EQ(outcome, CasWriteOutcome::Committed);
    EXPECT_EQ(backend->put_attempts.load(), 1u);
}

TEST(CasRequestController, UncertainResolvesDifferentThrowsCorruption)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    backend->put_thrower = [] { throw Poco::TimeoutException("scripted: ambiguous"); };
    backend->setGetOverride(resultWithBytes("someone-elses-bytes"));

    CasRequestController controller(backend, CasRequestBudget{});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        controller.putIfAbsentControlled("k", "payload", [] { return true; });
    });
}

/// GET-absent NEVER yields DefiniteFailure (spec §writer-side-linearization): the SAME (key, bytes) is
/// retried up to `max_attempts`, and only THEN does the call give up with Unresolved.
TEST(CasRequestController, UncertainResolvesAbsentRetriesSameKeyWithinBudget)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    backend->put_thrower = [] { throw Poco::TimeoutException("scripted: ambiguous"); };
    backend->setGetOverride(std::nullopt);   /// absent on every resolve

    CasRequestBudget budget;
    budget.max_attempts = 3;
    CasRequestController controller(backend, budget);
    const auto outcome = controller.putIfAbsentControlled("k", "payload", [] { return true; });
    EXPECT_EQ(outcome, CasWriteOutcome::Unresolved);
    EXPECT_EQ(backend->put_attempts.load(), 3u);   /// every attempt targeted the SAME key/bytes
}

/// The operation deadline — not just the attempt-count budget — cuts a retry loop short: a fake clock
/// advances by a fixed step per now_ms() call (no sleeps), and max_attempts is generous enough that only
/// the deadline check can be what stops the loop.
TEST(CasRequestController, OperationDeadlineExhaustionReturnsUnresolvedBeforeMaxAttempts)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    backend->put_thrower = [] { throw Poco::TimeoutException("scripted: ambiguous"); };
    backend->setGetOverride(std::nullopt);   /// absent on every resolve

    uint64_t clock = 0;
    auto now_ms = [&clock]() -> uint64_t { const uint64_t t = clock; clock += 200; return t; };

    CasRequestBudget budget;
    budget.max_attempts = 10;
    budget.attempt_timeout_ms = 50;
    budget.operation_deadline_ms = 450;
    CasRequestController controller(backend, budget, now_ms);
    const auto outcome = controller.putIfAbsentControlled("k", "payload", [] { return true; });
    EXPECT_EQ(outcome, CasWriteOutcome::Unresolved);
    EXPECT_EQ(backend->put_attempts.load(), 2u);   /// cut off well before the 10-attempt budget
}

TEST(CasRequestController, FenceLostBeforeAttemptSendsNoAttempt)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    CasRequestController controller(backend, CasRequestBudget{});
    const auto outcome = controller.putIfAbsentControlled("k", "payload", [] { return false; });
    EXPECT_EQ(outcome, CasWriteOutcome::Unresolved);
    EXPECT_EQ(backend->put_attempts.load(), 0u);
}

/// The write itself may have landed, but a fence lost between the write and this call's own final
/// check must never surface as Committed (RFC §ack-and-cache-rules: no ACK, no cache update on that
/// path) — the caller sees Unresolved and must not treat the operation as acknowledged.
TEST(CasRequestController, FenceLostAfterWriteNeverReturnsCommitted)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();   /// real in-memory commit path
    int fence_calls = 0;
    auto fence_ok = [&fence_calls] { return fence_calls++ == 0; };   /// true once, then false

    CasRequestController controller(backend, CasRequestBudget{});
    const auto outcome = controller.putIfAbsentControlled("k", "payload", fence_ok);
    EXPECT_EQ(outcome, CasWriteOutcome::Unresolved);
    EXPECT_EQ(backend->put_attempts.load(), 1u);   /// the write itself DID happen
    EXPECT_TRUE(backend->head("k").exists);        /// ...it is durable; never claimed as Committed here
}

TEST(CasRequestController, DefiniteFailurePropagatesImmediatelyWithoutResolve)
{
    auto backend = std::make_shared<ScriptedControllerBackend>();
    backend->put_thrower = [] { throw DB::S3Exception("scripted: malformed", Aws::S3::S3Errors::UNKNOWN, "MalformedXML"); };

    CasRequestController controller(backend, CasRequestBudget{});
    const auto outcome = controller.putIfAbsentControlled("k", "payload", [] { return true; });
    EXPECT_EQ(outcome, CasWriteOutcome::DefiniteFailure);
    EXPECT_EQ(backend->put_attempts.load(), 1u);   /// no retry, no resolve GET issued
}

/// Startup validation (RFC §required-timeout-model): a consistent default budget is accepted silently;
/// either inequality violated on its own is rejected with BAD_ARGUMENTS.
TEST(CasRequestController, ValidateBudgetAcceptsConsistentDefaults)
{
    EXPECT_NO_THROW(validateCasRequestBudget(CasRequestBudget{}, /*mount_lease_ttl_ms=*/30000, /*mount_renew_period_ms=*/10000));
}

TEST(CasRequestController, ValidateBudgetRejectsAttemptTimeoutPlusMarginAtOrAboveLeaseTtl)
{
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 25000;
    budget.lease_safety_margin_ms = 5000;   /// sums to EXACTLY the lease TTL below — not strictly less
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        validateCasRequestBudget(budget, /*mount_lease_ttl_ms=*/30000, /*mount_renew_period_ms=*/10000);
    });
}

TEST(CasRequestController, ValidateBudgetRejectsAttemptTimeoutAboveOperationDeadline)
{
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 6000;
    budget.operation_deadline_ms = 5000;
    budget.lease_safety_margin_ms = 1000;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        validateCasRequestBudget(budget, /*mount_lease_ttl_ms=*/30000, /*mount_renew_period_ms=*/10000);
    });
}

#endif
