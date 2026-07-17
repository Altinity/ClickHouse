#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>

#include <Common/Exception.h>
#include <Common/LoggingHelpers.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace ProfileEvents
{
    extern const Event CasConditionalWriteAttempts;
    extern const Event CasConditionalWriteCommitted;
    extern const Event CasConditionalWriteDefiniteFailure;
    extern const Event CasConditionalWriteUnresolved;
    extern const Event CasConditionalWriteSdkRetries;
    extern const Event CasConditionalWriteFenceLostPostWrite;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
}
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
    /// reissuing, never toward a false
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

/// The default inter-attempt backoff sleep. NOT a race-fix sleep: it is deliberate, bounded,
/// fence-gated pacing of reissues toward a recovering object store, and it is injectable so tests
/// never wait on it.
void threadSleepMs(uint64_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// Deterministic caller/local bugs the create retry loop must surface immediately:
/// reissuing only replays the same failure — up to ~12 minutes of budget × putBlob's outer loop at
/// the defaults — and buries the root cause behind a retryable ABORTED. The set:
///   LOGICAL_ERROR   — a local invariant violation (e.g. uploadFromSource's source-size check; pinned
///                     by `CasPartWriteTxn.PutBlobWrongSizeFailsClosed`, which caught exactly this class)
///   NOT_IMPLEMENTED — a mode/capability guard (e.g. `promoteStaged` on a backend without a native
///                     conditional server-side copy) — a deterministic configuration bug
///   BAD_ARGUMENTS   — a deterministic encode/argument rejection (e.g. BAD_ARGUMENTS escaping
///                     buildHeader's second, intended_ref-less encode)
///   CORRUPTED_DATA  — integrity failure; retrying re-reads/re-streams the same bad bytes (the same
///                     fail-fast rule the driver-side correctness markers enforce)
/// Fail-safe either way: a propagated exception is never a false Committed.
bool isDeterministicLocalFailure(int code)
{
    return code == ErrorCodes::LOGICAL_ERROR || code == ErrorCodes::NOT_IMPLEMENTED
        || code == ErrorCodes::BAD_ARGUMENTS || code == ErrorCodes::CORRUPTED_DATA;
}

}

