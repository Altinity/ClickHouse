#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasIntentRules.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <base/defines.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>

namespace ProfileEvents
{
    extern const Event CASGCRetiredSparedByReref;
    extern const Event CASGCUnmatchedRemoveDeltas;
    extern const Event CASGCIntentLeftOnSpare;
    extern const Event CASGCIntentTokenRejected;
    extern const Event CASGCIntentAdopted;
    extern const Event CASGCIntentStandingAnomaly;
}
#include <city.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

const UInt128 kZeroSourceId{0};

/// Run records held behind a row whose create is unanswered; past this the merge waits for the oldest answer.
/// Bounds memory, not requests.
constexpr size_t kHeldRunRecords = 16384;

/// Streams a shard's prior source-edge run with one block resident, plus up to `kPendingHeadLookaheadRows`
/// decoded rows while a pending-row scan is buffering: chains the run SEGMENTS the caller resolved from the
/// parent seal (`blob_target_runs` filtered to one shard) and exposes the next row to the fold merge. The
/// prior run carries
/// BOTH surviving edges (`RunMarker::Edge`) AND the retired `RunMarker::Condemned` sentinel rows at the zero source id,
/// so the cursor stops at edges AND at condemned rows (exposing the type via `rowType`), while zero-marker
/// sentinels are dropped on carry (per-generation, never carried forward). Row/key invariants are enforced
/// while streaming: `RunMarker::Edge` never at `source_id = 0`; sentinel rows (`RunMarker::Zero` /
/// `RunMarker::Condemned`) ONLY at `source_id = 0`; at most one sentinel per blob. Resolution uses the exact object references supplied by the caller, so a run
/// sealed for generation G that physically lives under an older generation's key is reached
/// without key construction. An empty `segments` is the fresh-pool / empty baseline. The row stream is
/// globally sorted by (blob_hash, source_id), so `key()` values are non-decreasing.
class PriorEdgeCursor
{
public:
    /// The key codec is stateless and self-describing, so a run may freely mix supported hash algorithms.
    PriorEdgeCursor(CasOperation & op_, const std::vector<RunRef> & segments_)
        : op(op_), segments(segments_)
    {
        advance();
    }

    bool valid() const { return has_current; }
    const String & key() const { return current.key; }
    /// The value byte of the current row: `RunMarker::Edge` (a surviving edge) or `RunMarker::Condemned` (a retired
    /// sentinel row). Zero markers are never surfaced (dropped on carry).
    RunMarker rowType() const { return current.type; }
    /// The decoded retired sentinel for the current row (only valid when `rowType() == RunMarker::Condemned`).
    const CondemnedRow & condemnedRow() const { return current.condemned; }

    /// Advance to the next surviving edge OR retired sentinel, dropping zero markers, enforcing the
    /// row/key invariants, and crossing segment boundaries.
    void advance()
    {
        if (!lookahead.empty())
        {
            current = std::move(lookahead.front());
            lookahead.pop_front();
            current_offered = offered_ahead > 0;
            if (offered_ahead > 0)
                --offered_ahead;
            has_current = true;
            return;
        }
        current_offered = false;
        while (true)
        {
            if (reader)
            {
                if (auto row = pullRow())
                {
                    current = std::move(*row);
                    has_current = true;
                    return;
                }
                /// Segment fully drained: verify its whole-file checksum against
                /// the seal's RunRef.checksum BEFORE the fold acts on any of its rows. This is the fold —
                /// the checksum's most important consumer (it decides deletions). Fail-closed on mismatch.
                reader->verifyAgainst(segments[seg_idx].checksum);
                reader.reset();
                ++seg_idx;
            }

            /// Open the next resolved segment; the segment list is exhausted => the chain is done.
            if (seg_idx >= segments.size())
            {
                has_current = false;
                return;
            }
            /// Typed open validates the NDJSON header before any row is consumed. Each row carries its
            /// own algorithm byte, so no separate width gate is needed.
            reader = openSourceEdgeRun(op, segments[seg_idx].key);
            reader_drained = false;
        }
    }

    /// Offers the condemned rows from the current row onward, in order, each until `offer` accepts it: rows
    /// of the OPEN segment are decoded into a buffer of at most `max_rows`. Stops when `offer` returns false
    /// (that row is offered again next time), the buffer is full, or the open segment ends: opening the next segment,
    /// and verifying the checksum of the one that ends, stay with `advance`. Rows are validated as `advance`
    /// validates them, so a corrupt row throws here a few rows early.
    void scanAhead(size_t max_rows, const std::function<bool(const BlobRef &, const CondemnedRow &)> & offer)
    {
        if (has_current && !current_offered)
        {
            if (!offerRow(current, offer))
                return;
            current_offered = true;
        }
        while (offered_ahead < lookahead.size())
        {
            if (!offerRow(lookahead[offered_ahead], offer))
                return;
            ++offered_ahead;
        }
        while (reader && lookahead.size() < max_rows)
        {
            auto row = pullRow();
            if (!row)
                return;
            lookahead.push_back(std::move(*row));
            if (!offerRow(lookahead.back(), offer))
                return;
            ++offered_ahead;
        }
    }

private:
    struct Row
    {
        String key;
        BlobRef ref;
        RunMarker type = RunMarker::Edge;
        CondemnedRow condemned;
    };

