#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// In-memory description of a blob incarnation condemned by the in-degree merge. The token and size
/// come from a `HEAD` of the blob, or from the intent's key when an adopted orphan's body is absent
/// or replaced; the exact-token deletion is issued later.
/// Entries are decoded from `RunMarker::Condemned` rows and are
/// returned through `RetiredMergeResult`; the type itself has no serialized representation.
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    BlobRef ref{};
    PersistedEtag token;   /// the exact incarnation GC observed; the delete re-heads and compares it
    uint64_t size = 0;
    uint64_t condemn_round = 0;   /// the GC round that condemned this row. Consulted by GC only; the writer never reads it.
    bool delete_pending = false;  /// Set on a `pending` and a `standing` row, clear on a `candidate`.
    bool standing = false;           /// Implies `delete_pending`. The body of this incarnation is deleted or known absent; the intent still exists.
};

/// What a fold `HEAD` hook saw at a blob key: the body, nothing when it is absent, or `undecodable` for a body
/// shorter than the pool's blob envelope, which GC never condemns, adopts or deletes.
struct BlobHead
{
    std::optional<Meta> meta;
    bool undecodable = false;

    BlobHead(std::optional<Meta> meta_ = std::nullopt) : meta(std::move(meta_)) {} /// NOLINT(google-explicit-constructor)
    BlobHead(std::nullopt_t) {} /// NOLINT(google-explicit-constructor)
    BlobHead(Meta meta_) : meta(std::move(meta_)) {} /// NOLINT(google-explicit-constructor)

    static BlobHead undecodableBody()
    {
        BlobHead head;
        head.undecodable = true;
        return head;
    }
};

/// The fold's HEAD hooks (`head_blob`, `peek_head`, `pending_head`): the caller issues the request on its own
/// admitted operation and reports what it saw.
using BlobHeadFn = std::function<BlobHead(const BlobRef &)>;
/// Creates the deletion intent of a blob at an identity and reports whether it is known to exist.
using IntentCreateFn = std::function<IntentCreateResult(const BlobRef &, const CanonicalIdentity &)>;
/// Waits for the answer of one started create, or rethrows what the create threw.
using IntentCreateAnswer = std::function<IntentCreateResult()>;

/// The fold's intent creates. `start` sends one create; the fold keeps at most `window` answers outstanding
/// and writes each row from its own create's answer, in run order. A plain `IntentCreateFn` answers at once.
struct IntentCreates
{
    std::function<IntentCreateAnswer(const BlobRef &, const CanonicalIdentity &)> start;
    size_t window = 1;

