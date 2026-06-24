#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
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

/// Binary codec helpers (Plan 3c-tail: JSON family removed — all mutable objects are now protobuf;
/// see `CasFormat.h` + `cas_root_shard.proto`). Hashed/identity objects (envelope, tree payload,
/// blob payloads, gc/snap) use the standard IO helpers directly — these named wrappers exist so a
/// 128-bit serialization can never be mis-paired with the wrong byte order.

/// ---------------------------------------------------------------------------------------------
/// On-disk UInt128 wire forms. CAS serializes a 128-bit hash in two distinct non-hex byte orders,
/// and BOTH are FROZEN — changing the bytes breaks every object already written. These named, typed
/// helpers exist so a 128-bit (de)serialization can never be mis-paired with the wrong order: a site
/// asks for the order it means by name instead of open-coding it. (The lowercase-hex form lives in
/// `CasIds.h` as `u128ToHex` / `hexToU128` and is out of scope here.)
///
/// LE binary form — used by the hashed/identity codecs (envelope header fields, the canonical tree
/// payload, the gc snapshot body): exactly `writeBinaryLittleEndian` / `readBinaryLittleEndian`.
inline void writeU128LE(WriteBuffer & out, const UInt128 & v) { writeBinaryLittleEndian(v, out); }
inline UInt128 readU128LE(ReadBuffer & in) { UInt128 v; readBinaryLittleEndian(v, in); return v; }

/// BE 16-byte form — used by the root-shard manifest's protobuf `bytes` fields (`tree_id`,
/// `tree_hash`, `file_hash`). Body copied VERBATIM from the former hand-rolled
/// `u128ToBytes` / `u128FromBytes` in `CasRootShardCodec.cpp` so the bytes are unchanged.
inline std::string u128ToBytesBE(const UInt128 & v)
{
    std::string out(16, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(v >> (8 * (15 - i))));
    return out;
}

inline UInt128 u128FromBytesBE(const std::string & b, std::string_view what)
{
    if (b.size() != 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: tree_id must be 16 bytes, got {}", what, b.size());
    UInt128 v = 0;
    for (int i = 0; i < 16; ++i)
        v = (v << 8) | static_cast<UInt8>(b[i]);
    return v;
}

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
