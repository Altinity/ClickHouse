#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>

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
    PendingGc,     /// listed in the retired set (condemned / delete_pending) — deletion is scheduled; EXPECTED
    AwaitingGc,    /// edges still in the GC snapshot (drop/reclaim not folded yet) or GC never ran — EXPECTED
    Unaccounted,   /// absent from the whole GC view — transient for a fast create+drop between rounds;
                   /// PERSISTENT occurrences should be impossible (INV-2 reachability-before-content)
    StaleEdge,     /// every source edge the GC snapshot still holds on this blob names a manifest that no
                   /// longer exists anywhere in the pool, so the matching `-1` can never fold: the blob's
                   /// in-degree can never reach zero and the incremental GC can never reclaim it. Only a
                   /// full rebuild of the in-degree state can. ERROR — never an `AwaitingGc` "expected"
                   /// backlog, which is exactly the label that used to hide it.
    SnapshotOracleMismatch,  /// a published table snapshot's bytes diverge from an independent replay of
                             /// its logs — cache/codec corruption, ERROR
    CorruptedRun,  /// a GC source-edge run's whole-file seal checksum (`RunRef::checksum`) disagrees with
                   /// the stored bytes — cataloged so the read-only audit enumerates every finding in one
                   /// pass; deletion-deriving consumers (`fold`, `zeroInDegree`, `previewDeletes`) still
                   /// fail closed on the same mismatch. ERROR
    /// The two verdicts of the arithmetic ref-stream walk (spec §7). They are about a NAMESPACE, not an
    /// object, and the `key` of such a row is the ref-log key the walk stopped on.
    ChainBroken,   /// a ref-log id is absent BELOW a confirmed durable same-epoch id. Ids are dense
                   /// `1..T` within `(namespace, epoch)` (INV-1), so this is not the end of a stream —
                   /// a durable record is missing and every transaction above it is unreachable. ERROR
    Unchecked,     /// the walk could not prove this namespace's stream EITHER WAY (an unprovable epoch
                   /// crossing, an undecodable body, an oracle that could not replay). Not a finding and
                   /// not a clean bill of health: the honest third answer, reported so nobody reads a
                   /// silence as a proof.
    LifelessKey,   /// a key under `cas/refs/` or a namespace `_files/` subtree that names no namespace
                   /// LIFE -- the un-incarnated (Stage A) shape the `Layout` parsers refuse. It belongs
                   /// to no namespace, so no per-namespace verdict can carry it. ERROR
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
    /// per manifest body, and the cheap summary path must stay request-for-request unchanged.
    uint64_t stale_edge = 0;

    /// The per-hash `.meta` descriptor sibling of a blob body:
    /// pairing check between the `blobs/` physical listing's `.meta` keys and its body keys.
    /// ADVISORY, not a hard finding: GC deletes the body FIRST and then drops the `.meta` on a bounded,
    /// error-suppressed advisory pool that runs strictly after (and may drop the op — see `CasGc`), so a
    /// single raw LIST legitimately observes a body-less `.meta` mid-graduation and NO finite grace makes
    /// a persistent one hard evidence. Counted and reported; excluded from `clean()`.
    uint64_t meta_without_body = 0;   /// a `.meta` object with no body — INV-META-BODY advisory
    uint64_t body_without_meta = 0;   /// a body with no `.meta` — a not-yet-adopted or interrupted-birth
                                       /// artifact; benign, NOT a dangle

    /// Snapshot integrity oracle: for each table with a published snapshot
    /// whose covered logs still survive, fsck independently replays those logs and re-encodes the
    /// snapshot; a byte divergence from the published object means the writer cache or a codec is
    /// corrupt and is a hard ERROR. `snapshot_oracle_checked` counts tables the oracle could actually
    /// verify (logs present); tables whose covered logs were already cleaned are skipped, not counted.
    uint64_t snapshot_oracle_mismatches = 0;
    uint64_t snapshot_oracle_checked = 0;

    /// GC source-edge runs whose whole-file seal checksum did not match the stored bytes. Cataloged
    /// with the run key in `objects`; the audit CONTINUES — a read-only auditor
    /// enumerates all problems in one pass rather than aborting on the first corrupt run.
    uint64_t corrupted_runs = 0;

    /// The arithmetic ref-stream walk (spec §7). fsck reads each namespace's stream by EXACT KEY from
    /// `_ckpt.checkpoint`'s successor upward — never from a listing, which may omit durable records —
    /// and reports one verdict per namespace.
    ///
    /// `chain_broken` counts namespaces with a proven hole (see `FsckClass::ChainBroken`) and is a HARD
    /// ERROR: part of `clean`, and the command exits nonzero on it. `unchecked` counts namespaces the
    /// walk (or the snapshot oracle) could not prove either way; it is COVERAGE, not a finding, so it
    /// is reported and printed but does not make a report unclean — exactly like `partial`. A pool with
    /// nothing wrong reads `chain_broken=0 unchecked=0`, so `unchecked` is never a resting state.
    ///
    /// `ref_records_walked` is how many ref-log records the walk actually read and proved, summed over
    /// namespaces. It is what makes "the tail above the checkpoint was walked" observable rather than
    /// inferred from the absence of a complaint.
    uint64_t chain_broken = 0;
    uint64_t unchecked = 0;
    uint64_t ref_records_walked = 0;

    /// Keys the namespace enumeration could not attribute to any namespace (see `FsckClass::LifelessKey`
    /// and `Cas::NamespaceListing`). Counted DISTINCT by key: the scan enumerates namespaces several
    /// times and every sweep sees the same offending key, so a per-sweep count would multiply one
    /// defect. A hard finding -- behind Stage B's format bump such a key is corruption, and an audit is
    /// where an operator finds out about it.
    uint64_t lifeless_keys = 0;

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
    /// GC backlog classes do not make a report unclean, and `meta_without_body` is advisory (see its
    /// field: GC's body-then-meta delete ordering makes a body-less `.meta` a legitimate transient with
    /// no finite hard horizon); a partial report only covers the visited subset. `stale_edge` is a hard
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
/// The SIZE IS DEDUCED, deliberately: a fixed `std::array<FsckHardFinding, N>` would reject an added row
/// as "excess elements in array initializer", which stops the build but prints none of the guidance the
/// assert below carries. Deduced, an added row compiles and the assert is what speaks.
inline constexpr std::array kFsckHardFindings{
    FsckHardFinding{"dangling", &FsckReport::dangling},
    FsckHardFinding{"snapshot_oracle_mismatches", &FsckReport::snapshot_oracle_mismatches},
    FsckHardFinding{"corrupted_runs", &FsckReport::corrupted_runs},
    FsckHardFinding{"stale_edge", &FsckReport::stale_edge},
    FsckHardFinding{"chain_broken", &FsckReport::chain_broken},
    FsckHardFinding{"lifeless_keys", &FsckReport::lifeless_keys},
};

