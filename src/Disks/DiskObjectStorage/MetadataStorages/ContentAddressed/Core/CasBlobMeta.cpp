#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace DB::Cas
{

namespace
{
constexpr std::string_view MAGIC = "CAMT";
constexpr size_t BODY_LEN = 4 + 1 + 1 + 8 + 8;   /// magic + version + state + condemn_round + size

void putU64LE(String & out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint64_t getU64LE(std::string_view b, size_t off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(b[off + i])) << (8 * i);
    return v;
}
}

String encodeBlobMeta(const BlobMeta & meta)
{
    String out;
    out.reserve(BODY_LEN);
    out.append(MAGIC);
    out.push_back(static_cast<char>(meta.version));
    out.push_back(static_cast<char>(static_cast<uint8_t>(meta.state)));
    putU64LE(out, meta.condemn_round);
    putU64LE(out, meta.size);
    return out;
}

BlobMeta decodeBlobMeta(std::string_view bytes)
{
    if (bytes.size() != BODY_LEN || bytes.substr(0, 4) != MAGIC)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: bad magic or length ({} bytes)", bytes.size());
    BlobMeta m;
    m.version = static_cast<uint8_t>(bytes[4]);
    if (m.version != 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: unknown version {}", m.version);
    const uint8_t s = static_cast<uint8_t>(bytes[5]);
    if (s > static_cast<uint8_t>(MetaState::Condemned))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: bad state {}", s);
    m.state = static_cast<MetaState>(s);
    m.condemn_round = getU64LE(bytes, 6);
    m.size = getU64LE(bytes, 14);
    return m;
}

std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const UInt128 & hash)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    auto got = backend.get(key);
    if (!got)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(got->bytes), .etag = got->token};
}

CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const UInt128 & hash, const BlobMeta & meta)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.casPut(key, encodeBlobMeta(meta), std::nullopt);
}

CasResult casMeta(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected, const BlobMeta & meta)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.casPut(key, encodeBlobMeta(meta), expected);
}

DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.deleteExact(key, expected);
}

}
