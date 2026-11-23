#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <functional>

namespace DB::Cas
{

uint64_t manifestCleanupShard(const ManifestId & id, uint64_t gc_shards)
{
    /// gc_shards >= 1 is enforced by GcState decode (CORRUPTED_DATA on 0).
    /// Hash the qualified id (namespace plus all three `ManifestRef` components) using the same
    /// mixing as `std::hash<ManifestId>`. Two namespaces can legally carry the same
    /// `ManifestRef` without addressing the same object, so their cleanup work must never be
    /// merged.
    const size_t h = std::hash<ManifestId>{}(id);
    return static_cast<uint64_t>(h) % gc_shards;
}

}
