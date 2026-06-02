#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartId.h>

#include <Common/SipHash.h>
#include <base/hex.h>

#include <array>
#include <string_view>

namespace DB::ContentAddressed
{

/// Mutable/non-deterministic files that must not contribute to the part identity.
static bool isExcludedFromPartId(std::string_view file)
{
    static constexpr std::array excluded{"uuid.txt", "txn_version.txt", "metadata_version.txt"};
    for (const auto & e : excluded)
        if (file == e)
            return true;
    return false;
}

std::string computePartId(const std::map<std::string, BlobEntry> & blobs)
{
    /// std::map iterates in sorted key order, so the (logical_file, checksum) stream is canonical.
    SipHash hash;
    for (const auto & [file, blob] : blobs)
    {
        if (isExcludedFromPartId(file))
            continue;
        hash.update(file);
        hash.update(blob.checksum);
    }
    return getHexUIntLowercase(hash.get128());
}

}