    IntentCreates() = default;
    IntentCreates(IntentCreateFn create); /// NOLINT(google-explicit-constructor)
    explicit operator bool() const { return static_cast<bool>(start); }
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

/// A condemned source-edge sentinel row. It keeps the full incarnation, dialect included, because deletion
/// must remain exact-token guarded.
struct CondemnedRow
{
    bool delete_pending = false;
    PersistedEtag token;   // the incarnation the exact-token delete must re-observe
    uint64_t size = 0;
    uint64_t condemn_round = 0;
    bool marker_confirmed = false;   // set only in rows of earlier builds; nothing reads it
    bool standing = false;           // implies `delete_pending`; the body of this incarnation is deleted or known absent, the intent still exists
    /// Spelled out rather than defaulted because `PersistedEtag` carries no equality of its
    /// own. A field added above belongs here too.
    bool operator==(const CondemnedRow & o) const
    {
        return delete_pending == o.delete_pending
            && token.dialect == o.token.dialect && token.value == o.token.value
            && size == o.size && condemn_round == o.condemn_round
            && marker_confirmed == o.marker_confirmed && standing == o.standing;
    }
};

/// The condemned fields of a `RunMarker::Condemned` record.
CondemnedRow condemnedRowOf(const SourceEdgeRecord & rec);

/// Look-ahead for the body `HEAD` of pending rows and the `HEAD` of spared rows, supplied by the GC round. The fold
/// still decides on the value it takes; only the moment of the fetch moves.
struct PendingHeadHints
{
    /// Reserves and hints the `HEAD` of a later row: the body `HEAD` of a pending row, or the spare `HEAD` of a row
    /// whose blob gains a reference this round. Skips a row that needs neither. False: the window is full, or the
    /// row's blob was touched this round and may be decided inline at zero; the scan stops and offers the row again
    /// later.
    std::function<bool(const BlobRef &, const CondemnedRow &)> offer;
    /// Called after the fold takes a pending row's redelete slot and before it scans ahead: the row's own
    /// reservation is released and the row is not hinted again, so its slot is counted once.
    std::function<void(const BlobRef &)> take_pending;
    /// Called before the fold scans ahead of a spared row. Like `take_pending`, it sends the row's own `HEAD` to the
    /// pool if no scan hinted it, so it counts against `gc_io_concurrency`.
    std::function<void(const BlobRef &)> take_spare;
};

/// Rows the merge decodes ahead of its position to find pending rows. Bounds memory, not requests.
inline constexpr size_t kPendingHeadLookaheadRows = 16384;

/// Owns the stream under a `SourceEdgeRunReader` and yields its decoded records. The whole-object chained
/// CityHash128 is accumulated as the run streams; `verifyAgainst` checks it against the fold seal's
/// `RunRef.checksum` AFTER the run is fully drained and BEFORE the caller acts on it (a deletion decision).
class SourceEdgeRunView
{
public:
    /// false once the run's `{"n"}` trailer is consumed (the trailer count is verified there).
    bool next(SourceEdgeRecord & rec);
    /// Verify the accumulated whole-file checksum against the seal's `RunRef.checksum`; CORRUPTED_DATA on
    /// mismatch. Call after draining the run and before acting on its records.
    void verifyAgainst(const UInt128 & expected);
    /// The accumulated whole-file checksum after draining the run — non-throwing, for a read-only auditor
    /// (fsck) that catalogues a mismatch and continues instead of failing closed. Call after draining.
    UInt128 accumulatedChecksum();

private:
    friend SourceEdgeRunView openSourceEdgeRun(std::string_view bytes);
    friend SourceEdgeRunView openSourceEdgeRun(CasOperation & op, const String & key);
    /// Keep the underlying stream alive for the reader, which borrows it rather than owning it.
    explicit SourceEdgeRunView(std::unique_ptr<ReadBuffer> stream_);

    std::unique_ptr<ReadBuffer> stream;             /// owns the backend stream / memory buffer the reader borrows
    std::unique_ptr<SourceEdgeRunReader> reader;    /// over *stream (non-movable => held by pointer, destroyed before stream)
};

/// Open a typed source-edge run. The NDJSON header must identify a `cas_run` of kind `source_edge`;
/// otherwise opening fails closed. The memory overload borrows caller-owned bytes. The backend overload
/// streams the write-once object: the reader itself holds one record-sized buffer, but the open stream
/// underneath it buffers on its own terms, unbounded and unmeasured here.
SourceEdgeRunView openSourceEdgeRun(std::string_view bytes);
SourceEdgeRunView openSourceEdgeRun(CasOperation & op, const String & key);

/// Store a deterministic write-once artifact (same inputs => byte-identical bytes): the blob in-degree
/// runs and fold seals. `create`; a `Conflict` means the write was refused, and only what its resolve
/// read OBSERVED decides what that means: byte-equal bytes are our own deterministic replay (adopt,
/// no-op), divergent bytes or a key that reads as absent are impossible under correct operation and
/// fail closed with `CORRUPTED_DATA` rather than let a divergent artifact disagree with the adopted
/// snapshot, and an unobserved key proves nothing and is reported as the ordinary refusal it is --
/// a corruption verdict there would be a deterministic local failure no caller retries. It is
/// NOT for observation-bearing artifacts (outcome logs) — those carry HEAD-observed
/// incarnations that two observers may legitimately differ on and keep first-durable-write-wins semantics.
void putDeterministicArtifact(CasOperation & op, const String & key, const String & bytes);

/// One source-edge update before merging: the edge `(ref, source_id)`, and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    BlobRef ref{};
    UInt128 source_id{};   /// `sourceEdgeId(ManifestId, path)` — an edge identity, not a content hash
    bool remove = false;
    /// Round-local index of the ref transaction that emitted this delta, into `Cas::TxnApplyLedger`.
    /// NEVER persisted and never part of any comparator: it exists only so the reducer can prove it
    /// consumed at least one delta from every transaction the round declared covered (probe B2). It
    /// lands in the struct's existing tail padding — see `kBlobDeltaSize` below.
    ///
    /// DECLARED LAST, and that is load-bearing: several tests brace-initialise a `BlobDelta`
    /// POSITIONALLY as `{ref, source_id, remove}`. Inserting a field ahead of `remove` silently rebinds
    /// that third initialiser to the ordinal and turns every removal delta into an activation — a
    /// compiling, type-correct, wrong-answer change. Any future field goes after this one, or every
    /// aggregate initialiser gets converted to designated form first.
    uint32_t txn_ordinal = 0;
};

