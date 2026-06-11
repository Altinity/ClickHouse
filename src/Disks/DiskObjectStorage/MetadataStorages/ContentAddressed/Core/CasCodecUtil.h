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

/// Read exactly `n` raw bytes.
inline String readFixedBytes(ReadBuffer & in, size_t n)
{
    String s(n, '\0');
    in.readStrict(s.data(), n);
    return s;
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
        if (e.code() == ErrorCodes::CANNOT_READ_ALL_DATA || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: truncated encoded data", what);
        throw;
    }
}

}
