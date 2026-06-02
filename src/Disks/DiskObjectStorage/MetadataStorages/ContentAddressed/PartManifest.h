#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <cstdint>
#include <map>
#include <string>

namespace DB::ContentAddressed
{

struct BlobEntry
{
    /// The BARE content hash of the blob (NOT the full object key). The full key is derived on the
    /// boundary via blobKey(prefix, BlobHash); typing it as BlobHash makes that distinction a
    /// compile error instead of a data-loss bug.
    BlobHash key;
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
