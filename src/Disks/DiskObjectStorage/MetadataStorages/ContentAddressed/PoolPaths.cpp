#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
#include <algorithm>
#include <vector>

namespace DB::ContentAddressed
{

/// Prepend the object-storage common key prefix to a bare key. An empty prefix yields exactly the
/// bare key (no leading slash), so existing layouts and seeded tests are byte-for-byte unchanged.
/// A non-empty prefix is joined with a single '/' (any trailing '/' on the prefix is collapsed).
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

static std::string fanOut(const std::string & prefix, const std::string & hash)
{
    if (hash.size() < 4)
        return prefix + "/" + hash; /// short hashes (tests) — no fan-out
    return prefix + "/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
}

// ==== CA GC S3: generationed key builders (spec §6) ====
//
// The fan-out directory `blobs/<H0>/<H1>/<H>/` (resp. `parts/<p0>/<p1>/<part_id>/`) is the SAME fan-out as
// before; what changes is that `<H>` is now a directory whose children are the per-generation objects. So
// the g=0 object lands at `blobs/<H0>/<H1>/<H>/0`, exactly one component deeper than the old bare key.

BlobObjectKey blobGenKey(const std::string & key_prefix, const BlobHash & blob_hash, uint64_t generation)
{
    return BlobObjectKey(withPrefix(key_prefix, fanOut("blobs", blob_hash.string()) + "/" + std::to_string(generation)));
}

BlobObjectKey blobTombstoneKey(const std::string & key_prefix, const BlobHash & blob_hash, uint64_t generation)
{
    return BlobObjectKey(
        withPrefix(key_prefix, fanOut("blobs", blob_hash.string()) + "/" + std::to_string(generation) + std::string(kTombstoneSuffix)));
}

std::string blobActiveKey(const std::string & key_prefix, const BlobHash & blob_hash)
{
    return withPrefix(key_prefix, fanOut("blobs", blob_hash.string()) + "/" + std::string(kActiveHintName));
}

std::string blobGenPrefix(const std::string & key_prefix, const BlobHash & blob_hash)
{
    /// The LIST prefix for ALL generations / tombstones / the active hint of one H (trailing '/').
    return withPrefix(key_prefix, fanOut("blobs", blob_hash.string()) + "/");
}

PartObjectKey partGenKey(const std::string & key_prefix, const PartId & part_id, uint64_t generation)
{
    return PartObjectKey(withPrefix(key_prefix, fanOut("parts", part_id.string()) + "/" + std::to_string(generation)));
}

PartObjectKey partTombstoneKey(const std::string & key_prefix, const PartId & part_id, uint64_t generation)
{
    return PartObjectKey(
        withPrefix(key_prefix, fanOut("parts", part_id.string()) + "/" + std::to_string(generation) + std::string(kTombstoneSuffix)));
}

std::string partActiveKey(const std::string & key_prefix, const PartId & part_id)
{
    return withPrefix(key_prefix, fanOut("parts", part_id.string()) + "/" + std::string(kActiveHintName));
}

std::string partGenPrefix(const std::string & key_prefix, const PartId & part_id)
{
    return withPrefix(key_prefix, fanOut("parts", part_id.string()) + "/");
}

BlobObjectKey blobKey(const std::string & key_prefix, const BlobHash & blob_hash)
{
    return blobGenKey(key_prefix, blob_hash, 0);
}

PartObjectKey partKey(const std::string & key_prefix, const PartId & part_id)
{
    return partGenKey(key_prefix, part_id, 0);
}

std::optional<uint64_t> parseGenFromKey(const std::string & key, bool & is_tombstone)
{
    is_tombstone = false;
    /// The generation component is the last path component: `…/<g>` or `…/<g>.tombstone`.
    const auto slash = key.rfind('/');
    std::string_view last = slash == std::string::npos ? std::string_view(key) : std::string_view(key).substr(slash + 1);

    if (last.size() >= kTombstoneSuffix.size() && last.substr(last.size() - kTombstoneSuffix.size()) == kTombstoneSuffix)
    {
        is_tombstone = true;
        last = last.substr(0, last.size() - kTombstoneSuffix.size());
    }

    if (last.empty() || last == kActiveHintName)
        return std::nullopt;

    uint64_t value = 0;
    for (char c : last)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    return value;
}

std::optional<GenObjectKeyParts> parseGenObjectKey(const std::string & key_prefix, const std::string & key)
{
    /// The inverse of blobGenKey / partGenKey. The key is `<prefix>/{blobs,parts}/<fan-out…>/<id>/<g>`,
    /// so the LAST component is the generation, the SECOND-to-last is the bare identity, and the FIRST
    /// component after the prefix is the kind root (`blobs` / `parts`). A tombstone / active / malformed
    /// shape returns nullopt (parseGenFromKey screens out the tombstone suffix and `active`).
    bool is_tombstone = false;
    const std::optional<uint64_t> gen = parseGenFromKey(key, is_tombstone);
    if (!gen || is_tombstone)
        return std::nullopt;

    /// Strip the common prefix to get the bare key, mirroring withPrefix (single '/' join, no leading '/').
    std::string bare = key;
    {
        std::string p = key_prefix;
        while (!p.empty() && p.back() == '/')
            p.pop_back();
        if (!p.empty())
        {
            const std::string joined = p + "/";
            if (key.rfind(joined, 0) != 0)
                return std::nullopt;
            bare = key.substr(joined.size());
        }
    }

    /// Split the bare key into components. The first is the kind root; the last is `<g>` (already parsed);
    /// the one before it is the bare identity `<H>` / `<part_id>`.
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= bare.size(); ++i)
    {
        if (i == bare.size() || bare[i] == '/')
        {
            parts.emplace_back(std::string_view(bare).substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() < 3) /// at minimum `{blobs,parts}/<id>/<g>` (short-hash, no fan-out)
        return std::nullopt;

    GenObjectKeyParts result;
    if (parts.front() == "blobs")
        result.is_blob = true;
    else if (parts.front() == "parts")
        result.is_blob = false;
    else
        return std::nullopt;
    result.identity = std::string(parts[parts.size() - 2]);
    result.generation = *gen;
    if (result.identity.empty())
        return std::nullopt;
    return result;
}

std::string partsPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "parts");
}

