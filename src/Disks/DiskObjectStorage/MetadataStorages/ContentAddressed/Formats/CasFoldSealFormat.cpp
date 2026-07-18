#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

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

/// Emit one run record (`k` = "btr"); the caller sorts the vector by key first.
void writeRun(WriteBuffer & out, std::string_view kind, const RunRef & r)
{
    bool first = true;
    writeKey(out, "k", first);     writeStringValue(out, kind);
    writeKey(out, "key", first);   writeStringValue(out, r.key);
    writeKey(out, "ck", first);    writeHex128Value(out, r.checksum);
    writeKey(out, "shard", first); writeIntText(r.shard, out);
    writeKey(out, "gen", first);   writeU64StringValue(out, r.generation);
    closeObject(out, first);
    writeChar('\n', out);
}

void writeSortedRuns(WriteBuffer & out, std::string_view kind, std::vector<RunRef> runs)
{
    std::sort(runs.begin(), runs.end(), [](const RunRef & a, const RunRef & b) { return a.key < b.key; });
    for (const RunRef & r : runs)
        writeRun(out, kind, r);
}

}

String encodeFoldSeal(const CasFoldSeal & seal)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::FoldSeal);

    /// meta line
    {
        bool first = true;
        writeKey(out, "g", first);  writeU64StringValue(out, seal.generation);
        writeKey(out, "pg", first); writeU64StringValue(out, seal.parent_generation);
        closeObject(out, first);
        writeChar('\n', out);
    }

    uint64_t n = 0;

    /// coverage (std::map => key-sorted)
    for (const auto & [key, cov] : seal.per_ns_shard)
    {
        bool first = true;
        writeKey(out, "k", first);    writeStringValue(out, "cov");
        writeKey(out, "key", first);  writeStringValue(out, key);
        writeKey(out, "cls", first);  writeIntText(static_cast<uint32_t>(cov.classification), out);
        writeTokenFields(out, first, cov.folded_token);   /// tt + tv
        writeKey(out, "lfe", first);  writeU64StringValue(out, cov.last_folded_ref_id.writer_epoch);
        writeKey(out, "lfs", first);  writeU64StringValue(out, cov.last_folded_ref_id.ref_sequence);
        closeObject(out, first);
        writeChar('\n', out);
        ++n;
    }

    writeSortedRuns(out, "btr", seal.blob_target_runs);
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
        writeChar('\n', out);
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
        writeChar('\n', out);
        ++n;
    }

    writeTrailerLine(out, n);
    out.finalize();
    return out.str();
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
            String tv;
            TokenType tt{};
            bool have_tt = false;
            ShardCoverage cov;
            while (r.nextKey(key))
            {
                if (key == "key") map_key = r.readString();
                else if (key == "cls") cov.classification = static_cast<uint8_t>(r.readU64Number());
                else if (key == "tt") { tt = tokenTypeFromWord(r.readString(), "fold seal"); have_tt = true; }
                else if (key == "tv") tv = r.readString();
                else if (key == "lfe") cov.last_folded_ref_id.writer_epoch = r.readU64String();
                else if (key == "lfs") cov.last_folded_ref_id.ref_sequence = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown cov key '{}'", key);
            }
            if (!have_tt)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: cov missing tt");
            cov.folded_token = Token{tv, tt};
            seal.per_ns_shard[map_key] = cov;
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
            seal.condemned_summary[shard] = s;
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
            const String map_key = ns + "\n" + renderRefTxnId(txn);   /// mirrors the prior decoder's key
            seal.ns_cleanup_items[map_key] = RefNsCleanupItem{RootNamespace{ns}, txn, st};
        }
        else
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown record kind '{}'", kind);

        if (!l.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: junk after record");
        ++seen;
    }
}

}
