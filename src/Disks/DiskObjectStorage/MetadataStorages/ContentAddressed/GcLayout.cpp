#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLayout.h>
#include <algorithm>
#include <string>

namespace DB::ContentAddressed
{

/// Prepend the object-storage common key prefix to a bare key. An empty prefix yields exactly the
/// bare key (no leading slash). A non-empty prefix is joined with a single '/' (any trailing '/' on
/// the prefix is collapsed). Mirrors the identical helper in PoolPaths.cpp — each file owns its copy
/// so neither pulls in the other's translation unit.
static std::string withPrefix(const std::string & key_prefix, const std::string & bare)
{
    if (key_prefix.empty())
        return bare;
    std::string p = key_prefix;
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    if (p.empty())
        return bare;
    return p + "/" + bare;
}

/// Zero-pad an epoch to a fixed width so a LEXICAL LIST of gc/snap sorts in NUMERIC epoch order. 20 digits
/// covers the full uint64_t range, so no real epoch ever overflows the field and the lexical order is
/// total over every representable epoch.
static std::string paddedEpoch(uint64_t epoch)
{
    std::string s = std::to_string(epoch);
    if (s.size() < 20)
        s.insert(s.begin(), 20 - s.size(), '0');
    return s;
}

ShardId shardForHash(const BlobHash & blob_hash)
{
    /// Map the content hash's prefix uniformly onto [0, kGcShardCount). The hash is lowercase hex, so its
    /// first nibbles are uniformly distributed; fold the leading hex digits into an integer and mask the
    /// low log2(kGcShardCount) bits (kGcShardCount is a power of two). A hash too short to carry a prefix
    /// (only the short synthetic hashes used in unit tests) deterministically lands in shard 0.
    const std::string & h = blob_hash.string();
    uint32_t acc = 0;
    /// Two hex digits = one byte = 8 bits is ample for kGcShardCount up to 256; consume up to 4 digits so
    /// the partition stays uniform if kGcShardCount grows. A non-hex character (defensive) contributes 0.
    const size_t take = std::min<size_t>(h.size(), 4);
    for (size_t i = 0; i < take; ++i)
    {
        const char c = h[i];
        uint32_t nibble = 0;
        if (c >= '0' && c <= '9')
            nibble = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            nibble = static_cast<uint32_t>(c - 'A' + 10);
        acc = (acc << 4) | nibble;
    }
    return static_cast<ShardId>(acc & (kGcShardCount - 1));
}

ShardId shardForPartId(const PartId & part_id)
{
    /// A `PartId` is a lowercase-hex digest (the content hash of the manifest body), so its prefix nibbles
    /// are uniformly distributed on [0, kGcShardCount) by the same argument as `shardForHash`. Reinterpret
    /// the part_id string as a BlobHash for the prefix-bit extraction — both are lowercase-hex digests, so
    /// the nibble-fold logic is identical and the two shard assignments are always in sync.
    /// `GcLogWriter::shardForPartId` delegates here rather than duplicating this logic.
    return shardForHash(BlobHash(part_id.string()));
}

std::string gcCurrentEpochKey(const std::string & key_prefix, ShardId shard)
{
    return withPrefix(key_prefix, "gc/current_epoch/" + std::to_string(shard));
}

std::string gcLogPrefix(const std::string & key_prefix, uint64_t epoch, ShardId shard)
{
    /// <key_prefix>/gc/log/<epoch>.<shard>/ — the LIST prefix for one epoch's coalesced delta objects.
    /// The epoch is LISTed by exact (epoch, shard), never scanned in lexical order, so it is not padded.
    return withPrefix(key_prefix, "gc/log/" + std::to_string(epoch) + "." + std::to_string(shard) + "/");
}

GcLogObjectKey gcLogEventKey(const std::string & key_prefix, uint64_t epoch, ShardId shard, const std::string & event_id)
{
    return GcLogObjectKey(gcLogPrefix(key_prefix, epoch, shard) + event_id);
}

GcSnapObjectKey gcSnapKey(const std::string & key_prefix, uint64_t epoch, ShardId shard)
{
    /// <key_prefix>/gc/snap/<padded-epoch>.<shard> — zero-padded epoch so a lexical LIST is numeric order.
    return GcSnapObjectKey(withPrefix(key_prefix, "gc/snap/" + paddedEpoch(epoch) + "." + std::to_string(shard)));
}

std::string gcSnapPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "gc/snap/");
}

