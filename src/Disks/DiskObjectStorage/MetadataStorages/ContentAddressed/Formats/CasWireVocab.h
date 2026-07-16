#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
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

/// Append `,"<p>me":"<writer_epoch>","<p>mb":"<build_sequence>","<p>mo":<manifest_ordinal>` for a
/// ManifestRef under an optional key prefix `p` ("" for a bare row, "o"/"n" for the old/new binding of
/// an OwnerTransition op, etc.). writer_epoch / build_sequence are unbounded u64 -> decimal strings;
/// manifest_ordinal is bounded [1, kMaxManifestOrdinal] -> a JSON number. Promoted here from
/// `CasRefWireVocab` (codecs-v3 phase 6, FLAG D): shared verbatim by the refsnaplog codecs AND
/// `CasPartManifestFormat`'s descriptor line.
void writeManifestRefFields(WriteBuffer & out, bool & first, std::string_view prefix, const ManifestRef & r);

/// Build a ManifestRef from the three decoded field values and validate it (`checkManifestRef` range
/// rules — writer_epoch/build_sequence nonzero, manifest_ordinal in [1, kMaxManifestOrdinal]).
/// `caller`/`what` name the codec + field in a CORRUPTED_DATA message.
ManifestRef manifestRefFromFields(uint64_t writer_epoch, uint64_t build_sequence, uint64_t manifest_ordinal,
                                  std::string_view caller, std::string_view what);

}
