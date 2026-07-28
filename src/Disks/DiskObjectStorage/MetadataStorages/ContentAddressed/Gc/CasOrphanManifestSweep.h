#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <base/types.h>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// One writer build prefix under `cas/manifests/<ns>/`: the canonical hex `<epoch-hex>-<seq-hex>/`
/// directory encoded as canonical hexadecimal epoch and sequence components.
struct BuildPrefix
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
};

/// Counters returned by one bounded cursor page. `listed` counts keys in the backend page, `skipped`
/// counts malformed, protected, ineligible, budget-exhausted, or race-spared keys, and `deleted` counts
/// only successful exact-token deletions. `next_cursor` and `wrapped` describe the backend cursor; the
/// cursor is a cleanup-progress hint and is never used as reachability authority.
struct ManifestSweepResult
{
    String next_cursor;
    bool wrapped = false;
    uint64_t listed = 0;
    uint64_t deleted = 0;
    uint64_t skipped = 0;
};

/// Per-namespace pre-precommit orphan sweep. Deletes
/// manifest bodies written before `PrecommitAdd` and never named by any live owner, scoped to ONE
/// namespace + ONE build prefix. Rules:
///   - eligibility from the durable watermark fact only: the retired sentinel
///     (`min_active == UINT64_MAX`), or `min_active > build_sequence`, or a replaced incarnation —
///     NEVER a frozen-seq / judged-dead heuristic alone (a missing watermark => not eligible);
///   - the active `ManifestId` set comes from the namespace's committed + live-precommit owner view;
///   - delete only bodies whose `ManifestId` is ABSENT from the active set, by exact token;
///   - emits NO blob deltas (a pre-precommit body never contributed `+1`);
///   - a 404 between listing and deletion is record-and-continue, never a throw;
///   - never GETs a condemned body to revive it — eligibility +
///     exact-token delete only.
/// Returns the number of bodies actually deleted (a `DeleteClass::Deleted`-classified exact-token
/// delete only, never a spared `NotFound`/`TokenMismatch`) — the decommission manifest-debris drain
/// (`Core/CasDecommission.cpp`) sums this across every eligible build prefix into
/// `DecommissionReport::manifest_debris_removed`.
///
/// `warnings`, when non-null, opts in to the decommission drain's tolerate-and-continue contract: a
/// per-key transient failure (a thrown backend exception on `head`/`deleteExact`)
/// is pushed onto `*warnings` and the sweep continues with the next key, instead of throwing out of
/// this call; likewise a protection-view-unavailable namespace (the pre-existing corrupt-snapshot skip
/// below) also pushes a "cannot confirm emptiness" warning, not just a `LOG_WARNING`. `warnings ==
/// nullptr` (the default, every pre-existing caller) preserves the original behaviour exactly: a
/// per-key failure propagates as an exception (fail-close default), and the protection-view skip is
/// log-only. `NotFound`/`TokenMismatch` delete outcomes stay silently spared either way — those are the
/// normal "a fresh owner reclaimed it" race the periodic sweep expects, not a failure to warn about.
uint64_t sweepNamespace(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix,
                        std::vector<String> * warnings = nullptr);

/// Whether `prefix` is sweep-eligible by the durable watermark fact alone. The floor is read from the
/// mount lease identified by the namespace's server-root prefix, not inferred from the manifest key or a
/// judged-dead heuristic. A missing lease provides no deletion authority, so the prefix is not eligible.
bool prefixEligible(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix);

/// Cursor-paced bounded orphan part-manifest sweep over `cas/manifests/`. It evaluates each namespace's
/// durable protection view before deleting and uses exact-token deletion, so a concurrent owner or a
/// changed object token is spared. The cursor is a best-effort cleanup cursor, never reachability
/// authority. The page lists at most `list_budget` keys and deletes at most `delete_budget` of them.
ManifestSweepResult sweepManifestCursorPage(
    Pool & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget);

}
