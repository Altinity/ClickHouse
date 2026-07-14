#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <base/types.h>
#include <optional>

namespace DB::Cas
{

/// One writer build prefix under `cas/manifests/<ns>/`: the canonical hex `<epoch-hex>-<seq-hex>/`
/// directory (spec §Manifest Identifier).
struct BuildPrefix
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
};

struct ManifestSweepResult
{
    String next_cursor;
    bool wrapped = false;
    uint64_t listed = 0;
    uint64_t deleted = 0;
    uint64_t skipped = 0;
};

/// Per-namespace pre-precommit orphan sweep (spec rev. 15 §Orphan-Part-Manifest-Cleanup-Sweep). Deletes
/// manifest bodies written before `PrecommitAdd` and never named by any live owner, scoped to ONE
/// namespace + ONE build prefix. Rules:
///   - eligibility from the durable watermark fact ONLY (OQ6): the retired sentinel
///     (`min_active == UINT64_MAX`), or `min_active > build_sequence`, or a replaced incarnation —
///     NEVER a frozen-seq / judged-dead heuristic alone (a missing watermark => not eligible);
///   - the active `ManifestId` set comes from the namespace's committed + live-precommit owner view;
///   - delete only bodies whose `ManifestId` is ABSENT from the active set, by exact token;
///   - emits NO blob deltas (a pre-precommit body never contributed `+1`);
///   - a 404 mid-sweep is record-and-continue, never a throw (feedback_ca_gc_never_throw_on_404);
///   - never GETs a condemned body to revive it (feedback_ca_resurrect_invariant) — eligibility +
///     exact-token delete only.
/// Returns the number of bodies actually deleted (a `DeleteClass::Deleted`-classified exact-token
/// delete only, never a spared `NotFound`/`TokenMismatch`) — the decommission manifest-debris drain
/// (`Core/CasDecommission.cpp`) sums this across every eligible build prefix into
/// `DecommissionReport::manifest_debris_removed`.
uint64_t sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix);

/// Whether `prefix` is sweep-eligible by the durable watermark fact alone (OQ6). The watermark is resolved
/// from the namespace's server_root_id, not by parsing writer identity. No watermark => not eligible.
bool prefixEligible(Store & store, const RootNamespace & ns, const BuildPrefix & prefix);

/// Cursor-paced bounded orphan part-manifest sweep over `cas/manifests/`. The cursor is a best-effort
/// cleanup cursor, never reachability authority. It deletes at most `delete_budget` keys from at most
/// `list_budget` listed keys.
ManifestSweepResult sweepManifestCursorPage(
    Store & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget);

}
