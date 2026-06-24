#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Common/Exception.h>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

/// Shared enum <-> string mappings for the strict-JSON GC codecs (retired sets, gc/snap,
/// gc/outcomes). One mapping for all consumers: the `ObjectKind` mapping used to live as
/// file-local copies in `CasGcFormats.cpp` and `CasGcSnap.cpp`, and the `TokenType` mapping
/// lived solely in `CasGcFormats.cpp` until the outcomes codec became its second consumer —
/// at three codecs, divergence became a real hazard, so the mappings now live here. Unknown
/// string on decode is corruption (fail closed); an out-of-range enum value at the write site
/// is likewise reported as corruption.

inline std::string_view objectKindToString(ObjectKind kind)
{
    switch (kind)
    {
        case ObjectKind::Blob: return "blob";
        case ObjectKind::Tree: return "tree";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS codec: invalid object kind {}", static_cast<int>(kind));
}

inline ObjectKind objectKindFromString(std::string_view s, std::string_view what)
{
    if (s == "blob")
        return ObjectKind::Blob;
    if (s == "tree")
        return ObjectKind::Tree;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid object kind '{}'", what, s);
}

inline std::string_view tokenTypeToString(TokenType type)
{
    switch (type)
    {
        case TokenType::ETag: return "etag";
        case TokenType::Generation: return "generation";
        case TokenType::Emulated: return "emulated";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS codec: invalid token type {}", static_cast<int>(type));
}

inline TokenType tokenTypeFromString(std::string_view s, std::string_view what)
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