/// TRIPWIRE. A hard finding has to reach THREE surfaces, and each one has been forgotten at least once:
/// the text summary line (`formatFsckSummary`), `CommandFsck::executeImpl`'s nonzero-exit set, and the
/// SQL result row (`contentAddressedFsckColumns` + `appendContentAddressedFsckRow`). Four times now a
/// term was added to `clean` and one of the three was missed, each time with the rule written down in
/// prose and each time the prose not holding.
///
/// WHAT THIS ASSERT CHECKS, precisely: that the number of hard findings still equals the number written
/// here. Nothing more. It does NOT check that any surface renders them -- it cannot see the renderers,
/// which is the whole reason it lives with the struct: this header is included by the summary
/// formatter, by `programs/disks`, and by `InterpreterSystemQuery.cpp`, so changing the list breaks the
/// build in every TU that owes an update, including the two no unit test can reach.
///
/// The summary line is checked for real, by a test that iterates the list
/// (`CasFsckSummary.EveryHardFindingAppearsOnTheSummaryLine`). The exit set and the SQL row are NOT --
/// for those, this assert plus the list below it is the whole of the mechanism, so bumping the number
/// without visiting them defeats it. Bump it only after all three are done.
static_assert(kFsckHardFindings.size() == 6,
    "A hard finding was added to or removed from `kFsckHardFindings`, which is `FsckReport::clean`. "
    "Before updating this count, render it in ALL THREE surfaces: `formatFsckSummary`'s line, "
    "`CommandFsck::executeImpl`'s nonzero-exit set, and `contentAddressedFsckColumns` + "
    "`appendContentAddressedFsckRow`. Two of the three have no test that can fail for you.");

inline bool FsckReport::clean() const
{
    for (const FsckHardFinding & finding : kFsckHardFindings)
        if (this->*finding.value != 0)
            return false;
    return true;
}

/// Independently recompute reachability from authoritative refs (never from GC state or snapshots) and
/// diff it against a raw object listing. The operation is read-only; `detail` populates per-object rows.
/// `deadline`, if set, bounds the WHOLE scan: it is checked between list pages and reachability
/// refs, throwing `TIMEOUT_EXCEEDED` if exceeded (a slow-but-progressing scan surfaces a clear
/// error instead of an opaque hang) — unless `partial_on_deadline` is set, in which case the
/// accumulated lower-bound counts are returned instead, flagged via `FsckReport::partial`. A single
/// LIST page stuck in S3-client retries is bounded separately by the disk's S3 retry/timeout
/// settings, not here. `namespace_prefix`, if non-empty, scopes the scan to namespaces with this
/// prefix and skips the pool-wide unreachable classification (dangling-only mode).
FsckReport runFsck(Pool & store, bool detail, FsckProgress on_progress = {},
                   std::optional<std::chrono::steady_clock::time_point> deadline = {},
                   bool partial_on_deadline = false, const String & namespace_prefix = {});

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

}
