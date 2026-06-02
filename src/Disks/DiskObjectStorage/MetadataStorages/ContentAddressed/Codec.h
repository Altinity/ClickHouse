#pragma once
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <array>
#include <cstdint>
#include <string>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::ContentAddressed
{

/// The ONE shared on-object codec for every persisted content-addressed format (the part manifest,
/// the ref payload, the per-ref sidecar, the pool ownership marker). These objects are
/// content-addressed (their bytes are hashed) and may be written on one architecture and read on
/// another (CI runs amd64 and arm64), so the format must be:
///   - explicitly LITTLE-ENDIAN (never host byte order), so identical logical content yields
///     byte-identical objects regardless of the writer's architecture (cross-arch determinism);
///   - VERSIONED with a fail-closed reader, so a future additive change bumps the version and an
///     older reader refuses to misinterpret a newer object rather than silently corrupting data;
///   - EXACTLY parsed (length-prefixed strings, fixed-width integers), never "fuzzy" / best-effort.
///
/// Every format begins with a `FormatHeader` = a 4-byte ASCII magic + a 1-byte version. The reader
/// rejects an unrecognised magic (`CORRUPTED_DATA`) and a version newer than it understands
/// (`NOT_IMPLEMENTED`, fail-closed) BEFORE reading the body. Integers in the body use
/// `writeBinaryLittleEndian`/`readBinaryLittleEndian` (fixed width) or `writeVarUInt`/`readVarUInt`
/// (varint); strings use `writeStringBinary`/`readStringBinary` (a varint length prefix + the raw
/// bytes), which tolerate embedded NULs and reject a forged over-long length.

/// A 4-byte ASCII magic identifying a format family. Constructed from a 4-character string literal.
using FormatMagic = std::array<char, 4>;

constexpr FormatMagic makeMagic(const char (&literal)[5])
{
    return FormatMagic{literal[0], literal[1], literal[2], literal[3]};
}

/// `MAGIC(4) + version(1)`: the fixed prefix of every content-addressed object. The version is a
/// single byte (formats evolve by small additive steps; 256 versions is ample and keeps the header
/// minimal). Writing emits exactly 5 bytes; reading validates both fields fail-closed.
struct FormatHeader
{
    FormatMagic magic{};
    uint8_t version = 0;

    void write(WriteBuffer & out) const
    {
        out.write(magic.data(), magic.size());
        writeBinaryLittleEndian(version, out);
    }

    /// Read and VALIDATE the header: the magic must equal `expected_magic` and the version must be in
    /// [1, max_supported_version]. A wrong magic is `CORRUPTED_DATA` (a stray / foreign object); a
    /// version above what this build understands is `NOT_IMPLEMENTED` (fail closed — never reinterpret
    /// a newer object with an older parser). Returns the validated version so the body parser can
    /// branch on it for additive fields.
    static uint8_t readAndValidate(
        ReadBuffer & in, const FormatMagic & expected_magic, uint8_t max_supported_version, std::string_view what)
    {
        FormatMagic got{};
        in.readStrict(got.data(), got.size());
        if (got != expected_magic)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed {}: bad magic", what);

        uint8_t version = 0;
        readBinaryLittleEndian(version, in);
        if (version == 0 || version > max_supported_version)
            throw Exception(
                ErrorCodes::NOT_IMPLEMENTED,
                "ContentAddressed {}: unsupported format version {} (this build understands up to {})",
                what, static_cast<uint32_t>(version), static_cast<uint32_t>(max_supported_version));
        return version;
    }
};

}
