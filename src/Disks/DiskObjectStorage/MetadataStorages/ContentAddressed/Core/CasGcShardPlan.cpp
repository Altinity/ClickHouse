#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <functional>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

uint64_t manifestCleanupShard(const ManifestId & id, uint64_t gc_shards)
{
    /// gc_shards >= 1 is enforced by GcState decode (CORRUPTED_DATA on 0).
    /// Hash the QUALIFIED id (namespace + all three ManifestRef components) using the same mixing
    /// as `std::hash<ManifestId>`. Routing by `ManifestRef` alone would be the modeled
    /// `SabotageKeyByRefNotId` hazard: two namespaces can legally carry the same `ManifestRef`
    /// without addressing the same object, so their cleanup work must never be merged.
    const size_t h = std::hash<ManifestId>{}(id);
    return static_cast<uint64_t>(h) % gc_shards;
}

ShardReducer::ShardReducer(uint64_t shard_, uint64_t gc_shards_)
    : shard(shard_), gc_shards(gc_shards_)
{
    if (gc_shards_ == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "ShardReducer: gc_shards must be >= 1");
    if (shard_ >= gc_shards_)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "ShardReducer: shard {} is out of range [0, {})", shard_, gc_shards_);
}

bool ShardReducer::owns(const UInt128 & blob_hash) const
{
    return blobShard(blob_hash, gc_shards) == shard;
}

std::vector<RunRef> ShardReducer::reduce(Backend & backend, const Layout & layout,
                                         uint64_t prior_generation, uint64_t prior_attempt,
                                         uint64_t new_generation, uint64_t attempt,
                                         std::vector<BlobDelta> shard_deltas)
{
    std::vector<RunRef> out_runs;
    foldDeltasIntoGeneration(backend, layout, prior_generation, prior_attempt, new_generation, attempt, shard,
                             std::move(shard_deltas), out_runs);
    return out_runs;
}

}
