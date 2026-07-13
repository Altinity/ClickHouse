#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <base/types.h>
#include <cstdint>
#include <exception>
#include <functional>
#include <string_view>

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

/// The three separate limits a CAS-owned retry controller enforces for ONE logical conditional-write
/// operation (RFC cas-s3-timeout-retry-control §required-timeout-model). Never represented by a
/// single `request_timeout_ms` value — see `validateCasRequestBudget` for the relationship a writable
/// mount enforces at startup, and `CasRequestController` for the runtime use.
struct CasRequestBudget
{
    /// Maximum client wait budgeted for one HTTP attempt. `CasRequestController` uses this ONLY as a
    /// per-attempt scheduling check (an attempt is not started unless it could still finish inside the
    /// operation deadline) — the actual socket-level wait is configured on the object storage's client
    /// (Task 4, ObjectStorageBackend's single-attempt client), not by this struct.
    uint64_t attempt_timeout_ms = 5000;
    /// Maximum wall-clock time for the COMPLETE logical operation — every attempt and every exact-key
    /// resolution — counted from the first call to `putIfAbsentControlled`. A DURATION, not an
    /// absolute deadline: each call establishes its own `now + operation_deadline_ms` bound.
    uint64_t operation_deadline_ms = 20000;
    /// Maximum number of controlled attempts for one logical operation (the first attempt counts as 1).
    uint32_t max_attempts = 5;
    /// Startup-only margin folded into `validateCasRequestBudget`'s inequality against the mount lease
    /// TTL. Not consulted at runtime by the controller itself — the caller's `fence_ok` callback (backed
    /// by the local write fence's own deadline) is what actually gates lease-relative timing per attempt.
    uint64_t lease_safety_margin_ms = 2000;
};

/// Startup validation (RFC §required-timeout-model): a writable mount refuses to open with an
/// inconsistent budget rather than silently falling back to an unbounded/unsafe retry policy. Throws
/// `BAD_ARGUMENTS` unless BOTH hold:
///   attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms
///   attempt_timeout_ms <= operation_deadline_ms
/// `mount_renew_period_ms` takes no part in the inequality (the renewer keeps the fence deadline
/// refreshed well ahead of the TTL by construction) — it is accepted only so the effective-values log
/// line records the full picture in one place.
void validateCasRequestBudget(const CasRequestBudget & budget, uint64_t mount_lease_ttl_ms, uint64_t mount_renew_period_ms);

/// CAS-owned retry controller (RFC cas-s3-timeout-retry-control): the ONLY place that decides whether a
/// conditional-write attempt may be reissued, and the seam Tasks 8-11 build the ref writer's append lane
/// on. It does not itself touch a writer cache or return ACK — RFC §ack-and-cache-rules' "update the
/// cache and return ACK only after outcome resolution and a final fence check" is the CALLER's job,
/// driven off the `CasWriteOutcome` this class returns.
class CasRequestController
{
public:
    /// `now_ms_`: monotonic-ish clock, defaulting to `std::chrono::steady_clock`; tests inject a fake
    /// one to drive deadline behavior deterministically (no sleeps).
    CasRequestController(BackendPtr backend_, CasRequestBudget budget_, std::function<uint64_t()> now_ms_ = {});

    /// Controlled `putIfAbsent` with resolve-before-reissue (RFC §resolve-before-reissuing). Performs at
    /// most `budget.max_attempts` attempts of the exact SAME (key, bytes) — never a different key, never
    /// a different body — bounded by `budget.operation_deadline_ms` measured from this call's own start.
    /// `fence_ok` is consulted before EVERY attempt (a false answer sends no further attempt) and once
    /// more before a `Committed` return (a false answer there means the write may have landed but this
    /// call reports `Unresolved`, never a false `Committed` — RFC §ack-and-cache-rules). An uncertain
    /// attempt is resolved via `resolveByExactGet` before deciding whether to reissue.
    /// Throws `CORRUPTED_DATA` if resolution ever observes DIFFERENT valid bytes at `key` — a real
    /// conflict, never collapsed into `Unresolved`/`DefiniteFailure`. Returns `Unresolved` (never
    /// throws) when the fence is lost or the budget is exhausted before a definite outcome is reached.
    /// `out_token` (optional): set ONLY on a `Committed` return, to the committed incarnation's token —
    /// the attempt's own `PutResult` token, or the token the resolve GET observed when it proved an
    /// earlier ambiguous attempt landed. Lets audit emitters (e.g. `Build::stageManifest`'s
    /// `ManifestPut` event) keep the token without a follow-up HEAD. Untouched on any other return.
    CasWriteOutcome putIfAbsentControlled(std::string_view key, std::string_view bytes,
                                          const std::function<bool()> & fence_ok, Token * out_token = nullptr);

    /// One-shot exact-key resolution of an uncertain immutable-create (RFC §resolve-before-reissuing):
    ///   - identical bytes observed at `key`  -> Committed (the earlier attempt DID commit)
    ///   - DIFFERENT bytes observed at `key`  -> throws CORRUPTED_DATA (a real conflict, not a retry
    ///     signal — never silently treated as ambiguous)
    ///   - absent, or the GET itself fails    -> Unresolved (another attempt may still be legal)
    /// NEVER returns DefiniteFailure: an absent or unreadable key proves nothing about whether the
    /// original request will eventually be provably non-applied, so resolution alone can never produce
    /// that verdict. `out_token` (optional): set ONLY on `Committed`, to the observed incarnation's token.
    CasWriteOutcome resolveByExactGet(std::string_view key, std::string_view expected_bytes,
                                      Token * out_token = nullptr);

private:
    BackendPtr backend;
    CasRequestBudget budget;
    std::function<uint64_t()> now_ms;
};

}
