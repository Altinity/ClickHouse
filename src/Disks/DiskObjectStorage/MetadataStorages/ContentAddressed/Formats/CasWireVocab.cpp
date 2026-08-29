#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

static_assert(casEnumTableCoversEnum<kTokenTypeWords, TokenType>());
static_assert(casEnumTableCoversEnum<kObjectKindWords, ObjectKind>());

std::string_view tokenTypeToWord(TokenType t)
{
    return kTokenTypeWords.toWord(t, "CAS wire: TokenType");
}

TokenType tokenTypeFromWord(std::string_view w, std::string_view what)
{
    return kTokenTypeWords.fromWord(w, what);
}

BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what)
{
    return kBlobHashAlgoWords.fromWord(w, what);
}

std::string_view objectKindToWord(ObjectKind k)
{
    return kObjectKindWords.toWord(k, "CAS wire: ObjectKind");
}

ObjectKind objectKindFromWord(std::string_view w, std::string_view what)
{
    return kObjectKindWords.fromWord(w, what);
}

void writeTokenFields(CasJsonWriter & out, bool & first, const Token & t)
{
    writeKey(out, "tt", first);
    writeStringValue(out, tokenTypeToWord(t.type));
    writeKey(out, "tv", first);
    writeStringValue(out, t.value);
}

void writeBlobRefFields(CasJsonWriter & out, bool & first, const BlobRef & r)
{
    writeKey(out, "ha", first);
    writeStringValue(out, blobHashAlgoName(r.algo));
    writeKey(out, "h", first);
    writeStringValue(out, codecFor(r.algo).toHex(r.digest));
}

void writeManifestRefFields(CasJsonWriter & out, bool & first, std::string_view prefix, const ManifestRef & r)
{
    /// Unlike the WriteBuffer overload, the two-part key() form appends the prefix and name back
    /// to back with no composed String(prefix) + "..." temporary.
    out.key(prefix, "me", first);
    out.u64StringValue(r.writer_epoch);
    out.key(prefix, "mb", first);
    out.u64StringValue(r.build_sequence);
    out.key(prefix, "mo", first);
    out.u64Number(r.manifest_ordinal);
}

ManifestRef manifestRefFromFields(uint64_t writer_epoch, uint64_t build_sequence, uint64_t manifest_ordinal,
                                  std::string_view caller, std::string_view what)
{
    /// Check the upper bound before narrowing the caller-supplied value to the in-memory ordinal
    /// type. `checkManifestRef` then applies the shared nonzero and lower-bound checks.
    if (manifest_ordinal > kMaxManifestOrdinal)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: {} manifest_ordinal {} out of range", caller, what, manifest_ordinal);
    ManifestRef r;
    r.writer_epoch = writer_epoch;
    r.build_sequence = build_sequence;
    r.manifest_ordinal = static_cast<uint32_t>(manifest_ordinal);
    checkManifestRef(r, caller, what);
    return r;
}

}
