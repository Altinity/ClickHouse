#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>

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
    SnapshotOracleMismatch,  /// a published table snapshot's bytes diverge from an independent replay of
                             /// its logs — cache/codec corruption, ERROR
    CorruptedRun,  /// a GC source-edge run's whole-file seal checksum (`RunRef::checksum`) disagrees with
                   /// the stored bytes — cataloged so the read-only audit enumerates every finding in one
                   /// pass; deletion-deriving consumers (`fold`, `zeroInDegree`, `previewDeletes`) still
                   /// fail closed on the same mismatch. ERROR
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

    /// The per-hash `.meta` descriptor sibling of a blob body:
    /// pairing check between the `blobs/` physical listing's `.meta` keys and its body keys.
    uint64_t meta_without_body = 0;   /// a `.meta` object with no body — INV-META-BODY violation (ERROR)
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
    /// GC backlog classes do not make a report unclean; a partial report only covers the visited subset.
    bool clean() const { return dangling == 0 && meta_without_body == 0 && snapshot_oracle_mismatches == 0 && corrupted_runs == 0; }
};

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

}
