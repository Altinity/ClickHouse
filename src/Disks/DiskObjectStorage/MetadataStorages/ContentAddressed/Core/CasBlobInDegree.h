#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// One condemned object. `condemn_round` is the round that condemned the incarnation, used by GC's
/// round-paced graduation gate. IN-MEMORY ONLY (retired-in-snapshot 2026-07-10): the durable `RetiredSet`
/// object family is gone; this struct lives solely as the element type of `RetiredMergeResult` (below),
/// populated from the `kCondemned` rows decoded out of the source-edge runs. (Relocated here from the
/// deleted CasGcFormats.h in the codecs-v3 phase-2 cutover — it has no wire codec, so it did not move
/// to Formats/.)
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    BlobRef ref{};
    Token token;          /// the exact incarnation token GC observed (exact-token delete)
    uint64_t size = 0;
    uint64_t condemn_round = 0;   /// the GC round that condemned this incarnation (round-paced
                                  /// graduation: an entry graduates only once condemn_round < the
                                  /// current round). Consulted by GC only; the writer never reads it.
    bool delete_pending = false;  /// two-phase graduation (spec Task-9 amendment): floor-passed and
                                  /// published for deletion; the NEXT pass executes the exact-token
                                  /// delete (pre-CAS, safe at any leader staleness) and drops the entry.
                                  /// Terminal: a pending entry is never un-pended (writers keep seeing
                                  /// it condemned and recreate).
};

/// Sealed source-edge run row tags (spec §2.1). `kEdgeActive`/`kZeroMarker` were promoted from a
/// `.cpp`-local anonymous namespace: `kCondemned` (the retired-in-snapshot row, Task 2) needs to
/// share the tag byte space, and `decodeCondemnedRow` (a public entry point) must be able to
/// recognize its own tag.
constexpr char kEdgeActive = 0x01;   // sealed-run row: a surviving active edge
constexpr char kZeroMarker = 0x00;   // sealed-run row: blob transitioned to zero this generation
constexpr char kCondemned  = 0x02;   // sealed-run row: blob retired-in-snapshot (spec §2.1)

/// Source-edge run key schema (Phase 3 T3, mixed-algo pools): a SINGLE schema, self-describing per
/// row via the leading algo byte — replaces the pool-wide-width schemas 1/2 (DELETED, along with
/// `sourceEdgeDigestLen`/`sourceEdgeKeySchemaFor`/the width-stateful `SourceEdgeKeyCodec(uint8_t)`
/// ctor). A run may now carry rows for MULTIPLE algos at once (mixed-algo pool), each at its own
/// algo's width; there is no pool-wide "the" digest width anymore.
constexpr uint8_t kSourceEdgeKeySchema = 3;

/// Stateless key codec (Phase 3 T3): key = algo(u8) ++ digest[blobHashLenFor(algo)] ++ source_id(16
/// BE) — 33 bytes for a 16-byte-digest algo, 49 for sha256. All static: no width state, because the
/// width is self-describing per key (the leading algo byte). Raw lexicographic key order equals
/// `(algo, digest, source_id)` order (== `BlobRef::operator<` then `source_id`), because the algo
/// byte always decides before any digest byte can (a smaller algo byte sorts first regardless of the
/// digest bytes that follow, since it is fewer bytes in and lexicographic comparison is byte-by-byte
/// left to right).
class SourceEdgeKeyCodec
{
public:
    SourceEdgeKeyCodec() = delete;

    /// key = algo(u8) ++ digest[blobHashLenFor(algo)] ++ source_id(16 BE); 33 or 49 bytes.
    static String key(const BlobRef & ref, const UInt128 & source_id);
    /// Parse a key. Throws `NOT_IMPLEMENTED` on an unknown algo byte, `CORRUPTED_DATA` on a wrong
    /// total length for a known algo. Zero-tails the digest (beyond the algo's own width).
    static void parse(std::string_view key, BlobRef & ref, UInt128 & source_id);
    /// The sparse-index seek prefix for a blob: algo(u8) ++ digest[blobHashLenFor(algo)].
    static String seekPrefix(const BlobRef & ref);
};

