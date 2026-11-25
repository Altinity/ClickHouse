#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/Exception.h>
#include <base/hex.h>
#include <algorithm>
#include <charconv>

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace DB::Cas
{

namespace
{

/// Parses a canonical unsigned-decimal path segment (the shape `std::to_string` produces): non-empty,
/// digits only, no leading zero unless the segment is exactly "0", and fits in a `uint64_t`. Returns
/// `std::nullopt` for anything else -- never throws, since key parsing classifies a foreign/malformed
/// segment as debris, not an error (mirrors `parseManifestKey`'s ordinal parse).
std::optional<uint64_t> parseCanonicalU64(std::string_view s)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0'))
        return std::nullopt;
    for (char c : s)
        if (c < '0' || c > '9')
            return std::nullopt;
    uint64_t v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return std::nullopt;   /// overflowed uint64_t or otherwise not fully consumed
    return v;
}

/// `<algoName>` -> `BlobHashAlgo` through the wire table itself, whose coverage is proven against
/// the enum at compile time. A hand-written candidate list here would be a second enumeration that
/// a new algorithm could silently outgrow: the parser would reject a segment the writer emits.
/// Key parsing answers "is this key ours?", so an unknown segment is `nullopt` -- debris, not
/// corruption -- which is why it scans rather than calling the throwing `fromWord`.
std::optional<BlobHashAlgo> algoFromPathSegment(std::string_view algo_name)
{
    for (const auto & entry : kBlobHashAlgoWords.entries)
        if (algo_name == entry.word)
            return entry.value;
    return std::nullopt;
}

/// The `<algo>/<shard>/<hex>` segments of a blob, marker or intent key. Accepts every spelling `fromHex`
/// accepts; a caller that needs the writer's own spelling checks it.
std::optional<BlobRef> parseBlobSegments(std::string_view algo_name, std::string_view shard, std::string_view hex)
{
    const std::optional<BlobHashAlgo> algo = algoFromPathSegment(algo_name);
    if (!algo || hex.size() != 2 * blobHashLenFor(*algo) || shard != hex.substr(0, 2))
        return std::nullopt;
    /// Syntax is checked here so that no exception is read as "not a blob key": a memory limit must propagate.
    if (std::any_of(hex.begin(), hex.end(), [](char c) { return unhex(c) == 0xff; }))
        return std::nullopt;
    return BlobRef{*algo, codecFor(*algo).fromHex(hex)};
}

}

/// Blob-key operations are kept together with their parsing inverse. `CasTypes.h` supplies the
/// complete `BlobRef` type and the hash-algorithm helpers used to render and validate the path.
String Layout::blobKey(const BlobRef & ref) const
{
    return shardedKey("blobs/" + String(blobHashAlgoName(ref.algo)), blobHexOf(ref));
}

String Layout::blobMetaKey(const BlobRef & ref) const
{
    return blobKey(ref) + String(kLegacyMarkerSuffix);
}

std::optional<BlobRef> Layout::parseBlobKey(std::string_view key) const
{
    std::string_view rest = key;
    if (hasLegacyMarkerSuffix(rest))
        rest.remove_suffix(kLegacyMarkerSuffix.size());

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
    return parseBlobSegments(algo_name, rest.substr(0, shard_sep), rest.substr(shard_sep + 1));
}

std::optional<BlobRef> Layout::parseLegacyMarkerKey(std::string_view key) const
{
    if (!hasLegacyMarkerSuffix(key))
        return std::nullopt;
    const std::optional<BlobRef> ref = parseBlobKey(key);
    if (!ref || blobMetaKey(*ref) != key)
        return std::nullopt;   /// `fromHex` takes spellings the writer never emits; only the exact key is a marker
    return ref;
}

String Layout::blobIntentKey(const BlobRef & ref, const CanonicalIdentity & identity) const
{
    /// `CanonicalIdentity` is a plain aggregate; a key the parser rejects would never be met by the orphan sweep.
    if (!opaqueTagFromHex(identity.dialect, identity.tag_hex))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CasLayout: '{}' is not a canonical {} tag for a blob intent key",
            identity.tag_hex, dialectWord(identity.dialect));
    const String hex = blobHexOf(ref);
    String key = blobIntentsPrefix() + String(blobHashAlgoName(ref.algo)) + "/" + hex.substr(0, 2) + "/" + hex + "/"
        + String(dialectWord(identity.dialect)) + "/" + identity.tag_hex;
    if (key.size() > kBlobIntentKeyMaxBytes)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CasLayout: a blob intent key of {} bytes is over the {} byte limit; the pool prefix is {} bytes",
            key.size(), kBlobIntentKeyMaxBytes, prefix.size());
    return key;
}