/// The fold's per-edge row runs over potentially millions of records per round, so its size is a
/// deliberate property, not an accident. Pinned here (rather than left to a perf run to discover)
/// because probe B2's `txn_ordinal` was added into the existing tail padding: the row did NOT grow.
/// A future field that breaks this assertion is a hot-path change and must be argued as one.
static constexpr size_t kBlobDeltaSize = 64;
static_assert(sizeof(BlobDelta) == kBlobDeltaSize,
              "BlobDelta is the fold's hot per-edge row; growing it is a hot-path change");

/// One orphan-manifest source edge to retire without pretending it came from a ref transaction.
/// The reducer applies this as an idempotent exact-key removal, but it deliberately consumes no B2
/// transaction ordinal and an already-absent edge is not an unmatched-remove correctness signal.
struct BlobSourceRetirement
{
    BlobRef ref{};
    UInt128 source_id{};
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
/// incarnation, paired with the STALE entry's it superseded. Kept as its own struct (rather than a
/// field bolted onto `RetiredEntry`) so the common merge element stays slim — only replaced entries
/// carry the extra superseded incarnation.
struct ReplacedEntry
{
    RetiredEntry fresh;   /// the freshly condemned CURRENT incarnation (also pushed into still_retired byte-identically)
    PersistedEtag old_token;   /// the superseded (stale) entry's — what republication replaced
};

/// One example of an unmatched-remove delta, kept for the caller's single once-per-round WARNING
/// (see `RetiredMergeResult::unmatched_removes` below) — naming the blob and source id is enough to
/// start an investigation without logging every occurrence from the hot inner loop.
struct UnmatchedRemoveExample
{
    BlobRef ref{};
    UInt128 source_id{};
};

/// A pending row whose body `HEAD` in the fold showed the row's own incarnation. The caller sends
/// `DELETE B If-Match <raw token of observed>` after the fold; the run already holds the row as `standing`.
struct BodyDelete
{
    RetiredEntry entry;
    std::optional<Meta> observed;
};

/// Outcome of the retired merge: the same
/// streaming pass that folds edges settles every prior retired entry and detects new candidates.
struct RetiredMergeResult
{
    std::vector<RetiredEntry> still_retired;   /// every row the output run holds, in run order
    std::vector<RetiredEntry> confirmed;       /// rows this pass wrote `pending` after a confirmed intent create
    std::vector<RetiredEntry> spared;          /// in-degree above zero: row dropped
    std::vector<BodyDelete> body_deletes;      /// rows written `standing` whose body the caller deletes before the commit
    std::vector<RetiredEntry> to_standing;     /// rows this pass moved to `standing`: body delete sent, or body absent
    std::vector<WriteOnceKey> intent_deletes;  /// intents of incarnations observed gone; the caller deletes them before the commit
    /// Of `spared`, the intents whose `HEAD` showed another incarnation; deleted in the same batch as `intent_deletes`.
    std::vector<WriteOnceKey> spare_intent_deletes;
    std::vector<ReplacedEntry> replaced;  /// rows moved to the incarnation now at their key; caller emits blob_retire_replaced
    std::vector<RetiredEntry> adopted;    /// rows this pass wrote for nominated intents that had no row
    /// Intent creates that were sent and not confirmed; the row stays a candidate. One example for the caller's ERROR.
    uint64_t unconfirmed_creates = 0;
    String unconfirmed_create_example;
    /// Observations and rows whose token names no incarnation; nothing is created or deleted for them.
    uint64_t uncanonical_tokens = 0;
    String uncanonical_example;   /// the first blob key
    /// `standing` rows whose `HEAD` showed their own token again: carried, nothing deleted.
    uint64_t standing_anomalies = 0;
    String standing_anomaly_example;   /// the first blob key
    /// Bodies shorter than the pool's blob envelope: no row is written for them, a row is carried, nothing is
    /// deleted. The first blob key, for the caller's ERROR.
    uint64_t undecodable_bodies = 0;
    String undecodable_example;
    /// Such bodies at spared rows: the row is dropped, its intent kept, nothing deleted.
    uint64_t undecodable_spares = 0;
    String undecodable_spare_example;

