#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnumStrings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

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

constexpr uint64_t OUTCOME_LOG_VERSION = 1;

/// `OutcomeKind` <-> string. Unknown string on decode is corruption (fail closed). File-local:
/// the outcome log is this enum's only codec (the shared kind/token_type mappings live in
/// `CasEnumStrings.h`).
std::string_view outcomeToString(OutcomeKind outcome)
{
    switch (outcome)
    {
        case OutcomeKind::Deleted: return "deleted";
        case OutcomeKind::Absent: return "absent";
        case OutcomeKind::Replaced: return "replaced";
        case OutcomeKind::Spared: return "spared";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: invalid outcome {}", static_cast<int>(outcome));
}

OutcomeKind outcomeFromString(std::string_view s, std::string_view what)
{
    if (s == "deleted")
        return OutcomeKind::Deleted;
    if (s == "absent")
        return OutcomeKind::Absent;
    if (s == "replaced")
        return OutcomeKind::Replaced;
    if (s == "spared")
        return OutcomeKind::Spared;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid outcome '{}'", what, s);
}

}

String encodeOutcomeLog(const OutcomeLog & log)
{
    WriteBufferFromOwnString out;
    JsonObjectWriter writer(out);
    writer.field("format", "cas_gc_outcomes");
    writer.field("version", OUTCOME_LOG_VERSION);

    /// `entries` is an array of flat objects: open it through beginValueField, then write each entry
    /// with its own JsonObjectWriter (same separator/escaping rules).
    writer.beginValueField("entries");
    writeChar('[', out);
    bool first = true;
    for (const auto & entry : log.entries)
    {
        if (!first)
            writeChar(',', out);
        first = false;

        JsonObjectWriter entry_writer(out);
        entry_writer.field("kind", objectKindToString(entry.kind));
        entry_writer.field("hash", u128ToHex(entry.hash));
        entry_writer.field("token", entry.token.value);
        entry_writer.field("token_type", tokenTypeToString(entry.token.type));
        entry_writer.field("outcome", outcomeToString(entry.outcome));
        entry_writer.finalize();
    }
    writeChar(']', out);
    writer.finalize();
    return std::move(out.str());
}

OutcomeLog decodeOutcomeLog(std::string_view data)
{
    return decodeJsonGuarded("outcome log", [&]
    {
        auto obj = parseJsonDocument(data, "cas_gc_outcomes", OUTCOME_LOG_VERSION, "outcome log");
        checkNoUnknownKeys(*obj, {"format", "version", "entries"}, "outcome log");

        auto entries = requireArray(*obj, "entries", "outcome log");

        OutcomeLog log;
        for (size_t i = 0; i < entries->size(); ++i)
        {
            auto entry_obj = requireObjectAt(*entries, i, "outcome log");
            checkNoUnknownKeys(*entry_obj, {"kind", "hash", "token", "token_type", "outcome"}, "outcome log entry");

            OutcomeEntry entry;
            entry.kind = objectKindFromString(requireString(*entry_obj, "kind", "outcome log"), "outcome log");
            entry.hash = requireHash(*entry_obj, "hash", "outcome log");
            entry.token.value = requireString(*entry_obj, "token", "outcome log");
            entry.token.type = tokenTypeFromString(requireString(*entry_obj, "token_type", "outcome log"), "outcome log");
            entry.outcome = outcomeFromString(requireString(*entry_obj, "outcome", "outcome log"), "outcome log");
            log.entries.push_back(std::move(entry));
        }
        return log;
    });
}

}
