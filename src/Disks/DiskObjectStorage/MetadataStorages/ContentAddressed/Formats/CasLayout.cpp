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

std::optional<ParsedRefObjectKey> Layout::parseRefObjectKey(std::string_view key) const
{
    const String base = casRefsPrefix();
    if (!key.starts_with(base))
        return std::nullopt;
    std::string_view rest = key;
    rest.remove_prefix(base.size());

    const size_t id_sep = rest.rfind('/');
    if (id_sep == std::string_view::npos)
        return std::nullopt;
    std::string_view id_part = rest.substr(id_sep + 1);
    std::string_view before_id = rest.substr(0, id_sep);

    const size_t kind_sep = before_id.rfind('/');
    if (kind_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view kind_seg = before_id.substr(kind_sep + 1);
    const std::string_view ns_part = before_id.substr(0, kind_sep);
    if (ns_part.empty())
        return std::nullopt;

    RefObjectKind kind{};
    if (kind_seg == "_cleanup")
        kind = RefObjectKind::Cleanup;
    else if (kind_seg == "_log")
        kind = RefObjectKind::Log;
    else if (kind_seg == "_snap")
        kind = RefObjectKind::Snap;
    else
        return std::nullopt;

    std::string_view render = id_part;
    if (kind == RefObjectKind::Snap || kind == RefObjectKind::Log)
    {
        /// `_log` and `_snap` are always-compressed text stored under a `.zst` suffix. `_cleanup`
        /// stays a bare zero-byte marker and is intentionally not part of that compression family.
        constexpr std::string_view kZstSuffix = ".zst";
        if (!render.ends_with(kZstSuffix))
            return std::nullopt;
        render.remove_suffix(kZstSuffix.size());
    }
    else if (render.find('.') != std::string_view::npos)
    {
        return std::nullopt;   /// `_cleanup` ids never carry an extension
    }

    const auto txn_id = parseRefTxnId(render);
    if (!txn_id)
        return std::nullopt;

    return ParsedRefObjectKey{RootNamespace{String(ns_part)}, kind, *txn_id};
}

std::optional<ManifestId> Layout::parseManifestKey(std::string_view key) const
{
    const String base = casManifestsPrefix();
    if (!key.starts_with(base))
        return std::nullopt;
    std::string_view rest = key;
    rest.remove_prefix(base.size());

    const size_t file_sep = rest.rfind('/');
    if (file_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view file = rest.substr(file_sep + 1);
    const std::string_view before_file = rest.substr(0, file_sep);

    const size_t build_sep = before_file.rfind('/');
    if (build_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view build_seg = before_file.substr(build_sep + 1);
    const std::string_view ns_part = before_file.substr(0, build_sep);
    if (ns_part.empty())
        return std::nullopt;

    const auto build = parseRefTxnId(build_seg);
    if (!build)
        return std::nullopt;

    const std::string_view kManifestSuffix = storedSuffix(FormatId::PartManifest);
    constexpr size_t kOrdinalDigits = 6;
    if (file.size() != kOrdinalDigits + kManifestSuffix.size() || !file.ends_with(kManifestSuffix))
        return std::nullopt;
    const std::string_view ordinal_str = file.substr(0, kOrdinalDigits);
    uint32_t ordinal = 0;
    for (char c : ordinal_str)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        ordinal = ordinal * 10 + static_cast<uint32_t>(c - '0');
    }
    if (ordinal == 0 || ordinal > kMaxManifestOrdinal)
        return std::nullopt;

    ManifestId parsed;
    parsed.root_namespace = RootNamespace{String(ns_part)};
    parsed.ref.writer_epoch = build->writer_epoch;
    parsed.ref.build_sequence = build->ref_sequence;
    parsed.ref.manifest_ordinal = ordinal;
    return parsed;
}

/// A namespace must be non-empty, with no leading/trailing '/', no empty segment ("//"), and no
/// segment equal to the reserved "_files".
void Layout::checkNamespace(const RootNamespace & ns) const
{
    const String & s = ns.string();
    if (s.empty())
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "CasLayout: namespace must be non-empty");

    size_t start = 0;
    while (true)
    {
        size_t end = s.find('/', start);
        const String segment = s.substr(start, end == String::npos ? String::npos : end - start);
        if (segment.empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: namespace '{}' has an empty segment (leading/trailing or doubled '/')", s);
        if (segment == "_files")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: namespace '{}' uses the reserved segment '_files'", s);
        if (segment == "_manifests")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: namespace '{}' uses the reserved segment '_manifests'", s);
        if (end == String::npos)
            break;
        start = end + 1;
    }
}

}
