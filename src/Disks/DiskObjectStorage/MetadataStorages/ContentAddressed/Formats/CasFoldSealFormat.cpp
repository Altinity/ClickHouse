#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasByteBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <algorithm>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

std::string_view holdReasonToWord(HoldReason r)
{
    switch (r)
    {
        case HoldReason::GapBelowWitness:        return "gap_below_witness";
        case HoldReason::UnconsumedSealCrossing: return "unconsumed_seal_crossing";
        case HoldReason::WitnessDisappeared:     return "witness_disappeared";
        case HoldReason::BodyUndecodable:        return "body_undecodable";
        case HoldReason::ManifestBodyMissing:    return "manifest_body_missing";
        case HoldReason::CheckpointUndecodable:  return "checkpoint_undecodable";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown hold reason {}", static_cast<int>(r));
}

namespace
{

std::string_view nsCleanupStateToWord(RefNsCleanupState s)
{
    switch (s)
    {
        case RefNsCleanupState::Pending:   return "pending";
        case RefNsCleanupState::Completed: return "completed";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown ns-cleanup state {}", static_cast<int>(s));
}

RefNsCleanupState nsCleanupStateFromWord(std::string_view w)
{
    if (w == "pending")   return RefNsCleanupState::Pending;
    if (w == "completed") return RefNsCleanupState::Completed;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown ns-cleanup state '{}'", w);
}

HoldReason holdReasonFromWord(std::string_view w)
{
    if (w == "gap_below_witness")        return HoldReason::GapBelowWitness;
    if (w == "unconsumed_seal_crossing") return HoldReason::UnconsumedSealCrossing;
    if (w == "witness_disappeared")      return HoldReason::WitnessDisappeared;
    if (w == "body_undecodable")         return HoldReason::BodyUndecodable;
    if (w == "manifest_body_missing")    return HoldReason::ManifestBodyMissing;
    if (w == "checkpoint_undecodable")   return HoldReason::CheckpointUndecodable;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown hold reason '{}'", w);
}

/// The classification set is CLOSED. Every consumer of a coverage row branches on exact values — the
/// sweep's §6 deletion premise refuses a row by testing `== 4` and then `== 0` — so a value outside the
/// set is not an unknown variant to be tolerated forward: it is a row that passes every refusal written
/// in terms of the set and reaches the irreversible delete. One predicate, used by both directions, so
/// the writer's self-check and the reader's fail-close can never name different sets.
bool isKnownClassification(uint64_t classification)
{
    return classification == 0 || classification == 1 || classification == 2 || classification == 4;
}

/// A hold names a position the fold must resolve, and both components of that id are nonzero (the
/// canonical `RefTxnId` rule `renderRefTxnId` enforces for every id that becomes a key). A zero
/// component is not a weaker hold, it is a self-erasing one: no position sorts below `{0, 0}`, so the
/// carry rule clears it on the first round that folds anything, and the durable evidence that the
/// namespace ever stopped disappears without a single record having been resolved.
bool isCanonicalHoldPosition(const RefTxnId & at)
{
    return at.writer_epoch != 0 && at.ref_sequence != 0;
}

/// Insert a decoded record under a key that must appear at most ONCE in the object. Plain
/// `map[key] = value` is last-wins, and last-wins is not a lossy nicety here: a repeated coverage key
/// lets a later, clean row overwrite the held one that a whole namespace's retention rests on, which is
/// exactly how a forged or mis-merged seal would erase the only durable record of an unresolved
/// position. One record per key, or the object is corrupt.
template <typename Map, typename Key, typename Value>
void insertRecordOnce(Map & map, const Key & key, Value && value, std::string_view what)
{
    if (!map.emplace(key, std::forward<Value>(value)).second)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS fold seal: a second {} record for '{}' — a key appears at most once, and accepting the "
            "duplicate would silently overwrite the record already read",
            what, key);
}

/// Emit one run record (`k` = "btr") WITHOUT its line terminator; the caller closes (and measures) the
/// line, and sorts the vector by key first.
void writeRun(CasJsonWriter & out, std::string_view kind, const RunRef & r)
{
    bool first = true;
    writeKey(out, "k", first);     writeStringValue(out, kind);
    writeKey(out, "key", first);   writeStringValue(out, r.key);
    writeKey(out, "ck", first);    writeHex128Value(out, r.checksum);
    writeKey(out, "shard", first); writeIntText(r.shard, out);
    writeKey(out, "gen", first);   writeU64StringValue(out, r.generation);
    closeObject(out, first);
}

}

FoldSealCaps foldSealCaps()
{
    const FormatTraits & t = traitsFor(FormatId::FoldSeal);
    return FoldSealCaps{.line_cap = t.line_cap, .object_cap = t.object_cap};
}

void checkFoldSealObjectBytes(uint64_t encoded_bytes)
{
    const uint64_t object_cap = foldSealCaps().object_cap;
    if (!fitsObjectCap(encoded_bytes, /*entries_reservation*/0, object_cap))
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "CAS fold seal: the seal encodes to {} bytes, over the {}-byte object cap. Writing it "
            "would leave a durable seal no later round can read; the round is refused before the PUT "
            "and retries.",
            encoded_bytes, object_cap);
}

