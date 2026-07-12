#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h>

#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <chrono>
#include <utility>

namespace ProfileEvents
{
    extern const Event CasConditionalWriteAttempts;
    extern const Event CasConditionalWriteCommitted;
    extern const Event CasConditionalWriteDefiniteFailure;
    extern const Event CasConditionalWriteUnresolved;
    extern const Event CasConditionalWriteSdkRetries;
}

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
}

namespace DB::Cas
{

#if USE_AWS_S3
namespace
{

/// A synchronous rejection PROVING the request was never applied — matched by the canonical S3 error
/// code STRING (many of these are UNKNOWN in the SDK's modeled enum, mirroring
/// ObjectStorageBackend::finalizeConditionalWrite's own name-first matching) plus the modeled enum
/// value where one exists, belt-and-suspenders.
bool isMalformedRequest(const S3Exception & e)
{
    const String & name = e.getExceptionName();
    return name == "MalformedXML" || name == "MalformedPOSTRequest" || name == "InvalidArgument"
        || name == "InvalidRequest" || name == "InvalidBucketName" || name == "KeyTooLongError"
        || e.getS3ErrorCode() == Aws::S3::S3Errors::INVALID_PARAMETER_VALUE
        || e.getS3ErrorCode() == Aws::S3::S3Errors::INVALID_REQUEST
        || e.getS3ErrorCode() == Aws::S3::S3Errors::VALIDATION;
}

bool isEntityTooLarge(const S3Exception & e)
{
    /// No modeled enum value for this error — name-only match, same as PreconditionFailed elsewhere.
    return e.getExceptionName() == "EntityTooLarge";
}

bool isAccessDenied(const S3Exception & e)
{
    const String & name = e.getExceptionName();
    return name == "AccessDenied" || name == "InvalidAccessKeyId" || name == "SignatureDoesNotMatch"
        || name == "InvalidToken" || name == "ExpiredToken" || name == "AccountProblem"
        || e.getS3ErrorCode() == Aws::S3::S3Errors::ACCESS_DENIED
        || e.getS3ErrorCode() == Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID
        || e.getS3ErrorCode() == Aws::S3::S3Errors::SIGNATURE_DOES_NOT_MATCH
        || e.getS3ErrorCode() == Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID;
}

}
#endif

CasWriteOutcome classifyConditionalWriteResult(const std::exception & e)
{
#if USE_AWS_S3
    /// `PreconditionFailed`/`NoSuchKey` (a lost If-None-Match/If-Match — see
    /// ObjectStorageBackend::finalizeConditionalWrite for the exact matching), any 5xx
    /// (InternalError/ServiceUnavailable/SlowDown/RequestTimeout), and any S3 error this function does
    /// not recognize all fall through to the fail-safe default below: Unresolved. Only the WHITELIST
    /// below proves the request was never applied.
    if (const auto * s3e = dynamic_cast<const S3Exception *>(&e))
    {
        if (isMalformedRequest(*s3e) || isEntityTooLarge(*s3e) || isAccessDenied(*s3e))
            return CasWriteOutcome::DefiniteFailure;
    }
#endif
    /// Poco::Net::NetException (connection loss) / Poco::TimeoutException (client-side timeout) and
    /// every other error type: the request's fate is unproven — fail toward "resolve before
    /// reissuing" (RFC cas-s3-timeout-retry-control §resolve-before-reissuing), never toward a false
    /// DefiniteFailure.
    return CasWriteOutcome::Unresolved;
}

void recordConditionalWriteAttemptStarted()
{
    ProfileEvents::increment(ProfileEvents::CasConditionalWriteAttempts);
}

void recordConditionalWriteOutcome(CasWriteOutcome outcome)
{
    switch (outcome)
    {
        case CasWriteOutcome::Committed:
            ProfileEvents::increment(ProfileEvents::CasConditionalWriteCommitted);
            return;
        case CasWriteOutcome::DefiniteFailure:
            ProfileEvents::increment(ProfileEvents::CasConditionalWriteDefiniteFailure);
            return;
        case CasWriteOutcome::Unresolved:
            ProfileEvents::increment(ProfileEvents::CasConditionalWriteUnresolved);
            return;
    }
}

void recordConditionalWriteSdkRetryConsidered()
{
    ProfileEvents::increment(ProfileEvents::CasConditionalWriteSdkRetries);
}

namespace
{

uint64_t steadyClockNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

void validateCasRequestBudget(const CasRequestBudget & budget, uint64_t mount_lease_ttl_ms, uint64_t mount_renew_period_ms)
{
    if (!(budget.attempt_timeout_ms + budget.lease_safety_margin_ms < mount_lease_ttl_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: attempt_timeout_ms ({}) + lease_safety_margin_ms ({}) must be "
            "strictly less than the mount lease TTL ({} ms) — RFC cas-s3-timeout-retry-control "
            "§required-timeout-model. A writable mount refuses to open with this budget.",
            budget.attempt_timeout_ms, budget.lease_safety_margin_ms, mount_lease_ttl_ms);
    if (!(budget.attempt_timeout_ms <= budget.operation_deadline_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: attempt_timeout_ms ({}) must not exceed operation_deadline_ms "
            "({}) — a single attempt cannot outlast the logical operation it belongs to.",
            budget.attempt_timeout_ms, budget.operation_deadline_ms);

    LOG_INFO(getLogger("CasRequestControl"),
        "CAS request budget in effect: attempt_timeout_ms={} operation_deadline_ms={} max_attempts={} "
        "lease_safety_margin_ms={} (mount_lease_ttl_ms={} mount_renew_period_ms={})",
        budget.attempt_timeout_ms, budget.operation_deadline_ms, budget.max_attempts,
        budget.lease_safety_margin_ms, mount_lease_ttl_ms, mount_renew_period_ms);
}

CasRequestController::CasRequestController(BackendPtr backend_, CasRequestBudget budget_, std::function<uint64_t()> now_ms_)
    : backend(std::move(backend_))
    , budget(budget_)
    , now_ms(now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(steadyClockNowMs))
{
}

CasWriteOutcome CasRequestController::resolveByExactGet(std::string_view key, std::string_view expected_bytes)
{
    const String key_s{key};
    std::optional<GetResult> got;
    try
    {
        got = backend->get(key_s);
    }
    catch (const std::exception &)
    {
        /// The GET itself failed (network, auth, ...): the object's identity cannot be proven either
        /// way — RFC §resolve-before-reissuing leaves this Unresolved, exactly like an absent read.
        return CasWriteOutcome::Unresolved;
    }

    if (!got)
        return CasWriteOutcome::Unresolved;   /// absent -> another attempt may still be legal

    if (got->bytes == expected_bytes)
        return CasWriteOutcome::Committed;    /// identical deterministic bytes -> the earlier attempt DID commit

    /// A DIFFERENT valid object at the exact key this create intended: a real conflict, not a retryable
    /// ambiguity. Fail closed rather than silently treating it as Unresolved/DefiniteFailure.
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CasRequestController: exact-key resolution at '{}' observed a DIFFERENT object than the one "
        "this attempt intended to create — a real conflict (RFC cas-s3-timeout-retry-control "
        "§resolve-before-reissuing), not a retryable ambiguity", key_s);
}

CasWriteOutcome CasRequestController::putIfAbsentControlled(
    std::string_view key, std::string_view bytes, const std::function<bool()> & fence_ok)
{
    const String key_s{key};
    const String bytes_s{bytes};
    const uint64_t deadline_ms = now_ms() + budget.operation_deadline_ms;

    for (uint32_t attempt = 1; attempt <= budget.max_attempts; ++attempt)
    {
        /// Gate BEFORE every attempt (RFC §ack-and-cache-rules step 1 / §required-timeout-model): the
        /// local mount fence must still hold, and there must be enough of the operation's own deadline
        /// left for one more attempt to plausibly complete. Neither check sends anything to the backend.
        if (!fence_ok())
            return CasWriteOutcome::Unresolved;
        if (now_ms() + budget.attempt_timeout_ms > deadline_ms)
            return CasWriteOutcome::Unresolved;

        CasWriteOutcome attempt_outcome;
        try
        {
            const PutResult put = backend->putIfAbsent(key_s, bytes_s);
            /// PreconditionFailed here means only "the key already exists" — it does NOT prove who
            /// created it (possibly OUR earlier unresolved attempt). Collapse it onto Unresolved so it
            /// goes through the SAME resolve-before-reissue path as an ambiguous exception, never a
            /// false DefiniteFailure/Committed.
            attempt_outcome = put.outcome == PutOutcome::Done ? CasWriteOutcome::Committed : CasWriteOutcome::Unresolved;
        }
        catch (const std::exception & e)
        {
            attempt_outcome = classifyConditionalWriteResult(e);
        }

        if (attempt_outcome == CasWriteOutcome::DefiniteFailure)
            return CasWriteOutcome::DefiniteFailure;   /// proven never applied — no resolve, no retry

        if (attempt_outcome == CasWriteOutcome::Unresolved)
        {
            /// Resolve-before-reissue (RFC §resolve-before-reissuing). May throw CORRUPTED_DATA (a real
            /// conflict) straight out of this call — that is never a retry signal.
            attempt_outcome = resolveByExactGet(key_s, bytes_s);
            if (attempt_outcome == CasWriteOutcome::Unresolved)
                continue;   /// absent/unreadable: another attempt of the SAME (key, bytes) may be legal
        }

        /// attempt_outcome == Committed here (either the attempt's own 2xx, or resolution found
        /// identical bytes). Final fence check before reporting success (RFC §ack-and-cache-rules step
        /// 4): a fence lost here means the write may have landed but this call must never claim it did.
        if (!fence_ok())
            return CasWriteOutcome::Unresolved;
        return CasWriteOutcome::Committed;
    }

    return CasWriteOutcome::Unresolved;   /// attempt budget exhausted without a definite outcome
}

}
