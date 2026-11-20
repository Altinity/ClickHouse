#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasToolDeadline.h>
#include <Common/ThreadPool_fwd.h>

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Optional progress sink for `runFsck`: called periodically during the listing and reachability
/// walk so a long scan over a large/slow pool is visibly progressing (not hung). `phase` names the
/// current step; `objects`/`pages` are running counts. Default {} = no progress (existing callers).
using FsckProgress = std::function<void(std::string_view phase, uint64_t objects, uint64_t pages)>;

/// Classification assigned to each object examined by `runFsck`.
///
/// The reachability classes are derived only from authoritative refs and the physical object listing.
/// The GC-related classes are an additional explanation for present-but-unreferenced blobs; GC state
/// is used for labeling only and can never make a referenced object appear safe. Integrity classes are
/// hard findings: the report remains unclean when any of them is present.
enum class FsckClass : uint8_t
{
    Reachable,     /// reachable from a live ref AND present in the object store
    Dangling,      /// reachable from a live ref but the object is MISSING — INV-NO-LOSS violation
    Unreachable,   /// pre-precommit manifest debris (labeled reclaimable / in-flight)
    /// The GC pipeline deletes present-but-unreferenced blobs in explicit stages, so these classes
    /// distinguish expected in-flight work from an object outside the GC view. They are labels only,
    /// never inputs to reachability.
    PendingGc,     /// has a `candidate` or `pending` row for the body present now — deletion is scheduled; EXPECTED. A
                   /// `pending` row is counted here only after its deletion intent was read and found.
    AwaitingGc,    /// edges still in the GC snapshot (drop/reclaim not folded yet) or GC never ran — EXPECTED
    Unaccounted,   /// absent from the whole GC view — transient for a fast create+drop between rounds;
                   /// PERSISTENT occurrences should be impossible (INV-2 reachability-before-content)
    StaleEdge,     /// every source edge the GC snapshot still holds on this blob names a manifest that no
                   /// longer exists anywhere in the pool, so the matching `-1` can never fold: the blob's
                   /// in-degree can never reach zero and the incremental GC can never reclaim it. Only a
                   /// full rebuild of the in-degree state can. ERROR — never an `AwaitingGc` "expected"
                   /// backlog, which is exactly the label that used to hide it.
    CorruptedRun,  /// a GC source-edge run, or the fold seal naming it, that a GC round refuses as corrupt
                   /// bookkeeping; the note says why. ERROR
    /// The two verdicts of the ref-stream walk. They are about a NAMESPACE, not an object; the row's `key`
    /// identifies the exact log where the walk stopped or the checkpoint-named snapshot base whose
    /// required triple could not be validated.
    ChainBroken,   /// the chain ends below a listed log, or its snapshot base, a record or a link on it is
                   /// corrupt while `_ckpt` is unchanged. ERROR
    Unchecked,     /// the walk could not prove this namespace's stream EITHER WAY (an unprovable epoch
                   /// crossing, an undecodable body, or unstable authority/transport). Not a finding and
                   /// not a clean bill of health: the honest third answer, reported so nobody reads a
                   /// silence as a proof. Also an unreferenced blob of a GC shard whose snapshot rows were not
                   /// read: the refusal or the unread snapshot is the finding, and the blob is not examined.
    LifelessKey,   /// a namespace-tree key the `Layout` parsers refuse (a malformed/non-canonical shape,
                   /// including the un-incarnated layout), OR a catalog incarnation that is
                   /// ambiguous or otherwise unreadable. Neither a current writer nor the catalog's own
                   /// reverse life index can produce this key's meaning, so it belongs to no namespace
                   /// and no per-namespace verdict can carry it. ERROR
    JanitorPending,/// a COMPLETE, canonical namespace-life key (parses via the exact writer grammar,
                   /// nonzero 32-hex life id) whose life is simply absent from a catalog cut taken AFTER
                   /// the physical listing. This is the protocol-produced interval between a fenced GC
                   /// exact-deleting a `Removing` catalog row and the perpetual `NamespaceJanitor`
                   /// reaching this key on a later bounded page -- inert debris, not damage. Reported
                   /// as a soft finding: NOT in `kFsckHardFindings`, does not fail the report.
    /// The five verdicts on a deletion intent (`blob-intents/`). The key names the blob incarnation the
    /// intent warns about; the verdict compares it with the body now at the blob's key. An intent outlives
    /// its body until GC removes it, so a missing body is the normal state of a deleted blob. The first
    /// three need the per-intent reads of a verifying scan (`verify_intents`); a plain scan classifies an
    /// intent from its key alone.
    IntentCurrent,   /// the body present now is the incarnation the intent names
    IntentReplaced,  /// the body present now is another incarnation; GC removes this intent later
    IntentBodyGone,  /// no body at the blob's key; GC removes the intent in a later round
    IntentMalformed, /// the key is not an intent key, or (verifying scan) the bytes are not the intent record. ERROR
    IntentUnread,    /// a read or a `HEAD` failed, or the body's token has no canonical identity: the scan could
                     /// not verify what it was asked to. ERROR
    /// A `pending` row is the one state in which GC deletes a body without re-reading the intent, so the
    /// intent of every `pending` row is read.
    PendingWithoutIntent,  /// a `pending` row, its intent absent, the body present with the row's token: GC would
                           /// delete a body no writer is warned about. ERROR
    PendingIntentAbsent,   /// a `pending` row, its intent absent, the body absent or another token: the row is about
                           /// to become `standing` or to move; nothing is at risk
    PendingTokenRejected,  /// a `pending` row whose token has no canonical identity: no intent key exists for it
    StandingPresent,       /// a `standing` row, the body present with the row's token: GC records it and deletes
                           /// nothing; the intent is present, so a writer publishes fresh
    /// A `.meta` object left from the marker protocol. Nothing reads it; `cas-migrate-pool` removes it
    /// and `--fix legacy-markers` can.
    LegacyMarker,
};

