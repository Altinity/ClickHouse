#pragma once
#include <IO/ReadBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

/// The CAS on-disk codecs use the standard IO helpers — `writeBinaryLittleEndian` /
/// `readBinaryLittleEndian` over `WriteBuffer` / `ReadBuffer` — for every field. The on-disk format
/// is byte-exact (it IS pool format v2): every multi-byte field is little-endian; `UInt128` is its
/// 16 little-endian bytes (low 64 bits first) — exactly what the helpers produce on any host
/// (`transformEndianness` handles wide integers). String lengths ride separately as explicit
/// fixed-width LE fields, so the codecs do not use the varint-prefixed `writeStringBinary` family.

/// Read exactly `n` raw bytes. The bounds check MUST precede the allocation: `n` typically comes
/// from a length field just read off the wire, so on corrupted input it can be huge (a u32 field
/// admits 4 GiB) — allocating first would mean a multi-GiB transient allocation, which under a
/// memory tracker surfaces as MEMORY_LIMIT_EXCEEDED instead of the pinned CORRUPTED_DATA.
/// Comparing against `available` as the exact remainder is valid because all CAS codec decoding
/// reads from `ReadBufferFromMemory`: the whole object is in memory, so `available` is exactly
/// the number of bytes left.
inline String readFixedBytes(ReadBuffer & in, size_t n)
{
    if (n > in.available())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS codec: truncated encoded data: need {} bytes, {} available", n, in.available());
    String s(n, '\0');
    in.readStrict(s.data(), n);
    return s;
}

/// The shared object header: char[4] magic, u8 version, u8[3] reserved=0.
constexpr size_t CODEC_RESERVED_BYTES = 3;

inline void writeHeader(WriteBuffer & out, std::string_view magic, uint8_t version)
{
    writeString(magic, out);
    writeBinaryLittleEndian(version, out);
    for (size_t i = 0; i < CODEC_RESERVED_BYTES; ++i)
        writeBinaryLittleEndian(static_cast<uint8_t>(0), out);
}

/// Reads and validates magic + version + reserved. Future version => NOT_IMPLEMENTED (checked
/// before the reserved bytes — a v2 header may repurpose them); everything else => CORRUPTED_DATA.
inline void readHeader(ReadBuffer & in, std::string_view magic, uint8_t current_version, std::string_view what)
{
    if (readFixedBytes(in, magic.size()) != magic)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: bad magic", what);

    uint8_t version = 0;
    readBinaryLittleEndian(version, in);
    if (version > current_version)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "CAS {}: unsupported version {}", what, version);
    if (version != current_version)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid version {}", what, version);

    for (size_t i = 0; i < CODEC_RESERVED_BYTES; ++i)
    {
        uint8_t reserved = 0;
        readBinaryLittleEndian(reserved, in);
        if (reserved != 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: nonzero reserved byte", what);
    }
}

inline void requireNoTrailingBytes(ReadBuffer & in, std::string_view what)
{
    if (!in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: {} trailing bytes after the end of the encoded data", what, in.available());
}

/// Decode-boundary guard. The codecs parse fully materialized objects, so running out of bytes is
/// data corruption, not an IO condition: translate the standard reading errors into CORRUPTED_DATA —
/// the code the protocol layers and tests pin for truncated persisted objects.
template <typename F>
auto decodeGuarded(std::string_view what, F && f)
{
    try
    {
        return f();
    }
    catch (const Exception & e)
    {
        /// This translation is only safe because the guarded lambdas read exclusively from in-memory
        /// buffers — no real IO inside, so these codes cannot mean a transient read failure.
        if (e.code() == ErrorCodes::CANNOT_READ_ALL_DATA || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: truncated encoded data ({})", what, e.message());
        throw;
    }
}

}
