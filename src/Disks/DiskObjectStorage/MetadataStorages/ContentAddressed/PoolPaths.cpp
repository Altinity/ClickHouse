#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <vector>

namespace DB::ContentAddressed
{

static std::string fanOut(const std::string & prefix, const std::string & hash)
{
    if (hash.size() < 4)
        return prefix + "/" + hash; /// short hashes (tests) — no fan-out
    return prefix + "/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
}

std::string blobKey(const std::string & file_checksum)
{
    return fanOut("blobs", file_checksum);
}

std::string partKey(const std::string & part_id)
{
    return fanOut("parts", part_id);
}

std::string refsPrefix(const std::string & server_id, const std::string & table_uuid)
{
    return "store/" + server_id + "/" + table_uuid + "/refs/";
}

std::string refKey(const std::string & server_id, const std::string & table_uuid, const std::string & part_name)
{
    return refsPrefix(server_id, table_uuid) + part_name;
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

std::optional<PartFilePath> parsePartFilePath(const std::string & path)
{
    auto p = splitNonEmpty(path);
    if (p.size() < 3)
        return std::nullopt;

    PartFilePath r;
    r.table_uuid = p[1];
    r.part_name = p[2];
    if (p.size() >= 4)
    {
        std::string file = p[3];
        for (size_t i = 4; i < p.size(); ++i)
            file += "/" + p[i];
        r.file = file;
    }
    return r;
}

std::optional<std::string> parseTableUuid(const std::string & path)
{
    auto p = splitNonEmpty(path);
    if (p.size() == 2)
        return p[1];
    return std::nullopt;
}

}
