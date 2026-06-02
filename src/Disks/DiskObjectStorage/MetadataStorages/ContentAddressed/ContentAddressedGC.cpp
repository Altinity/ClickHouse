#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
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

namespace
{

std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key)
{
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

}

/// ==== Pool enumeration (was PoolScan) ====

PartId partIdFromRefPayload(const std::string & payload)
{
    size_t begin = payload.find_first_of("0123456789abcdef");
    if (begin == std::string::npos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: ref payload has no part id");
    size_t end = payload.find_first_not_of("0123456789abcdef", begin);
    return PartId(payload.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
}

std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix)
{
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    std::vector<std::string> keys;
    keys.reserve(children.size());
    for (const auto & child : children)
        keys.push_back(child->relative_path);
    return keys;
}

std::set<PartId> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix)
{
    /// Every server's/table's refs live under refsRootPrefix. The same root also holds verbatim
    /// table-level files under the store/server/uuid/files/ layout, so we keep only keys whose path has a
    /// /refs/ segment (the ref objects); their payload is the live part id.
    static const std::string refs_segment = "/refs/";
    std::set<PartId> live;
    for (const auto & key : listKeysUnder(object_storage, refsRootPrefix(key_prefix)))
    {
        if (key.find(refs_segment) == std::string::npos)
            continue;
        live.insert(partIdFromRefPayload(readSmallObject(object_storage, key)));
    }
    return live;
}

/// ==== Pure reachability/sweep algorithms (was Reachability) ====

std::set<BlobObjectKey> markReachableBlobs(
    const std::string & key_prefix, const std::set<PartId> & live_part_ids, const PartManifestResolver & resolve)
{
    std::set<BlobObjectKey> reachable;
    for (const auto & id : live_part_ids)
    {
        PartManifest manifest = resolve(id);
        for (const auto & blob : manifest.blobs)
            /// `blob.second.key` is the BARE content hash (BlobHash); project it to the FULL blob
            /// object key (`blobKey` fan-out) so it matches the keys listed under `blobsPrefix`.
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

/// ==== Un-wired refcount seam (was BlobRefIndex) ====
///
/// NOTE: BlobRefIndex is an un-wired future seam (B9), not on the M1 GC path.

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

/// ==== Sweep driver ====

ContentAddressedGC::ContentAddressedGC(ObjectStoragePtr object_storage_, std::string key_prefix_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
{
}

SweepStats ContentAddressedGC::runSweepOnce(int64_t now, int64_t grace)
{
    /// 1. The live roots: every part id named by a published ref.
    std::set<PartId> live = listLivePartIds(object_storage, key_prefix);

    /// 2. The reachable blob set, computed from the live parts' manifests. B18 fail-close: a live ref
    /// whose manifest is missing throws CORRUPTED_DATA so the sweep aborts WITHOUT deleting anything
    /// (a partial reachable set must never drive deletion — it would drop a live part's blobs).
    PartManifestResolver resolve = [this](const PartId & part_id) -> PartManifest
    {
        const PartObjectKey manifest_key = partKey(key_prefix, part_id);
        if (!object_storage->tryGetObjectMetadata(manifest_key.string(), /*with_tags=*/false))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed GC: live ref points at missing manifest {}", manifest_key.string());
        StoredObject object(manifest_key.string());
        auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return PartManifest::deserialize(bytes);
    };
    /// Reachable is a std::set<BlobObjectKey>: it can only be tested against listed blobs after those
    /// are wrapped into BlobObjectKey too, so a bare-hash-vs-object-key mismatch cannot compile.
    std::set<BlobObjectKey> reachable_blobs = markReachableBlobs(key_prefix, live, resolve);

    /// 3. The full unreferenced object-key set: every manifest not backing a live part, plus every
    /// blob not reachable from a live manifest. Sweep scope is strictly parts/ and blobs/. This is the
    /// ONE well-marked boundary where the raw listed strings are wrapped into their typed object keys.
    std::set<PartObjectKey> live_part_keys;
    for (const auto & part_id : live)
        live_part_keys.insert(partKey(key_prefix, part_id));

    std::set<std::string> unreferenced;
    for (const auto & key : listKeysUnder(object_storage, partsPrefix(key_prefix)))
    {
        if (!live_part_keys.contains(PartObjectKey(key)))
            unreferenced.insert(key);
    }
    for (const auto & key : listKeysUnder(object_storage, blobsPrefix(key_prefix)))
    {
        if (!reachable_blobs.contains(BlobObjectKey(key)))
            unreferenced.insert(key);
    }

    /// 4. Apply the grace-from-unreachability policy and carry the updated timer state forward.
    SweepResult res = selectForSweep(unreferenced, first_unreachable, now, grace);
    first_unreachable = std::move(res.first_unreachable);

    /// 5. Delete only the objects past grace. NOTHING above can have deleted anything: a throw in
    /// steps 1-2 (scan / missing manifest) aborts before this point with the pool intact.
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
