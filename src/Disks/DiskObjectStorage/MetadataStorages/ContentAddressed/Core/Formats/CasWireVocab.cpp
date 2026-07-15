#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
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

std::string_view tokenTypeToWord(TokenType t)
{
    switch (t)
    {
        case TokenType::ETag:       return "etag";
        case TokenType::Generation: return "generation";
        case TokenType::Emulated:   return "emulated";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS wire: unknown TokenType {}", static_cast<int>(t));
}

TokenType tokenTypeFromWord(std::string_view w, std::string_view what)
{
    if (w == "etag")       return TokenType::ETag;
    if (w == "generation") return TokenType::Generation;
    if (w == "emulated")   return TokenType::Emulated;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown token type '{}'", what, w);
}

BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what)
{
    if (w == "ch128")  return BlobHashAlgo::CityHash128;
    if (w == "xxh3")   return BlobHashAlgo::XXH3_128;
    if (w == "sha256") return BlobHashAlgo::Sha256;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown blob hash algo '{}'", what, w);
}

std::string_view objectKindToWord(ObjectKind k)
{
    switch (k)
    {
        case ObjectKind::Blob: return "blob";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS wire: unknown ObjectKind {}", static_cast<int>(k));
}

ObjectKind objectKindFromWord(std::string_view w, std::string_view what)
{
    if (w == "blob") return ObjectKind::Blob;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown object kind '{}'", what, w);
}

void writeTokenFields(WriteBuffer & out, bool & first, const Token & t)
{
    writeKey(out, "tt", first);
    writeStringValue(out, tokenTypeToWord(t.type));
    writeKey(out, "tv", first);
    writeStringValue(out, t.value);
}

void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r)
{
    writeKey(out, "ha", first);
    writeStringValue(out, blobHashAlgoName(r.algo));
    writeKey(out, "h", first);
    writeStringValue(out, codecFor(r.algo).toHex(r.digest));
}

}
