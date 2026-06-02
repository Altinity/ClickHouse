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

}
