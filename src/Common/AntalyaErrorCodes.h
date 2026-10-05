#pragma once

/// Local IDs are append-only, never reused, and independent of upstream assignments.
/// Keep this registry ordered by local ID. Wire codes are `10000 + local_id`.
/// Codes must fit in `UInt16` to preserve their identity in part and background-pool logs.
#define APPLY_FOR_ANTALYA_ERROR_CODES(M) \
    M(1, CATALOG_NAMESPACE_DISABLED) \
    M(2, PENDING_MUTATIONS_NOT_ALLOWED) \
    M(3, EXPORT_PARTITION_ALREADY_EXPORTED) \
    M(4, PARTITION_EXPORT_FAILED) \
    M(5, CAS_WRITE_UNATTRIBUTED) \
    M(6, CAS_DELETE_MARKER)

namespace DB::ErrorCodes
{
inline constexpr int ANTALYA_ERROR_CODE_BASE = 10000;
}
