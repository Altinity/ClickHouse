#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

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
    /// STRICTLY less, and the strictness is the load-bearing half. `attempt_timeout_ms >
    /// operation_deadline_ms` is the obvious error — a single attempt cannot outlast the logical
    /// operation it belongs to. EQUALITY is the subtle one, and it is worse than useless: the deadline
    /// is captured as `now + operation_deadline_ms` and every pre-send gate below asks
    /// `now + attempt_timeout_ms > deadline_ms`, so equal values collapse that to `now_2 > now_1` and
    /// ONE elapsed millisecond between the two clock reads refuses the operation having sent NOTHING.
    /// The resulting behaviour is "mostly works, occasionally refuses with nothing sent", decided by
    /// the scheduler rather than by the budget — exactly the flakiness this validation exists to catch,
    /// and observed three times in tests before it was forbidden. A caller that wants one attempt says
    /// `max_attempts = 1`; the equality adds only the race.
    if (!(budget.attempt_timeout_ms < budget.operation_deadline_ms))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS request budget rejected: attempt_timeout_ms ({}) must be strictly less than "
            "operation_deadline_ms ({}) — equality turns the pre-send gate into a wall-clock race that "
            "refuses after a single elapsed tick, having sent nothing. Use max_attempts to bound the "
            "number of attempts.",
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

}