String encodeFoldSeal(const CasFoldSeal & seal)
{
    const FoldSealCaps caps = foldSealCaps();
    CasJsonWriter out(256);

    /// EVERY line this encoder emits is measured against the LINE cap, on the bytes actually emitted --
    /// escaping, framing and all -- rather than on an estimate of them. A line that does not fit is not
    /// a large line, it is an UNREADABLE one: `readLine` refuses it, so the whole object is lost.
    /// Refuse here, where nothing is durable yet. The header and trailer are measured too, even though
    /// their lengths are bounded by construction — a gate with an unstated exception is one that a
    /// later edit widens without noticing.
    const auto checkLineBytes = [&](uint64_t bytes, std::string_view what)
    {
        if (!fitsLineCap(bytes, caps.line_cap))
            throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                "CAS fold seal: the {} line encodes to {} bytes, over the {}-byte line cap; a longer "
                "line cannot be read back, so the seal is refused before it is written",
                what, bytes, caps.line_cap);
    };

    writeHeaderLine(out, FormatId::FoldSeal);   /// emits its own terminator
    checkLineBytes(out.size() - 1, "header");

    size_t line_start = out.size();
    const auto closeLine = [&](std::string_view what)
    {
        checkLineBytes(out.size() - line_start, what);
        writeChar('\n', out);
        line_start = out.size();
    };

    /// meta line
    {
        bool first = true;
        writeKey(out, "g", first);  writeU64StringValue(out, seal.generation);
        writeKey(out, "pg", first); writeU64StringValue(out, seal.parent_generation);
        closeObject(out, first);
        closeLine("meta");
    }

    uint64_t n = 0;

    /// coverage (std::map => key-sorted)
    for (const auto & [key, cov] : seal.per_ns_shard)
    {
        /// THE STRICT GRAMMAR, enforced where the bytes are produced. Every refusal below is
        /// `LOGICAL_ERROR`: this row came from our own fold, so an ill-formed one is a bug in this
        /// process, not corruption arriving from a store — and none of these shapes is repairable once
        /// durable, so none is ever written.
        ///
        /// A classification outside the closed set first, because the two checks after it are stated in
        /// terms of the set and a row they cannot classify makes their answers meaningless.
        if (!isKnownClassification(cov.classification))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS fold seal: coverage '{}' has classification {}, which is not one of the four the "
                "fold grammar defines (0 absent, 1 unchanged, 2 folded, 4 clamped) — every consumer "
                "branches on those exact values, so this row would pass refusals meant to stop it",
                key, cov.classification);
        /// A classification-4 row whose hold was dropped is indistinguishable, once durable, from a
        /// namespace that stopped for no reason — and a hold on any other classification claims a stop
        /// that did not happen.
        if ((cov.classification == 4) != cov.hold.has_value())
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS fold seal: coverage '{}' has classification {} and {} hold — the hold fields are "
                "required for classification 4 and forbidden otherwise",
                key, cov.classification, cov.hold ? "a" : "no");
        /// A hold that names no position resolves itself on the next round (nothing sorts below
        /// `{0, 0}`) and cannot be rendered where the sweep reports why it retained a manifest.
        if (cov.hold && !isCanonicalHoldPosition(cov.hold->offending_position))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS fold seal: coverage '{}' is held at {}-{} — a hold's offending position has both "
                "components nonzero; a zero one would be cleared by the first record the next round "
                "folds, erasing the only durable evidence that the namespace stopped",
                key, cov.hold->offending_position.writer_epoch, cov.hold->offending_position.ref_sequence);

        bool first = true;
        writeKey(out, "k", first);    writeStringValue(out, "cov");
        writeKey(out, "key", first);  writeStringValue(out, key);
        writeKey(out, "cls", first);  writeIntText(static_cast<uint32_t>(cov.classification), out);
        writeKey(out, "lfe", first);  writeU64StringValue(out, cov.last_folded_ref_id.writer_epoch);
        writeKey(out, "lfs", first);  writeU64StringValue(out, cov.last_folded_ref_id.ref_sequence);
        if (cov.hold)
        {
            writeKey(out, "hr", first);  writeStringValue(out, holdReasonToWord(cov.hold->reason));
            writeKey(out, "hpe", first); writeU64StringValue(out, cov.hold->offending_position.writer_epoch);
            writeKey(out, "hps", first); writeU64StringValue(out, cov.hold->offending_position.ref_sequence);
            writeKey(out, "hrc", first); writeIntText(cov.hold->retry_count, out);
            writeKey(out, "hnr", first); writeU64StringValue(out, cov.hold->next_retry_round);
        }
        closeObject(out, first);
        closeLine("cov");
        ++n;
    }

    {
        std::vector<RunRef> runs = seal.blob_target_runs;
        std::sort(runs.begin(), runs.end(), [](const RunRef & a, const RunRef & b) { return a.key < b.key; });
        for (const RunRef & r : runs)
        {
            writeRun(out, "btr", r);
            closeLine("btr");
        }
    }
    n += seal.blob_target_runs.size();

    /// condemned summary (std::map<uint64> => shard-sorted)
    for (const auto & [shard, s] : seal.condemned_summary)
    {
        bool first = true;
        writeKey(out, "k", first);     writeStringValue(out, "cnd");
        writeKey(out, "shard", first); writeIntText(shard, out);
        writeKey(out, "ct", first);    writeIntText(s.condemned_total, out);
        writeKey(out, "pt", first);    writeIntText(s.pending_total, out);
        writeKey(out, "ocr", first);   writeU64StringValue(out, s.oldest_nonpending_condemn_round);
        closeObject(out, first);
        closeLine("cnd");
        ++n;
    }

    /// ns-cleanup items (std::map<String> => key-sorted)
    for (const auto & [key, item] : seal.ns_cleanup_items)
    {
        bool first = true;
        writeKey(out, "k", first);   writeStringValue(out, "nsc");
        writeKey(out, "ns", first);  writeStringValue(out, item.ns.string());
        writeKey(out, "rte", first); writeU64StringValue(out, item.remove_txn_id.writer_epoch);
        writeKey(out, "rts", first); writeU64StringValue(out, item.remove_txn_id.ref_sequence);
        writeKey(out, "st", first);  writeStringValue(out, nsCleanupStateToWord(item.state));
        closeObject(out, first);
        closeLine("nsc");
        ++n;
    }

    const size_t trailer_start = out.size();
    writeTrailerLine(out, n);   /// emits its own terminator
    checkLineBytes(out.size() - trailer_start - 1, "trailer");

    String text = std::move(out).take();
    checkFoldSealObjectBytes(text.size());
    return text;
}

