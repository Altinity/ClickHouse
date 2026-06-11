#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
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

constexpr uint64_t GC_STATE_VERSION = 1;
constexpr uint64_t RETIRED_SET_VERSION = 1;

/// `ObjectKind` <-> string. Unknown string on decode is corruption (fail closed).
std::string_view objectKindToString(ObjectKind kind)
{
    switch (kind)
    {
        case ObjectKind::Blob: return "blob";
        case ObjectKind::Tree: return "tree";
        case ObjectKind::Pack: return "pack";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid object kind {}", static_cast<int>(kind));
}

ObjectKind objectKindFromString(std::string_view s, std::string_view what)
{
    if (s == "blob")
        return ObjectKind::Blob;
    if (s == "tree")
        return ObjectKind::Tree;
    if (s == "pack")
        return ObjectKind::Pack;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid object kind '{}'", what, s);
}

/// `TokenType` <-> string. Unknown string on decode is corruption (fail closed).
std::string_view tokenTypeToString(TokenType type)
{
    switch (type)
    {
        case TokenType::ETag: return "etag";
        case TokenType::Generation: return "generation";
        case TokenType::Emulated: return "emulated";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid token type {}", static_cast<int>(type));
}

TokenType tokenTypeFromString(std::string_view s, std::string_view what)
{
    if (s == "etag")
        return TokenType::ETag;
    if (s == "generation")
        return TokenType::Generation;
    if (s == "emulated")
        return TokenType::Emulated;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid token type '{}'", what, s);
}

}

String encodeGcState(const GcState & state)
{
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
    writeChar('}', out);
    return std::move(out.str());
}

GcState decodeGcState(std::string_view data)
{
    return decodeJsonGuarded("gc/state", [&]
    {
        auto obj = parseJsonDocument(data, "cas_gc_state", GC_STATE_VERSION, "gc/state");
        checkNoUnknownKeys(*obj, {"format", "version", "round", "fence_seq"}, "gc/state");

        GcState state;
        state.round = requireU64(*obj, "round", "gc/state");
        state.fence_seq = requireU64(*obj, "fence_seq", "gc/state");
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

}
