#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasStore.h>

#include <cstdint>
#include <string>
#include <vector>

namespace DB::Cas
{

/// What `decommissionPoolMember` did to a DEAD pool member's namespace (design
/// 2026-07-13-cas-pool-member-decommission §core). Namespace erasure (this task, Task 2) fills the
/// first five fields; the manifest/staging/mountpoint sweeps (Task 3) and the slot deletion (Task 4)
/// fill the rest -- their fields default to "nothing done yet" so a Task-2-only run reports honestly.
struct DecommissionReport
{
    String srid;                                 /// the decommissioned member's server_root_id
    uint64_t namespaces_removed = 0;              /// tables whose namespace this run erased
    uint64_t namespaces_already_removed = 0;      /// tables a PRIOR (partial) run had already erased
    uint64_t committed_refs_removed = 0;
    uint64_t precommits_removed = 0;
    uint64_t edge_deltas_emitted = 0;             /// == committed_refs_removed + precommits_removed
    uint64_t manifest_debris_removed = 0;         /// Task 3
    uint64_t staging_objects_removed = 0;         /// Task 3
    uint64_t mountpoint_objects_removed = 0;      /// Task 3
    bool slot_removed = false;                    /// Task 4
    std::vector<String> warnings;                 /// non-empty ⇒ the pool slot is kept (Task 4)
};

/// Operator-driven offline erasure of a DEAD pool member's namespace (`SYSTEM CONTENT ADDRESSED DROP
/// POOL MEMBER`, design 2026-07-13-cas-pool-member-decommission §core). Opens an admin writer mount
/// impersonating `victim_srid` (`Store::openForDecommission` -- fail-closed on a live member) and
/// erases every one of the victim's namespaces via the ordinary `Store::dropNamespace` writer path:
/// this call is a WRITER, never GC, and never invents a ref transition of its own. `sink` (when set)
/// receives `MemberDecommission` audit events for the run's begin/per-namespace/end.
DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                          const String & victim_srid, const CasEventSink & sink = {});

}
