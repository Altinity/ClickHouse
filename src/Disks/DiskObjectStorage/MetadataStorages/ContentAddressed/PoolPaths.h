#pragma once
#include <optional>
#include <string>

namespace DB::ContentAddressed
{

// Content-addressed object keys with 2x2 hex prefix fan-out (S3 per-prefix limits).
std::string blobKey(const std::string & file_checksum);
std::string partKey(const std::string & part_id);

// Per-server/per-table ref object key: store/<server_id>/<table_uuid>/refs/<part_name>.
std::string refsPrefix(const std::string & server_id, const std::string & table_uuid);
std::string refKey(const std::string & server_id, const std::string & table_uuid, const std::string & part_name);

// Per-server/per-table direct object key for non-part / table-level files (e.g.
// format_version.txt, later mutation_*.txt). These are stored verbatim (no content addressing,
// no ref, no footer) under store/<server_id>/<table_uuid>/files/<tail>, where <tail> is the
// path beyond the table dir <uuid[:3]>/<uuid>/.
// TODO(phase4-gc): non-part objects are GC roots.
std::string tableFilesPrefix(const std::string & server_id, const std::string & table_uuid);
std::string tableFileKey(const std::string & server_id, const std::string & table_uuid, const std::string & tail);

struct PartFilePath
{
    std::string table_uuid;
    std::string part_name;
    std::string file; /// empty when the path is a part directory
};

// Parse a disk-relative ClickHouse path <uuid[:3]>/<uuid>/<part>[/<file>].
// Returns nullopt if the path is the table dir or shallower (fewer than 3 components).
std::optional<PartFilePath> parsePartFilePath(const std::string & path);

// Returns the table_uuid iff path is exactly the table dir <uuid[:3]>/<uuid>[/] (2 components).
std::optional<std::string> parseTableUuid(const std::string & path);

// True iff the path addresses a file inside a part dir, i.e. <uuid[:3]>/<uuid>/<part>/<file>
// (4+ components, non-empty file). These are content-addressed (ref + footer + blob). Everything
// else handled by writeFile / file reads (e.g. <uuid[:3]>/<uuid>/format_version.txt, 3 components)
// is a non-part / table-level file handled by plain passthrough.
bool isPartFilePath(const std::string & path);

struct TableFilePath
{
    std::string table_uuid;
    std::string tail; /// path beyond the table dir <uuid[:3]>/<uuid>/
};

// Parse a non-part / table-level file path: <uuid[:3]>/<uuid>/<tail...> where <tail...> is not a
// single part-dir-shaped component path. Returns nullopt if the path is shallower than the table
// dir, or if it is a part-file path (use isPartFilePath / parsePartFilePath for those).
std::optional<TableFilePath> parseTableFilePath(const std::string & path);

}