void validateCasRequestBudget(const CasRequestBudget & budget, uint64_t mount_lease_ttl_ms, uint64_t mount_renew_period_ms)
{
    if (budget.max_attempts < 1)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: max_attempts must be at least 1 (got {}) — zero would let "
            "putIfAbsentControlled return Unresolved without ever sending an attempt.",
            budget.max_attempts);

    /// Overflow-safe: `attempt_timeout_ms + lease_safety_margin_ms` could wrap uint64 for absurd config
    /// values, which would make the sum spuriously small and the inequality below pass when it should
    /// fail closed. Compare via subtraction against the (unsigned, so already non-negative) TTL instead
    /// of computing the sum directly.
    if (!(budget.attempt_timeout_ms < mount_lease_ttl_ms
          && budget.lease_safety_margin_ms < mount_lease_ttl_ms - budget.attempt_timeout_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: attempt_timeout_ms ({}) + lease_safety_margin_ms ({}) must be "
            "strictly less than the mount lease TTL ({} ms). A writable mount refuses to open with "
            "this budget.",
            budget.attempt_timeout_ms, budget.lease_safety_margin_ms, mount_lease_ttl_ms);
    if (!(budget.attempt_timeout_ms <= budget.operation_deadline_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: attempt_timeout_ms ({}) must not exceed operation_deadline_ms "
            "({}) — a single attempt cannot outlast the logical operation it belongs to.",
            budget.attempt_timeout_ms, budget.operation_deadline_ms);
    if (!(budget.retry_initial_backoff_ms <= budget.retry_max_backoff_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: retry_initial_backoff_ms ({}) must not exceed "
            "retry_max_backoff_ms ({}) — the capped-exponential backoff cap cannot sit below its own "
            "starting value. Set both to 0 to disable inter-attempt backoff.",
            budget.retry_initial_backoff_ms, budget.retry_max_backoff_ms);

    LOG_INFO(getLogger("CasRequestControl"),
        "CAS request budget in effect: attempt_timeout_ms={} operation_deadline_ms={} max_attempts={} "
        "lease_safety_margin_ms={} retry_initial_backoff_ms={} retry_max_backoff_ms={} "
        "(mount_lease_ttl_ms={} mount_renew_period_ms={})",
        budget.attempt_timeout_ms, budget.operation_deadline_ms, budget.max_attempts,
        budget.lease_safety_margin_ms, budget.retry_initial_backoff_ms, budget.retry_max_backoff_ms,
        mount_lease_ttl_ms, mount_renew_period_ms);
}

namespace
{
/// Shared by both public entry points below so the log line and the exception's message text can
/// never drift apart. Rate-limited (not per-distinct-`why` -- `LogSeriesLimiter` keys on the LOGGER
/// NAME only, so under a sustained outage where `why` keeps changing slightly, only the first message
/// in each window prints; this is the intended throttle, not a bug). Warning-level visibility is
/// intentional: this condition is expected to self-heal
/// (the caller retries), but an operator watching CAS logs directly should see it without having to
/// know to look at system.replication_queue.
void logCasWriteRetryLater(const String & why)
{
    LogSeriesLimiter log(getLogger("CasWriteRetryLater"), /*allowed_count=*/1, /*interval_s=*/30);
    LOG_WARNING(log, "CAS write could not be committed ({}); retrying later", why);
}
}

[[noreturn]] void throwCasWriteRetryLater(const String & why)
{
    logCasWriteRetryLater(why);
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why)
{
    logCasWriteRetryLater(why);
    return std::make_exception_ptr(
        Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why));
}

CasRequestController::CasRequestController(BackendPtr backend_, CasRequestBudget budget_, std::function<uint64_t()> now_ms_,
                                           std::function<void(uint64_t)> sleep_ms_)
    : backend(std::move(backend_))
    , budget(budget_)
    , now_ms(now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(steadyClockNowMs))
    , sleep_ms(sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(threadSleepMs))
{
}

void CasRequestController::setSleepFnForTest(std::function<void(uint64_t)> sleep_ms_)
{
    sleep_ms = sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(threadSleepMs);
}

uint64_t CasRequestController::backoffBeforeAttempt(uint32_t next_attempt) const
{
    const uint64_t initial = budget.retry_initial_backoff_ms;
    const uint64_t cap = budget.retry_max_backoff_ms;
    if (initial == 0 || next_attempt < 2)
        return 0;
    /// Saturating `initial << doublings`: `initial > cap >> doublings` implies the unshifted product
    /// already exceeds the cap, so return the cap without ever computing an overflowing shift.
    const uint32_t doublings = next_attempt - 2;
    if (doublings >= 63 || initial > (cap >> doublings))
        return cap;
    return std::min(initial << doublings, cap);
}

bool CasRequestController::pauseBeforeReissue(uint32_t completed_attempt, uint64_t deadline_ms, const std::function<bool()> & fence_ok)
{
    /// Fence BEFORE the sleep (the pre-attempt fence rule applies to the whole loop, not just the
    /// attempt): a fence lost mid-backoff aborts the operation instantly — sleeping first would keep a
    /// fenced writer alive for up to a full backoff cap after it lost its right to write.
    if (!fence_ok())
        return false;
    const uint64_t backoff = backoffBeforeAttempt(completed_attempt + 1);
    if (backoff == 0)
        return true;
    /// Never serve a sleep the operation cannot afford: if the backoff plus one more attempt would
    /// cross the operation deadline, give up NOW instead of sleeping into a guaranteed Unresolved.
    if (now_ms() + backoff + budget.attempt_timeout_ms > deadline_ms)
        return false;
    sleep_ms(backoff);
    return true;
}

CasWriteOutcome CasRequestController::resolveByExactGet(std::string_view key, std::string_view expected_bytes,
                                                        Token * out_token)
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
        /// way — an unresolved read leaves this Unresolved, exactly like an absent read.
        return CasWriteOutcome::Unresolved;
    }

    if (!got)
        return CasWriteOutcome::Unresolved;   /// absent -> another attempt may still be legal

    if (got->bytes == expected_bytes)
    {
        if (out_token)
            *out_token = got->token;
        return CasWriteOutcome::Committed;    /// identical deterministic bytes -> the earlier attempt DID commit
    }

    /// A DIFFERENT valid object at the exact key this create intended: a real conflict, not a retryable
    /// ambiguity. Fail closed rather than silently treating it as Unresolved/DefiniteFailure.
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CasRequestController: exact-key resolution at '{}' observed a DIFFERENT object than the one "
        "this attempt intended to create — a real conflict, not a retryable ambiguity", key_s);
}

