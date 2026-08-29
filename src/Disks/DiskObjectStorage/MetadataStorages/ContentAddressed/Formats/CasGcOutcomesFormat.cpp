#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>

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

namespace GcOutcomesWire
{
    constexpr WireKey kind{"k"};
    constexpr WireKey outcome{"oc"};
}

constexpr EnumWireTable<OutcomeKind, 4> kOutcomeKindWords{{{
    {OutcomeKind::Deleted, "deleted"},
    {OutcomeKind::Absent, "absent"},
    {OutcomeKind::Replaced, "replaced"},
    {OutcomeKind::Spared, "spared"},
}}};

static_assert(casEnumTableCoversEnum<kOutcomeKindWords, OutcomeKind>());

OutcomeKind outcomeKindFromWord(std::string_view w)
{
    return kOutcomeKindWords.fromWord(w, "CAS outcome log outcome kind");
}

}

std::string_view outcomeKindToWireWord(OutcomeKind outcome)
{
    return kOutcomeKindWords.toWord(outcome, "CAS outcome log outcome kind");
}

String encodeOutcomeLog(const OutcomeLog & log)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::GcOutcomes);
    for (const OutcomeEntry & e : log.entries)
    {
        bool first = true;
        writeStringField(out, GcOutcomesWire::kind, objectKindToWord(e.kind), first);
        writeBlobRefFields(out, first, e.ref);   /// algo + digest
        writeTokenFields(out, first, e.token);   /// token_type + token
        writeStringField(out, GcOutcomesWire::outcome, outcomeKindToWireWord(e.outcome), first);
        closeObject(out, first);
        writeChar('\n', out);
    }
    writeTrailerLine(out, log.entries.size());
    return std::move(out).take();
}

OutcomeLog decodeOutcomeLog(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcOutcomes);
    const uint64_t line_cap = traitsFor(FormatId::GcOutcomes).line_cap;

    OutcomeLog log;
    while (true)
    {
        const String line = readLine(in, line_cap, "outcome log");
        ReadBufferFromMemory line_in(line.data(), line.size());
        JsonObjectReader r(line_in, KeyStrictness::Tolerant, "outcome log");

        String key;
        /// The first key distinguishes a trailer ("n") from a record ("k").
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: empty line");
        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            while (r.nextKey(key))
                r.skipUnknown(key);
            if (!line_in.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: bytes after trailer");
            if (n != log.entries.size())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS outcome log: trailer count {} != {} records", n, log.entries.size());
            return log;
        }

        OutcomeEntry e;
        BlobRefFields blob_ref_fields;
        TokenFields token_fields;
        do
        {
            if (key == GcOutcomesWire::kind) e.kind = objectKindFromWord(r.readString(), "outcome log");
            else if (matchBlobRefFields(key, r, blob_ref_fields)) {}
            else if (matchTokenFields(key, r, token_fields)) {}
            else if (key == GcOutcomesWire::outcome) e.outcome = outcomeKindFromWord(r.readString());
            else r.skipUnknown(key);
        } while (r.nextKey(key));

        if (!blob_ref_fields.algo_word || !blob_ref_fields.digest_hex || !token_fields.type_word)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: record missing ha/h/tt");
        e.ref = blob_ref_fields.build("outcome log");
        e.token = Token{token_fields.value.value_or(""), tokenTypeFromWord(*token_fields.type_word, "outcome log")};
        if (!line_in.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: junk after record");
        log.entries.push_back(std::move(e));
    }
}

}
