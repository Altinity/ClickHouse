#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

ShardScatter::ShardScatter(uint64_t gc_shards_)
    : gc_shards(gc_shards_), buckets(gc_shards_)
{
}

void ShardScatter::emit(const std::vector<UInt128> & blob_hashes, int sign)
{
    for (const UInt128 & h : blob_hashes)
    {
        const uint64_t s = blobShard(h, gc_shards);
        buckets[s].push_back(BlobDelta{.blob_hash = h, .delta = sign});
    }
}

void ShardScatter::scatter(const std::vector<ManifestEntry> & old_entries,
                           const std::vector<ManifestEntry> & new_entries)
{
    /// Collect Blob-placement hashes from each side.
    std::vector<UInt128> old_hashes;
    for (const ManifestEntry & e : old_entries)
        if (e.placement == EntryPlacement::Blob)
            old_hashes.push_back(e.blob_hash);

    std::vector<UInt128> new_hashes;
    for (const ManifestEntry & e : new_entries)
        if (e.placement == EntryPlacement::Blob)
            new_hashes.push_back(e.blob_hash);

    /// SabotageCrossShardDisplacement guard: if both sides have the same hashes (multiset equal),
    /// this is an owner-move — no net blob delta.  Compare sorted copies (manifests have no
    /// duplicate-path guarantee per shard scatter; the fold barrier already ensures owner moves
    /// carry the SAME ManifestRef so the blob sets ARE identical).
    std::vector<UInt128> sorted_old = old_hashes;
    std::vector<UInt128> sorted_new = new_hashes;
    std::sort(sorted_old.begin(), sorted_old.end());
    std::sort(sorted_new.begin(), sorted_new.end());
    if (sorted_old == sorted_new && !sorted_old.empty())
        return;   /// pure owner move — no blob delta

    emit(old_hashes, -1);
    emit(new_hashes, +1);
}

std::vector<std::vector<BlobDelta>> ShardScatter::take()
{
    std::vector<std::vector<BlobDelta>> result = std::move(buckets);
    buckets.assign(gc_shards, {});   /// leave in a defined (empty) state; caller must not reuse
    return result;
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
                                         uint64_t prior_generation, uint64_t new_generation,
                                         std::vector<BlobDelta> shard_deltas)
{
    std::vector<RunRef> out_runs;
    foldDeltasIntoGeneration(backend, layout, prior_generation, new_generation, shard,
                             std::move(shard_deltas), out_runs);
    return out_runs;
}

}