CasWriteOutcome CasRequestController::putIfAbsentControlled(
    std::string_view key, std::string_view bytes, const std::function<bool()> & fence_ok, Token * out_token)
{
    const String key_s{key};
    const String bytes_s{bytes};
    const uint64_t deadline_ms = now_ms() + budget.operation_deadline_ms;

    for (uint32_t attempt = 1; attempt <= budget.max_attempts; ++attempt)
    {
        /// Gate BEFORE every attempt: the
        /// local mount fence must still hold, and there must be enough of the operation's own deadline
        /// left for one more attempt to plausibly complete. Neither check sends anything to the backend.
        if (!fence_ok())
            return CasWriteOutcome::Unresolved;
        if (now_ms() + budget.attempt_timeout_ms > deadline_ms)
            return CasWriteOutcome::Unresolved;

        /// The committed incarnation's token, filled by whichever leg proves Committed below.
        Token committed_token;
        CasWriteOutcome attempt_outcome;
        try
        {
            const PutResult put = backend->putIfAbsent(key_s, bytes_s);
            /// PreconditionFailed here means only "the key already exists" — it does NOT prove who
            /// created it (possibly OUR earlier unresolved attempt). Collapse it onto Unresolved so it
            /// goes through the SAME resolve-before-reissue path as an ambiguous exception, never a
            /// false DefiniteFailure/Committed.
            attempt_outcome = put.outcome == PutOutcome::Done ? CasWriteOutcome::Committed : CasWriteOutcome::Unresolved;
            if (put.outcome == PutOutcome::Done)
                committed_token = put.token;
        }
        catch (const std::exception & e)
        {
            attempt_outcome = classifyConditionalWriteResult(e);
        }

        if (attempt_outcome == CasWriteOutcome::DefiniteFailure)
            return CasWriteOutcome::DefiniteFailure;   /// proven never applied — no resolve, no retry

        if (attempt_outcome == CasWriteOutcome::Unresolved)
        {
            /// Resolve-before-reissue. May throw CORRUPTED_DATA (a real
            /// conflict) straight out of this call — that is never a retry signal.
            attempt_outcome = resolveByExactGet(key_s, bytes_s, &committed_token);
            if (attempt_outcome == CasWriteOutcome::Unresolved)
            {
                /// Absent/unreadable: another attempt of the SAME (key, bytes) may be legal — after the
                /// fence-gated capped-exponential backoff (pauseBeforeReissue). No pause after the LAST
                /// attempt: the budget is spent, sleeping would only delay the Unresolved verdict.
                if (attempt == budget.max_attempts || !pauseBeforeReissue(attempt, deadline_ms, fence_ok))
                    return CasWriteOutcome::Unresolved;
                continue;
            }
        }

        /// attempt_outcome == Committed here (either the attempt's own 2xx, or resolution found
        /// identical bytes). Final fence check before reporting success: a fence lost here means the
        /// write may have landed but this call must never claim it did. Count this "response observed
        /// after the local fence" leg separately from the generic Unresolved classifier so a cross-epoch
        /// fence loss is visible rather than folded into ordinary retry-budget exhaustion.
        if (!fence_ok())
        {
            ProfileEvents::increment(ProfileEvents::CasConditionalWriteFenceLostPostWrite);
            return CasWriteOutcome::Unresolved;
        }
        if (out_token)
            *out_token = committed_token;
        return CasWriteOutcome::Committed;
    }

    return CasWriteOutcome::Unresolved;   /// attempt budget exhausted without a definite outcome
}

