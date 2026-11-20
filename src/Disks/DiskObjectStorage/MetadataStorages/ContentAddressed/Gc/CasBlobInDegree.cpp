#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
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

/// Streams a shard's prior source-edge runs for the fold merge, plus up to `kPendingHeadLookaheadRows` decoded rows
/// while a pending-row scan is buffering. Rows come from `AdoptedRunReader`, which checks them; this cursor only drops
/// zero markers, which are per-generation and never carried, and exposes surviving edges and condemned rows in
/// `(blob, source_id)` order. An empty `segments` is the fresh-pool / empty baseline.
class PriorEdgeCursor
{
public:
    PriorEdgeCursor(CasOperation & op, const std::vector<RunRef> & segments, uint64_t shard, AdoptedRunContext parent)
        : runs(op, segments, shard, parent)
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

    /// Advance to the next surviving edge OR retired sentinel, crossing run boundaries.
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
            if (auto row = pullRow())
            {
                current = std::move(*row);
                has_current = true;
                return;
            }
            /// The drained run's checksum is verified here, before the fold acts on any row of the next one.
            if (!runs.openNextRun())
            {
                has_current = false;
                return;
            }
        }
    }

    /// Offers the condemned rows from the current row onward, in order, each until `offer` accepts it: rows
    /// of the OPEN run are decoded into a buffer of at most `max_rows`. Stops when `offer` returns false
    /// (that row is offered again next time), the buffer is full, or the open run ends: opening the next run,
    /// and verifying the checksum of the one that ends, stay with `advance`. Rows are checked as `advance`
    /// checks them, so a corrupt row throws here a few rows early.
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
        while (lookahead.size() < max_rows)
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

    /// The next surviving edge or retired sentinel of the open run; nullopt once it is drained.
    std::optional<Row> pullRow()
    {
        SourceEdgeRecord rec;
        while (runs.nextInRun(rec))
        {
            if (rec.marker == RunMarker::Zero)
                continue;
            if (rec.marker == RunMarker::Condemned)
                return Row{.key = SourceEdgeKeyCodec::key(rec.ref, rec.source_id), .ref = rec.ref,
                           .type = RunMarker::Condemned, .condemned = condemnedRowOf(rec)};
            return Row{.key = SourceEdgeKeyCodec::key(rec.ref, rec.source_id), .ref = rec.ref,
                       .type = RunMarker::Edge, .condemned = {}};
        }
        return std::nullopt;
    }

    AdoptedRunReader runs;
    Row current;
    bool has_current = false;
    bool current_offered = false;
    /// Rows decoded ahead of `current`; the first `offered_ahead` of them were offered.
    std::deque<Row> lookahead;
    size_t offered_ahead = 0;
};

}

AdoptedRunReader::AdoptedRunReader(CasOperation & op_, const std::vector<RunRef> & runs_, uint64_t shard_,
                                   AdoptedRunContext context_)
    : op(op_), runs(runs_), shard(shard_), context(context_)
{
}

bool AdoptedRunReader::nextInRun(SourceEdgeRecord & rec)
{
    if (!view || drained)
        return false;
    if (!view->next(rec))
    {
        drained = true;
        return false;
    }
    checkRow(rec);
    return true;
}

bool AdoptedRunReader::openNextRun()
{
    if (view)
    {
        chassert(drained);
        view->verifyAgainst(currentRun().checksum);
        view.reset();
    }
    if (next_run >= runs.size())
        return false;
    drained = false;
    /// Typed open validates the NDJSON header before any row is consumed.
    view = openSourceEdgeRun(op, runs[next_run++].key);
    return true;
}

bool AdoptedRunReader::next(SourceEdgeRecord & rec)
{
    while (!nextInRun(rec))
        if (!openNextRun())
            return false;
    return true;
}

const RunRef & AdoptedRunReader::currentRun() const
{
    chassert(next_run > 0);
    return runs[next_run - 1];
}

