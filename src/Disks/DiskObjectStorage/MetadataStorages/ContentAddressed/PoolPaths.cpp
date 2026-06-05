#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
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
    return withPrefix(key_prefix, shadow_table_dir + "/refs/");
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

bool isShadowPath(const std::string & path)
{
    size_t i = 0;
    while (i < path.size() && path[i] == '/')
        ++i;
    const auto first_end = path.find('/', i);
    const std::string_view first = first_end == std::string::npos
        ? std::string_view(path).substr(i)
        : std::string_view(path).substr(i, first_end - i);
    return first == kShadowDirName;
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

// Locate the <uuid[:3]>/<uuid> anchor inside a split path. ClickHouse table data paths on an Atomic
// database look like <prefix...>/<uuid[:3]>/<uuid>/<rest...>, where <uuid[:3]> is exactly the first 3
// characters of the following <uuid> component. The leading <prefix...> is "store" on a real server
// but is absent in the unit tests, so anchoring on the uuid pair makes parsing robust to either
// shape. Returns the index of the <uuid> component (so the component after it is the part dir).
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

// True iff a path component looks like a MergeTree part directory. A non-Atomic database (Ordinary,
// Memory, Lazy, …) stores a table under data/<db>/<table>/ — NOT the UUID-anchored store/ layout — so
// the uuid anchor cannot find the table/part boundary there, and a uuid-only parser misclassified
// every part file as a verbatim non-part file (write went to a verbatim key under tmp_insert_<part>/,
// the tmp->final rename moved nothing, the read of the final part dir found no object → data loss,
// B40). A part directory carries the MergeTree block-range suffix _<min>_<max>_<level>[_<mutation>]
// (e.g. all_1_1_0, 20200101_1_1_0_5) and keeps it through every temporary/operation prefix the part
// passes through (tmp_insert_all_1_1_0, tmp_merge_all_1_2_1, delete_tmp_all_1_1_0). So a component is
// a part dir iff its last 3 underscore-separated groups are all non-empty decimal numbers (with an
// optional 4th numeric group for the mutation level). This is grammar-only — no dependency on the
// Storages layer — and is used only as a FALLBACK when the uuid anchor is absent, so the Atomic
// layout and the unit tests (which use the uui/uuid-N shape) are unchanged.
static bool looksLikePartDir(const std::string & name)
{
    std::vector<std::string> groups;
    std::string cur;
    for (char c : name)
    {
        if (c == '_')
        {
            groups.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    groups.push_back(cur);

    // Need at least <partition>_<min>_<max>_<level>: a partition group plus 3 trailing numeric groups.
    if (groups.size() < 4)
        return false;

    auto is_number = [](const std::string & s)
    {
        if (s.empty())
            return false;
        for (char c : s)
            if (c < '0' || c > '9')
                return false;
        return true;
    };

    // The trailing groups are <min>_<max>_<level>[_<mutation>]: require 3 numeric tail groups, and if
    // a 4th-from-last group is also numeric treat it as the mutation-level form. We only need the last
    // three to be numeric to recognize a part dir (the partition id is arbitrary text before them).
    const size_t n = groups.size();
    return is_number(groups[n - 1]) && is_number(groups[n - 2]) && is_number(groups[n - 3]);
}

// The table/part split of a path. table_start..part_idx are the table-identifier components; part_idx
// is the part directory; components after it are the in-part file path. For the Atomic layout the
// table identifier is the single <uuid> component (table_start == part_idx - 1, preserving the
// historical "table_uuid is just the uuid" identity and dedup/golden tests); for the non-Atomic
// layout it is the full data/<db>/<table> path (table_start == 0).
struct PartDirAnchor
{
    size_t table_start;
    size_t part_idx;
};

// Locate the part-directory component. Anchor on the uuid pair first (Atomic + the unit tests); if
// that is absent (non-Atomic data/<db>/<table>/<part>/…), fall back to the RIGHTMOST component that
// looks like a part dir, with the whole preceding path as the table identifier. Returns nullopt when
// the path has no part component at all (a table dir or a shallower/non-part path).
static std::optional<PartDirAnchor> findPartDirComponent(const std::vector<std::string> & p)
{
    if (auto uuid_idx = findTableUuidComponent(p))
    {
        const size_t part_idx = *uuid_idx + 1;
        if (part_idx < p.size())
        {
            // The reserved deduplication-log directory is a table-level subdir, not a part dir, so a
            // path <uuid>/deduplication_logs/<file> resolves as a table-level file (see
            // kDeduplicationLogsDirName) rather than being content-addressed as a part.
            if (p[part_idx] == kDeduplicationLogsDirName)
                return std::nullopt;
            return PartDirAnchor{*uuid_idx, part_idx}; // table id = the single <uuid> component
        }
        return std::nullopt; // table dir, no part component after the uuid
    }

    // No uuid anchor: a non-Atomic table path. The table identifier must be at least one component
    // (a real table dir, never the bare disk root), so the part dir is at index >= 1. Scan right to
    // left so a part-dir-shaped table/partition name earlier in the path cannot steal the anchor.
    for (size_t i = p.size(); i-- > 1;)
        if (looksLikePartDir(p[i]))
            return PartDirAnchor{0, i}; // table id = the whole path before the part dir
    return std::nullopt;
}

// Join components [start, end) with '/' into a single table identifier (one component for Atomic, the
// data/<db>/<table> path for non-Atomic), used opaquely as a stable per-table segment of the ref/store
// object key (store/<server>/<table_id>/refs/…).
static std::string joinTableId(const std::vector<std::string> & p, size_t start, size_t end)
{
    std::string id;
    for (size_t i = start; i < end; ++i)
    {
        if (!id.empty())
            id += "/";
        id += p[i];
    }
    return id;
}

std::optional<PartFilePath> parsePartFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);
    auto anchor = findPartDirComponent(p);
    if (!anchor)
        return std::nullopt;

    PartFilePath r;
    r.table_uuid = joinTableId(p, anchor->table_start, anchor->part_idx);
    r.part_name = p[anchor->part_idx];
    if (anchor->part_idx + 1 < p.size())
    {
        std::string file = p[anchor->part_idx + 1];
        for (size_t i = anchor->part_idx + 2; i < p.size(); ++i)
            file += "/" + p[i];
        r.file = file;
    }
    // FREEZE target: shadow/<backup_name>/…/<part>. The frozen ref lives in the shadow/ namespace, so
    // capture both the backup name (the component right after the reserved "shadow" root) and the
    // literal shadow table dir — the joined components before the part dir
    // (shadow/<backup>/store/<uuid[:3]>/<uuid>) — for the commit / read / remove routing. The inner
    // store/<uuid>/<part> anchor above is unaffected by the prefix.
    if (p.size() >= 2 && p[0] == kShadowDirName)
    {
        r.backup_name = p[1];
        r.shadow_table_dir = joinTableId(p, 0, anchor->part_idx);
    }
    return r;
}

std::optional<std::string> parseTableUuid(const std::string & path)
{
    auto p = splitNonEmpty(path);

    // Atomic layout: exactly the table dir <prefix...>/<uuid[:3]>/<uuid>[/] — nothing after the uuid.
    if (auto uuid_idx = findTableUuidComponent(p); uuid_idx && *uuid_idx + 1 == p.size())
        return p[*uuid_idx];

    // Non-Atomic layout: a directory path with no part-dir component is the table dir data/<db>/<table>.
    // Require at least two components so the bare disk root (or a single generic dir) is never taken as
    // a table dir; the table identifier is the full joined path (consistent with parsePartFilePath).
    if (findTableUuidComponent(p))
        return std::nullopt; // had a uuid anchor but something followed it: not a table dir
    if (p.size() >= 2 && !findPartDirComponent(p))
        return joinTableId(p, 0, p.size());
    return std::nullopt;
}

bool endsWithTableUuidPair(const std::string & path)
{
    auto p = splitNonEmpty(path);
    auto uuid_idx = findTableUuidComponent(p);
    return uuid_idx && *uuid_idx + 1 == p.size();
}

bool isPartFilePath(const std::string & path)
{
    // A file inside a part dir: <table_path...>/<part>/<file> => at least one component after the
    // part dir. findPartDirComponent locates the part dir for both the Atomic and non-Atomic layouts.
    auto p = splitNonEmpty(path);
    auto anchor = findPartDirComponent(p);
    return anchor && anchor->part_idx + 1 < p.size();
}

std::optional<TableFilePath> parseTableFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);

    // Atomic layout: a table-level file lives under the table dir, i.e. at least one component after
    // the uuid. The tail is EVERYTHING after the uuid joined by '/', so a table-level file in a
    // subdirectory (e.g. deduplication_logs/deduplication_log_1.txt) keeps its full sub-path. A part
    // file is excluded earlier by isPartFilePath (it has a part-dir component); this function is only
    // reached for non-part paths, so a deeper tail here is always a genuine table-level sub-path.
    if (auto uuid_idx = findTableUuidComponent(p))
    {
        if (*uuid_idx + 1 >= p.size())
            return std::nullopt; // the bare table dir, no file tail
        TableFilePath r;
        r.table_uuid = p[*uuid_idx];
        r.tail = joinTableId(p, *uuid_idx + 1, p.size());
        return r;
    }

    // Non-Atomic layout: a path with no part-dir component whose last component is the table-level
    // file, and the components before it are the table dir data/<db>/<table>. Require the table id to
    // be at least one component (a real table, never the bare disk root) and the path to have no part
    // dir (else it is a part file). Consistent with parsePartFilePath's table identifier.
    if (p.size() < 2 || findPartDirComponent(p))
        return std::nullopt;

    // A reserved table-level subdirectory (deduplication_logs/) splits the path explicitly: the table id
    // is everything before it, the tail is the reserved dir and everything under it. Without this the
    // generic "last component is the file" rule would fold the subdir into the table id and mis-scope the
    // log objects. The reserved component must be at index >= 1 so the table id is never the bare root.
    for (size_t i = 1; i + 1 < p.size(); ++i)
    {
        if (p[i] == kDeduplicationLogsDirName)
        {
            TableFilePath r;
            r.table_uuid = joinTableId(p, 0, i);
            r.tail = joinTableId(p, i, p.size());
            return r;
        }
    }

    TableFilePath r;
    r.table_uuid = joinTableId(p, 0, p.size() - 1);
    r.tail = p.back();
    return r;
}

}
