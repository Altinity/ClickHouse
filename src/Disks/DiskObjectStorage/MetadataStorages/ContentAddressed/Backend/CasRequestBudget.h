#pragma once
#include <cstdint>

namespace DB::Cas
{

/// The three separate limits a CAS-owned retry controller enforces for ONE logical conditional-write
/// operation. Never represented by a single `request_timeout_ms` value — see `validateCasRequestBudget`
/// for the relationship a writable mount enforces at startup, and `CasRequestController` for the
/// runtime use.
struct CasRequestBudget
{
    /// Maximum client wait budgeted for one HTTP attempt. `CasRequestController` uses this ONLY as a
    /// per-attempt scheduling check (an attempt is not started unless it could still finish inside the
    /// operation deadline) — the actual socket-level wait is configured on the object storage's client
    /// (the object storage backend's single-attempt client), not by this struct.
    uint64_t attempt_timeout_ms = 5000;
    /// Maximum wall-clock time for the COMPLETE logical operation — every attempt, every exact-key
    /// resolution, and every inter-attempt backoff sleep — counted from the first call to
    /// `putIfAbsentControlled`. A DURATION, not an absolute deadline: each call establishes its own
    /// `now + operation_deadline_ms` bound.
    ///
    /// This deadline is the authoritative bound on how long a CAS conditional write keeps riding an S3
    /// disruption server-side before the caller sees an abort. 90s absorbs a ~60s object-store outage
    /// with margin (see the arithmetic on `max_attempts` below) — PROVIDED the mount fence stays alive.
    /// The fence, not this deadline, is
    /// what binds under a TOTAL outage: lease renewals are conditional writes against the same store,
    /// so when everything is unreachable the fence deadline freezes at `last_renew + mount_lease_ttl`
    /// and `fence_ok` stops the loop ≈ TTL−attempt_timeout−margin (~23s) after the last successful
    /// renewal — the required fail-closed behavior (never an attempt past the lease), not a
    /// budget limitation. While renewals DO land (blips, throttling, partial outages — the runtime-owned
    /// renewal worker keeps extending the fence deadline), the op is NOT bounded by
    /// the lease TTL and rides the full deadline here.
    uint64_t operation_deadline_ms = 90000;
    /// Maximum number of controlled attempts for one logical operation (the first attempt counts as 1).
    /// Sized so the operation deadline above — never this count — is what binds under the observed
    /// failure shape (~3s adaptive first-attempt PUT timeout per failed attempt + capped-exponential
    /// backoff): 16 attempts × ~3s + Σ backoff (0.2+0.4+0.8+1.6+3.2 + 10×5 = 56.2s) ≈ 104s > 90s.
    uint32_t max_attempts = 16;
    /// Startup-only margin folded into `validateCasRequestBudget`'s inequality against the mount lease
    /// TTL. Not consulted at runtime by the controller itself — the caller's `fence_ok` callback (backed
    /// by the local write fence's own deadline) is what actually gates lease-relative timing per attempt.
    uint64_t lease_safety_margin_ms = 2000;
    /// Inter-attempt backoff (`cas_s3_retry_initial_backoff_ms` /
    /// `cas_s3_retry_max_backoff_ms`): the sleep before reissuing
    /// after an ambiguous attempt whose resolve observed the key absent, capped exponential —
    /// `initial · 2^(reissues-1)`, never above `retry_max_backoff_ms`. 0 disables backoff (immediate
    /// reissue — the pre-backoff behavior, and what most exhaustion-path unit tests configure). The
    /// controller checks the fence BEFORE every sleep and never sleeps past the operation deadline.
    uint64_t retry_initial_backoff_ms = 200;
    uint64_t retry_max_backoff_ms = 5000;

    /// Recovery-level retry (`CasRefLedger::ensureRefTableRecovered`): a whole ref-table recovery
    /// attempt (LIST + snapshot/log GETs + seal PUT) that fails with a transient NETWORK_ERROR is
    /// retried, with capped-exponential backoff, until this total wall-clock budget is spent — then the
    /// error propagates and the table's load fails for this touch (the `lazy_load_tables` database
    /// setting makes the NEXT touch retry). This sits ON TOP of the per-request `operation_deadline_ms`
    /// envelope above: one recovery attempt may itself burn ~90s inside a single seal PUT. Independent
    /// of the mount-lease invariants validated in `validateCasRequestBudget` — not part of that
    /// inequality set.
    uint64_t recovery_retry_budget_ms = 120000;
    uint64_t recovery_retry_initial_backoff_ms = 1000;
    uint64_t recovery_retry_max_backoff_ms = 30000;
};

/// Startup validation: a writable mount refuses to open with an inconsistent budget rather than
/// silently falling back to an unbounded or unsafe retry policy. Throws
/// `BAD_ARGUMENTS` unless ALL hold:
///   attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms
///   attempt_timeout_ms < operation_deadline_ms          (STRICTLY — see below)
///   retry_initial_backoff_ms <= retry_max_backoff_ms
///
/// The middle one is strict on purpose. Equality does not mean "one attempt's worth of budget": the
/// deadline is captured as `now + operation_deadline_ms` and each pre-send gate asks
/// `now + attempt_timeout_ms > deadline_ms`, so equal values reduce it to `now_2 > now_1` and a single
/// elapsed millisecond refuses the operation having sent NOTHING. Bound the attempt COUNT with
/// `max_attempts`, never by starving the deadline.
/// `mount_renew_period_ms` takes no part in the inequality (the renewer keeps the fence deadline
/// refreshed well ahead of the TTL by construction) — it is accepted only so the effective-values log
/// line records the full picture in one place.
///
/// A successor mounting over an unclean predecessor waits at least one lease TTL, plus its
/// materialization grace period, before trusting recovery listings. This is long enough for any
/// conditional PUT still in flight at the predecessor to either land or be abandoned by its own
/// exhausted retry budget. The predecessor's budget is constrained by
/// `attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms`, so no additional handover
/// check is needed here.
void validateCasRequestBudget(const CasRequestBudget & budget, uint64_t mount_lease_ttl_ms, uint64_t mount_renew_period_ms);

}
