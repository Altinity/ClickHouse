#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Reachability.h>

namespace DB::ContentAddressed
{

std::set<std::string> markReachableBlobs(const std::set<std::string> & live_part_ids, const FooterResolver & resolve)
{
    std::set<std::string> reachable;
    for (const auto & id : live_part_ids)
    {
        Footer footer = resolve(id);
        for (const auto & blob : footer.blobs)
            reachable.insert(blob.second.key);
    }
    return reachable;
}

}
