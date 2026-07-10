#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <vector>

namespace DB::Cas
{

/// Sealed source-edge run row tags (spec §2.1). `kEdgeActive`/`kZeroMarker` were promoted from a
/// `.cpp`-local anonymous namespace: `kCondemned` (the retired-in-snapshot row, Task 2) needs to
/// share the tag byte space, and `decodeCondemnedRow` (a public entry point) must be able to
/// recognize its own tag.
constexpr char kEdgeActive = 0x01;   // sealed-run row: a surviving active edge
constexpr char kZeroMarker = 0x00;   // sealed-run row: blob transitioned to zero this generation
constexpr char kCondemned  = 0x02;   // sealed-run row: blob retired-in-snapshot (spec §2.1)
constexpr uint8_t kSourceEdgeKeySchema = 1;

/// Deterministic 16-byte id of a source edge (ManifestId, path). Distinctness only — not reconstructable.
UInt128 sourceEdgeId(const ManifestId & id, const String & path);
/// 32-byte run key = blob_hash(16 BE) ++ source_id(16 BE); lexicographic == (blob_hash, source_id) order.
String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id);
bool parseSrcEdgeRunKey(const String & key, UInt128 & blob_hash, UInt128 & source_id);

/// The zero source_id is the reserved sentinel key (used internally for the zero-marker row) —
/// producers of real source edges must fail closed on a hash collision with it (spec §2.1).
void assertValidSourceEdgeId(const UInt128 & source_id);

/// A retired-in-snapshot row (spec §2.1): a blob condemned within the source-edge run itself,
/// carrying the exact incarnation token to delete and the round it was condemned at.
/// [0x02][flags][token_type][round BE64][size BE64][token_len BE16][token bytes]
struct CondemnedRow
{
    bool delete_pending = false;
    Token token;                 // {value, type} — full token, spec §2.1
    uint64_t size = 0;
    uint64_t condemn_round = 0;
    bool operator==(const CondemnedRow &) const = default;
};

String encodeCondemnedRow(const CondemnedRow & row);          // [0x02][flags][token_type][round][size][len][value]
CondemnedRow decodeCondemnedRow(std::string_view payload);    // throws CORRUPTED_DATA per spec §2.1

/// Typed open (spec §2.1): validates kind == SourceEdge and key_schema == kSourceEdgeKeySchema,
/// fails closed otherwise. ALL source-edge run readers go through this.
RunFileReader openSourceEdgeRun(std::string_view bytes);

/// Write-once for a DETERMINISTIC artifact (same inputs => byte-identical bytes): the blob in-degree
/// runs AND the fold/completion seals. `putIfAbsent`; on a `PreconditionFailed` the key is already
/// occupied — `get` it and compare bytes: byte-equal means our own deterministic replay (adopt, no-op),
/// divergent bytes are impossible under correct operation and we fail closed with `CORRUPTED_DATA`
/// rather than let a divergent artifact disagree with the adopted snapshot. This is the spec's
/// "deterministic artifacts: byte-equal-or-CORRUPTED_DATA" rule (spec §strict-put-if-absent). It is
/// NOT for the observation-bearing artifacts (retired set, outcome log) — those carry HEAD-observed
/// tokens that two observers may legitimately differ on and keep first-durable-write-wins semantics.
void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes);

/// One source-edge update pre-merge: the edge (blob_hash, source_id), and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    UInt128 blob_hash{};
    UInt128 source_id{};
    bool remove = false;
};

/// A blob whose active source-edge set became empty this generation — a retire candidate.
struct BlobCandidate
{
    UInt128 hash{};
};

/// Merge the prior generation's blob source-edge run for `shard` with `scattered` deltas, producing the
/// new generation's write-once run under blobTargetRunKey(new_generation, attempt, shard, 0). Streaming:
/// prior run + scattered deltas are sorted by (blob_hash, source_id) and merged via two-cursor scan; the
/// source-edge set is idempotent under re-fold (identical (blob_hash, source_id) pairs deduplicate). A blob
/// whose active edge set transitions to exactly empty this generation is written as an explicit zero-marker
/// row so `zeroInDegree` can stream it; prior-generation zero markers are dropped. Appends the produced
/// run's `RunRef` (key + footer checksum + `shard` + `new_generation`) to `out_runs` for the fold seal.
///
/// `prior_runs` are the PARENT generation's run segments for this shard, RESOLVED BY THE CALLER from the
/// parent fold seal's `blob_target_runs` (filtered to `shard`). An empty vector is the fresh-pool / empty
/// baseline. Resolution moves to the caller (2026-07-02 snapshot-streaming, T0) because with reference-
/// parent carry a run sealed for the parent generation may physically live under an OLDER generation's key
/// — the seal ref names the real key, key construction would not. `new_generation`/`attempt`/`shard` name
/// only the OUTPUT run's key namespace.
/// One resurrect-supersede (audit fix, 2026-07-08): the fresh entry that re-condemns the CURRENT
/// token, paired with the STALE entry's token it superseded. Kept as its own struct (rather than a
/// field bolted onto `RetiredEntry`) because `RetiredEntry` is the durable `RetiredSet` element —
/// widening it would touch the encode/decode format for every entry, not just replaced ones.
struct ReplacedEntry
{
    RetiredEntry fresh;   /// the freshly condemned CURRENT token (also pushed into still_retired byte-identically)
    Token old_token;      /// the superseded (stale) entry's token — what the resurrect replaced
};

