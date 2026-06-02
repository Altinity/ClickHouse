#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/BlobRefIndex.h>

namespace DB::ContentAddressed
{

void InMemoryBlobRefIndex::addPart(const std::string & part_id, const Footer & footer)
{
    if (!applied_parts.insert(part_id).second)
        return; /// idempotent: this part's refs are already counted

    for (const auto & blob : footer.blobs)
        counts[blob.second.key] += 1;
}

void InMemoryBlobRefIndex::removePart(const std::string & part_id, const Footer & footer)
{
    if (applied_parts.erase(part_id) == 0)
        return; /// this part was not applied

    for (const auto & blob : footer.blobs)
    {
        auto it = counts.find(blob.second.key);
        if (it != counts.end() && --it->second <= 0)
            it->second = 0;
    }
}

int64_t InMemoryBlobRefIndex::refcount(const std::string & blob_key) const
{
    auto it = counts.find(blob_key);
    return it == counts.end() ? 0 : it->second;
}

std::set<std::string> InMemoryBlobRefIndex::unreferenced() const
{
    std::set<std::string> result;
    for (const auto & item : counts)
        if (item.second <= 0)
            result.insert(item.first);
    return result;
}

}
