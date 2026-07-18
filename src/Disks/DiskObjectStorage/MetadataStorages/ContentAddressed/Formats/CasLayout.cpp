#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/Exception.h>

namespace DB::Cas
{

/// Blob-key operations are kept together with their parsing inverse. `CasTypes.h` supplies the
/// complete `BlobRef` type and the hash-algorithm helpers used to render and validate the path.
String Layout::blobKey(const BlobRef & ref) const
{
    return shardedKey("blobs/" + String(blobHashAlgoName(ref.algo)), blobHexOf(ref));
}

String Layout::blobMetaKey(const BlobRef & ref) const
{
    return blobKey(ref) + ".meta";
}

std::optional<BlobRef> Layout::parseBlobKey(std::string_view key) const
{
    std::string_view rest = key;
    static constexpr std::string_view kMetaSuffix = ".meta";
    if (rest.size() >= kMetaSuffix.size() && rest.substr(rest.size() - kMetaSuffix.size()) == kMetaSuffix)
        rest.remove_suffix(kMetaSuffix.size());

    const String blobs_root = blobsPrefix();   /// "<prefix>/blobs/"
    if (rest.size() <= blobs_root.size() || !rest.starts_with(blobs_root))
        return std::nullopt;
    rest.remove_prefix(blobs_root.size());

    const size_t algo_sep = rest.find('/');
    if (algo_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view algo_name = rest.substr(0, algo_sep);
    rest.remove_prefix(algo_sep + 1);

    const size_t shard_sep = rest.find('/');
    if (shard_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view shard = rest.substr(0, shard_sep);
    const std::string_view hex = rest.substr(shard_sep + 1);
    if (shard.size() != 2 || hex.size() < 2 || shard != hex.substr(0, 2))
        return std::nullopt;   /// malformed shard/hex shape -- not ours

    /// `<algoName>` -> `BlobHashAlgo`: the small enum-value set makes a linear scan against
    /// `blobHashAlgoName` (the ONE name authority) cheaper and safer than a second name table that
    /// could drift from it.
    std::optional<BlobHashAlgo> algo;
    for (BlobHashAlgo candidate : {BlobHashAlgo::CityHash128, BlobHashAlgo::XXH3_128, BlobHashAlgo::Sha256})
        if (algo_name == blobHashAlgoName(candidate))
        {
            algo = candidate;
            break;
        }
    if (!algo)
        return std::nullopt;   /// unknown/foreign algo segment -- debris, not ours

    if (hex.size() != 2 * blobHashLenFor(*algo))
        return std::nullopt;   /// a KNOWN algo but the wrong-width hex -- not ours either

    try
    {
        return BlobRef{*algo, codecFor(*algo).fromHex(String(hex))};
    }
    catch (const Exception &)
    {
        return std::nullopt;   /// non-hex characters -- malformed, not ours
    }
}

}
