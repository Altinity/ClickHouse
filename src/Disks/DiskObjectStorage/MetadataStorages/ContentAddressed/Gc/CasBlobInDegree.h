#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// In-memory description of a blob incarnation condemned by the in-degree merge. The exact token and
/// size are captured from the blob HEAD so the GC caller can issue an exact-token deletion later;
/// `condemn_round` controls round-paced graduation. Entries are decoded from `kCondemned` rows and are
/// returned through `RetiredMergeResult`; the type itself has no serialized representation.
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    BlobRef ref{};
    Token token;          /// the exact incarnation token GC observed (exact-token delete)
    uint64_t size = 0;
    uint64_t condemn_round = 0;   /// the GC round that condemned this incarnation (round-paced
                                  /// graduation: an entry graduates only once condemn_round < the
                                  /// current round). Consulted by GC only; the writer never reads it.
    bool delete_pending = false;  /// Two-phase graduation: floor-passed and
                                  /// published for deletion; the NEXT pass executes the exact-token
                                  /// delete (pre-CAS, safe at any leader staleness) and drops the entry.
                                  /// Terminal: a pending entry is never un-pended (writers keep seeing
                                  /// it condemned and recreate).
};

/// Backend-independent codec for source-edge keys. A key is `algo` (u8), the digest at that algorithm's
/// native width, and `source_id` (16 bytes, big-endian). The packed byte order is exactly
/// `(BlobRef, source_id)` order, which lets the fold merge compare keys directly. The leading algorithm
/// byte makes the digest width self-describing; supported algorithms may therefore be mixed in one run.
class SourceEdgeKeyCodec
{
public:
    SourceEdgeKeyCodec() = delete;

    /// key = algo(u8) ++ digest[blobHashLenFor(algo)] ++ source_id(16 BE); 33 or 49 bytes.
    static String key(const BlobRef & ref, const UInt128 & source_id);
    /// Parse a key. Throws `NOT_IMPLEMENTED` on an unknown algo byte, `CORRUPTED_DATA` on a wrong
    /// total length for a known algo. Zero-tails the digest (beyond the algo's own width).
    static void parse(std::string_view key, BlobRef & ref, UInt128 & source_id);
};

/// Deterministic 16-byte id of a source edge (ManifestId, path). Distinctness only — not reconstructable.
UInt128 sourceEdgeId(const ManifestId & id, const String & path);

/// The zero source_id is the reserved sentinel key (used internally for the zero-marker row) —
/// producers of real source edges must fail closed on a hash collision with it.
void assertValidSourceEdgeId(const UInt128 & source_id);

/// Serialized payload of a condemned source-edge sentinel. The payload retains the full incarnation
/// token, including its type, because deletion must remain exact-token guarded. Its fixed prefix is
/// `[0x02][flags][token_type][round BE64][size BE64][token_len BE16]`, followed by token bytes.
struct CondemnedRow
{
    bool delete_pending = false;
    Token token;                 // {value, type} — the full token required by exact-token deletion
    uint64_t size = 0;
    uint64_t condemn_round = 0;
    bool operator==(const CondemnedRow &) const = default;
};

/// Encode a condemned-row payload. Throws `CORRUPTED_DATA` if the token cannot fit in its u16 length.
String encodeCondemnedRow(const CondemnedRow & row);

/// Decode and validate a condemned-row payload. Unknown flags, token types, or inconsistent lengths
/// throw `CORRUPTED_DATA`.
CondemnedRow decodeCondemnedRow(std::string_view payload);

