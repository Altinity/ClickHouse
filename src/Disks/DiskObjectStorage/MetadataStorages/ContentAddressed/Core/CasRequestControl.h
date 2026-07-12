#pragma once
#include <base/types.h>
#include <cstdint>
#include <exception>

namespace DB::Cas
{

/// Outcome of ONE HTTP attempt for a CAS conditional write (`If-None-Match`/`If-Match`), issued with
/// the generic S3 client's transparent retries disabled for that attempt (RFC
/// cas-s3-timeout-retry-control §disable-transparent-conditional-write-retries). This is the seam the
/// full retry controller (Task 5, `CasRequestController`) is built on: it decides whether another
/// attempt is legal and how an uncertain result is resolved.
///   - Committed: the attempt's own request completed successfully (2xx) — the object is durable.
///   - DefiniteFailure: a synchronous rejection that PROVES the request was never applied server-side
///     — a WHITELISTED malformed-request / entity-too-large / access-denied error ONLY. Never
///     `PreconditionFailed`: a lost precondition means the key exists, not that the request failed.
///   - Unresolved: everything else — `PreconditionFailed`/`NoSuchKey`, a client-side timeout, a
///     connection loss, a 5xx, or any error this classifier does not recognize. The caller resolves
///     the exact key (Task 5) before deciding whether another attempt is legal; ambiguity always
///     resolves toward Unresolved, never toward a false DefiniteFailure or a false Committed.
enum class CasWriteOutcome : uint8_t
{
    Committed,
    DefiniteFailure,
    Unresolved,
};

/// The success path: `buf.finalize()` returned without throwing. Always Committed — kept as a named,
/// counted entry point so both paths of a classify-then-record call site read the same way (see the
/// exception overload below).
constexpr CasWriteOutcome classifyConditionalWriteResult()
{
    return CasWriteOutcome::Committed;
}

/// The exception path: classify what `buf.finalize()` threw for ONE CAS conditional-write HTTP
/// attempt, per RFC cas-s3-timeout-retry-control §operation-classes. Pure — never rethrows, never
/// touches counters; see recordConditionalWriteOutcome for the counters hookup.
CasWriteOutcome classifyConditionalWriteResult(const std::exception & e);

/// Records the start of one HTTP attempt for a CAS conditional write (the attempts counter).
void recordConditionalWriteAttemptStarted();

/// Records one attempt's terminal outcome (the per-class outcome counters). Callers pass the result of
/// whichever classifyConditionalWriteResult overload applies, or an outcome already known by
/// construction (e.g. the legacy `PutOutcome::PreconditionFailed` path, which today resolves without
/// throwing — see ObjectStorageBackend::nativeConditionalPut).
void recordConditionalWriteOutcome(CasWriteOutcome outcome);

/// Records that the S3 SDK's retry strategy was consulted about issuing a SECOND (or later) HTTP
/// attempt for a CAS conditional write (RFC cas-s3-timeout-retry-control §observability: "SDK-level
/// retries, which must remain zero for conditional writes"). The single-attempt client's retry
/// strategy (CasObjectStorageBackend.cpp) always answers no, but the consultation itself proves the
/// first attempt did not conclusively succeed from the SDK's point of view — this is the live tripwire
/// that makes the counter provably wired, rather than a value nothing ever touches.
void recordConditionalWriteSdkRetryConsidered();

}
