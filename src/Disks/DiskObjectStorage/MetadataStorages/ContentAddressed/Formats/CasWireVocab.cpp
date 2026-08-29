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

void writeManifestRefFields(CasJsonWriter & out, bool & first, const ManifestRefWireKeys & keys, const ManifestRef & r)
{
    writeKey(out, keys.epoch, first);
    out.u64StringValue(r.writer_epoch);
    writeKey(out, keys.build, first);
    out.u64StringValue(r.build_sequence);
    writeKey(out, keys.ord, first);
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

ManifestRef ManifestRefFields::buildRef(std::string_view what, std::string_view context) const
{
    if (!epoch || !build || !ord)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: {} manifest_ref missing epoch/build/ord", what, context);
    return manifestRefFromFields(*epoch, *build, *ord, what, context);
}

BlobRef BlobRefFields::build(std::string_view what) const
{
    if (!algo_word || !digest_hex)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: blob ref missing ha/h", what);
    const BlobHashAlgo algo = blobHashAlgoFromWord(*algo_word, what);
    /// Validate the digest width before calling `fromHex`. A width mismatch otherwise produces
    /// `BAD_ARGUMENTS` instead of the `CORRUPTED_DATA` required for malformed serialized input,
    /// allowing an invalid record to escape the decoder's fail-closed error contract.
    const uint64_t expected_hex_len = blobHashLenFor(algo) * 2;
    if (digest_hex->size() != expected_hex_len)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: digest hex width {} does not match algo width {}", what, digest_hex->size(), expected_hex_len);
    return BlobRef{algo, codecFor(algo).fromHex(*digest_hex)};
}

}
