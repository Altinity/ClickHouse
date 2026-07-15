#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobEnvelopeFormat.h>
#include <IO/WriteBuffer.h>
#include <string_view>

namespace DB::Cas
{

/// Shared JSON rendering of the value sub-types every text codec embeds (spec §object-dispositions:
/// "subformat wire shapes become JSON sub-objects of their parents under the same key-naming
/// policy"). Enum values render as full WORDS (spec universal convention); the reverse maps are
/// fail-closed. Consumed by cas_gc_outcomes here and by refsnaplog / runs / blob envelope later.

std::string_view tokenTypeToWord(TokenType t);
TokenType tokenTypeFromWord(std::string_view w, std::string_view what);

/// Write side reuses blobHashAlgoName (CasBlobHasher.h) directly; this is the fail-closed inverse.
BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what);

std::string_view objectKindToWord(ObjectKind k);
ObjectKind objectKindFromWord(std::string_view w, std::string_view what);

/// Append `,"tt":"<type-word>","tv":"<value>"` to an in-progress object (the caller owns `first`).
void writeTokenFields(WriteBuffer & out, bool & first, const Token & t);
/// Append `,"ha":"<algo-word>","h":"<hex-at-algo-width>"`.
void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r);

}