/// One object or integrity finding emitted in detailed mode, or emitted for every missing reachable
/// object even in summary mode. `key` identifies the physical or logical object; `size` is its listed
/// size and is zero for a missing object. `reachable_from` contains `"namespace/ref"` owners for
/// reachable and dangling objects, or a diagnostic note for other classifications.
struct FsckObject
{
    String key;
    ObjectKind kind = ObjectKind::Blob;
    uint64_t size = 0;                   /// on-disk object size (0 when dangling)
    FsckClass cls = FsckClass::Reachable;
    std::vector<String> reachable_from;  /// "ns/ref" labels (populated for reachable/dangling when detail)
};

/// Aggregate result of a read-only `runFsck` scan.
///
/// Reachability and byte counters describe the scan's authoritative-ref view. `unreachable` is the
/// total of all present-but-unreferenced objects, including the GC pipeline classes and manifest debris,
/// and is intentionally retained as one monotone number for residual-settling monitoring. The detailed
/// `objects` list is populated according to the scan's `detail` mode. In partial mode all counters are
/// lower bounds over the portion walked before the deadline; `clean` must not be used as a claim about
/// the unvisited part of the pool.
struct FsckReport
{
    uint64_t reachable = 0;
    uint64_t dangling = 0;
    /// TOTAL of everything present-but-unreferenced (blob pipeline classes below + manifest debris).
    /// Kept as the sum so residual-settling loops (soak) keep one monotone number to watch.
    uint64_t unreachable = 0;
    uint64_t pending_gc = 0;     /// blobs in the retired set — deletion scheduled (expected)
    uint64_t awaiting_gc = 0;    /// blobs whose drop is not folded yet / GC never ran (expected)
    uint64_t unaccounted = 0;    /// blobs outside the GC view (transient or anomaly)
    /// Blobs whose every remaining source edge names a manifest that no longer exists — permanently
    /// stuck at a nonzero in-degree, unreclaimable by the incremental GC. A hard ERROR (see
    /// `FsckClass::StaleEdge`). Populated only in `detail` mode: naming the live sources costs one GET
    /// per manifest body, which the summary path does not pay.
    uint64_t stale_edge = 0;

    /// Deletion intents under `blob-intents/` (see the `Intent*` classes of `FsckClass`). `intent_malformed`
    /// and `intent_unread` are hard findings; the others are informational. A scan with a namespace scope
    /// lists no intents and leaves them all at zero.
    uint64_t intents_listed = 0;      /// intents whose key parses (a plain scan counts them and reads none)
    uint64_t intent_current = 0;     /// the next three only in a verifying scan
    uint64_t intent_replaced = 0;
    uint64_t intent_body_gone = 0;
    uint64_t intent_malformed = 0;
    /// Reads or `HEAD`s of a deletion intent or its body that failed, a GC snapshot whose fold seal is absent under
    /// an unchanged `gc/state`, and a GC snapshot not read because `gc/state` moved at every read. A hard finding: the scan could not verify what it was asked to, and a refused prefix
    /// would otherwise read as a clean pool.
    uint64_t intent_unread = 0;
    String intent_unread_reason;     /// the first failure: error code and message, shortened
    bool intents_verified = false;   /// the scan ran the per-intent verification

