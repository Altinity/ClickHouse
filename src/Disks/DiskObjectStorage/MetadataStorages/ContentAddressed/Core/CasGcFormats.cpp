#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint8_t GC_STATE_VERSION = 1;
constexpr uint8_t RETIRED_SET_VERSION = 1;
constexpr size_t RESERVED_BYTES = 3;

void writeHeader(String & out, std::string_view magic, uint8_t version)
{
    out += magic;
    writeU8(out, version);
    for (size_t i = 0; i < RESERVED_BYTES; ++i)
        writeU8(out, 0);
}

/// Reads and validates magic + version + reserved. Future version => NOT_IMPLEMENTED (checked
/// before the reserved bytes — a v2 header may repurpose them); everything else => CORRUPTED_DATA.
void readHeader(ByteReader & r, std::string_view magic, uint8_t current_version, std::string_view what)
{
    const String found = r.readBytes(magic.size());
    if (found != magic)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: bad magic", what);

    const uint8_t version = r.readU8();
    if (version > current_version)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS {}: unsupported version {}", what, version);
    if (version != current_version)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid version {}", what, version);

    for (size_t i = 0; i < RESERVED_BYTES; ++i)
        if (r.readU8() != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: nonzero reserved byte", what);
}

void requireNoTrailingBytes(const ByteReader & r, std::string_view what)
{
    if (!r.eof())
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CAS {}: {} trailing bytes after the end of the encoded data", what, r.remaining());
}

}

String encodeGcState(const GcState & state)
{
    String out;
    writeHeader(out, "CAGS", GC_STATE_VERSION);
    writeLE64(out, state.round);
    writeLE64(out, state.fence_seq);
    return out;
}

GcState decodeGcState(std::string_view data)
{
    ByteReader r(data);
    readHeader(r, "CAGS", GC_STATE_VERSION, "gc/state");

    GcState state;
    state.round = r.readLE64();
    state.fence_seq = r.readLE64();

    requireNoTrailingBytes(r, "gc/state");
    return state;
}

String encodeRetiredSet(const RetiredSet & set)
{
    String out;
    writeHeader(out, "CART", RETIRED_SET_VERSION);
    writeLE64(out, set.entries.size());
    for (const auto & entry : set.entries)
    {
        writeU8(out, static_cast<uint8_t>(entry.kind));
        writeU128LE(out, entry.hash);
        writeU8(out, static_cast<uint8_t>(entry.token.type));
        writeLE16(out, static_cast<uint16_t>(entry.token.value.size()));
        out += entry.token.value;
        writeLE64(out, entry.size);
    }
    return out;
}

RetiredSet decodeRetiredSet(std::string_view data)
{
    ByteReader r(data);
    readHeader(r, "CART", RETIRED_SET_VERSION, "retired set");

    RetiredSet set;
    const uint64_t entry_count = r.readLE64();
    for (uint64_t i = 0; i < entry_count; ++i)
    {
        RetiredEntry entry;

        const uint8_t kind = r.readU8();
        if (kind < static_cast<uint8_t>(ObjectKind::Blob) || kind > static_cast<uint8_t>(ObjectKind::Pack))
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid object kind {}", kind);
        entry.kind = static_cast<ObjectKind>(kind);

        entry.hash = r.readU128LE();

        const uint8_t token_type = r.readU8();
        if (token_type < static_cast<uint8_t>(TokenType::ETag) || token_type > static_cast<uint8_t>(TokenType::Emulated))
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid token type {}", token_type);
        entry.token.type = static_cast<TokenType>(token_type);

        const uint16_t token_len = r.readLE16();
        entry.token.value = r.readBytes(token_len);

        entry.size = r.readLE64();

        set.entries.push_back(std::move(entry));
    }

    requireNoTrailingBytes(r, "retired set");
    return set;
}

}
