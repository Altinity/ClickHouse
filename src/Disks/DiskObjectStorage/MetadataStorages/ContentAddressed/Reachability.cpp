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

SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace)
{
    SweepResult res;
    for (const auto & key : unreferenced)
    {
        auto it = first_unreachable.find(key);
        int64_t since = (it == first_unreachable.end()) ? now : it->second;
        if (now - since >= grace)
            res.to_delete.push_back(key);
        else
            res.first_unreachable[key] = since; /// keep ageing
    }
    return res; /// objects no longer unreferenced are dropped from first_unreachable (timer cleared)
}

}