std::optional<Layout::ParsedBlobIntentKey> Layout::parseBlobIntentKey(std::string_view key) const
{
    const String root = blobIntentsPrefix();
    if (!key.starts_with(root) || isBlobIntentProbeKey(key))
        return std::nullopt;
    std::string_view rest = key.substr(root.size());

    /// <algo>/<shard>/<hex>/<dialect>/<tag_hex>: exactly five segments.
    std::string_view segments[5];
    for (size_t i = 0; i < 4; ++i)
    {
        const size_t sep = rest.find('/');
        if (sep == std::string_view::npos)
            return std::nullopt;
        segments[i] = rest.substr(0, sep);
        rest.remove_prefix(sep + 1);
    }
    if (rest.find('/') != std::string_view::npos)
        return std::nullopt;
    segments[4] = rest;
    const std::string_view hex = segments[2];
    const std::string_view tag_hex = segments[4];

    const std::optional<BlobRef> ref = parseBlobSegments(segments[0], segments[1], hex);
    if (!ref || blobHexOf(*ref) != hex)
        return std::nullopt;   /// `fromHex` also takes uppercase; the writer never emits it

    const std::optional<Dialect> dialect = dialectFromWord(segments[3]);
    if (!dialect || !opaqueTagFromHex(*dialect, tag_hex))
        return std::nullopt;
    return ParsedBlobIntentKey{*ref, CanonicalIdentity{*dialect, String(tag_hex)}};
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
    const std::string_view before_kind = before_id.substr(0, kind_sep);
    if (before_kind.empty())
        return std::nullopt;

    RefObjectKind kind{};
    if (kind_seg == "_log")
        kind = RefObjectKind::Log;
    else if (kind_seg == "_snap")
        kind = RefObjectKind::Snap;
    else
        return std::nullopt;

    std::string_view render = id_part;
    /// `_log` and `_snap` are always-compressed text stored under a `.zst` suffix.
    constexpr std::string_view kZstSuffix = ".zst";
    if (!render.ends_with(kZstSuffix))
        return std::nullopt;
    render.remove_suffix(kZstSuffix.size());

    const auto txn_id = parseRefTxnId(render);
    if (!txn_id)
        return std::nullopt;

    if (before_kind.find('/') != std::string_view::npos)
        return std::nullopt;
    return ParsedRefObjectKey{namespaceLifePhysicalIdOf(key, before_kind), kind, *txn_id};
}

std::optional<NamespaceLifePhysicalId> Layout::parseRefCkptKey(std::string_view key) const
{
    const String base = namespaceStateRootPrefix();
    if (!key.starts_with(base))
        return std::nullopt;
    std::string_view rest = key;
    rest.remove_prefix(base.size());

    /// `<life_id>/_ckpt<suffix>` -- the leaf is the fixed name plus whatever the registry's
    /// compression policy appends. Built from the same pieces `refCkptKey` uses, so the two cannot
    /// drift apart.
    const String leaf = "_ckpt" + String(storedSuffix(FormatId::RefCkpt));
    const size_t leaf_sep = rest.rfind('/');
    if (leaf_sep == std::string_view::npos || rest.substr(leaf_sep + 1) != leaf)
        return std::nullopt;
    const std::string_view life_id = rest.substr(0, leaf_sep);
    if (life_id.empty() || life_id.find('/') != std::string_view::npos)
        return std::nullopt;

    return namespaceLifePhysicalIdOf(key, life_id);
}

