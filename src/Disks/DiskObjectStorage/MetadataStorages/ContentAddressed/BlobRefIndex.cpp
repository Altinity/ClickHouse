#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/BlobRefIndex.h>

namespace DB::ContentAddressed
{

void InMemoryBlobRefIndex::addPart(const PartId & part_id, const PartManifest & manifest)
{
    if (!applied_parts.insert(part_id).second)
        return; /// idempotent: this part's refs are already counted

    for (const auto & blob : manifest.blobs)
        counts[blob.second.key] += 1;
}

void InMemoryBlobRefIndex::removePart(const PartId & part_id, const PartManifest & manifest)
{
    if (applied_parts.erase(part_id) == 0)
        return; /// this part was not applied

    for (const auto & blob : manifest.blobs)
    {
        auto it = counts.find(blob.second.key);
        if (it != counts.end() && --it->second <= 0)
            it->second = 0;
    }
}

int64_t InMemoryBlobRefIndex::refcount(const BlobHash & blob_hash) const
{
    auto it = counts.find(blob_hash);
    return it == counts.end() ? 0 : it->second;
}

std::set<BlobHash> InMemoryBlobRefIndex::unreferenced() const
{
    std::set<BlobHash> result;
    for (const auto & item : counts)
        if (item.second <= 0)
            result.insert(item.first);
    return result;
}

}