CasFoldSeal decodeFoldSeal(std::string_view data, std::optional<uint64_t> expected_generation)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::FoldSeal);
    const uint64_t line_cap = traitsFor(FormatId::FoldSeal).line_cap;

    CasFoldSeal seal;

    /// meta line
    {
        const String meta = readLine(in, line_cap, "fold seal");
        ReadBufferFromMemory m(meta.data(), meta.size());
        JsonObjectReader r(m, KeyStrictness::Strict, "fold seal");
        String key;
        while (r.nextKey(key))
        {
            if (key == "g") seal.generation = r.readU64String();
            else if (key == "pg") seal.parent_generation = r.readU64String();
            else r.skipUnknown(key);   /// Strict => any unknown key is CORRUPTED_DATA
        }
    }

    uint64_t seen = 0;
    while (true)
    {
        const String line = readLine(in, line_cap, "fold seal");
        ReadBufferFromMemory l(line.data(), line.size());
        JsonObjectReader r(l, KeyStrictness::Strict, "fold seal");
        String key;
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: empty line");

        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            if (r.nextKey(key))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: trailer has extra keys");
            if (!l.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: bytes after trailer");
            if (n != seen)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: trailer count {} != {} records", n, seen);
            if (expected_generation && seal.generation != *expected_generation)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: body generation {} does not match the requested generation {}",
                    seal.generation, *expected_generation);
            return seal;
        }
        if (key != "k")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: record must start with \"k\"");
        const String kind = r.readString();

        if (kind == "cov")
        {
            String map_key;
            ShardCoverage cov;
            /// Read WIDE and validated before it is narrowed to the persisted byte. `cls` is the field
            /// every consumer branches on, and a plain `static_cast<uint8_t>` maps 258 onto 2 ("all
            /// records through the cursor were folded") and 256 onto 0 — a forged or damaged seal would
            /// buy full coverage with an integer no reader ever sees.
            std::optional<uint64_t> classification;
            /// The hold fields are read individually so the grammar can be checked on WHICH of them
            /// arrived, not merely on how many. `JsonObjectReader` already rejects a duplicate key, so
            /// a second `hr` can never quietly rewrite the reason.
            std::optional<HoldReason> hold_reason;
            std::optional<uint64_t> hold_epoch;
            std::optional<uint64_t> hold_sequence;
            std::optional<uint32_t> hold_retry_count;
            std::optional<uint64_t> hold_next_retry_round;
            while (r.nextKey(key))
            {
                if (key == "key") map_key = r.readString();
                else if (key == "cls") classification = r.readU64Number();
                else if (key == "lfe") cov.last_folded_ref_id.writer_epoch = r.readU64String();
                else if (key == "lfs") cov.last_folded_ref_id.ref_sequence = r.readU64String();
                else if (key == "hr") hold_reason = holdReasonFromWord(r.readString());
                else if (key == "hpe") hold_epoch = r.readU64String();
                else if (key == "hps") hold_sequence = r.readU64String();
                else if (key == "hrc") hold_retry_count = r.readU32Number();
                else if (key == "hnr") hold_next_retry_round = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown cov key '{}'", key);
            }

            /// `cls` is required, not defaulted: an absent one would read as 0 ("no round folded this
            /// namespace"), which is a claim about a fold, not the absence of one.
            if (!classification)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: cov '{}' missing cls", map_key);
            if (!isKnownClassification(*classification))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: coverage '{}' has classification {}, which is not one of the four "
                    "the fold grammar defines (0 absent, 1 unchanged, 2 folded, 4 clamped)",
                    map_key, *classification);
            cov.classification = static_cast<uint8_t>(*classification);   /// in range, so narrowing is exact

            /// The same strict grammar the encoder enforces, applied to bytes we did not write. A
            /// PARTIAL hold is corruption, never a hold with defaults: a hold whose offending position
            /// defaulted to `{0,0}` would be cleared by the very first record the next round folds.
            const bool any_hold_field = hold_reason || hold_epoch || hold_sequence
                || hold_retry_count || hold_next_retry_round;
            const bool every_hold_field = hold_reason && hold_epoch && hold_sequence
                && hold_retry_count && hold_next_retry_round;
            if (cov.classification == 4)
            {
                if (!every_hold_field)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS fold seal: coverage '{}' is held (classification 4) but its hold is "
                        "incomplete — reason, offending position, retry count and next retry round are "
                        "all required", map_key);
                /// PRESENT is not enough: the position must be one a fold can actually retry. `{0, 0}`
                /// (or either component zero) is the shape that quietly deletes the hold — the carry
                /// rule keeps a hold only while the walk stops BELOW it, and nothing is below zero — and
                /// it cannot be rendered where the sweep names the position it retained a manifest for.
                if (!isCanonicalHoldPosition(RefTxnId{*hold_epoch, *hold_sequence}))
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS fold seal: coverage '{}' is held at {}-{} — a hold's offending position has "
                        "both components nonzero; a zero one clears itself on the next round",
                        map_key, *hold_epoch, *hold_sequence);
                cov.hold = RefHold{.reason = *hold_reason,
                                   .offending_position = RefTxnId{*hold_epoch, *hold_sequence},
                                   .retry_count = *hold_retry_count,
                                   .next_retry_round = *hold_next_retry_round};
            }
            else if (any_hold_field)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: coverage '{}' carries hold fields at classification {} — they are "
                    "forbidden on anything but a held (classification 4) row",
                    map_key, cov.classification);
            insertRecordOnce(seal.per_ns_shard, map_key, cov, "coverage");
        }
        else if (kind == "btr")
        {
            RunRef run;
            while (r.nextKey(key))
            {
                if (key == "key") run.key = r.readString();
                else if (key == "ck") run.checksum = r.readHex128();
                else if (key == "shard") run.shard = r.readU64Number();
                else if (key == "gen") run.generation = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown run key '{}'", key);
            }
            seal.blob_target_runs.push_back(run);
        }
        else if (kind == "cnd")
        {
            uint64_t shard = 0;
            CondemnedSummary s;
            while (r.nextKey(key))
            {
                if (key == "shard") shard = r.readU64Number();
                else if (key == "ct") s.condemned_total = r.readU64Number();
                else if (key == "pt") s.pending_total = r.readU64Number();
                else if (key == "ocr") s.oldest_nonpending_condemn_round = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown cnd key '{}'", key);
            }
            insertRecordOnce(seal.condemned_summary, shard, s, "condemned-summary");
        }
        else if (kind == "nsc")
        {
            String ns;
            RefTxnId txn;
            RefNsCleanupState st = RefNsCleanupState::Pending;
            bool have_st = false;
            while (r.nextKey(key))
            {
                if (key == "ns") ns = r.readString();
                else if (key == "rte") txn.writer_epoch = r.readU64String();
                else if (key == "rts") txn.ref_sequence = r.readU64String();
                else if (key == "st") { st = nsCleanupStateFromWord(r.readString()); have_st = true; }
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown nsc key '{}'", key);
            }
            if (!have_st)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: nsc missing st");
            /// Checked before it is rendered, and reported as corruption: `renderRefTxnId` refuses a zero
            /// component with `LOGICAL_ERROR`, which is an ABORT under a debug or sanitizer build — so an
            /// omitted or zeroed `rte`/`rts` on the wire would take the process down inside a decode of
            /// foreign bytes instead of failing this read closed.
            if (txn.writer_epoch == 0 || txn.ref_sequence == 0)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: ns-cleanup item for '{}' names removal transaction {}-{} — both "
                    "components of the id are required and nonzero",
                    ns, txn.writer_epoch, txn.ref_sequence);
            const String map_key = ns + "\n" + renderRefTxnId(txn);   /// mirrors the prior decoder's key
            insertRecordOnce(seal.ns_cleanup_items, map_key,
                             RefNsCleanupItem{RootNamespace{ns}, txn, st}, "ns-cleanup");
        }
        else
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown record kind '{}'", kind);

        if (!l.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: junk after record");
        ++seen;
    }
}

}
