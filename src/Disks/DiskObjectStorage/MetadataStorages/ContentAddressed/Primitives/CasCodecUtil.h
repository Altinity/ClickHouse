#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <limits>
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

/// Binary codec helpers shared by the remaining custom-binary codecs (the refsnaplog log/snapshot
/// objects and `CasPool`'s mutable-files payload). The control-plane objects moved to the v3 text
/// codecs in `Core/Formats/` (codecs-v3 phase 2); these named wrappers survive for the length-prefixed
/// binary bodies and exist so a 128-bit serialization can never be mis-paired with the wrong byte order.

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

/// BE 16-byte form — used by protobuf `bytes` fields to encode UInt128 values in big-endian order.
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
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: big-endian UInt128 field must be 16 bytes, got {}", what, b.size());
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

/// Length-prefixed byte string, shared by every codec that embeds one (`CasRefLogCodec`,
/// `CasRefSnapshotCodec`): `u32 length (LE) | bytes`.
/// The explicit guard (rather than relying on an op/byte budget elsewhere to keep every string short)
/// is the point where a length silently truncated by the `u32` cast would otherwise corrupt the wire.
inline void writeLenPrefixed(WriteBuffer & out, const String & s)
{
    if (s.size() > std::numeric_limits<uint32_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS codec: string field of {} bytes exceeds UInt32 length prefix", s.size());
    writeBinaryLittleEndian(static_cast<uint32_t>(s.size()), out);
    out.write(s.data(), s.size());
}

inline String readLenPrefixed(ReadBuffer & in)
{
    uint32_t len = 0;
    readBinaryLittleEndian(len, in);
    return readFixedBytes(in, len);
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

/// Canonical clean relative path, shared by every codec that embeds a ref/file name in its body
/// (`CasRefLogCodec`, `CasRefSnapshotCodec`): non-empty, no NUL byte, no backslash, and no segment
/// that is empty (rejects a leading/trailing/doubled '/'), ".", or "..". Names in this family
/// originate from part names -- a NUL byte is never legitimate there, so it fails closed rather than
/// being silently truncated or passed through.
inline bool isCanonicalRefName(std::string_view name)
{
    if (name.empty() || name.find('\0') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        return false;
    size_t start = 0;
    while (true)
    {
        const size_t end = name.find('/', start);
        const std::string_view segment
            = name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

/// Throws CORRUPTED_DATA naming both `caller` (the codec, e.g. "RefLogTxn") and `what` (the field,
/// e.g. "set_payload ref_name") when `name` fails `isCanonicalRefName`.
inline void checkCanonicalRefName(std::string_view name, std::string_view caller, std::string_view what)
{
    if (!isCanonicalRefName(name))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "{}: {} is not a canonical clean relative path: '{}'", caller, what, name);
}

/// `ManifestRef` field validity, shared by every codec that embeds one (`CasRefLogCodec`,
/// `CasRefSnapshotCodec`): `writer_epoch`/`build_sequence` nonzero, `manifest_ordinal` in
/// `[1, kMaxManifestOrdinal]` -- the same range `manifestOrdinalFileName` (`CasManifestId.h`) enforces
/// at key-construction time. Throws CORRUPTED_DATA naming both `caller` (the codec) and `what` (the
/// field, e.g. "set_payload manifest_ref").
inline void checkManifestRef(const ManifestRef & ref, std::string_view caller, std::string_view what)
{
    if (ref.writer_epoch == 0 || ref.build_sequence == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "{}: {} manifest_ref writer_epoch/build_sequence must both be nonzero, got {}-{}",
            caller, what, ref.writer_epoch, ref.build_sequence);
    if (ref.manifest_ordinal == 0 || ref.manifest_ordinal > kMaxManifestOrdinal)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "{}: {} manifest_ref manifest_ordinal {} out of range", caller, what, ref.manifest_ordinal);
}

}