/// Bridges the backend-free `Formats/CasRecordStreamFormat` NDJSON reader to the
/// `(key, payload)` BYTE interface the fold / `zeroInDegree` / `previewDeletes` / `fsck` consumers use:
/// `next` reconstructs the packed `SourceEdgeKeyCodec` key and the original payload bytes (a single
/// marker byte for an edge / zero row, or the `encodeCondemnedRow` blob for a condemned row) from the
/// decoded NDJSON record. So the codec stays backend-free while the consumers keep their exact parse /
/// compare logic. The whole-object chained CityHash128 is accumulated as the run streams; `verifyAgainst`
/// checks it against the fold seal's `RunRef.checksum` AFTER the run is fully drained and BEFORE the
/// caller acts on it (a deletion decision).
class SourceEdgeRunView
{
public:
    /// false once the run's `{"n"}` trailer is consumed (the trailer count is verified there). `key` is
    /// the reconstructed `SourceEdgeKeyCodec::key(ref, source_id)`; `payload` is the original marker byte
    /// or `encodeCondemnedRow` bytes.
    bool next(String & key, String & payload);
    /// Verify the accumulated whole-file checksum against the seal's `RunRef.checksum`; CORRUPTED_DATA on
    /// mismatch. Call after draining the run and before acting on its records.
    void verifyAgainst(const UInt128 & expected);
    /// The accumulated whole-file checksum after draining the run — non-throwing, for a read-only auditor
    /// (fsck) that catalogues a mismatch and continues instead of failing closed. Call after draining.
    UInt128 accumulatedChecksum();

private:
    friend SourceEdgeRunView openSourceEdgeRun(std::string_view bytes);
    friend SourceEdgeRunView openSourceEdgeRun(Backend & backend, const String & key);
    /// Keep the underlying stream alive for the reader, which borrows it rather than owning it.
    explicit SourceEdgeRunView(std::unique_ptr<ReadBuffer> stream_);

    std::unique_ptr<ReadBuffer> stream;             /// owns the backend stream / memory buffer the reader borrows
    std::unique_ptr<SourceEdgeRunReader> reader;    /// over *stream (non-movable => held by pointer, destroyed before stream)
};

/// Open a typed source-edge run. The NDJSON header must identify a `cas_run` of kind `source_edge`;
/// otherwise opening fails closed. The memory overload borrows caller-owned bytes. The backend overload
/// streams the write-once object through `getStream`, retaining only one record-sized buffer.
SourceEdgeRunView openSourceEdgeRun(std::string_view bytes);
SourceEdgeRunView openSourceEdgeRun(Backend & backend, const String & key);

/// Store a deterministic write-once artifact (same inputs => byte-identical bytes): the blob in-degree
/// runs and fold seals. `putIfAbsent`; on a `PreconditionFailed` the key is already
/// occupied — `get` it and compare bytes: byte-equal means our own deterministic replay (adopt, no-op),
/// divergent bytes are impossible under correct operation and we fail closed with `CORRUPTED_DATA`
/// rather than let a divergent artifact disagree with the adopted snapshot. Deterministic artifacts are
/// therefore byte-equal-or-`CORRUPTED_DATA`. It is
/// NOT for observation-bearing artifacts (outcome logs) — those carry HEAD-observed
/// tokens that two observers may legitimately differ on and keep first-durable-write-wins semantics.
void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes);

/// One source-edge update before merging: the edge `(ref, source_id)`, and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    BlobRef ref{};
    UInt128 source_id{};   /// `sourceEdgeId(ManifestId, path)` — an edge identity, not a content hash
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
/// `prior_runs` are the parent generation's run segments for this shard, resolved by the caller from the
/// parent fold seal's `blob_target_runs` (filtered to `shard`). An empty vector is the fresh-pool / empty
/// baseline. A run sealed for one generation may physically live under an older generation's key, so the
/// seal's exact reference is authoritative and key construction is not used. `new_generation`, `attempt`,
/// and `shard` name only the output run's key namespace.
/// The fresh entry that re-condemns the current
/// token, paired with the STALE entry's token it superseded. Kept as its own struct (rather than a
/// field bolted onto `RetiredEntry`) so the common merge element stays slim — only replaced entries
/// carry the extra superseded token.
struct ReplacedEntry
{
    RetiredEntry fresh;   /// the freshly condemned CURRENT token (also pushed into still_retired byte-identically)
    Token old_token;      /// the superseded (stale) entry's token — what the resurrect replaced
};