    /// Count of `remove == true` deltas that matched no presence for their `(BlobRef, source_id)` key —
    /// neither the prior run nor an earlier delta in the same `scattered` batch had activated it. The
    /// in-degree model is a SET, so an unmatched remove is a per-key no-op BY DESIGN (never a false
    /// deletion), but a persistent nonzero rate means removal deltas are reaching the reducer without
    /// their matching activation, which is a correctness signal worth paging on. See
    /// `ProfileEvents::CASGCUnmatchedRemoveDeltas`.
    uint64_t unmatched_removes = 0;
    /// The first unmatched remove observed this merge, for the caller's WARNING (one example is enough
    /// to start an investigation; logging every occurrence would flood a hot per-edge inner loop).
    std::optional<UnmatchedRemoveExample> unmatched_remove_example;
};

/// Cumulative per-round cap over EVERY destructive-or-observability-write work family a round touches —
/// blob intent creates and body deletes (this file), the orphan-manifest planner's namespace fan-out and recovery
/// walk (`CasOrphanManifestSweep.cpp`), ref-object/generation-prefix cleanup, the post-CAS hand-off
/// reclaim, the post-CAS manifest-body cleanup, and the `GcOutcomes` audit log (`CasGc.cpp`). One instance
/// is owned by `Gc::runRegularRound` per round and passed by reference/pointer into each family's call
/// in turn, so a cap is cumulative across the WHOLE round, not reset per shard or per namespace -- except
/// `max_outcome_log_bytes`, which is per shard. `0` in any bound is unbounded, the same opt-out
/// convention every other CAS round budget uses, except `max_outcome_log_bytes`: it defaults to 128 MiB
/// and at `0` drops every row. A default-constructed instance therefore reproduces pre-budget behavior
/// everywhere but for that limit. Exhausting a bound never drops
/// WORK: each destructive family's caller carries the excess in its own already-durable pipeline (still
/// `delete_pending`/condemned rows, a retained sweep candidate, a ref object left for next round) and
/// retries next round with a fresh budget, or -- where nothing durable is left to carry (manifest cleanup,
/// the outcome log) -- exhaustion drops only the redundant record of a decision the round already made
/// safely elsewhere (see each field's own comment). Reusable as-is by a future bounded-parallel-walk: give
/// each worker an atomic increment (or a partitioned share of a bound) instead of inventing a new
/// accounting shape.
struct GcRoundWorkBudget
{
    uint64_t max_intent_creates = 0;
    uint64_t max_redeletes = 0;
    uint64_t intent_creates_used = 0;
    uint64_t redeletes_used = 0;
    /// (shard, blob) of the last create sent. A round that spent the create budget resumes after it, so a key
    /// whose create never confirms cannot take the budget ahead of the keys behind it in every round.
    using CreatePosition = std::pair<uint64_t, BlobRef>;
    std::optional<CreatePosition> creates_resume_after;
    std::optional<CreatePosition> last_create_sent;

    bool intentCreateAvailable() const { return max_intent_creates == 0 || intent_creates_used < max_intent_creates; }
    bool intentCreateAdmits(uint64_t shard, const BlobRef & ref) const
    {
        return intentCreateAvailable() && (!creates_resume_after || *creates_resume_after < CreatePosition{shard, ref});
    }
    /// Where the next round resumes: after the last create when the budget ran out, else from the start.
    std::optional<CreatePosition> nextCreatesResumeAfter() const
    {
        return intentCreateAvailable() ? std::nullopt : last_create_sent;
    }
    bool redeleteAvailable() const { return max_redeletes == 0 || redeletes_used < max_redeletes; }

    /// Orphan-manifest planner (`planManifestCursorPage` / `activeManifestKeys`): how many DISTINCT
    /// namespaces the sweep may build a fresh protection view for in total this round (cumulative
    /// across pages, not per page), and how many ref-log GET/decode
    /// operations the owner recovery and the committed-tail walk may spend in total across every
    /// namespace this round. Both caps exist because building a namespace's protection view (a catalog-authoritative
    /// table recovery plus a committed-tail walk) is the expensive, potentially-unbounded step the
    /// nomination/list budgets never covered.
    uint64_t max_sweep_namespaces = 0;
    uint64_t max_sweep_recovery_ops = 0;
    uint64_t sweep_namespaces_used = 0;
    uint64_t sweep_recovery_ops_used = 0;

    bool sweepNamespaceAvailable() const { return max_sweep_namespaces == 0 || sweep_namespaces_used < max_sweep_namespaces; }
    bool sweepRecoveryOpAvailable() const { return max_sweep_recovery_ops == 0 || sweep_recovery_ops_used < max_sweep_recovery_ops; }