    /// `pending` rows whose present body is the row's incarnation (see the `Pending*` classes of `FsckClass`).
    /// `pending_without_intent` is a hard finding.
    uint64_t pending_without_intent = 0;
    uint64_t pending_intent_absent = 0;
    uint64_t pending_token_rejected = 0;
    /// `standing` rows over a body that is present with the row's token. Counted apart from `pending_gc`:
    /// advisory, the report stays clean.
    uint64_t standing_present = 0;

    /// `.meta` objects of the marker protocol found under `blobs/`: debris, informational. The pairing
    /// of bodies with markers no longer exists. `legacy_markers_deleted` counts the ones a fix run removed.
    uint64_t legacy_marker = 0;
    uint64_t legacy_markers_deleted = 0;
    uint64_t legacy_markers_changed = 0;   /// replaced between the fix's `HEAD` and its delete, and left

    /// Fold seals and GC source-edge runs a GC round refuses as corrupt, under a `gc/state` that did not move while
    /// they were read. Cataloged with the key and the reason in `objects`. A refused shard is read no further; the
    /// audit goes on with the next shard.
    uint64_t corrupted_runs = 0;

    /// The arithmetic ref-stream walk (spec §7). fsck reads each namespace's stream by EXACT KEY from
    /// `_ckpt.checkpoint`'s successor upward — never from a listing, which may omit durable records —
    /// and reports one verdict per namespace.
    ///
    /// `chain_broken` counts namespaces with a proven hole (see `FsckClass::ChainBroken`) and is a HARD
    /// ERROR: part of `clean`, and the command exits nonzero on it. `unchecked` counts namespaces the
    /// walk could not prove either way; it is COVERAGE, not a finding, so it
    /// is reported and printed but does not make a report unclean — exactly like `partial`. A pool with
    /// nothing wrong reads `chain_broken=0 unchecked=0`, so `unchecked` is never a resting state.
    ///
    /// `ref_records_walked` is how many ref-log records the walk actually read and proved, summed over
    /// namespaces. It is what makes "the tail above the checkpoint was walked" observable rather than
    /// inferred from the absence of a complaint.
    uint64_t chain_broken = 0;
    uint64_t unchecked = 0;
    uint64_t ref_records_walked = 0;

    /// Keys the namespace enumeration could not attribute to any namespace, OR a catalog incarnation
    /// that is ambiguous or unreadable (see `FsckClass::LifelessKey` and `Cas::NamespaceListing`). Does
    /// NOT include a complete, canonical namespace-life key whose life is simply absent from the catalog
    /// -- that is `namespace_janitor_pending`, counted separately and not a hard finding. Counted
    /// DISTINCT by key: the scan enumerates namespaces several times and every sweep sees the same
    /// offending key, so a per-sweep count would multiply one defect. A hard finding: no current writer
    /// can produce this key's meaning, and an audit is where an operator finds out about it.
    uint64_t lifeless_keys = 0;

    /// Canonical namespace-life keys whose life is absent from a catalog cut taken AFTER the physical
    /// listing (see `FsckClass::JanitorPending`). SOFT: never in `kFsckHardFindings`, never fails the
    /// report. Persistent non-convergence across authorized janitor cycles is an operational leak
    /// question (`CASGCNamespaceCleanupLeaks`, the `namespace_cleanup` GC-log phase), not an integrity
    /// finding this counter can answer on its own -- one snapshot cannot prove an unbounded leak.
    uint64_t namespace_janitor_pending = 0;
    uint64_t namespace_janitor_pending_bytes = 0;
    uint64_t namespace_janitor_pending_lives = 0;   /// distinct life ids counted above

    uint64_t physical_bytes = 0;
    uint64_t referenced_logical_bytes = 0;
    uint64_t total_blob_refs = 0;
    uint64_t distinct_blobs = 0;

    /// Set when the scan hit its deadline in partial mode: counts cover only what was walked
    /// before the deadline — a lower bound, not the pool truth.
    bool partial = false;
    String partial_reason;

    std::vector<FsckObject> objects;

    /// Return logical blob references per distinct reachable blob, or zero when no distinct blob was seen.
    double dedupRatio() const { return distinct_blobs ? double(total_blob_refs) / double(distinct_blobs) : 0.0; }

    /// Return whether the scan found no missing reachable object or hard integrity violation. Expected
    /// GC backlog classes do not make a report unclean, and neither do the informational intent classes
    /// or `legacy_marker`; a partial report only covers the visited subset. `stale_edge` is a hard
    /// finding, but it is only ever nonzero in `detail` mode — a clean summary report says nothing about
    /// stale edges, exactly as a partial report says nothing about the unvisited part of the pool.
    /// `chain_broken` is a hard finding in every mode. `unchecked` deliberately is NOT one: it says the
    /// walk proved nothing about those namespaces, which is a statement about COVERAGE, and folding it
    /// in here would make "cannot prove" indistinguishable from "found broken".
    /// Defined out-of-line below, over `kFsckHardFindings`, so that "a term of `clean`" and "a row of
    /// that list" are the same thing rather than two lists that can drift.
    bool clean() const;
};

