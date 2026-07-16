#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <base/types.h>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// One writer build prefix under `cas/manifests/<ns>/`: the canonical hex `<epoch-hex>-<seq-hex>/`
/// directory (spec §Manifest Identifier).
struct BuildPrefix
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
};

/// fix-round F9 (author-review: `reportLateLogsIfAny` re-emits the same `LOG_WARNING` +
/// `RefLateLogDetected` event every sweep pass, with no dedup, until GC's ordinary covered-log cleanup
/// finally removes the log -- which can be many rounds later). A one-shot in-memory latch, keyed by
/// (namespace, rendered log id), OWNED BY THE LEADER (one `Gc` instance's lifetime -- a fresh leader,
/// after a steal or a process restart, starts with an empty set, at worst re-emitting once) and passed
/// down through `sweepNamespace`/`activeManifestKeys` to suppress a repeat report of the SAME anomaly.
using LateLogDedup = std::set<std::pair<String, String>>;

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
///
/// `warnings`, when non-null, OPTS IN to the decommission drain's tolerate-and-continue contract (spec
/// §core "Fail-close"): a per-key transient failure (a thrown backend exception on `head`/`deleteExact`)
/// is pushed onto `*warnings` and the sweep continues with the next key, instead of throwing out of
/// this call; likewise a protection-view-unavailable namespace (the pre-existing corrupt-snapshot skip
/// below) also pushes a "cannot confirm emptiness" warning, not just a `LOG_WARNING`. `warnings ==
/// nullptr` (the default, every pre-existing caller) preserves the original behaviour exactly: a
/// per-key failure propagates as an exception (fail-close default), and the protection-view skip is
/// log-only. `NotFound`/`TokenMismatch` delete outcomes stay silently spared either way — those are the
/// normal "a fresh owner reclaimed it" race the periodic sweep expects, not a failure to warn about.
/// `dedup`, when non-null, is threaded to `reportLateLogsIfAny` (see `LateLogDedup`'s own doc comment)
/// -- `nullptr` (the default, every pre-existing caller) preserves the original behaviour exactly: no
/// dedup, every provably-late log is reported on every pass that lists it.
uint64_t sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix,
                        std::vector<String> * warnings = nullptr, LateLogDedup * dedup = nullptr);

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
    uint64_t delete_budget,
    LateLogDedup * dedup = nullptr);

}