CasCreateResult CasRequestController::conditionalCreateControlled(
    std::string_view key, const std::function<PutResult()> & attempt, const std::function<bool()> & fence_ok)
{
    const String key_s{key};
    const uint64_t deadline_ms = now_ms() + budget.operation_deadline_ms;

    for (uint32_t attempt_no = 1; attempt_no <= budget.max_attempts; ++attempt_no)
    {
        /// Same pre-attempt gates as putIfAbsentControlled.
        if (!fence_ok())
            return {CasCreateOutcome::Unresolved, {}};
        if (now_ms() + budget.attempt_timeout_ms > deadline_ms)
            return {CasCreateOutcome::Unresolved, {}};

        std::optional<PutResult> put;
        try
        {
            put = attempt();
        }
        catch (const std::exception & e)
        {
            /// A deterministic LOCAL bug surfaced by the attempt itself — a caller/config error, never
            /// a wire ambiguity; reissuing would only replay it. Propagate unchanged: instant, loud,
            /// exactly the pre-controller behavior (see isDeterministicLocalFailure for the set and the
            /// per-code rationale). This deliberately differs from `putIfAbsentControlled`'s
            /// everything-Unresolved:
            /// that lane's byte-exact resolve makes retrying any unproven error harmless, while
            /// retrying a broken source/mode/encode here is pure noise. Fail-safe either way — a
            /// propagated exception is never a false Committed.
            if (const auto * db_e = dynamic_cast<const Exception *>(&e); db_e && isDeterministicLocalFailure(db_e->code()))
                throw;
            /// A whitelisted synchronous rejection PROVES the request was never applied: surface the
            /// original exception — the blob lane's callers always saw the raw storage error's root
            /// cause, and losing it behind an outcome enum here would only degrade diagnostics
            /// Anything else is ambiguous: fall through to the occupancy
            /// resolve below.
            if (classifyConditionalWriteResult(e) == CasWriteOutcome::DefiniteFailure)
                throw;
        }

        if (put)
        {
            if (put->outcome == PutOutcome::PreconditionFailed)
                return {CasCreateOutcome::Occupied, {}};

            /// Done. Final fence check before reporting success: a fence
            /// lost here means the write may have landed but this call must never claim it did.
            if (!fence_ok())
            {
                ProfileEvents::increment(ProfileEvents::CasConditionalWriteFenceLostPostWrite);
                return {CasCreateOutcome::Unresolved, {}};
            }
            return {CasCreateOutcome::Committed, put->token};
        }

        /// Ambiguous attempt: resolve by exact-key OCCUPANCY — one HEAD, never a body GET (the body
        /// may be multi-GB, and reading a possibly-condemned occupant would flirt with the resurrect
        /// invariant; the key's content-address IS the identity proof, see the header contract).
        bool occupied = false;
        bool head_answered = true;
        try
        {
            occupied = backend->head(key_s).exists;
        }
        catch (const std::exception &)
        {
            /// The HEAD itself failed: occupancy unproven either way. Reissuing is still safe — an
            /// occupant answers the reissued If-None-Match with PreconditionFailed (-> Occupied on
            /// the next round) — so treat exactly like "absent" and let the budget bound the loop.
            head_answered = false;
        }
        if (head_answered && occupied)
            return {CasCreateOutcome::Occupied, {}};

        if (attempt_no == budget.max_attempts || !pauseBeforeReissue(attempt_no, deadline_ms, fence_ok))
            return {CasCreateOutcome::Unresolved, {}};
    }

    return {CasCreateOutcome::Unresolved, {}};   /// attempt budget exhausted without a definite outcome
}

}