/// ONE hard finding: the name every surface renders it under, and the counter it reads.
struct FsckHardFinding
{
    std::string_view name;
    uint64_t FsckReport::* value;
};

/// THE HARD FINDINGS, and the single authority on what they are. `FsckReport::clean` is computed from
/// this list, so adding a term means adding a row here.
///
/// The name is the one the text summary line and the SQL result column both use, which is what lets a
/// test check a rendering surface by iterating this list instead of restating its contents.
/// The SIZE IS DEDUCED, deliberately. A fixed `std::array<FsckHardFinding, N>` rejects an added row with
/// an "excess elements in ..." diagnostic -- which stops the build, but its text carries none of the
/// guidance the assert below does, so the author learns only that they miscounted. (Which noun that
/// diagnostic uses depends on the brace form, so it is not quoted here.) Deduced, an added row compiles
/// and the assert is what speaks.
inline constexpr std::array kFsckHardFindings{
    FsckHardFinding{"dangling", &FsckReport::dangling},
    FsckHardFinding{"corrupted_runs", &FsckReport::corrupted_runs},
    FsckHardFinding{"stale_edge", &FsckReport::stale_edge},
    FsckHardFinding{"chain_broken", &FsckReport::chain_broken},
    FsckHardFinding{"lifeless_keys", &FsckReport::lifeless_keys},
    FsckHardFinding{"intent_malformed", &FsckReport::intent_malformed},
    FsckHardFinding{"intent_unread", &FsckReport::intent_unread},
    FsckHardFinding{"pending_without_intent", &FsckReport::pending_without_intent},
};

/// TRIPWIRE. A hard finding has to reach three CODE surfaces, and each has been forgotten at least once:
/// the text summary line (`formatFsckSummary`), `CommandFsck::executeImpl`'s nonzero-exit set, and the
/// SQL result row (`contentAddressedFsckColumns` + `appendContentAddressedFsckRow`). It has happened
/// repeatedly, on more than one occasion and to more than one term, each time with the rule written down
/// in prose and each time the prose not holding. (No count is given: the records that document those
/// episodes do not support one number, and a tally nobody can reconstruct is the same defect as the rest.)
///
/// ONE ROW OF THIS LIST IS DELIBERATELY NOT IN THE EXIT SET, so the "three surfaces" rule has a named
/// exception rather than a silent violation: `stale_edge` is nonzero only under `--detail`, and
/// `CommandFsck::executeImpl` prints it as a `note:` and never throws. What licenses that is the pair --
/// a documented reason AND a compensating gate elsewhere (`stale_edge_verdict` in
/// `utils/ca-soak/soak/fsck.py`, asserted by the soak checkpoint in `soak/run.py`, which fails closed
/// when the key is absent). A new finding may take the same exception only WITH both halves; without
/// them it belongs in the exit set.
///
/// WHAT THIS ASSERT CHECKS, precisely: that the number of hard findings still equals the number written
/// here. Nothing more. It does NOT check that any surface renders them -- it cannot see the renderers,
/// which is the whole reason it lives with the struct: this header is included by the summary formatter,
/// by `programs/disks/CommandFsck.cpp`, and by `src/Interpreters/InterpreterSystemQuery.cpp`, so changing
/// the list breaks the build in every TU that owes an update, including the two no unit test can reach.
///
/// The summary line is checked for real, by a test that iterates the list
/// (`CasFsckSummary.EveryHardFindingAppearsOnTheSummaryLine`). The exit set and the SQL row are NOT --
/// for those, this assert plus the list below it is the whole of the mechanism, so bumping the number
/// without visiting them defeats it. Bump it only after all three are done.
///
/// AND IT REACHES NO PROSE. The rule is also restated in `docs/superpowers/cas/AGENTS.md`
/// and in the soak harness's comments and messages; those restatements have gone stale before -- more
/// than once, about the exit set -- and nothing here can break a build over them. (No count is given,
/// for the same reason the paragraph above gives none: nobody keeping a tally of restatements can
/// promise its own count will not go stale next.) They are a fourth surface, unfenced by construction.
static_assert(kFsckHardFindings.size() == 8,
    "A hard finding was added to or removed from `kFsckHardFindings`, which is `FsckReport::clean`. "
    "Before updating this count, render it in ALL THREE code surfaces: `formatFsckSummary`'s line, "
    "`CommandFsck::executeImpl`'s nonzero-exit set, and `contentAddressedFsckColumns` + "
    "`appendContentAddressedFsckRow`. Two of the three have no test that can fail for you -- the "
    "comment above this assert says which. A finding may be left out of the exit set only the way "
    "`stale_edge` is: with a documented reason AND a compensating soak assert.");