std::string blobsPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "blobs");
}

std::string refsRootPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "store/");
}

std::string refsPrefix(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid)
{
    return withPrefix(key_prefix, "store/" + server_id + "/" + table_uuid + "/refs/");
}

RefObjectKey refKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name)
{
    return RefObjectKey(refsPrefix(key_prefix, server_id, table_uuid) + part_name);
}

RefMetaObjectKey refMetaKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name)
{
    /// Sidecar sits next to the ref, under the same refs/ prefix (see header): refs/<part_name>.meta.
    return RefMetaObjectKey(refsPrefix(key_prefix, server_id, table_uuid) + part_name + std::string(kRefMetaSuffix));
}

RefMetaObjectKey refMutableFileKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name, const std::string & file)
{
    /// Per-file object next to the ref: refs/<part_name>.<file>.meta (see header).
    return RefMetaObjectKey(refsPrefix(key_prefix, server_id, table_uuid) + part_name + "." + file + std::string(kRefMetaSuffix));
}

std::string shadowRefsRootPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "shadow/");
}

std::string shadowRefsPrefix(const std::string & key_prefix, const std::string & shadow_table_dir)
{
    /// Mirror the physical store tree: the refs live directly under the literal shadow table dir
    /// (shadow/<backup>/store/<uuid[:3]>/<uuid>) so the enumeration's intermediate levels resolve via
    /// the same generic child-derivation as the live table dir.
    ///
    /// `shadow_table_dir` arrives either from `joinTableId` (no trailing slash) or as a free-form directory
    /// `path` from `existsDirectory`/`listDirectory` (which MAY carry a trailing slash). Strip a trailing
    /// slash before appending `/refs/` so the join produces exactly one separator — a literal `<uuid>//refs/`
    /// is normalized by the local POSIX FS but rejected by S3/MinIO (XMinioInvalidObjectName).
    std::string dir = shadow_table_dir;
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back();
    return withPrefix(key_prefix, dir + "/refs/");
}

RefObjectKey shadowRefKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name)
{
    return RefObjectKey(shadowRefsPrefix(key_prefix, shadow_table_dir) + part_name);
}

RefMetaObjectKey shadowRefMetaKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name)
{
    return RefMetaObjectKey(shadowRefsPrefix(key_prefix, shadow_table_dir) + part_name + std::string(kRefMetaSuffix));
}

RefMetaObjectKey shadowRefMutableFileKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name, const std::string & file)
{
    return RefMetaObjectKey(shadowRefsPrefix(key_prefix, shadow_table_dir) + part_name + "." + file + std::string(kRefMetaSuffix));
}

bool isRefMetaKey(const std::string & key)
{
    return key.size() >= kRefMetaSuffix.size()
        && std::string_view(key).substr(key.size() - kRefMetaSuffix.size()) == kRefMetaSuffix;
}

std::string tableFilesPrefix(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid)
{
    return withPrefix(key_prefix, "store/" + server_id + "/" + table_uuid + "/files/");
}

std::string tableFileKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & tail)
{
    return tableFilesPrefix(key_prefix, server_id, table_uuid) + tail;
}

std::string poolMetaKey(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "_pool_meta");
}

std::string poolMountersPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "_pool_meta.mounters/");
}

std::string poolMounterKey(const std::string & key_prefix, const std::string & server_id)
{
    return poolMountersPrefix(key_prefix) + server_id;
}

std::string sessionsPrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "sessions/");
}

std::string sessionKey(const std::string & key_prefix, const std::string & session_id)
{
    return sessionsPrefix(key_prefix) + session_id;
}

std::string fencePrefix(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "fence/");
}

std::string fenceKey(const std::string & key_prefix, uint64_t n)
{
    return fencePrefix(key_prefix) + std::to_string(n);
}

std::string gcLockKey(const std::string & key_prefix)
{
    return withPrefix(key_prefix, "gc.lock");
}

std::string diskFileKey(const std::string & key_prefix, const std::string & path)
{
    /// Verbatim: the raw disk-relative path joined under the object-storage common key prefix with
    /// the same empty-prefix-safe rule as every other key builder.
    return withPrefix(key_prefix, path);
}


}