std::optional<ParsedNamespaceFileKey> Layout::parseNamespaceFileKey(std::string_view key) const
{
    const String base = namespaceStateRootPrefix();
    if (!key.starts_with(base))
        return std::nullopt;
    std::string_view rest = key;
    rest.remove_prefix(base.size());

    /// The first `/_files/` separates the single `<life_id>` segment from the relative name. A
    /// relative name may itself contain `_files`, so the first occurrence is the delimiter.
    static constexpr std::string_view kFilesSegment = "/_files/";
    const size_t files_pos = rest.find(kFilesSegment);
    if (files_pos == std::string_view::npos)
        return std::nullopt;   /// no reserved segment: a loose mountpoint object, not one of our files

    const std::string_view life_id = rest.substr(0, files_pos);
    const std::string_view relative_name = rest.substr(files_pos + kFilesSegment.size());
    if (relative_name.empty())
        return std::nullopt;   /// the files prefix itself names no file

    if (life_id.empty() || life_id.find('/') != std::string_view::npos)
        return std::nullopt;
    /// Mirror `namespaceFileKey`'s writer-side grammar: a relative name no current writer could have
    /// produced (a dirty path, `..` segment, doubled slash, ...) is not one of ours, even though the
    /// `_files/` segment and life id both parse. `namespaceLifePhysicalIdOf` below throws
    /// `CORRUPTED_DATA` for the sibling life-id case; this mirrors that severity for symmetry.
    if (!isCleanRelativeNamespaceFileName(relative_name))
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CasLayout: object '{}' names no clean namespace file: '{}' is not a relative path any "
            "current writer could have produced", key, relative_name);
    return ParsedNamespaceFileKey{namespaceLifePhysicalIdOf(key, life_id), String(relative_name)};
}

NamespaceLifePhysicalId Layout::namespaceLifePhysicalIdOf(std::string_view key, std::string_view segment) const
{
    const auto incarnation = parseIncarnation(segment);
    if (!incarnation)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CasLayout: object '{}' names no life: '{}' is not 32 lower-case hex digits of a nonzero "
            "life id. Pools whose keys predate the opaque-life layout are rejected by the "
            "pool-metadata format gate before this physical-key parser is reached",
            key, segment);
    return *incarnation;
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

std::optional<ParsedBlobTargetRunKey> Layout::parseBlobTargetRunKey(std::string_view key) const
{
    const String base = prefix + "/gc/gen/";
    if (!key.starts_with(base))
        return std::nullopt;
    std::string_view rest = key;
    rest.remove_prefix(base.size());

    /// Splits the next '/'-delimited segment off the front of `rest`, returning `std::nullopt` if
    /// `rest` has no further '/' (an incomplete key shape).
    auto takeSegment = [](std::string_view & s) -> std::optional<std::string_view>
    {
        const size_t sep = s.find('/');
        if (sep == std::string_view::npos)
            return std::nullopt;
        const std::string_view seg = s.substr(0, sep);
        s.remove_prefix(sep + 1);
        return seg;
    };

    const auto generation_seg = takeSegment(rest);
    if (!generation_seg)
        return std::nullopt;
    const auto generation = parseCanonicalU64(*generation_seg);
    if (!generation)
        return std::nullopt;

    const auto attempt_lit = takeSegment(rest);
    if (!attempt_lit || *attempt_lit != "attempt")
        return std::nullopt;

    const auto attempt_seg = takeSegment(rest);
    if (!attempt_seg)
        return std::nullopt;
    const auto attempt = parseCanonicalU64(*attempt_seg);
    if (!attempt)
        return std::nullopt;

    const auto blob_target_lit = takeSegment(rest);
    if (!blob_target_lit || *blob_target_lit != "blob_target")
        return std::nullopt;

    const auto shard_seg = takeSegment(rest);
    if (!shard_seg)
        return std::nullopt;
    const auto shard = parseCanonicalU64(*shard_seg);
    if (!shard)
        return std::nullopt;

    /// `rest` is now the final segment (`seq`): reject trailing garbage (a further '/').
    if (rest.find('/') != std::string_view::npos)
        return std::nullopt;
    const auto seq = parseCanonicalU64(rest);
    if (!seq)
        return std::nullopt;

    return ParsedBlobTargetRunKey{*generation, *attempt, *shard, *seq};
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

std::optional<uint64_t> Layout::stagedEpochOf(std::string_view rest)
{
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos)
        return rest.empty() ? std::nullopt : std::optional<uint64_t>(0);
    const std::string_view dir = rest.substr(0, slash);
    const std::string_view leaf = rest.substr(slash + 1);
    if (dir.empty() || leaf.empty() || leaf.find('/') != std::string_view::npos)
        return std::nullopt;
    uint64_t epoch = 0;
    const auto [end, ec] = std::from_chars(dir.data(), dir.data() + dir.size(), epoch);
    if (ec != std::errc{} || end != dir.data() + dir.size())
        return std::nullopt;
    return epoch;
}

}