    /// Cleanup families (`Gc::cleanupRefObjects`, `deletePrefixWholesale`'s caller in
    /// `Gc::pruneSupersededGenerations`). `deletePrefixWholesale` already takes a `bounded_remaining`
    /// count; `prefixWholesaleRemaining` turns the shared round budget into that same count (its ONE
    /// unbounded sentinel, `0 == max_prefix_wholesale_objects`, maps to the function's own "no cap"
    /// value, `UINT64_MAX`) so every caller can pass "the round's remainder" instead of `UINT64_MAX`
    /// outright.
    uint64_t max_ref_cleanup_objects = 0;
    uint64_t ref_cleanup_objects_used = 0;
    uint64_t max_prefix_wholesale_objects = 0;
    uint64_t prefix_wholesale_objects_used = 0;

    bool refCleanupAvailable() const { return max_ref_cleanup_objects == 0 || ref_cleanup_objects_used < max_ref_cleanup_objects; }
    uint64_t prefixWholesaleRemaining() const
    {
        if (max_prefix_wholesale_objects == 0)
            return std::numeric_limits<uint64_t>::max();
        return prefix_wholesale_objects_used < max_prefix_wholesale_objects
            ? max_prefix_wholesale_objects - prefix_wholesale_objects_used : 0;
    }

    /// The post-CAS hand-off reclaim (`Gc::runRegularRound`) draws from its OWN reserve, never from
    /// `max_prefix_wholesale_objects` above. The prune is safe to under-serve in any one round -- its
    /// cursor never regresses, so a partially-drained generation is simply finished next round -- but the
    /// hand-off is a ONE-SHOT event with no reclaimer behind it besides `fsck`: a generation it cannot
    /// fully reclaim this round is never revisited (the parent-seal difference that triggers it does not
    /// recur once the ref has moved). Sharing one pool would let a prune-heavy round starve the hand-off
    /// to zero every time; a separate reserve makes that impossible regardless of how much the prune
    /// consumes.
    uint64_t max_handoff_prefix_wholesale_objects = 0;
    uint64_t handoff_prefix_wholesale_objects_used = 0;

    uint64_t handoffPrefixWholesaleRemaining() const
    {
        if (max_handoff_prefix_wholesale_objects == 0)
            return std::numeric_limits<uint64_t>::max();
        return handoff_prefix_wholesale_objects_used < max_handoff_prefix_wholesale_objects
            ? max_handoff_prefix_wholesale_objects - handoff_prefix_wholesale_objects_used : 0;
    }

    /// `GcOutcomes` per-shard body (`Gc::runRegularRound`'s `redelete`/`spared` loops): a pure
    /// observability record of a settlement decision that has ALREADY happened -- the merge unconditionally
    /// decides `spared` for any entry whose in-degree recovered (INV_NO_LOSS: a fresh dedup-adopt must
    /// never be treated as still condemned, past any budget), so this cap governs only whether the decision
    /// gets an audit-log row, never the decision itself. Nothing is retained or retried on exhaustion --
    /// there is nothing left to retry, the entry already left the retired pipeline correctly.
    uint64_t max_outcome_entries = 0;
    uint64_t outcome_entries_used = 0;

    bool outcomeEntryAvailable() const { return max_outcome_entries == 0 || outcome_entries_used < max_outcome_entries; }

    /// Byte limit of the encoded outcome log of ONE shard, independent of the entry budget (the tighter
    /// of the two wins). It keeps the log under the format's decompressed object cap, which only the
    /// decoder enforces. Rows past it are dropped like rows past the entry budget.
    static constexpr uint64_t kDefaultMaxOutcomeLogBytes = 128ULL * 1024 * 1024;
    uint64_t max_outcome_log_bytes = kDefaultMaxOutcomeLogBytes;

    struct OutcomeLogLedger
    {
        uint64_t bytes = kOutcomeLogFramingBound;
        uint64_t dropped = 0;
    };
    std::map<uint64_t, OutcomeLogLedger> outcome_log_ledgers;