void AdoptedRunReader::refuseRow(const SourceEdgeRecord & rec, const String & what) const
{
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS source-edge run `{}` of GC shard {}: the row of blob {} source {} {}. {}",
        currentRun().key, shard, blobIdOf(rec.ref), u128ToHex(rec.source_id), what, kCorruptBookkeeping);
}

void AdoptedRunReader::checkRow(const SourceEdgeRecord & rec)
{
    /// The merge decides a blob from the rows of its own shard alone, and assumes them sorted.
    if (blobShard(rec.ref, context.gc_shards) != shard)
        refuseRow(rec, fmt::format("belongs to GC shard {} of {}, not to shard {} whose run holds it",
            blobShard(rec.ref, context.gc_shards), context.gc_shards, shard));
    const std::pair<BlobRef, UInt128> key{rec.ref, rec.source_id};
    if (previous_key && key < *previous_key)
        refuseRow(rec, "sorts before the row above it");
    previous_key = key;
    if (rec.source_id == kZeroSourceId)
    {
        /// A sentinel key carries exactly one row per blob and never an edge.
        if (rec.marker == RunMarker::Edge)
            refuseRow(rec, "is an active edge at the reserved sentinel source_id 0");
        if (last_sentinel_blob == rec.ref)
            refuseRow(rec, "is a duplicate sentinel row for one blob");
        last_sentinel_blob = rec.ref;
    }
    else if (rec.marker != RunMarker::Edge)
    {
        refuseRow(rec, fmt::format("is a sentinel row type 0x{:02x} at a non-sentinel key", static_cast<uint8_t>(rec.marker)));
    }
    /// The parent round stamped every row it condemned; a later stamp would reach the rules on the cut that minted it.
    if (rec.marker == RunMarker::Condemned && rec.condemn_round > context.parent_round)
        refuseRow(rec, fmt::format("is stamped with round {}, after the parent round {}", rec.condemn_round, context.parent_round));
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

RunRef foldDeltasIntoGeneration(CasOperation & op, const Layout & layout, const std::vector<RunRef> & prior_runs,
                                AdoptedRunContext parent, uint64_t new_generation, uint64_t attempt, uint64_t shard,
                                std::vector<BlobDelta> scattered, ShardFoldOptions options)
{
    if (options.suppress_destructive && !options.intent_nominations.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS fold: orphan intent nominations passed to a suppressed round");

    RetiredMergeResult sink;
    RetiredMergeResult & rmr = options.out_retired ? *options.out_retired : sink;

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
    std::sort(options.source_retirements.begin(), options.source_retirements.end(),
        [](const BlobSourceRetirement & a, const BlobSourceRetirement & b)
        {
            if (a.ref != b.ref) return a.ref < b.ref;
            return a.source_id < b.source_id;
        });
    /// Stable, so the first listed intent of a key is the one adopted.
    std::stable_sort(options.intent_nominations.begin(), options.intent_nominations.end(),
        [](const Layout::ParsedBlobIntentKey & a, const Layout::ParsedBlobIntentKey & b) { return a.ref < b.ref; });

    PriorEdgeCursor cursor(op, prior_runs, shard, parent);

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
    const size_t create_window = std::max<size_t>(1, options.create_intent.window);

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
            rmr.unconfirmed_creates.note([&] { return created.detail; });
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
    auto createAndRecord = [&](RetiredEntry row, const CanonicalIdentity & identity)
    {
        if (options.work_budget)
        {
            ++options.work_budget->intent_creates_used;
            options.work_budget->last_create_sent = GcRoundWorkBudget::RunPosition{shard, cur_blob};
        }
        row.delete_pending = false;
        row.standing = false;
        rmr.still_retired.push_back(std::move(row));
        outstanding.push_back(OutstandingCreate{.answer = options.create_intent.start(cur_blob, identity),
                                                .row = rmr.still_retired.size() - 1});
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
        row.condemn_round = options.condemn_round;
        return row;
    };

    auto noteUncanonical = [&]
    {
        rmr.uncanonical_tokens.note([&] { return layout.blobKey(cur_blob); });
        ProfileEvents::increment(ProfileEvents::CASGCIntentTokenRejected);
    };

    auto noteUndecodable = [&]
    {
        rmr.undecodable_bodies.note([&] { return layout.blobKey(cur_blob); });
    };

    /// A body GC cannot decode is never condemned, adopted or deleted: the row, if any, is carried unchanged.
    auto carryUndecodable = [&](const std::optional<RetiredEntry> & row)
    {
        noteUndecodable();
        if (row)
            rmr.still_retired.push_back(*row);
    };

    /// A spare drops the row and never carries it, so no row sits beside live edges of its blob.
    auto spareRow = [&](const RetiredEntry & row, const std::optional<CanonicalIdentity> & intent_to_delete)
    {
        if (row.delete_pending)
        {
            LOG_DEBUG(getLogger("CasGcFold"),
                "CAS gc fold ({}): {} blob {} (condemned at round {}, observed at round {}) is referenced "
                "again; the row is dropped and {}", layout.poolPrefix(),
                row.standing ? "standing" : "pending", blobIdOf(row.ref), row.condemn_round, options.current_round,
                intent_to_delete ? "the body has another incarnation, so its intent is deleted" : "its intent stays");
            ProfileEvents::increment(ProfileEvents::CASGCRetiredSparedByReref);
            if (intent_to_delete)
                rmr.spare_intent_deletes.push_back(layout.writeOnceBlobIntentKey(cur_blob, *intent_to_delete));
            else
                ProfileEvents::increment(ProfileEvents::CASGCIntentLeftOnSpare);
        }
        rmr.spared.push_back(row);
    };

    /// Settles the blob being closed: builds the one `IntentRuleInput` from this pass's facts and acts on the
    /// decision. The decision is authoritative; nothing here re-derives an identity or a rule.
    ///
    /// Sparing is decided against the in-degree this merge just computed: an edge folded after the condemnation
    /// drops the row whatever its state, so no row outlives a cut that shows a reference.
    auto settleBlob = [&](const std::optional<RetiredEntry> & row)
    {
        IntentRuleInput input;
        input.row = row;
        input.at_zero = cur_edges == 0;
        input.key_touched = cur_touched;
        input.fresh_transition = !row && input.at_zero && cur_touched;
        input.suppressed = options.suppress_destructive;
        input.create_budget_admits
            = options.create_intent && (!options.work_budget || options.work_budget->intentCreateAdmits(shard, cur_blob));
        input.redelete_budget_admits = !options.work_budget || options.work_budget->redeleteAdmits(shard, cur_blob);

        /// A row carried after its `HEAD` keeps its slot, so the budget bounds the round's `HEAD`s of pending rows;
        /// the rotation in `GcRoundWorkBudget` keeps such a row from holding the slot in every round.
        bool redelete_slot_taken = false;
        const auto takeRedeleteSlot = [&]
        {
            if (options.work_budget)
                options.work_budget->takeRedeleteSlot(shard, cur_blob);
            redelete_slot_taken = true;
        };
        switch (decisionHeadNeed(input))
        {
            case DecisionHeadNeed::None:
                break;
            case DecisionHeadNeed::Spare:
            {
                if (!options.spare_head)
                {
                    spareRow(*row, std::nullopt);
                    return;
                }
                /// Before the take, so the `HEAD`s of the spared rows behind this one are in flight while it waits.
                if (options.pending_hints)
                {
                    options.pending_hints->take_spare(cur_blob);
                    cursor.scanAhead(kPendingHeadLookaheadRows, options.pending_hints->offer);
                }
                const BlobHead observed = options.spare_head(cur_blob);
                if (observed.undecodable)
                {
                    rmr.undecodable_spares.note([&] { return layout.blobKey(cur_blob); });
                    spareRow(*row, std::nullopt);
                    return;
                }
                input.head = classifyDecisionHead(row->token, observed.meta);
                break;
            }
            case DecisionHeadNeed::TouchedKey:
            {
                const BlobHeadFn & hook = row ? options.peek_head : options.head_blob;
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
                if (!options.pending_head)
                {
                    rmr.still_retired.push_back(*row);
                    return;
                }
                takeRedeleteSlot();
                /// After the slot is taken, so a look-ahead hint never takes it.
                if (options.pending_hints)
                {
                    options.pending_hints->take_pending(cur_blob);
                    cursor.scanAhead(kPendingHeadLookaheadRows, options.pending_hints->offer);
                }
                const BlobHead observed = options.pending_head(cur_blob);
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
        const bool spends_redelete_slot = decision.step == IntentStep::DeleteBody || decision.step == IntentStep::Standing;
        if (spends_redelete_slot && !redelete_slot_taken)
            takeRedeleteSlot();

        switch (decision.step)
        {
            case IntentStep::None:
                return;
            case IntentStep::Create:
                createAndRecord(row ? *row : observedRow(decision, *input.head), *decision.intent_to_create);
                return;
            case IntentStep::Candidate:
                rmr.still_retired.push_back(observedRow(decision, *input.head));
                return;
            case IntentStep::Carry:
                rmr.still_retired.push_back(*row);
                return;
            case IntentStep::Anomaly:
                rmr.standing_anomalies.note([&] { return layout.blobKey(cur_blob); });
                ProfileEvents::increment(ProfileEvents::CASGCIntentStandingAnomaly);
                rmr.still_retired.push_back(*row);
                return;
            case IntentStep::Drop:
                if (!input.at_zero)
                    spareRow(*row, decision.intent_to_delete);
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
                if (decision.step == IntentStep::TransitionCreate)
                    createAndRecord(std::move(moved), *decision.intent_to_create);
                else
                    rmr.still_retired.push_back(std::move(moved));
                rmr.replaced.push_back(ReplacedEntry{.row = rmr.still_retired.size() - 1, .old_token = row->token});
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
        if (!options.peek_head)
            return;
        const BlobHead observed = options.peek_head(cur_blob);
        if (observed.undecodable)
        {
            carryUndecodable(std::nullopt);
            return;
        }
        const DecisionHead head = classifyDecisionHead(*token, observed.meta);
        RetiredEntry row;
        row.kind = ObjectKind::Blob;
        row.ref = cur_blob;
        row.condemn_round = options.condemn_round;
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

    while (cursor.valid() || di < scattered.size() || ri < options.source_retirements.size() || ni < options.intent_nominations.size())
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
        if (ri < options.source_retirements.size())
        {
            const String rk = SourceEdgeKeyCodec::key(options.source_retirements[ri].ref, options.source_retirements[ri].source_id);
            if ((!from_prior && key.empty()) || rk < key) { key = rk; from_prior = false; }
        }
        /// A nomination sits at its blob's sentinel key, so it opens the blob before any edge of it. On a tie with a
        /// prior condemned row the row goes first; the nomination is then taken at the same blob.
        bool from_nomination = false;
        if (ni < options.intent_nominations.size())
        {
            const String nk = SourceEdgeKeyCodec::key(options.intent_nominations[ni].ref, kZeroSourceId);
            if ((!from_prior && key.empty()) || nk < key) { key = nk; from_prior = false; from_nomination = true; }
        }
        if (from_nomination)
        {
            openBlobIfNeeded(options.intent_nominations[ni].ref);
            if (!cur_nomination)
                cur_nomination = options.intent_nominations[ni];
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
            if (options.out_applied_by_txn_ordinal)
                (*options.out_applied_by_txn_ordinal)[scattered[di].txn_ordinal] = 1;
            present = scattered[di].remove ? false : true;   // apply in order; last wins
            cur_touched = true;
            ++di;
        }

        /// Orphan nomination retires this exact manifest-source identity after ordinary ref deltas at
        /// the same key. It is accounting-neutral: absence is an idempotent no-op, not an unmatched
        /// ref removal, and there is no transaction ordinal to mark in B2's apply ledger.
        while (ri < options.source_retirements.size()
               && options.source_retirements[ri].ref == blob_ref && options.source_retirements[ri].source_id == source_id)
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
    return RunRef{.key = run_key, .checksum = run_checksum, .shard = shard, .key_generation = new_generation};
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