    static bool offerRow(const Row & row, const std::function<bool(const BlobRef &, const CondemnedRow &)> & offer)
    {
        if (row.type != RunMarker::Condemned)
            return true;
        return offer(row.ref, row.condemned);
    }

    /// The next surviving edge or retired sentinel of the open segment, validated; nullopt once it is drained.
    std::optional<Row> pullRow()
    {
        if (reader_drained)
            return std::nullopt;
        SourceEdgeRecord rec;
        while (reader->next(rec))
        {
            const RunMarker v = rec.marker;
            if (rec.source_id == kZeroSourceId)
            {
                /// A sentinel key carries exactly one row per blob and never an edge.
                if (v == RunMarker::Edge)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS source-edge run: active edge at the reserved sentinel source_id 0");
                if (have_sentinel_blob && sentinel_blob == rec.ref)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS source-edge run: duplicate sentinel row for one blob");
                have_sentinel_blob = true;
                sentinel_blob = rec.ref;
                if (v == RunMarker::Zero)
                    continue;   // A zero marker is per-generation and is dropped on carry.
                /// A retired sentinel: surface it (settled at close-out, not an edge).
                return Row{.key = SourceEdgeKeyCodec::key(rec.ref, rec.source_id), .ref = rec.ref,
                           .type = RunMarker::Condemned, .condemned = condemnedRowOf(rec)};
            }

            /// A non-sentinel key must carry a surviving edge and nothing else.
            if (v != RunMarker::Edge)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS source-edge run: sentinel row type 0x{:02x} at a non-sentinel key",
                    static_cast<uint8_t>(v));
            return Row{.key = SourceEdgeKeyCodec::key(rec.ref, rec.source_id), .ref = rec.ref,
                       .type = RunMarker::Edge, .condemned = {}};
        }
        reader_drained = true;
        return std::nullopt;
    }

    CasOperation & op;
    const std::vector<RunRef> & segments;

    size_t seg_idx = 0;
    std::optional<SourceEdgeRunView> reader;
    /// The open segment returned its last row; `advance` verifies it and opens the next.
    bool reader_drained = false;
    Row current;
    bool has_current = false;
    bool current_offered = false;
    /// Rows decoded ahead of `current`; the first `offered_ahead` of them were offered.
    std::deque<Row> lookahead;
    size_t offered_ahead = 0;

    /// Duplicate-sentinel guard: the last blob for which a sentinel row was seen (across skipped zero
    /// markers too). Rows are globally sorted, so two sentinels for one blob are adjacent.
    bool have_sentinel_blob = false;
    BlobRef sentinel_blob{};
};

}

