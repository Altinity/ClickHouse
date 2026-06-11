#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
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

constexpr uint64_t ROOT_VERSION = 1;

/// `JournalRecord::Op` <-> string. Unknown string on decode is corruption (fail closed).
std::string_view journalOpToString(JournalRecord::Op op)
{
    switch (op)
    {
        case JournalRecord::Op::Add: return "add";
        case JournalRecord::Op::Remove: return "remove";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid journal op {}", static_cast<int>(op));
}

JournalRecord::Op journalOpFromString(std::string_view s, std::string_view what)
{
    if (s == "add")
        return JournalRecord::Op::Add;
    if (s == "remove")
        return JournalRecord::Op::Remove;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid journal op '{}'", what, s);
}

void writeMutableFiles(WriteBuffer & out, const std::map<String, String> & files)
{
    writeChar('{', out);
    bool first = true;
    for (const auto & [k, v] : files)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeJsonKey(out, k);
        writeJsonString(v, out);
    }
    writeChar('}', out);
}

}

String encodeRootShard(const RootShard & root)
{
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_root_shard", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(ROOT_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "shard_version");
    writeIntText(root.shard_version, out);
    writeChar(',', out);
    writeJsonKey(out, "fence_round");
    writeIntText(root.fence_round, out);
    writeChar(',', out);

    /// refs: JSON object keyed by ref name, iterated in std::map's sorted order.
    writeJsonKey(out, "refs");
    writeChar('{', out);
    bool first_ref = true;
    for (const auto & [name, payload] : root.refs)
    {
        if (!first_ref)
            writeChar(',', out);
        first_ref = false;

        writeJsonKey(out, name);
        writeChar('{', out);
        writeJsonKey(out, "tree");
        writeJsonString(u128ToHex(payload.tree_id), out);
        writeChar(',', out);
        writeJsonKey(out, "tree_size");
        writeIntText(payload.tree_size, out);
        writeChar(',', out);
        writeJsonKey(out, "mutable_files");
        writeMutableFiles(out, payload.mutable_files);
        writeChar('}', out);
    }
    writeChar('}', out);
    writeChar(',', out);

    /// journal: JSON array preserving insertion order.
    writeJsonKey(out, "journal");
    writeChar('[', out);
    bool first_rec = true;
    for (const auto & rec : root.journal)
    {
        if (!first_rec)
            writeChar(',', out);
        first_rec = false;

        writeChar('{', out);
        writeJsonKey(out, "op");
        writeJsonString(journalOpToString(rec.op), out);
        writeChar(',', out);
        writeJsonKey(out, "ref");
        writeJsonString(rec.ref_name, out);
        writeChar(',', out);
        writeJsonKey(out, "tree");
        writeJsonString(u128ToHex(rec.tree_id), out);
        writeChar(',', out);
        writeJsonKey(out, "at_version");
        writeIntText(rec.at_version, out);
        writeChar('}', out);
    }
    writeChar(']', out);

    writeChar('}', out);
    return std::move(out.str());
}

RootShard decodeRootShard(std::string_view data)
{
    return decodeJsonGuarded("root shard", [&]
    {
        auto obj = parseJsonDocument(data, "cas_root_shard", ROOT_VERSION, "root shard");
        checkNoUnknownKeys(*obj, {"format", "version", "shard_version", "fence_round", "refs", "journal"}, "root shard");

        RootShard root;
        root.shard_version = requireU64(*obj, "shard_version", "root shard");
        root.fence_round = requireU64(*obj, "fence_round", "root shard");

        auto refs = requireObject(*obj, "refs", "root shard");
        for (const auto & [name, value] : *refs)
        {
            if (value.type() != typeid(Poco::JSON::Object::Ptr))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: ref '{}' must be an object", name);
            auto ref_obj = value.extract<Poco::JSON::Object::Ptr>();
            checkNoUnknownKeys(*ref_obj, {"tree", "tree_size", "mutable_files"}, "root shard ref");

            RefPayload payload;
            payload.tree_id = requireHash(*ref_obj, "tree", "root shard ref");
            payload.tree_size = requireU64(*ref_obj, "tree_size", "root shard ref");
            payload.mutable_files = requireStringMap(*ref_obj, "mutable_files", "root shard ref");
            root.refs[name] = std::move(payload);
        }

        auto journal = requireArray(*obj, "journal", "root shard");
        for (size_t i = 0; i < journal->size(); ++i)
        {
            auto rec_obj = requireObjectAt(*journal, i, "root shard journal");
            checkNoUnknownKeys(*rec_obj, {"op", "ref", "tree", "at_version"}, "root shard journal record");

            JournalRecord rec;
            rec.op = journalOpFromString(requireString(*rec_obj, "op", "root shard journal"), "root shard journal");
            rec.ref_name = requireString(*rec_obj, "ref", "root shard journal");
            rec.tree_id = requireHash(*rec_obj, "tree", "root shard journal");
            rec.at_version = requireU64(*rec_obj, "at_version", "root shard journal");

            /// The GC fold replays the journal in order under a cursor bound: an out-of-order
            /// at_version would fold silently in vector order (a corruption-induced UNDER-count =
            /// a wrong delete later), and a record beyond shard_version would be silently skipped
            /// by the cursor window. Both are corruption - fail closed. NON-DECREASING, not
            /// strict: dropNamespace legally appends N Removes at the same committed version.
            if (rec.at_version > root.shard_version)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS root shard: journal at_version {} exceeds shard_version {}",
                    rec.at_version, root.shard_version);
            if (!root.journal.empty() && rec.at_version < root.journal.back().at_version)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS root shard: journal at_version {} after {} - the journal must be non-decreasing",
                    rec.at_version, root.journal.back().at_version);

            root.journal.push_back(std::move(rec));
        }

        return root;
    });
}

}
