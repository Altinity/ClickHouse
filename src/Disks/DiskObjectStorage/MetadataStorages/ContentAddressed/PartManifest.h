#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace DB::ContentAddressed
{

struct BlobEntry
{
    std::string key;
    uint64_t size = 0;
    std::string checksum;
    auto operator<=>(const BlobEntry &) const = default;
};

struct PartManifest
{
    std::map<std::string, BlobEntry> blobs;
    std::map<std::string, std::string> inlined;

    std::string serialize() const;
    static PartManifest deserialize(const std::string & bytes);

    static constexpr char MAGIC[5] = {'C', 'A', 'M', '0', '1'};
};

}
