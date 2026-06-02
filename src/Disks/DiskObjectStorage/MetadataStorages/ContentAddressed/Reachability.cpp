#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Reachability.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>

namespace DB::ContentAddressed
{

std::set<std::string> markReachableBlobs(
    const std::string & key_prefix, const std::set<std::string> & live_part_ids, const PartManifestResolver & resolve)
{
    std::set<std::string> reachable;
    for (const auto & id : live_part_ids)
    {
        PartManifest manifest = resolve(id);
        for (const auto & blob : manifest.blobs)
            /// `blob.second.key` is the BARE content hash; project it to the FULL blob object key
            /// (`blobKey` fan-out) so it matches the keys listed under `blobsPrefix` by the sweep.
            reachable.insert(blobKey(key_prefix, blob.second.key));
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