std::string gcLogRootPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "gc/log/");
}

// ==== CA GC S4 (#4): per-shard sealed-tombstone index ====

std::string gcSealedPrefix(const std::string & key_prefix, ShardId shard)
{
    /// LIST prefix for all sealed-index entries of a single shard: <key_prefix>/gc/sealed/<shard>/
    return withPrefix(key_prefix, "gc/sealed/" + std::to_string(shard) + "/");
}

std::string gcSealedKey(const std::string & key_prefix, ShardId shard, const std::string & identity, uint64_t generation, bool is_blob)
{
    /// Full object key: <key_prefix>/gc/sealed/<shard>/<identity>.<generation>.<b|p>
    /// Encoding: `identity` is a lowercase hex digest (no `.`), `generation` is decimal, type is `b`/`p`.
    /// Splitting the basename on `.` always yields exactly 3 non-empty fields — unambiguously parseable.
    return gcSealedPrefix(key_prefix, shard) + identity + "." + std::to_string(generation) + (is_blob ? ".b" : ".p");
}

std::optional<SealedIndexEntry> parseSealedIndexKey(const std::string & key_prefix, const std::string & key)
{
    /// Strip the leading key_prefix + "/" (mirroring withPrefix).
    std::string bare = key;
    {
        std::string p = key_prefix;
        while (!p.empty() && p.back() == '/')
            p.pop_back();
        if (!p.empty())
        {
            const std::string joined = p + "/";
            if (bare.rfind(joined, 0) != 0)
                return std::nullopt;
            bare = bare.substr(joined.size());
        }
    }

    /// The bare key must start with "gc/sealed/<shard>/". Confirm the shape without validating the shard
    /// number (the shard is implicit in the LIST prefix, not load-bearing for the parse result).
    const std::string_view sv(bare);
    if (sv.rfind("gc/sealed/", 0) != 0)
        return std::nullopt;

    /// The basename is everything after the last '/'. For "gc/sealed/<shard>/<basename>" there must be
    /// exactly 4 path components (gc, sealed, <shard>, <basename>), so at least 3 '/' separators.
    const auto last_slash = bare.rfind('/');
    if (last_slash == std::string::npos)
        return std::nullopt;
    const std::string_view basename = sv.substr(last_slash + 1);
    if (basename.empty())
        return std::nullopt;

    /// Split the basename on '.'. Expect exactly 3 parts: identity, generation (decimal), type (b or p).
    /// The identity is a hex digest that never contains '.', so this split is unambiguous.
    const size_t dot1 = basename.find('.');
    if (dot1 == std::string_view::npos)
        return std::nullopt;
    const std::string_view identity_sv = basename.substr(0, dot1);
    if (identity_sv.empty())
        return std::nullopt;

    const std::string_view rest = basename.substr(dot1 + 1);
    const size_t dot2 = rest.find('.');
    if (dot2 == std::string_view::npos)
        return std::nullopt;
    const std::string_view gen_sv = rest.substr(0, dot2);
    const std::string_view type_sv = rest.substr(dot2 + 1);

    /// The type suffix must be exactly `b` or `p`; anything else (including another `.`) is malformed.
    if (type_sv.empty() || type_sv.find('.') != std::string_view::npos)
        return std::nullopt;
    bool is_blob;
    if (type_sv == "b")
        is_blob = true;
    else if (type_sv == "p")
        is_blob = false;
    else
        return std::nullopt;

    /// Parse the generation as a decimal uint64. An empty or non-numeric string is malformed.
    if (gen_sv.empty())
        return std::nullopt;
    uint64_t generation = 0;
    for (char c : gen_sv)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        generation = generation * 10 + static_cast<uint64_t>(c - '0');
    }

    return SealedIndexEntry{std::string(identity_sv), generation, is_blob};
}

}
