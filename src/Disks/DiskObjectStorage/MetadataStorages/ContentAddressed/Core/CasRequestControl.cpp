#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h>

#include <Common/ProfileEvents.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

namespace ProfileEvents
{
    extern const Event CasConditionalWriteAttempts;
    extern const Event CasConditionalWriteCommitted;
    extern const Event CasConditionalWriteDefiniteFailure;
    extern const Event CasConditionalWriteUnresolved;
    extern const Event CasConditionalWriteSdkRetries;
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

}