    /// True if the audit row of `entry` for `shard` may be recorded; charges the entry and byte budgets.
    bool admitOutcomeRow(uint64_t shard, const OutcomeEntry & entry)
    {
        if (!outcomeEntryAvailable())
            return false;
        OutcomeLogLedger & ledger = outcome_log_ledgers.try_emplace(shard).first->second;
        const uint64_t row_bound = outcomeRowEncodedBound(entry);
        if (ledger.bytes + row_bound > max_outcome_log_bytes)
        {
            ++ledger.dropped;
            return false;
        }
        ledger.bytes += row_bound;
        ++outcome_entries_used;
        return true;
    }
};

/// Merge the prior generation's source-edge run with new deltas. The prior run's `RunMarker::Condemned` rows ride
/// the source-edge run at the zero-sentinel key (`source_id = 0`), so the prior run is also the retired input.
/// Each blob is decided when the merge closes it, after the round's cut and before the output run is written,
/// by `decideIntentStep` over the blob's post-merge in-degree, its row, whether a delta or retirement touched it,
/// and the decision `HEAD` of this round:
///   - a touched key at zero takes its `HEAD` from `head_blob` (no row: a fresh transition) or `peek_head` (a
///     row), for every rule of that row in this pass;
///   - an untouched pending row at zero whose body delete may run takes it from `pending_head`;
///   - a spared `pending` or `standing` row whose token canonicalises, in an unsuppressed round, takes it from
///     `spare_head`;
///   - no other row needs a `HEAD`.
/// A row minted in this pass is written as decided and gets no second decision. A hook that is unset makes the
/// rows that need it carry unchanged, a spared row is dropped instead, and a fresh transition without `head_blob`
/// records nothing. Creates go through `create_intent`, one per row, each charging `work_budget`'s create slot when
/// it is sent; up to its
/// window of them are outstanding while the merge goes on, and the run is written in order once each row's own
/// answer is in. Without `create_intent` no create is sent. A create that throws fails the fold. With
/// `suppress_destructive` no body or intent delete is decided.
/// `pending_hints`, when set, lets the merge hint the `HEAD`s of later pending and spared rows of the open segment.
/// `intent_nominations` are listed intents of this shard. They visit their key without touching it, and are adopted
/// after the key's own rules: a key with no row in the prior run, none from this pass, at zero, gets a row from
/// `peek_head` for the first nomination of the key: `candidate(t)` when the body shows the intent's token `t`,
/// `standing(t)` when it is absent or shows another identity. A nomination without a tag, or any nomination in a
/// suppressed round, is a `LOGICAL_ERROR`. Adoption sends no create and no delete; without `peek_head` nothing is adopted.
/// The merge comparator is exactly `(ref.algo, ref.digest, source_id)` (that is, `BlobRef::operator<`
/// followed by `source_id`), which is also the raw key order produced by `SourceEdgeKeyCodec`.
void foldDeltasIntoGeneration(CasOperation & op, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              uint64_t current_round = 0, uint64_t condemn_round = 0,
                              const BlobHeadFn & head_blob = {},
                              const BlobHeadFn & peek_head = {},
                              const IntentCreates & create_intent = {},
                              RetiredMergeResult * out_retired = nullptr,
                              bool suppress_destructive = false,
                              /// PROBE B2 (see `Cas::TxnApplyLedger`): when set, one byte is stored per
                              /// CONSUMED delta at `(*out_applied_by_txn_ordinal)[d.txn_ordinal]`. A raw
                              /// vector rather than a callback: this runs once per delta over a stream
                              /// that can reach millions of rows, and a `std::function` call there is
                              /// not free. Never read by the merge; write-only. The caller must size it
                              /// to cover every `txn_ordinal` present in `scattered`.
                              std::vector<uint8_t> * out_applied_by_txn_ordinal = nullptr,
                              std::vector<BlobSourceRetirement> source_retirements = {},
                              /// Shared by reference across every shard's call within one round;
                              /// `nullptr` (default) is unbounded, matching every existing caller.
                              GcRoundWorkBudget * work_budget = nullptr,
                              const PendingHeadHints * pending_hints = nullptr,
                              const BlobHeadFn & pending_head = {},
                              std::vector<Layout::ParsedBlobIntentKey> intent_nominations = {},
                              const BlobHeadFn & spare_head = {});

/// Stream the sealed in-degree runs named by `runs` (the current seal's `blob_target_runs` filtered to one
/// shard) and return every blob written at in-degree 0 (the candidates that transitioned to zero). An
/// empty `runs` is an empty baseline. Each `RunRef` supplies the exact object key, so resolution never
/// reconstructs a key from generation metadata.
std::vector<BlobCandidate> zeroInDegree(CasOperation & op, const std::vector<RunRef> & runs);

}