inline bool FsckReport::clean() const
{
    for (const FsckHardFinding & finding : kFsckHardFindings)
        if (this->*finding.value != 0)
            return false;
    return true;
}

/// Independently recompute reachability from authoritative refs (never from GC state or snapshots) and
/// diff it against a raw object listing. The operation is read-only unless `fix` is set; `detail`
/// populates per-object rows. A plain scan classifies deletion intents from the listing alone; it reads
/// the intent of each `pending` row. `verify_intents` adds, per intent, a `GET` of its bytes and a
/// `HEAD` of its body (two requests per intent) for the `Intent*` verdicts. `fix_legacy_markers` deletes each legacy
/// marker found with `removeLegacyMarkers`, through the scan's own read-only open (no mount, no lease), and needs a
/// scan without `namespace_prefix`, `fix_concurrency` markers at a time. It is the scan's only repair: nothing deletes an
/// intent or a body.
/// `deadline` bounds the WHOLE scan, each request's retries included, and throws `TIMEOUT_EXCEEDED` when it passes —
/// unless `partial_on_deadline` is set, in which case the accumulated lower-bound counts are returned instead,
/// flagged via `FsckReport::partial`. `namespace_prefix`, if non-empty, scopes the scan to namespaces with this
/// prefix and skips the pool-wide unreachable classification (dangling-only mode).
FsckReport runFsck(Pool & store, bool detail, FsckProgress on_progress = {},
                   const ToolDeadline & deadline = {},
                   bool partial_on_deadline = false, const String & namespace_prefix = {},
                   bool fix_legacy_markers = false, bool verify_intents = false, size_t fix_concurrency = 1);

/// Render the single machine-parseable summary line (no trailing newline). This is the ONLY view of a
/// report most consumers ever get -- the soak harness parses it, CI greps it, an operator reads it -- so
/// it lives here, next to the report and under test, rather than inline in the command where nothing
/// could reach it. Every term of `FsckReport::clean` MUST appear: a hard finding the line omits is a
/// finding no run will ever report, which is how `corrupted_runs` stayed invisible from the day it was
/// first counted. That requirement is CHECKED for this surface, not merely stated:
/// `CasFsckSummary.EveryHardFindingAppearsOnTheSummaryLine` iterates `kFsckHardFindings` and looks for
/// each name in the line, so a term added to the list and not rendered here fails that test. Zeros are
/// printed, never omitted: "absent" and "zero" are different facts, and consumers (e.g. the harness's
/// `stale_edge_verdict`) fail closed on absence by design.
String formatFsckSummary(const FsckReport & report);

/// A count of `FsckReport` as both report surfaces render it: its key on the summary line, and its column of
/// `SYSTEM CAS FSCK`, when it has one, with the column's position after the `disk` column.
struct FsckField
{
    std::string_view summary_key;
    std::string_view sql_column;   /// empty: not a column
    size_t sql_position = 0;
    uint64_t (*value)(const FsckReport &) = nullptr;
};

/// The `SYSTEM CAS FSCK` columns after `disk`, in column order.
std::vector<FsckField> fsckSqlColumns();

struct LegacyMarkerRemoval
{
    uint64_t deleted = 0;
    uint64_t changed = 0;   /// replaced between its `HEAD` and the delete, and left
};

/// The one removal of legacy markers, for `cas-migrate-pool` and `cas-fsck --fix legacy-markers`. Of `keys` it deletes
/// only those `Layout::parseLegacyMarkerKey` accepts, each by a `HEAD` and a delete conditional on the token that `HEAD`
/// saw, as many at a time as `io_pool` has threads; a marker replaced in between is left and counted. No server of generation 4
/// reads a marker, so servers may run meanwhile. A refused or unanswered delete, or a `deadline` that expired before a
/// `HEAD` or a delete, throws with the count deleted so far; a rerun continues. `on_progress` gets the running count
/// after each batch and before a throw.
LegacyMarkerRemoval removeLegacyMarkers(Pool & pool, const std::vector<String> & keys, ThreadPool & io_pool,
                                        const ToolDeadline & deadline = {},
                                        const std::function<void(uint64_t)> & on_progress = {});

}
