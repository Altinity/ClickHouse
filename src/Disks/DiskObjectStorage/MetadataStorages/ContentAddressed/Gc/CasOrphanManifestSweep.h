#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
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

/// One manifest object the sweep is considering, carrying both halves the §6 deletion premise needs:
/// the full object key (what the tail's removal targets are named by) and the build prefix it lives
/// under (whose `writer_epoch` is the epoch whose closing seal must be consumed).
struct ManifestKey
{
    String key;
    BuildPrefix prefix;
};

/// The durable per-namespace fold state the §6 deletion premise reads. It is taken from the ADOPTED
/// fold seal — the `gc/state` -> `fold_seal` pair the sweep already read to learn its cursor, kept
/// whole instead of reduced to that one field.
///
/// IT IS NOT DERIVED FROM A LISTING, and the type exists to keep it that way. Arithmetic ref intake
/// demoted the listing to a hint precisely because a store may omit a durable ref-log key from an
/// enumeration; a premise that re-derived its answer by listing the tail would inherit that hole and
/// license exactly the deletion it exists to withhold.
struct NamespaceFoldView
{
    /// The namespace's shard-0 coverage row. `nullopt` means the adopted seal carries no row for this
    /// namespace at all: no round has ever sealed a ref cursor for it, so no epoch's closing seal is
    /// proven consumed and every manifest under it is retained.
    std::optional<ShardCoverage> coverage;

    /// Manifest object keys the tail ABOVE the cursor names as REMOVAL targets, as the namespace's
    /// protection view collected them. Rule (2) is a POSITIVE test against this set: a key found here
    /// retains. Its negative direction proves nothing on its own — the set is assembled from the same
    /// enumeration arithmetic intake distrusts — which is why rule (1) and not this set is what makes
    /// the tail decidable. See `manifestDeletionPremise`.
    std::set<String> tail_removal_targets;
};

/// Spec §6, the sweep deletion premise, as ONE predicate both sweep paths call. A manifest of an
/// epoch-`E` build is deletable only when:
///   (1) the namespace cursor has consumed epoch `E`'s seal, AND
///   (2) no unconsumed tail record above the cursor names it as a removal target
///       (removals cross epochs; grants do not).
/// ANY uncertainty — an unreached frontier, an exhausted budget, a hold — means RETAIN. Delay is never
/// damage: a body kept one round longer costs storage, while a body deleted under an unproven cut is
/// either data loss (an unfolded `+1` still names it) or a fold that clamps forever on the missing body.
///
/// Rule (1) is what makes rule (2) decidable rather than a second guess at the same enumeration.
/// Grants do not cross epochs, so every `+1` that could name an epoch-`E` build lives among epoch `E`'s
/// own records; and an epoch is left ONLY over its consumed `EpochSeal` (INV-2), so a sealed cursor in
/// a STRICTLY HIGHER epoch is durable proof that every one of those records is folded. Removals do
/// cross epochs, which is why rule (2) exists at all and why it is a separate test.
///
/// `retain_reason`, when non-null, receives the reason the premise refused — it feeds the sweep's
/// `warnings` out-param so a retained manifest is a visible decision rather than a silent one.
bool manifestDeletionPremise(const NamespaceFoldView & view, const ManifestKey & manifest,
                             String * retain_reason);

/// Read one namespace's fold view out of the adopted fold seal. `tail_removal_targets` is left empty:
/// the callers that have a protection view fill it from theirs, and a caller that has none gets the
/// coverage half alone, which is the half rule (1) needs.
NamespaceFoldView namespaceFoldView(Pool & store, const RootNamespace & ns);

/// One-shot in-memory suppression for a late-reference-log report. The latch is keyed by namespace and
/// rendered log id and is owned by one leader for the lifetime of its `Gc` instance. It is passed through
/// the sweep to the LIST-only late-log detector, so a log that remains visible until ordinary covered-log
/// cleanup catches up is reported once per leader rather than once per sweep pass. A new leader after
/// leadership transfer or process restart starts with an empty latch and may report the same anomaly once.
using LateLogDedup = std::set<std::pair<String, String>>;

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
/// `dedup`, when non-null, is threaded to `reportLateLogsIfAny` (see `LateLogDedup`'s own doc comment)
/// -- `nullptr` (the default, every pre-existing caller) preserves the original behaviour exactly: no
/// dedup, every provably-late log is reported on every pass that lists it.
uint64_t sweepNamespace(Pool & store, const RootNamespace & ns, const BuildPrefix & prefix,
                        std::vector<String> * warnings = nullptr, LateLogDedup * dedup = nullptr);

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
    uint64_t delete_budget,
    LateLogDedup * dedup = nullptr);

}