/// Outcome of the retired (third) merge cursor (ack-floor redesign, spec 2026-07-02): the same
/// streaming pass that folds edges settles every prior retired entry and detects new candidates.
struct RetiredMergeResult
{
    std::vector<RetiredEntry> still_retired;   /// carried + newly-condemned + newly-PENDING entries (the next list)
    std::vector<RetiredEntry> graduated;       /// newly floor-passed this pass — published pending, deleted NEXT pass
    std::vector<RetiredEntry> spared;          /// in-degree recovered — entry dropped
    std::vector<RetiredEntry> redelete;        /// pending in the PRIOR list — execute deleteExact pre-CAS, drop
    std::vector<ReplacedEntry> replaced;  /// re-condemned CURRENT tokens that superseded a stale entry (resurrect-replaced); caller emits blob_retire_replaced
};

/// Three-cursor extension: `prior_retired` (Blob entries ONLY, sorted by hash ascending) rides the same
/// per-blob close-out as the two existing cursors. Settlement rules, in order, per entry for blob `h`
/// with post-merge in-degree `d`:
///   delete_pending (prior pass)                -> redelete if d = 0 (the caller executes the exact-token
///                                                 delete pre-CAS and the entry drops); d > 0 for a pending
///                                                 entry is structurally impossible — spared + loud log;
///   d > 0                                       -> spared (recovery wins even past the floor);
///   d = 0 and condemn_round < current_round     -> graduated: REPUBLISHED as delete_pending (two-phase
///                                                 graduation) — deleted the NEXT pass;
///   d = 0 otherwise                             -> still_retired, carried byte-unchanged.
/// CLAMP SUPPRESSION (2026-07-03, found live: 31 dangling in the night soak): when the pass's fold
/// CLAMPED any shard, landed-before-cut events may sit UNFOLDED behind the clamp — the lemma "landed
/// before the cut => folded before graduation" does not hold, so graduating or executing pending
/// deletes over this pass's in-degrees can delete a blob whose +1 is pending behind the clamp (the
/// model's SabotageSkipChangedShard counterexample, realized). With `suppress_destructive` the merge
/// neither graduates nor redeletes: pending entries carry UNCHANGED (still delete_pending) and
/// floor-passed entries stay condemned-only. Condemnation and sparing remain (both non-destructive).
/// A blob that transitions to zero THIS pass with no prior entry is condemned: `head_blob` captures
/// the exact incarnation token/size (absent object or empty head_blob -> nothing to delete, skipped)
/// and the entry is minted at `condemn_round`. Entries for blobs the merged stream never visits
/// (no edges, no deltas) settle at in-degree 0 by definition. The retired cursor never changes the
/// snapshot run bytes. Defaults keep the legacy call sites behavior-identical (empty retired,
/// current_round 0 => nothing graduates, no head_blob => nothing condemned).
///
/// `peek_head` (audit fix, 2026-07-08): a SIDE-EFFECT-FREE HEAD used ONLY by the resurrect-supersede
/// branch (a `prior_retired` entry whose blob re-touched this pass at net in-degree 0, current token
/// differs from the stale entry's token). `head_blob` is the FRESH-CONDEMN observation hook — it emits
/// the `IndegZero`/`GcRetireObserve`/`BlobRetire` trail and increments `CasGcRetiredCondemned`, which is
/// wrong for a supersede (the supersede's own event is `blob_retire_replaced`, emitted once by the
/// caller from `RetiredMergeResult::replaced`). Calling `head_blob` from the supersede branch used to
/// double-emit `blob_retire` alongside `blob_retire_replaced` and double-count the condemned counter;
/// `peek_head` is a plain HEAD with no events and no counters. No supersede detection happens if
/// `peek_head` is unset (default `{}`), independent of whether `head_blob` is set.
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              const std::vector<RetiredEntry> & prior_retired = {},
                              uint64_t current_round = 0, uint64_t condemn_round = 0,
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & head_blob = {},
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & peek_head = {},
                              RetiredMergeResult * out_retired = nullptr,
                              bool suppress_destructive = false);

/// Stream the sealed in-degree run named by `runs` (the current seal's `blob_target_runs` filtered to one
/// shard) and return every blob written at in-degree 0 (the candidates that transitioned to zero). An
/// empty `runs` is an empty baseline. Resolution is by ref (2026-07-02 T0), never by key construction.
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const std::vector<RunRef> & runs);

/// The in-degree of one blob in the sealed run named by `runs` (the seal's `blob_target_runs` for one
/// shard): 0 when the blob is absent from the run (or written as an explicit transitioned-to-0 row), else
/// its count. Used by `Gc::previewDeletes` and by tests (the round itself settles candidates inside the
/// three-cursor merge; there is no per-candidate point query anymore).
int64_t inDegreeInGeneration(Backend & backend, const std::vector<RunRef> & runs, const UInt128 & blob_hash);

}
