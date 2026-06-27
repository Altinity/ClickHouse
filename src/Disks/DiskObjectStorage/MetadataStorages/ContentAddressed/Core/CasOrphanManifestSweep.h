#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <base/types.h>
#include <optional>

namespace DB::Cas
{

/// One writer build prefix under `_manifests`: `<writer_instance_id>/<build_sequence>/`.
struct BuildPrefix
{
    String writer_instance_id;
    uint64_t build_sequence = 0;
};

/// A sweep target: one namespace + one eligible build prefix to scan this round.
struct SweepTarget
{
    RootNamespace ns;
    BuildPrefix prefix;
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
void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix);

/// Whether `prefix` is sweep-eligible by the durable watermark fact alone (OQ6). The watermark is keyed
/// by the server hex (the segment before ':' in `writer_instance_id`). No watermark => not eligible.
bool prefixEligible(Store & store, const BuildPrefix & prefix);

/// Choose one eligible (namespace, build prefix) to sweep this round, or nullopt when there is nothing
/// eligible. Bounded: at most one namespace + one prefix per round (a rare backstop, not the hot path).
std::optional<SweepTarget> pickOneSweepTarget(Store & store);

}
