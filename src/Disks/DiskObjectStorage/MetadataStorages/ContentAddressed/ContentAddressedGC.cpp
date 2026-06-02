#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolScan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Reachability.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>

#include <set>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace ContentAddressed
{

ContentAddressedGC::ContentAddressedGC(ObjectStoragePtr object_storage_, std::string key_prefix_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
{
}

SweepStats ContentAddressedGC::runSweepOnce(int64_t now, int64_t grace)
{
    /// 1. The live roots: every part id named by a published ref.
    std::set<std::string> live = listLivePartIds(object_storage, key_prefix);

    /// 2. The reachable blob set, computed from the live parts' footers. B18 fail-close: a live ref
    /// whose footer is missing throws CORRUPTED_DATA so the sweep aborts WITHOUT deleting anything
    /// (a partial reachable set must never drive deletion — it would drop a live part's blobs).
    FooterResolver resolve = [this](const std::string & part_id) -> Footer
    {
        const std::string footer_key = partKey(key_prefix, part_id);
        if (!object_storage->tryGetObjectMetadata(footer_key, /*with_tags=*/false))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed GC: live ref points at missing footer {}", footer_key);
        StoredObject object(footer_key);
        auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return Footer::deserialize(bytes);
    };
    std::set<std::string> reachable_blobs = markReachableBlobs(live, resolve);

    /// 3. The full unreferenced object-key set: every footer not backing a live part, plus every
    /// blob not reachable from a live footer. Sweep scope is strictly parts/ and blobs/.
    std::set<std::string> live_part_keys;
    for (const auto & part_id : live)
        live_part_keys.insert(partKey(key_prefix, part_id));

    std::set<std::string> unreferenced;
    for (const auto & key : listKeysUnder(object_storage, partsPrefix(key_prefix)))
    {
        if (!live_part_keys.contains(key))
            unreferenced.insert(key);
    }
    for (const auto & key : listKeysUnder(object_storage, blobsPrefix(key_prefix)))
    {
        if (!reachable_blobs.contains(key))
            unreferenced.insert(key);
    }

    /// 4. Apply the grace-from-unreachability policy and carry the updated timer state forward.
    SweepResult res = selectForSweep(unreferenced, first_unreachable, now, grace);
    first_unreachable = std::move(res.first_unreachable);

    /// 5. Delete only the objects past grace. NOTHING above can have deleted anything: a throw in
    /// steps 1-2 (scan / missing footer) aborts before this point with the pool intact.
    const std::string parts_root = partsPrefix(key_prefix);
    SweepStats stats;
    StoredObjects to_remove;
    to_remove.reserve(res.to_delete.size());
    for (const auto & key : res.to_delete)
    {
        if (key.rfind(parts_root, 0) == 0)
            ++stats.deleted_parts;
        else
            ++stats.deleted_blobs;
        to_remove.emplace_back(key);
    }
    object_storage->removeObjectsIfExist(to_remove);
    return stats;
}

}

}