UInt128 sourceEdgeId(const ManifestId & id, const String & path)
{
    String canon;
    canon += id.root_namespace.string();
    canon += '\0';
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    auto beU32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(id.ref.writer_epoch); beU64(id.ref.build_sequence); beU32(id.ref.manifest_ordinal);
    canon += '\0';
    canon += path;
    const auto h = CityHash_v1_0_2::CityHash128(canon.data(), canon.size());
    const UInt128 result = (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
    if (result == UInt128{0})
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS source edge: hash collided with the reserved sentinel id 0");
    return result;
}

void assertValidSourceEdgeId(const UInt128 & source_id)
{
    if (source_id == UInt128{0})
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS source edge: source_id 0 is the reserved sentinel key");
}

CondemnedRow condemnedRowOf(const SourceEdgeRecord & rec)
{
    return CondemnedRow{.delete_pending = rec.delete_pending, .token = rec.token, .size = rec.size,
                        .condemn_round = rec.condemn_round, .marker_confirmed = rec.marker_confirmed,
                        .standing = rec.standing};
}

IntentCreates::IntentCreates(IntentCreateFn create)
{
    if (!create)
        return;
    start = [fn = std::move(create)](const BlobRef & ref, const CanonicalIdentity & identity) -> IntentCreateAnswer
    {
        IntentCreateResult answer = fn(ref, identity);
        return [result = std::move(answer)] { return result; };
    };
}

SourceEdgeRunView::SourceEdgeRunView(std::unique_ptr<ReadBuffer> stream_)
    : stream(std::move(stream_))
    , reader(std::make_unique<SourceEdgeRunReader>(*stream))   /// the reader borrows *stream; both are members
{
}

bool SourceEdgeRunView::next(SourceEdgeRecord & rec)
{
    return reader->next(rec);
}

void SourceEdgeRunView::verifyAgainst(const UInt128 & expected)
{
    reader->verifyAgainst(expected);
}

UInt128 SourceEdgeRunView::accumulatedChecksum()
{
    return reader->accumulatedChecksum();
}

SourceEdgeRunView openSourceEdgeRun(std::string_view bytes)
{
    return SourceEdgeRunView(std::make_unique<ReadBufferFromMemory>(bytes.data(), bytes.size()));
}

SourceEdgeRunView openSourceEdgeRun(CasOperation & op, const String & key)
{
    /// Streaming: a forward-only read of the write-once run — nothing is materialized whole
    /// (cas_run is object_cap = 0). Absent object => fail-closed.
    auto stream = op.stream(key, Retry::standard());
    if (!stream)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: object {} is absent", key);
    return SourceEdgeRunView(std::move(stream));
}

namespace
{
/// len-drift guard (mirrors DigestCodec::checkZeroTail, CasBlobDigest.h): a caller passing a digest
/// wider than the algo's own width is a programming bug, not corrupted on-disk data — chassert, not
/// throw.
void checkZeroTailForAlgo(const BlobDigest & d, uint8_t digest_len, [[maybe_unused]] const char * what)
{
    for (size_t i = digest_len; i < d.bytes.size(); ++i)
        chassert(d.bytes[i] == 0, fmt::format("SourceEdgeKeyCodec::{}: non-zero byte at tail position {} (digest_len={})", what, i, digest_len));
}
}

String SourceEdgeKeyCodec::key(const BlobRef & ref, const UInt128 & source_id)
{
    const uint8_t digest_len = static_cast<uint8_t>(blobHashLenFor(ref.algo));
    checkZeroTailForAlgo(ref.digest, digest_len, "key");
    String out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(ref.algo)));
    out += String(reinterpret_cast<const char *>(ref.digest.bytes.data()), digest_len);
    out += u128ToBytesBE(source_id);
    return out;
}

