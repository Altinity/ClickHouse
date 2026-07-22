#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>

namespace DB::Cas
{

/// Authoritative, cache-bypassing probe of one key. NEVER conflates transport errors with absence:
/// timeouts / 5xx / connection errors => Indeterminate; permission errors => AccessDenied;
/// missing container/bucket/prefix-parent => ContainerAbsent; a clean authoritative miss => KeyAbsent.
///
/// Free-function entry point (spec §2) — a thin dispatch to the backend's own typed-evidence
/// classification (`Backend::probeSentinelRaw`; see there for the per-backend semantics: the
/// S3-native raw HEAD error, the Local container-directory stat, or the generic head/get-based
/// default for a backend without sharper evidence).
SentinelProbeResult probeSentinel(Backend & backend, const String & key);

/// Container proof: ListObjectsV2(max-keys=1, prefix=pool_root) — NOT bucket HEAD.
/// Returns Present when the LIST succeeded and found >=1 object, KeyAbsent when it succeeded and
/// found ZERO objects (the pool-wide emptiness observation), AccessDenied / ContainerAbsent /
/// Indeterminate otherwise. Thin dispatch to `Backend::probePrefixEmptinessRaw`.
SentinelProbeResult probePrefixEmptiness(Backend & backend, const String & pool_root_prefix);

}
