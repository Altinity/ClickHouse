#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>

namespace DB::ContentAddressed
{

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
// Memory, Lazy, ...) stores a table under data/<db>/<table>/ — NOT the UUID-anchored store/ layout —
// so the uuid anchor cannot find the table/part boundary there (B40: a uuid-only parser
// misclassified every non-Atomic part file as a verbatim file → data loss). A part directory
// carries the MergeTree block-range suffix _<min>_<max>_<level>[_<mutation>] (all_1_1_0,
// 20200101_1_1_0_5) and keeps it through every temporary/operation prefix (tmp_insert_all_1_1_0,
// tmp_merge_all_1_2_1, delete_tmp_all_1_1_0). So a component is a part dir iff its last 3
// underscore-separated groups are all non-empty decimal numbers. Grammar-only — no Storages
// dependency — and used only as a FALLBACK when the uuid anchor is absent.
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

    const size_t n = groups.size();
    return is_number(groups[n - 1]) && is_number(groups[n - 2]) && is_number(groups[n - 3]);
}

// The table/part split of a path. table_start..part_idx are the table-identifier components; part_idx
// is the part directory; components after it are the in-part file path. For the Atomic layout the
// table identifier is the single <uuid> component; for the non-Atomic layout it is the full
// data/<db>/<table> path (table_start == 0).
struct PartDirAnchor
{
    size_t table_start;
    size_t part_idx;
};

// Locate the part-directory component. Anchor on the uuid pair first (Atomic + the unit tests); if
// that is absent (non-Atomic data/<db>/<table>/<part>/...), fall back to the RIGHTMOST component
// that looks like a part dir, with the whole preceding path as the table identifier. Returns
// nullopt when the path has no part component at all (a table dir or a shallower/non-part path).
static std::optional<PartDirAnchor> findPartDirComponent(const std::vector<std::string> & p)
{
    if (auto uuid_idx = findTableUuidComponent(p))
    {
        const size_t part_idx = *uuid_idx + 1;
        if (part_idx < p.size())
        {
            // The reserved deduplication-log directory is a table-level subdir, not a part dir
            // (see kDeduplicationLogsDirName).
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

// Join components [start, end) with '/' into a single table identifier (one component for Atomic,
// the data/<db>/<table> path for non-Atomic), used opaquely as a stable per-table segment.
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
    // FREEZE target: shadow/<backup_name>/.../<part>. Capture both the backup name (the component
    // right after the reserved "shadow" root) and the literal shadow table dir — the joined
    // components before the part dir — for the commit / read / remove routing. The inner uuid
    // anchor above is unaffected by the prefix.
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

    // Non-Atomic layout: a directory path with no part-dir component is the table dir
    // data/<db>/<table>. Require at least two components so the bare disk root (or a single generic
    // dir) is never taken as a table dir.
    if (findTableUuidComponent(p))
        return std::nullopt; // had a uuid anchor but something followed it: not a table dir
    if (p.size() >= 2 && !findPartDirComponent(p))
        return joinTableId(p, 0, p.size());
    return std::nullopt;
}

bool isAtomicShardDir(const std::string & path)
{
    // The Atomic on-disk layout shards table dirs as `store/<u3>/<uuid>@cas@`, so `store/<u3>` is a
    // pure intermediate shard directory: the literal `store` root followed by exactly one 3-char
    // uuid-prefix component, with nothing after it. This is ambiguous with the non-Atomic
    // data/<db> fallback (both are two non-part components with no uuid anchor), so the router uses
    // this strict predicate to disambiguate before parseTableUuid.
    auto p = splitNonEmpty(path);
    return p.size() == 2 && p[0] == "store" && p[1].size() == 3;
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
    // part dir, for both the Atomic and non-Atomic layouts.
    auto p = splitNonEmpty(path);
    auto anchor = findPartDirComponent(p);
    return anchor && anchor->part_idx + 1 < p.size();
}

std::optional<TableFilePath> parseTableFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);

    // Atomic layout: a table-level file lives under the table dir, i.e. at least one component
    // after the uuid. The tail is EVERYTHING after the uuid joined by '/', so a table-level file in
    // a subdirectory (deduplication_logs/deduplication_log_1.txt) keeps its full sub-path. A part
    // file is excluded earlier by isPartFilePath; this function is only reached for non-part paths.
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
    // file, and the components before it are the table dir data/<db>/<table>. Require the table id
    // to be at least one component (a real table, never the bare disk root).
    if (p.size() < 2 || findPartDirComponent(p))
        return std::nullopt;

    // A reserved table-level subdirectory (deduplication_logs/) splits the path explicitly: the
    // table id is everything before it, the tail is the reserved dir and everything under it.
    // Without this the generic "last component is the file" rule would fold the subdir into the
    // table id and mis-scope the log objects. Index >= 1 so the table id is never the bare root.
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

std::string mirroredArchiveNamespace(const std::string & table_uuid)
{
    if (table_uuid.find('/') == std::string::npos)
    {
        /// Atomic: a bare uuid; mirror ClickHouse's store/<u3>/<uuid> fanout.
        const std::string u3 = table_uuid.substr(0, 3);
        return "store/" + u3 + "/" + table_uuid + std::string(kCasArchiveSuffix);
    }
    /// Non-Atomic: a full data/<db>/<tbl> path already; append the suffix to the last segment.
    return table_uuid + std::string(kCasArchiveSuffix);
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

}