void SourceEdgeKeyCodec::parse(std::string_view key, BlobRef & ref, UInt128 & source_id)
{
    if (key.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: empty key");
    const uint8_t algo_byte = static_cast<uint8_t>(key[0]);
    BlobHashAlgo algo{};
    switch (algo_byte)
    {
        case static_cast<uint8_t>(BlobHashAlgo::CityHash128): algo = BlobHashAlgo::CityHash128; break;
        case static_cast<uint8_t>(BlobHashAlgo::XXH3_128):    algo = BlobHashAlgo::XXH3_128; break;
        case static_cast<uint8_t>(BlobHashAlgo::Sha256):      algo = BlobHashAlgo::Sha256; break;
        default:
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "CAS source-edge run: unknown algo byte {} in key", algo_byte);
    }
    const uint8_t digest_len = static_cast<uint8_t>(blobHashLenFor(algo));
    if (key.size() != 1 + static_cast<size_t>(digest_len) + 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS source-edge run: malformed key ({} bytes, expected {})", key.size(), 1 + digest_len + 16);
    ref = BlobRef{};
    ref.algo = algo;
    memcpy(ref.digest.bytes.data(), key.data() + 1, digest_len);
    source_id = u128FromBytesBE(String(key.substr(1 + digest_len, 16)), "src-edge run key source_id");
}

void putDeterministicArtifact(CasOperation & op, const String & key, const String & bytes)
{
    WriteResult result = op.create(key, bytes, Retry::standard());
    if (const auto * conflict = std::get_if<Conflict>(&result))
    {
        /// Only something the resolve read actually OBSERVED can support a corruption verdict.
        if (const auto * occupant = std::get_if<Object>(&conflict->seen))
        {
            if (occupant->bytes == bytes)
                return;   /// our own deterministic replay; adopt (no-op).
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc: deterministic artifact at {} occupied by divergent bytes (impossible under "
                "correct operation; refusing to proceed)", key);
        }
        if (std::holds_alternative<ProvenAbsent>(conflict->seen))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc: deterministic artifact at {} refused the write but reads as absent (impossible "
                "under correct operation; refusing to proceed)", key);
        /// Nothing was observed, so the key's state is unknown. Reporting that as corruption would be a
        /// deterministic local failure, which nothing above this retries -- it falls through instead.
    }
    const String what = "CAS gc: deterministic artifact at " + key;
    orThrow(std::move(result), what);
}

void foldDeltasIntoGeneration(CasOperation & op, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              uint64_t current_round, uint64_t condemn_round,
                              const BlobHeadFn & head_blob,
                              const BlobHeadFn & peek_head,
                              const IntentCreates & create_intent,
                              RetiredMergeResult * out_retired,
                              bool suppress_destructive,
                              std::vector<uint8_t> * out_applied_by_txn_ordinal,
                              std::vector<BlobSourceRetirement> source_retirements,
                              GcRoundWorkBudget * work_budget,
                              const PendingHeadHints * pending_hints,
                              const BlobHeadFn & pending_head,
                              std::vector<Layout::ParsedBlobIntentKey> intent_nominations)
{
    if (suppress_destructive && !intent_nominations.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS fold: orphan intent nominations passed to a suppressed round");

    RetiredMergeResult sink;
    RetiredMergeResult & rmr = out_retired ? *out_retired : sink;

    // Deterministic input ordering produces a byte-reproducible run for safe retry and adoption.
    // MUST be stable: for the same (ref, source_id) the journal ordering is
    // activation-before-removal; "last wins" then correctly resolves to removal (edge absent).
    // An unstable sort can put removal before activation => last=activation => false positive.
    // The comparator is exactly (ref.algo, ref.digest, source_id) == BlobRef::operator< then
    // source_id — the same order the raw keys sort in (`SourceEdgeKeyCodec::key`'s algo
    // byte decides before any digest byte can), so the merge below stays a plain key comparison.
    std::stable_sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b)
        {
            if (a.ref != b.ref) return a.ref < b.ref;
            return a.source_id < b.source_id;
        });
    std::sort(source_retirements.begin(), source_retirements.end(),
        [](const BlobSourceRetirement & a, const BlobSourceRetirement & b)
        {
            if (a.ref != b.ref) return a.ref < b.ref;
            return a.source_id < b.source_id;
        });
    /// Stable, so the first listed intent of a key is the one adopted.
    std::stable_sort(intent_nominations.begin(), intent_nominations.end(),
        [](const Layout::ParsedBlobIntentKey & a, const Layout::ParsedBlobIntentKey & b) { return a.ref < b.ref; });

    PriorEdgeCursor cursor(op, prior_runs);

    DB::WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);   // sorted NDJSON; byte-deterministic for write-once adoption

    // Streaming two-cursor merge over the prior run (surviving edges AND retired RunMarker::Condemned
    // sentinel rows at the zero source id) and this round's edge deltas (by (blob_hash, source_id)). All
    // rows for one blob are adjacent in both inputs; the sentinel key (source_id 0) sorts first. We resolve
    // final presence per edge locally (idempotent: prior present + activate => present; any remove =>
    // absent), settle each blob's carried retired row against its post-merge in-degree at close-out, and
    // re-emit the surviving retired rows / zero-transition markers. O(block) IO + O(1) per current blob.
    size_t di = 0;
    size_t ri = 0;
    BlobRef cur_blob{};
    bool have_blob = false;
    uint64_t cur_edges = 0;              // surviving edges of cur_blob so far
    bool cur_touched = false;            // cur_blob had prior edges or deltas this generation
    std::optional<CondemnedRow> cur_condemned;   // the retired sentinel carried on the prior run for cur_blob
    std::optional<Layout::ParsedBlobIntentKey> cur_nomination;   // the first listed intent of cur_blob, never a touch
    std::optional<uint64_t> cur_row_create;   // the create cur_blob's row waits for
    size_t ni = 0;

    auto toRetiredEntry = [](const BlobRef & ref, const CondemnedRow & r) -> RetiredEntry
    {
        RetiredEntry e;
        e.kind = ObjectKind::Blob;
        e.ref = ref;
        e.token = r.token;
        e.size = r.size;
        e.condemn_round = r.condemn_round;
        e.delete_pending = r.delete_pending;
        e.standing = r.standing;
        return e;
    };
    /// The run is written in merge order, but a row whose create is unanswered is not known yet: it and every
    /// record after it are held until that answer is taken. Answers are taken in the order the creates were sent,
    /// which is run order, so each row is written from its own create's answer.
    struct OutstandingCreate
    {
        IntentCreateAnswer answer;
        size_t row = 0;                    /// in `rmr.still_retired`
        std::optional<size_t> replaced;    /// in `rmr.replaced`
    };
    struct HeldRecord
    {
        SourceEdgeRecord record;           /// an edge or a zero marker
        std::optional<size_t> row;         /// a condemned row, built from `rmr.still_retired` when it is written
        std::optional<uint64_t> create;    /// the create the row waits for
    };
    std::deque<OutstandingCreate> outstanding;
    std::deque<HeldRecord> held;
    uint64_t creates_sent = 0;
    uint64_t creates_answered = 0;
    const size_t create_window = std::max<size_t>(1, create_intent.window);

    auto writable = [&](const HeldRecord & r) { return !r.create || *r.create < creates_answered; };
    auto writeRecord = [&](const HeldRecord & r)
    {
        if (!r.row)
        {
            writer.append(r.record);
            return;
        }
        const RetiredEntry & e = rmr.still_retired[*r.row];
        writer.append(SourceEdgeRecord{.ref = e.ref, .source_id = kZeroSourceId, .marker = RunMarker::Condemned,
                                       .delete_pending = e.delete_pending, .token = e.token,
                                       .size = e.size, .condemn_round = e.condemn_round,
                                       .marker_confirmed = false, .standing = e.standing});
    };
    auto writeHeld = [&]
    {
        while (!held.empty() && writable(held.front()))
        {
            writeRecord(held.front());
            held.pop_front();
        }
    };
    /// The row is `pending` on a confirmed create, else `candidate`.
    auto takeOldestAnswer = [&]
    {
        OutstandingCreate create = std::move(outstanding.front());
        outstanding.pop_front();
        const IntentCreateResult created = create.answer();
        RetiredEntry & row = rmr.still_retired[create.row];
        row.delete_pending = created.confirmed();
        if (created.confirmed())
            rmr.confirmed.push_back(row);
        else
        {
            ++rmr.unconfirmed_creates;
            if (rmr.unconfirmed_create_example.empty())
                rmr.unconfirmed_create_example = created.detail;
        }
        if (create.replaced)
            rmr.replaced[*create.replaced].fresh = row;
        ++creates_answered;
        writeHeld();
    };
    auto emit = [&](HeldRecord r)
    {
        if (held.empty() && writable(r))
        {
            writeRecord(r);
            return;
        }
        held.push_back(std::move(r));
        writeHeld();
        while (held.size() > kHeldRunRecords && !outstanding.empty())
            takeOldestAnswer();
    };
    /// Records `row` as a candidate and sends the create a decision names. Only a sent create spends a budget slot.
    auto createAndRecord = [&](RetiredEntry row, const CanonicalIdentity & identity, std::optional<size_t> replaced)
    {
        if (work_budget)
        {
            ++work_budget->intent_creates_used;
            work_budget->last_create_sent = GcRoundWorkBudget::CreatePosition{shard, cur_blob};
        }
        row.delete_pending = false;
        row.standing = false;
        rmr.still_retired.push_back(std::move(row));
        outstanding.push_back(OutstandingCreate{.answer = create_intent.start(cur_blob, identity),
                                                .row = rmr.still_retired.size() - 1, .replaced = replaced});
        cur_row_create = creates_sent++;
        while (outstanding.size() >= create_window)
            takeOldestAnswer();
    };

    /// The row a decision writes for the incarnation its `HEAD` observed, stamped with this round.
    auto observedRow = [&](const IntentDecision & decision, const DecisionHead & head)
    {
        RetiredEntry row;
        row.kind = ObjectKind::Blob;
        row.ref = cur_blob;
        row.token = *decision.new_token;
        row.size = head.observed()->size;
        row.condemn_round = condemn_round;
        return row;
    };

    auto noteUncanonical = [&]
    {
        ++rmr.uncanonical_tokens;
        if (rmr.uncanonical_example.empty())
            rmr.uncanonical_example = layout.blobKey(cur_blob);
        ProfileEvents::increment(ProfileEvents::CASGCIntentTokenRejected);
    };

    /// A body GC cannot decode is never condemned, adopted or deleted: the row, if any, is carried unchanged.
    auto carryUndecodable = [&](const std::optional<RetiredEntry> & row)
    {
        ++rmr.undecodable_bodies;
        if (rmr.undecodable_example.empty())
            rmr.undecodable_example = layout.blobKey(cur_blob);
        if (row)
            rmr.still_retired.push_back(*row);
    };

    /// Settles the blob being closed: builds the one `IntentRuleInput` from this pass's facts and acts on the
    /// decision. The decision is authoritative; nothing here re-derives an identity or a rule.
    ///
    /// Sparing is decided against the in-degree this merge just computed: an edge folded after the condemnation
    /// drops the row whatever its state, so no row outlives a cut that shows a reference.
    auto settleBlob = [&](const std::optional<RetiredEntry> & row)
    {
        /// A destructive step on the cut that minted the row could miss a writer's edge.
        if (row && row->condemn_round >= condemn_round)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS fold: the row of {} is stamped with round {}, not before this pass's round {}: a row minted in this "
                "pass reached the rules", layout.blobKey(cur_blob), row->condemn_round, condemn_round);
        IntentRuleInput input;
        input.row = row;
        input.at_zero = cur_edges == 0;
        input.key_touched = cur_touched;
        input.fresh_transition = !row && input.at_zero && cur_touched;
        input.suppressed = suppress_destructive;
        input.create_budget_admits = create_intent && (!work_budget || work_budget->intentCreateAdmits(shard, cur_blob));
        input.redelete_budget_admits = !work_budget || work_budget->redeleteAvailable();

        bool redelete_slot_taken = false;
        switch (decisionHeadNeed(input))
        {
            case DecisionHeadNeed::None:
                break;
            case DecisionHeadNeed::TouchedKey:
            {
                const BlobHeadFn & hook = row ? peek_head : head_blob;
                if (!hook)
                {
                    if (row)
                        rmr.still_retired.push_back(*row);
                    return;
                }
                const BlobHead observed = hook(cur_blob);
                if (observed.undecodable)
                {
                    carryUndecodable(row);
                    return;
                }
                input.head = row ? classifyDecisionHead(row->token, observed.meta) : classifyDecisionHead(observed.meta);
                break;
            }
            case DecisionHeadNeed::PendingBody:
            {
                if (!pending_head)
                {
                    rmr.still_retired.push_back(*row);
                    return;
                }
                if (work_budget)
                    ++work_budget->redeletes_used;
                redelete_slot_taken = true;
                /// After the slot is taken, so a look-ahead hint never takes it.
                if (pending_hints)
                {
                    pending_hints->take_pending(cur_blob);
                    cursor.scanAhead(kPendingHeadLookaheadRows, pending_hints->offer);
                }
                const BlobHead observed = pending_head(cur_blob);
                if (observed.undecodable)
                {
                    carryUndecodable(row);
                    return;
                }
                input.head = classifyDecisionHead(row->token, observed.meta);
                break;
            }
        }

        if ((input.head && input.head->seen() == HeadSeen::Uncanonical) || (row && !row->token.canonical()))
            noteUncanonical();

        const IntentDecision decision = decideIntentStep(input);
        if ((decision.step == IntentStep::DeleteBody || decision.step == IntentStep::Standing) && !redelete_slot_taken
            && work_budget)
            ++work_budget->redeletes_used;

        switch (decision.step)
        {
            case IntentStep::None:
                return;
            case IntentStep::Create:
                createAndRecord(row ? *row : observedRow(decision, *input.head), *decision.intent_to_create, std::nullopt);
                return;
            case IntentStep::Candidate:
                rmr.still_retired.push_back(observedRow(decision, *input.head));
                return;
            case IntentStep::Carry:
                rmr.still_retired.push_back(*row);
                return;
            case IntentStep::Anomaly:
                ++rmr.standing_anomalies;
                if (rmr.standing_anomaly_example.empty())
                    rmr.standing_anomaly_example = layout.blobKey(cur_blob);
                ProfileEvents::increment(ProfileEvents::CASGCIntentStandingAnomaly);
                rmr.still_retired.push_back(*row);
                return;
            case IntentStep::Drop:
                if (!input.at_zero)
                {
                    if (row->delete_pending)
                    {
                        LOG_DEBUG(getLogger("CasGcFold"),
                            "CAS gc fold ({}): {} blob {} (condemned at round {}, observed at round {}) is referenced "
                            "again; the row is dropped and its intent stays", layout.poolPrefix(),
                            row->standing ? "standing" : "pending", blobIdOf(row->ref), row->condemn_round,
                            current_round);
                        ProfileEvents::increment(ProfileEvents::CASGCRetiredSparedByReref);
                        ProfileEvents::increment(ProfileEvents::CASGCIntentLeftOnSpare);
                    }
                    rmr.spared.push_back(*row);
                }
                return;
            case IntentStep::DeleteBody:
            case IntentStep::Standing:
            {
                RetiredEntry standing = *row;
                standing.standing = true;
                rmr.still_retired.push_back(standing);
                rmr.to_standing.push_back(std::move(standing));
                if (decision.step == IntentStep::DeleteBody)
                    rmr.body_deletes.push_back(BodyDelete{.entry = *row, .observed = input.head->observed()});
                return;
            }
            case IntentStep::TransitionCreate:
            case IntentStep::TransitionCandidate:
            {
                RetiredEntry moved = observedRow(decision, *input.head);
                rmr.replaced.push_back(ReplacedEntry{.fresh = moved, .old_token = row->token});
                if (decision.step == IntentStep::TransitionCreate)
                    createAndRecord(std::move(moved), *decision.intent_to_create, rmr.replaced.size() - 1);
                else
                    rmr.still_retired.push_back(std::move(moved));
                if (decision.intent_to_delete)
                    rmr.intent_deletes.push_back(layout.writeOnceBlobIntentKey(cur_blob, *decision.intent_to_delete));
                return;
            }
            case IntentStep::DeleteIntent:
                rmr.intent_deletes.push_back(layout.writeOnceBlobIntentKey(cur_blob, *decision.intent_to_delete));
                return;
        }
    };

    /// Adopts an intent that has no row. It sends only the `HEAD`, which follows the listing and the cut.
    auto adoptOrphan = [&](const Layout::ParsedBlobIntentKey & nomination)
    {
        const std::optional<PersistedEtag> token = persistedTokenOf(nomination.identity);
        if (!token)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS fold: orphan intent nomination of {} carries no tag", layout.blobKey(cur_blob));
        if (!peek_head)
            return;
        const BlobHead observed = peek_head(cur_blob);
        if (observed.undecodable)
        {
            carryUndecodable(std::nullopt);
            return;
        }
        const DecisionHead head = classifyDecisionHead(*token, observed.meta);
        RetiredEntry row;
        row.kind = ObjectKind::Blob;
        row.ref = cur_blob;
        row.condemn_round = condemn_round;
        switch (head.seen())
        {
            case HeadSeen::Uncanonical:
                noteUncanonical();
                return;
            case HeadSeen::Same:
                /// Not pending: no create of this round confirmed the intent.
                row.token = PersistedEtag::capture(head.observed()->etag);
                row.size = head.observed()->size;
                break;
            case HeadSeen::Absent:
            case HeadSeen::Other:
                row.token = *token;
                row.delete_pending = true;
                row.standing = true;
                break;
        }
        ProfileEvents::increment(ProfileEvents::CASGCIntentAdopted);
        rmr.adopted.push_back(row);
        rmr.still_retired.push_back(std::move(row));
    };

    auto closeBlob = [&]()
    {
        if (!have_blob)
            return;
        const size_t retired_before = rmr.still_retired.size();

        if (cur_condemned)
            settleBlob(toRetiredEntry(cur_blob, *cur_condemned));
        else if (cur_edges == 0 && cur_touched)
            settleBlob(std::nullopt);
        /// The key's own rules first: a row from the prior run or from this pass wins over the nomination.
        if (cur_nomination && !cur_condemned && cur_edges == 0 && rmr.still_retired.size() == retired_before)
            adoptOrphan(*cur_nomination);

        /// Emit at most one sentinel row per blob: the `RunMarker::Condemned` row when the blob has a row
        /// after this pass (still_retired grew for it), else a per-generation `RunMarker::Zero` when it
        /// transitioned to zero this pass with no row (absent at the condemnation, or its intent deleted). A blob with surviving edges (cur_edges > 0) emits neither — its edge rows
        /// were appended inline, and a condemned/zeroed blob has NO surviving edges, so appending the
        /// sentinel now (its key sorts first for the blob, and no edge rows precede it) keeps the run
        /// sorted. `still_retired` therefore mirrors exactly the emitted `RunMarker::Condemned` rows, in order.
        if (rmr.still_retired.size() > retired_before)
            emit(HeldRecord{.record = {}, .row = rmr.still_retired.size() - 1, .create = cur_row_create});
        else if (cur_edges == 0 && cur_touched)
            emit(HeldRecord{.record = SourceEdgeRecord{.ref = cur_blob, .source_id = kZeroSourceId, .marker = RunMarker::Zero},
                            .row = std::nullopt, .create = std::nullopt});
    };
    auto openBlobIfNeeded = [&](const BlobRef & b)
    {
        if (!have_blob || b != cur_blob)
        {
            closeBlob();
            cur_blob = b; have_blob = true; cur_edges = 0; cur_touched = false; cur_condemned.reset(); cur_nomination.reset();
            cur_row_create.reset();
        }
    };

    while (cursor.valid() || di < scattered.size() || ri < source_retirements.size() || ni < intent_nominations.size())
    {
        // Pick the smallest row key across the prior-run cursor and this round's deltas.
        String key;
        bool from_prior = false;
        if (cursor.valid()) { key = cursor.key(); from_prior = true; }
        if (di < scattered.size())
        {
            const String dk = SourceEdgeKeyCodec::key(scattered[di].ref, scattered[di].source_id);
            if (!from_prior || dk < key) { key = dk; from_prior = false; }
        }
        if (ri < source_retirements.size())
        {
            const String rk = SourceEdgeKeyCodec::key(source_retirements[ri].ref, source_retirements[ri].source_id);
            if ((!from_prior && key.empty()) || rk < key) { key = rk; from_prior = false; }
        }
        /// A nomination sits at its blob's sentinel key, so it opens the blob before any edge of it. On a tie with a
        /// prior condemned row the row goes first; the nomination is then taken at the same blob.
        bool from_nomination = false;
        if (ni < intent_nominations.size())
        {
            const String nk = SourceEdgeKeyCodec::key(intent_nominations[ni].ref, kZeroSourceId);
            if ((!from_prior && key.empty()) || nk < key) { key = nk; from_prior = false; from_nomination = true; }
        }
        if (from_nomination)
        {
            openBlobIfNeeded(intent_nominations[ni].ref);
            if (!cur_nomination)
                cur_nomination = intent_nominations[ni];
            ++ni;
            continue;
        }

        BlobRef blob_ref;
        UInt128 source_id;
        SourceEdgeKeyCodec::parse(key, blob_ref, source_id);   // throws CORRUPTED_DATA on a malformed key (fail-closed)
        openBlobIfNeeded(blob_ref);

        /// A retired sentinel row from the prior run: stash it for close-out settlement. It is not an edge
        /// and NEVER a touch — a carried RunMarker::Condemned row must not force a zero-marker or a peek_head HEAD
        /// and never a touch. Deltas never key the zero source id, so no delta merges at this key.
        if (from_prior && cursor.rowType() == RunMarker::Condemned)
        {
            cur_condemned = cursor.condemnedRow();
            cursor.advance();
            continue;
        }

        bool present = false;
        if (from_prior && cursor.key() == key) { present = true; cursor.advance(); cur_touched = true; }
        while (di < scattered.size()
               && scattered[di].ref == blob_ref && scattered[di].source_id == source_id)
        {
            /// An unmatched remove: `present` was false immediately before this remove delta is
            /// applied, meaning neither the prior run nor an earlier delta in this same scattered run
            /// for this key had activated it. The set semantics make this a harmless per-key no-op
            /// (never a false deletion), but a persistent nonzero rate is a correctness signal — count
            /// it and hand ONE example back to the caller, who logs once per round (never from this
            /// hot inner loop; it runs over potentially millions of rows).
            if (scattered[di].remove && !present)
            {
                ++rmr.unmatched_removes;
                ProfileEvents::increment(ProfileEvents::CASGCUnmatchedRemoveDeltas);
                if (!rmr.unmatched_remove_example)
                    rmr.unmatched_remove_example = UnmatchedRemoveExample{blob_ref, source_id};
            }
            /// PROBE B2: this delta reached a reducer and is being CONSUMED. Marked here rather than
            /// at run flush because the in-degree model is a SET — an unmatched `-1` and a duplicate
            /// `+1` legitimately vanish inside the merge, so a flush-side mark would fire on healthy
            /// rounds. See `Cas::TxnApplyLedger`.
            if (out_applied_by_txn_ordinal)
                (*out_applied_by_txn_ordinal)[scattered[di].txn_ordinal] = 1;
            present = scattered[di].remove ? false : true;   // apply in order; last wins
            cur_touched = true;
            ++di;
        }

        /// Orphan nomination retires this exact manifest-source identity after ordinary ref deltas at
        /// the same key. It is accounting-neutral: absence is an idempotent no-op, not an unmatched
        /// ref removal, and there is no transaction ordinal to mark in B2's apply ledger.
        while (ri < source_retirements.size()
               && source_retirements[ri].ref == blob_ref && source_retirements[ri].source_id == source_id)
        {
            present = false;
            cur_touched = true;
            ++ri;
        }

        if (present)
        {
            emit(HeldRecord{.record = SourceEdgeRecord{.ref = blob_ref, .source_id = source_id, .marker = RunMarker::Edge},
                            .row = std::nullopt, .create = std::nullopt});
            ++cur_edges;
        }
    }
    closeBlob();
    while (!outstanding.empty())
        takeOldestAnswer();
    chassert(held.empty());

    writer.finish();
    out.finalize();
    const String run_bytes = out.str();
    /// Whole-object streaming checksum: the same chained CityHash128 the reader
    /// accumulates on the read path, replacing the retired one-shot cityHash128. Carried by the fold
    /// seal's RunRef.checksum and verified before any consumer acts on the run.
    const UInt128 run_checksum = sourceEdgeRunChecksum(run_bytes);
    const String run_key = layout.blobTargetRunKey(new_generation, attempt, shard, 0);
    putDeterministicArtifact(op, run_key, run_bytes);
    out_runs.push_back(RunRef{.key = run_key, .checksum = run_checksum,
                              .shard = shard, .key_generation = new_generation});
}

std::vector<BlobCandidate> zeroInDegree(CasOperation & op, const std::vector<RunRef> & runs)
{
    std::vector<BlobCandidate> result;
    for (const RunRef & run : runs)
    {
        /// The caller passes the exact object key, so a run sealed
        /// for a later generation but physically living under an older key is reached directly. The run is
        /// streamed at O(one block) resident memory, never materialized whole. `openSourceEdgeRun` enforces
        /// the run kind + key schema; `RunMarker::Condemned` sentinel rows are skipped (only `RunMarker::Zero` counts).
        SourceEdgeRunView r = openSourceEdgeRun(op, run.key);
        SourceEdgeRecord rec;
        while (r.next(rec))
            if (rec.marker == RunMarker::Zero)
                result.push_back(BlobCandidate{.ref = rec.ref});
        /// Whole-file checksum: verify the drained run against the seal's
        /// RunRef.checksum BEFORE its candidates feed a GC delete decision. Fail-closed on mismatch.
        r.verifyAgainst(run.checksum);
    }
    return result;
}

}
