#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace DB::ContentAddressed
{

/// Seam (B9): the delta refcount over content-addressed blob hashes.
/// M1 ships InMemoryBlobRefIndex; a RocksDB-backed impl plugs in here unchanged.
class IBlobRefIndex
{
public:
    virtual ~IBlobRefIndex() = default;
    virtual void addPart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual void removePart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual int64_t refcount(const BlobHash & blob_hash) const = 0;
    virtual std::set<BlobHash> unreferenced() const = 0;
};

class InMemoryBlobRefIndex : public IBlobRefIndex
{
public:
    void addPart(const PartId & part_id, const PartManifest & manifest) override;
    void removePart(const PartId & part_id, const PartManifest & manifest) override;
    int64_t refcount(const BlobHash & blob_hash) const override;
    std::set<BlobHash> unreferenced() const override;

private:
    std::unordered_map<BlobHash, int64_t> counts;
    std::unordered_set<PartId> applied_parts; /// idempotency guard for add/remove
};

}
