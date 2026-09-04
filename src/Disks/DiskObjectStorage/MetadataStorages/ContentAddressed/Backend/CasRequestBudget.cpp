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

    LOG_INFO(getLogger("CasRequestBudget"),
        "CAS request budget in effect: attempt_timeout_ms={} lease_safety_margin_ms={} "
        "(mount_lease_ttl_ms={} mount_renew_period_ms={})",
        budget.attempt_timeout_ms, budget.lease_safety_margin_ms, mount_lease_ttl_ms, mount_renew_period_ms);
}

}
