#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnumStrings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <charconv>

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

constexpr uint64_t GC_STATE_VERSION = 3;   /// v3: folded_cursor moved from gc/state into the snap (B140-dangle fix)
constexpr uint64_t RETIRED_SET_VERSION = 1;

/// Writes a `{"key":u64,...}` object from a string-keyed map (used for the inner fence_version
/// objects). The keys are "ns/shard" strings — data, so they go through the escaping writer.
void writeU64MapObject(WriteBuffer & out, const std::map<String, uint64_t> & map)
{
    writeChar('{', out);
    bool first = true;
    for (const auto & [key, value] : map)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeJsonKey(out, key);
        writeIntText(value, out);
    }
    writeChar('}', out);
}

/// Reads a `{"key":u64,...}` object into a map; every value must be a strict u64 (the same
/// rejection set as requireU64).
std::map<String, uint64_t> u64MapFromObject(const Poco::JSON::Object & obj, std::string_view what)
{
    std::map<String, uint64_t> result;
    for (const auto & [key, value] : obj)
        result[key] = requireU64Var(value, key, what);
    return result;
}

}

String encodeGcState(const GcState & state)
{
    chassert(state.snap_shards >= 1);   /// catch a zeroed GC constant at the write site
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_gc_state", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(GC_STATE_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "round");
    writeIntText(state.round, out);
    writeChar(',', out);
    writeJsonKey(out, "fence_seq");
    writeIntText(state.fence_seq, out);
    writeChar(',', out);
    writeJsonKey(out, "snap_shards");
    writeIntText(state.snap_shards, out);
    writeChar(',', out);
    writeJsonKey(out, "snap_generation");
    writeIntText(state.snap_generation, out);
    writeChar(',', out);
    writeJsonKey(out, "lease");
    writeChar('{', out);
    writeJsonKey(out, "owner");
    writeJsonString(u128ToHex(state.lease.owner), out);
    writeChar(',', out);
    writeJsonKey(out, "seq");
    writeIntText(state.lease.seq, out);
    writeChar('}', out);
    writeChar(',', out);
    writeJsonKey(out, "fence_version");
    writeChar('{', out);
    bool first = true;
    for (const auto & [round, inner] : state.fence_version)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeJsonKey(out, std::to_string(round));
        writeU64MapObject(out, inner);
    }
    writeChar('}', out);
    writeChar('}', out);
    return std::move(out.str());
}

GcState decodeGcState(std::string_view data)
{
    return decodeJsonGuarded("gc/state", [&]
    {
        auto obj = parseJsonDocument(data, "cas_gc_state", GC_STATE_VERSION, "gc/state");
        checkNoUnknownKeys(*obj,
            {"format", "version", "round", "fence_seq", "snap_shards", "snap_generation",
             "lease", "fence_version"}, "gc/state");

        GcState state;
        state.round = requireU64(*obj, "round", "gc/state");
        state.fence_seq = requireU64(*obj, "fence_seq", "gc/state");
        state.snap_shards = requireU64(*obj, "snap_shards", "gc/state");
        if (state.snap_shards == 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: snap_shards must be >= 1");
        state.snap_generation = requireU64(*obj, "snap_generation", "gc/state");

        auto lease = requireObject(*obj, "lease", "gc/state");
        checkNoUnknownKeys(*lease, {"owner", "seq"}, "gc/state lease");
        state.lease.owner = requireHash(*lease, "owner", "gc/state lease");
        state.lease.seq = requireU64(*lease, "seq", "gc/state lease");

        auto fences = requireObject(*obj, "fence_version", "gc/state");
        for (const auto & [round_str, inner_var] : *fences)
        {
            uint64_t round = 0;
            const auto [end, ec] = std::from_chars(round_str.data(), round_str.data() + round_str.size(), round);
            if (ec != std::errc() || end != round_str.data() + round_str.size() || round_str.empty())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state: non-numeric fence_version round key '{}'", round_str);
            /// Canonical-form check: "07" parses to 7 but would re-encode as "7" — two keys aliasing
            /// one round inside a persisted object is corruption, not a tolerable spelling.
            if (std::to_string(round) != round_str)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state: non-canonical fence_version round key '{}'", round_str);
            if (inner_var.type() != typeid(Poco::JSON::Object::Ptr))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state: fence_version value for round '{}' must be an object", round_str);
            state.fence_version[round]
                = u64MapFromObject(*inner_var.extract<Poco::JSON::Object::Ptr>(), "gc/state fence_version");
        }
        return state;
    });
}

String encodeRetiredSet(const RetiredSet & set)
{
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_retired_set", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(RETIRED_SET_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "entries");
    writeChar('[', out);
    bool first = true;
    for (const auto & entry : set.entries)
    {
        if (!first)
            writeChar(',', out);
        first = false;

        writeChar('{', out);
        writeJsonKey(out, "kind");
        writeJsonString(objectKindToString(entry.kind), out);
        writeChar(',', out);
        writeJsonKey(out, "hash");
        writeJsonString(u128ToHex(entry.hash), out);
        writeChar(',', out);
        writeJsonKey(out, "token");
        writeJsonString(entry.token.value, out);
        writeChar(',', out);
        writeJsonKey(out, "token_type");
        writeJsonString(tokenTypeToString(entry.token.type), out);
        writeChar(',', out);
        writeJsonKey(out, "size");
        writeIntText(entry.size, out);
        writeChar('}', out);
    }
    writeChar(']', out);
    writeChar('}', out);
    return std::move(out.str());
}

RetiredSet decodeRetiredSet(std::string_view data)
{
    return decodeJsonGuarded("retired set", [&]
    {
        auto obj = parseJsonDocument(data, "cas_retired_set", RETIRED_SET_VERSION, "retired set");
        checkNoUnknownKeys(*obj, {"format", "version", "entries"}, "retired set");

        auto entries = requireArray(*obj, "entries", "retired set");

        RetiredSet set;
        for (size_t i = 0; i < entries->size(); ++i)
        {
            auto entry_obj = requireObjectAt(*entries, i, "retired set");
            checkNoUnknownKeys(*entry_obj, {"kind", "hash", "token", "token_type", "size"}, "retired set entry");

            RetiredEntry entry;
            entry.kind = objectKindFromString(requireString(*entry_obj, "kind", "retired set"), "retired set");
            entry.hash = requireHash(*entry_obj, "hash", "retired set");
            entry.token.value = requireString(*entry_obj, "token", "retired set");
            entry.token.type = tokenTypeFromString(requireString(*entry_obj, "token_type", "retired set"), "retired set");
            entry.size = requireU64(*entry_obj, "size", "retired set");
            set.entries.push_back(std::move(entry));
        }
        return set;
    });
}

String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    String out(24, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(hb.owner >> (8 * (15 - i))));
    for (int i = 0; i < 8; ++i)
        out[16 + i] = static_cast<char>(static_cast<UInt8>(hb.hb_seq >> (8 * (7 - i))));
    return out;
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    if (data.size() != 24)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: expected 24 bytes, got {}", data.size());
    GcHeartbeat hb;
    for (int i = 0; i < 16; ++i)
        hb.owner = (hb.owner << 8) | static_cast<UInt8>(data[i]);
    for (int i = 0; i < 8; ++i)
        hb.hb_seq = (hb.hb_seq << 8) | static_cast<UInt8>(static_cast<unsigned char>(data[16 + i]));
    return hb;
}

}
