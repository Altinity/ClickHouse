#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <limits>

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

constexpr uint8_t GC_STATE_VERSION = 1;
constexpr uint8_t RETIRED_SET_VERSION = 1;
constexpr size_t RESERVED_BYTES = 3;

void writeHeader(WriteBuffer & out, std::string_view magic, uint8_t version)
{
    writeString(magic, out);
    writeBinaryLittleEndian(version, out);
    for (size_t i = 0; i < RESERVED_BYTES; ++i)
        writeBinaryLittleEndian(static_cast<uint8_t>(0), out);
}

/// Reads and validates magic + version + reserved. Future version => NOT_IMPLEMENTED (checked
/// before the reserved bytes — a v2 header may repurpose them); everything else => CORRUPTED_DATA.
void readHeader(ReadBuffer & in, std::string_view magic, uint8_t current_version, std::string_view what)
{
    if (readFixedBytes(in, magic.size()) != magic)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: bad magic", what);

    uint8_t version = 0;
    readBinaryLittleEndian(version, in);
    if (version > current_version)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS {}: unsupported version {}", what, version);
    if (version != current_version)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid version {}", what, version);

    for (size_t i = 0; i < RESERVED_BYTES; ++i)
    {
        uint8_t reserved = 0;
        readBinaryLittleEndian(reserved, in);
        if (reserved != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS {}: nonzero reserved byte", what);
    }
}

void requireNoTrailingBytes(ReadBuffer & in, std::string_view what)
{
    if (!in.eof())
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CAS {}: {} trailing bytes after the end of the encoded data", what, in.available());
}

}

String encodeGcState(const GcState & state)
{
    WriteBufferFromOwnString out;
    writeHeader(out, "CAGS", GC_STATE_VERSION);
    writeBinaryLittleEndian(state.round, out);
    writeBinaryLittleEndian(state.fence_seq, out);
    return std::move(out.str());
}

GcState decodeGcState(std::string_view data)
{
    return decodeGuarded("gc/state", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());
        readHeader(in, "CAGS", GC_STATE_VERSION, "gc/state");

        GcState state;
        readBinaryLittleEndian(state.round, in);
        readBinaryLittleEndian(state.fence_seq, in);

        requireNoTrailingBytes(in, "gc/state");
        return state;
    });
}

String encodeRetiredSet(const RetiredSet & set)
{
    WriteBufferFromOwnString out;
    writeHeader(out, "CART", RETIRED_SET_VERSION);
    writeBinaryLittleEndian(static_cast<uint64_t>(set.entries.size()), out);
    for (const auto & entry : set.entries)
    {
        writeBinaryLittleEndian(static_cast<uint8_t>(entry.kind), out);
        writeBinaryLittleEndian(entry.hash, out);
        writeBinaryLittleEndian(static_cast<uint8_t>(entry.token.type), out);
        /// token_len is u16 on disk; a longer token would silently truncate the length while the
        /// full value is appended below — writer-side silent corruption. Guard before writing.
        if (entry.token.value.size() > std::numeric_limits<uint16_t>::max())
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
                "CAS retired set: token length {} exceeds the u16 on-disk limit", entry.token.value.size());
        writeBinaryLittleEndian(static_cast<uint16_t>(entry.token.value.size()), out);
        writeString(entry.token.value, out);
        writeBinaryLittleEndian(entry.size, out);
    }
    return std::move(out.str());
}

RetiredSet decodeRetiredSet(std::string_view data)
{
    return decodeGuarded("retired set", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());
        readHeader(in, "CART", RETIRED_SET_VERSION, "retired set");

        RetiredSet set;
        uint64_t entry_count = 0;
        readBinaryLittleEndian(entry_count, in);
        for (uint64_t i = 0; i < entry_count; ++i)
        {
            RetiredEntry entry;

            uint8_t kind = 0;
            readBinaryLittleEndian(kind, in);
            if (kind < static_cast<uint8_t>(ObjectKind::Blob) || kind > static_cast<uint8_t>(ObjectKind::Pack))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid object kind {}", kind);
            entry.kind = static_cast<ObjectKind>(kind);

            readBinaryLittleEndian(entry.hash, in);

            uint8_t token_type = 0;
            readBinaryLittleEndian(token_type, in);
            if (token_type < static_cast<uint8_t>(TokenType::ETag) || token_type > static_cast<uint8_t>(TokenType::Emulated))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS retired set: invalid token type {}", token_type);
            entry.token.type = static_cast<TokenType>(token_type);

            uint16_t token_len = 0;
            readBinaryLittleEndian(token_len, in);
            entry.token.value = readFixedBytes(in, token_len);

            readBinaryLittleEndian(entry.size, in);

            set.entries.push_back(std::move(entry));
        }

        requireNoTrailingBytes(in, "retired set");
        return set;
    });
}

}
