#pragma once
#include <cstdint>

namespace DB::Cas
{

/// The limits a writable mount is configured with. `attempt_timeout_ms` and `lease_safety_margin_ms`
/// are what `CasMountRuntime::admit` measures a request against, and the three `recovery_retry_*` fields
/// bound a whole ref-table recovery; see `validateCasRequestBudget` for the relationship a writable
/// mount enforces at startup.
struct CasRequestBudget
{
    /// Maximum client wait budgeted for one HTTP attempt. The request contract reserves this before
    /// every attempt it starts; the actual socket-level wait is configured on the object storage's
    /// client (the object storage backend's single-attempt client), not by this struct.
    uint64_t attempt_timeout_ms = 5000;
    /// Startup-only margin folded into `validateCasRequestBudget`'s inequality against the mount lease
    /// TTL. Not consulted at runtime by the engine itself -- the caller's fence (backed by the local
    /// write fence's own deadline) is what actually gates lease-relative timing per attempt.
    uint64_t lease_safety_margin_ms = 2000;

    /// Recovery-level retry (`CasRefLedger::ensureRefTableRecovered`): a whole ref-table recovery
    /// attempt (LIST + snapshot/log GETs + seal PUT) that fails with a transient NETWORK_ERROR is
    /// retried, with capped-exponential backoff, until this total wall-clock budget is spent — then the
    /// error propagates and the table's load fails for this touch (the `lazy_load_tables` database
    /// setting makes the NEXT touch retry). This sits ON TOP of each write's own `Retry` policy window
    /// (`Retry::standard()`'s 90s): one recovery attempt may itself burn ~90s inside a single seal PUT.
    /// Independent of the mount-lease invariants validated in `validateCasRequestBudget` — not part of
    /// that inequality set.
    uint64_t recovery_retry_budget_ms = 120000;
    uint64_t recovery_retry_initial_backoff_ms = 1000;
    uint64_t recovery_retry_max_backoff_ms = 30000;
};

/// Startup validation: a writable mount refuses to open with an inconsistent budget rather than
/// silently falling back to an unbounded or unsafe retry policy. Throws
/// `BAD_ARGUMENTS` unless:
///   attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms
///
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
