#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
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

BlobObjectKey blobKey(const std::string & key_prefix, const BlobHash & blob_hash)
{
    return BlobObjectKey(withPrefix(key_prefix, fanOut("blobs", blob_hash.string())));
}

PartObjectKey partKey(const std::string & key_prefix, const PartId & part_id)
{
    return PartObjectKey(withPrefix(key_prefix, fanOut("parts", part_id.string())));
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

std::string diskFileKey(const std::string & key_prefix, const std::string & path)
{
    /// Verbatim: the raw disk-relative path joined under the object-storage common key prefix with
    /// the same empty-prefix-safe rule as every other key builder.
    return withPrefix(key_prefix, path);
}

static std::vector<std::string> splitNonEmpty(const std::string & path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path)
    {
        if (c == '/')
        {
            if (!cur.empty())
                parts.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        parts.push_back(cur);
    return parts;
}

// Locate the <uuid[:3]>/<uuid> anchor inside a split path. ClickHouse table data paths look like
// <prefix...>/<uuid[:3]>/<uuid>/<rest...>, where <uuid[:3]> is exactly the first 3 characters of
// the following <uuid> component. The leading <prefix...> is "store" on a real server but is
// absent in the unit tests, so anchoring on the uuid pair makes parsing robust to either shape.
// Returns the index of the <uuid> component (so components after it are the part/file/tail).
static std::optional<size_t> findTableUuidComponent(const std::vector<std::string> & p)
{
    for (size_t i = 1; i < p.size(); ++i)
    {
        const auto & prefix = p[i - 1];
        const auto & uuid = p[i];
        if (prefix.size() == 3 && uuid.size() > 3 && uuid.compare(0, 3, prefix) == 0)
            return i;
    }
    return std::nullopt;
}

std::optional<PartFilePath> parsePartFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);
    auto uuid_idx = findTableUuidComponent(p);
    // Need at least the part component after the uuid: <uuid[:3]>/<uuid>/<part>.
    if (!uuid_idx || *uuid_idx + 1 >= p.size())
        return std::nullopt;

    const size_t part_idx = *uuid_idx + 1;
    PartFilePath r;
    r.table_uuid = p[*uuid_idx];
    r.part_name = p[part_idx];
    if (part_idx + 1 < p.size())
    {
        std::string file = p[part_idx + 1];
        for (size_t i = part_idx + 2; i < p.size(); ++i)
            file += "/" + p[i];
        r.file = file;
    }
    return r;
}

std::optional<std::string> parseTableUuid(const std::string & path)
{
    auto p = splitNonEmpty(path);
    auto uuid_idx = findTableUuidComponent(p);
    // Exactly the table dir <prefix...>/<uuid[:3]>/<uuid>[/]: nothing after the uuid.
    if (uuid_idx && *uuid_idx + 1 == p.size())
        return p[*uuid_idx];
    return std::nullopt;
}

bool isPartFilePath(const std::string & path)
{
    // A file inside a part dir: <uuid[:3]>/<uuid>/<part>/<file> => at least 2 components after
    // the uuid (the part dir and the file within it).
    auto p = splitNonEmpty(path);
    auto uuid_idx = findTableUuidComponent(p);
    return uuid_idx && *uuid_idx + 2 < p.size();
}

std::optional<TableFilePath> parseTableFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);
    auto uuid_idx = findTableUuidComponent(p);
    // Table-level files live directly under the table dir, i.e. exactly one component after the
    // uuid (e.g. format_version.txt). Deeper paths are part files (see isPartFilePath).
    if (!uuid_idx || *uuid_idx + 2 != p.size())
        return std::nullopt;

    TableFilePath r;
    r.table_uuid = p[*uuid_idx];
    r.tail = p[*uuid_idx + 1];
    return r;
}

}