/// Outcome of the retired merge: the same
/// streaming pass that folds edges settles every prior retired entry and detects new candidates.
struct RetiredMergeResult
{
    std::vector<RetiredEntry> still_retired;   /// carried + newly-condemned + newly-PENDING entries (the next list)
    std::vector<RetiredEntry> graduated;       /// newly floor-passed this pass — published pending, deleted NEXT pass
    std::vector<RetiredEntry> spared;          /// in-degree recovered — entry dropped
    std::vector<RetiredEntry> redelete;        /// pending in the PRIOR list — execute deleteExact pre-CAS, drop
    std::vector<ReplacedEntry> replaced;  /// re-condemned CURRENT tokens that superseded a stale entry (resurrect-replaced); caller emits blob_retire_replaced
};

/// Merge the prior generation's source-edge run with new deltas. The prior run's `kCondemned` rows RIDE
/// the source-edge run itself at the zero-sentinel key (`source_id = 0`), so there is no separate
/// `prior_retired` cursor — the prior run IS the retired input. `PriorEdgeCursor` decodes each sentinel
/// `kCondemned` row and hands it to the per-blob close-out (in ascending hash order, exactly the order
/// the old sorted vector had). Settlement rules, in order, per condemned row for blob `h` with post-merge
/// in-degree `d`:
///   delete_pending (prior pass)                -> redelete if d = 0 (the caller executes the exact-token
///                                                 delete pre-CAS and the entry drops); d > 0 for a pending
///                                                 entry is structurally impossible but reachable under
///                                                 races — spared + a loud log, never a
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
/// When the pass is clamped on any shard, landed-before-cut events may remain unfolded behind the clamp,
/// so graduating or executing pending
/// deletes over this pass's in-degrees can delete a blob whose +1 is pending behind the clamp (the
/// model's SabotageSkipChangedShard counterexample, realized). With `suppress_destructive` the merge
/// neither graduates nor redeletes: pending entries carry UNCHANGED (still delete_pending) and
/// floor-passed entries stay condemned-only. Condemnation and sparing remain (both non-destructive).
/// A blob that transitions to zero THIS pass with no prior entry is condemned: `head_blob` captures
/// the exact incarnation token/size (absent object or empty head_blob -> nothing to delete, skipped)
/// and the entry is minted at `condemn_round`. Entries for blobs the merged stream never visits
/// (no edges, no deltas) settle at in-degree 0 by definition. The retired cursor never changes the
/// snapshot run bytes. Defaults preserve the empty-retired behavior
/// current_round 0 => nothing graduates, no head_blob => nothing condemned).
///
/// `peek_head` is a side-effect-free HEAD used only by the resurrect-supersede
/// branch (a `prior_retired` entry whose blob re-touched this pass at net in-degree 0, current token
/// differs from the stale entry's token). `head_blob` is the FRESH-CONDEMN observation hook — it emits
/// the `IndegZero`/`GcRetireObserve`/`BlobRetire` trail and increments `CasGcRetiredCondemned`, which is
/// wrong for a supersede (the supersede's own event is `blob_retire_replaced`, emitted once by the
/// caller from `RetiredMergeResult::replaced`). Calling `head_blob` from the supersede branch used to
/// double-emit `blob_retire` alongside `blob_retire_replaced` and double-count the condemned counter;
/// `peek_head` is a plain HEAD with no events and no counters. No supersede detection happens if
/// `peek_head` is unset (default `{}`), independent of whether `head_blob` is set.
/// The merge comparator is exactly `(ref.algo, ref.digest, source_id)` (that is, `BlobRef::operator<`
/// followed by `source_id`), which is also the raw key order produced by `SourceEdgeKeyCodec`.
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

/// Stream the sealed in-degree runs named by `runs` (the current seal's `blob_target_runs` filtered to one
/// shard) and return every blob written at in-degree 0 (the candidates that transitioned to zero). An
/// empty `runs` is an empty baseline. Each `RunRef` supplies the exact object key, so resolution never
/// reconstructs a key from generation metadata.
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const std::vector<RunRef> & runs);

}
