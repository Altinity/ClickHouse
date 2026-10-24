#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DB::Cas
{

/// Counts the work performed by `decommissionPoolMember` for one pool member. The namespace counters
/// describe metadata and ref-log transitions; the object counters describe physical objects deleted by
/// the manifest, staging, and mountpoint drains. Blob bytes are intentionally not reported: removing
/// ref edges makes them eligible for ordinary GC, but this operation does not synchronously reclaim
/// shared content.
struct DecommissionReport
{
    String srid;                                  /// The decommissioned member's `server_root_id`.
    uint64_t namespaces_removed = 0;              /// Namespaces erased by this invocation.
    uint64_t namespaces_already_removed = 0;      /// Namespaces already `Removing` on entry.
    /// Victim rows still `Removing` when the run ended: GC deletes them; no rerun is needed for them.
    uint64_t namespaces_pending_gc = 0;
    uint64_t committed_refs_removed = 0;          /// Committed ref records removed by namespace drops.
    uint64_t precommits_removed = 0;              /// Precommit records removed by namespace drops.
    uint64_t edge_deltas_emitted = 0;             /// The sum of `committed_refs_removed` and `precommits_removed`.
    uint64_t manifest_debris_removed = 0;         /// Eligible manifest objects deleted from old build prefixes.
    uint64_t staging_objects_removed = 0;         /// Objects deleted from the member's staging prefix.
    uint64_t mountpoint_objects_removed = 0;      /// Objects deleted from the member's roots/mountpoint prefix.
    /// The owner carries a verified retirement: the member is retired, do not rerun.
    bool member_retired = false;
    /// Why the member is not retired: a drain failure, a lost claim, an unverified outcome (rerun), or an owner
    /// that moved on to a later life (read the owner before rerunning).
    std::vector<String> warnings;
};

/// Whether the slot still holds the admin's claim: the victim's uuid, `admin_epoch`, neither fenced nor
/// farewelled. Renewals change the ETag and `write_attempt_id`, so neither is compared.
bool adminClaimStillHeld(CasOperation & op, const Layout & layout, const String & srid, UInt128 victim_uuid, uint64_t admin_epoch);

/// Erases all content owned by a permanently dead pool member and retires its owner. Never deletes
/// `owner`, `epoch` or `mount`. The steps:
/// 1. Claim the member's slot as an administrative writer (`Pool::openForDecommission`). A live lease is
///    refused; an owner retired by an earlier run is accepted.
/// 2. Tombstone the owner on the admin's fenced mount plane: the next owner life, retired, keeping an earlier
///    verified mark. No attempt is sent after the admin's lease deadline.
/// 3. Confirm the claim still holds. A lost claim rolls the tombstone back unless the owner was already
///    retired; the same confirm runs on every exit before the farewell.
/// 4. Drain on the admin's fenced mount plane: each victim namespace through `Pool::dropNamespace`, then
///    eligible manifest debris, staging objects and mountpoint objects.
/// 5. Write the admin's clean farewell.
/// 6. Read the slot and the owner back and mark the retirement verified (the next owner life). Only a
///    verified mark reports `member_retired`.
///
/// A rerun resumes over the run's own unverified tombstone; a verified retirement is rerun-safe and
/// reports retired again. Catalog rows GC still has to delete count as `namespaces_pending_gc`, not as a
/// failure. Per-object drain failures are recorded in `DecommissionReport::warnings`; other failures,
/// including refusal to claim the member, propagate as exceptions. When set, `sink` receives
/// `MemberDecommission` audit events for the run's begin, per-namespace, and end milestones.
///
/// This is a writer operation, not GC: it emits the normal ref-edge deltas and does not invent ref
/// transitions.
///
/// `drain_now_fn`/`drain_sleep_fn`, when both set, replace the clock and retry sleep of the control-plane
/// request engine this function opens and of the admin `Pool`'s planes, and become the `Pool`'s boot clock
/// and retry sleep (`PoolConfig::boot_ms_fn`/`retry_sleep_fn`) whenever the caller left those unset: the
/// farewell deadline is bound to the boot clock, so one fake clock must drive all of them.
DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                          const String & victim_srid, const CasEventSink & sink = {},
                                          const std::function<void()> & request_gc_round = {},
                                          const std::function<uint64_t()> & drain_now_fn = {},
                                          const std::function<void(uint64_t)> & drain_sleep_fn = {});

}