/// Deterministic 16-byte id of a source edge (ManifestId, path). Distinctness only — not reconstructable.
UInt128 sourceEdgeId(const ManifestId & id, const String & path);

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
///   - borrowed-memory overload: zero-copy over caller-owned bytes.
///   - streaming overload: opens the write-once run object off `backend` at O(one block) resident
///     memory (the fold/preview readers use this; the returned reader is move-constructed).
RunFileReader openSourceEdgeRun(std::string_view bytes);
RunFileReader openSourceEdgeRun(Backend & backend, const String & key);

/// Write-once for a DETERMINISTIC artifact (same inputs => byte-identical bytes): the blob in-degree
/// runs AND the fold/completion seals. `putIfAbsent`; on a `PreconditionFailed` the key is already
/// occupied — `get` it and compare bytes: byte-equal means our own deterministic replay (adopt, no-op),
/// divergent bytes are impossible under correct operation and we fail closed with `CORRUPTED_DATA`
/// rather than let a divergent artifact disagree with the adopted snapshot. This is the spec's
/// "deterministic artifacts: byte-equal-or-CORRUPTED_DATA" rule (spec §strict-put-if-absent). It is
/// NOT for the observation-bearing artifacts (retired set, outcome log) — those carry HEAD-observed
/// tokens that two observers may legitimately differ on and keep first-durable-write-wins semantics.
void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes);

/// One source-edge update pre-merge: the edge (ref, source_id), and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    BlobRef ref{};
    UInt128 source_id{};   /// sourceEdgeId(ManifestId, path) — NOT a content hash; stays UInt128 (design §12)
    bool remove = false;
};

/// A blob whose active source-edge set became empty this generation — a retire candidate.
struct BlobCandidate
{
    BlobRef ref{};
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
/// field bolted onto `RetiredEntry`) so the common merge element stays slim — only replaced entries
/// carry the extra superseded token.
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

/// Two-cursor merge (retired-in-snapshot, spec §2.1/§3): the prior generation's `kCondemned` rows RIDE
/// the source-edge run itself at the zero-sentinel key (`source_id = 0`), so there is no separate
/// `prior_retired` cursor — the prior run IS the retired input. `PriorEdgeCursor` decodes each sentinel
/// `kCondemned` row and hands it to the per-blob close-out (in ascending hash order, exactly the order
/// the old sorted vector had). Settlement rules, in order, per condemned row for blob `h` with post-merge
/// in-degree `d`:
///   delete_pending (prior pass)                -> redelete if d = 0 (the caller executes the exact-token
///                                                 delete pre-CAS and the entry drops); d > 0 for a pending
///                                                 entry is structurally impossible but reachable under
///                                                 races (T1 TLA finding) — spared + a LOUD log, never a
///                                                 fail-closed abort;
///   d > 0                                       -> spared (recovery wins even past the floor);
///   d = 0 and condemn_round < current_round     -> graduated: REPUBLISHED as delete_pending (two-phase
///                                                 graduation) — deleted the NEXT pass;
///   d = 0 otherwise                             -> still_retired, carried byte-unchanged.
/// A carried `kCondemned` row is SETTLEMENT-ONLY: it never sets the blob's `cur_touched` bit, so a
/// generation that only carries the row emits no zero-marker and pays no `peek_head` HEAD. The surviving
/// `still_retired` entries are re-emitted as `kCondemned` sentinel rows into the OUTPUT run (one sentinel
/// per blob, emitted before the blob's edges since the sentinel key sorts first), so the next generation
/// reads them back — `still_retired` mirrors exactly those rows, in the same order.
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
/// Phase 3 T3: the run's key schema is now the single self-describing `kSourceEdgeKeySchema` (no
/// per-pool digest width parameter anymore — `SourceEdgeKeyCodec` is stateless and every row carries
/// its own algo byte), so mixed-algo deltas in ONE `scattered` batch settle in ONE fold, no per-algo
/// loop. The merge comparator is exactly `(ref.algo, ref.digest, source_id)` (== `BlobRef::operator<`
/// then `source_id`), which is also raw key order (see `SourceEdgeKeyCodec`).
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              uint64_t current_round = 0, uint64_t condemn_round = 0,
                              const std::function<std::optional<HeadResult>(const BlobRef &)> & head_blob = {},
                              const std::function<std::optional<HeadResult>(const BlobRef &)> & peek_head = {},
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
int64_t inDegreeInGeneration(Backend & backend, const std::vector<RunRef> & runs, const BlobRef & ref);

}
