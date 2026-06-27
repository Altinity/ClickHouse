#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <vector>

namespace DB::Cas
{

/// Blob target shard for a blob hash (spec §Sharding Model).
///
/// Deterministic and total over all hashes: the blob-target sharding axis uses the high 64 bits of
/// the 128-bit blob hash modulo `gc_shards`. The high bits are taken rather than the low bits so
/// that blobs whose hashes differ only in the low 64 bits — an adversarial corner case — still
/// spread across shards. `CityHash128` output has high entropy in BOTH halves, so both choices
/// are equivalent for organic workloads; high bits are the documented canonical choice.
///
/// NOTE: this is a SEPARATE sharding axis from the root-shard axis used by `Store::shardOf` and
/// the test helper `shardOfForTest` (which applies `CityHash64(ref_name) % root_shards` to
/// string ref names). The two axes are intentionally different: root shards partition the owner
/// journal by ref name; blob-target shards partition the in-degree reducer work by blob hash.
///
/// Properties:
///   - Deterministic/stable: same inputs always yield the same shard.
///   - Total: result is in [0, gc_shards).
///   - Single-shard equivalence: gc_shards == 1 always returns 0.
inline uint64_t blobShard(const UInt128 & blob_hash, uint64_t gc_shards)
{
    /// gc_shards >= 1 is enforced by GcState decode (CORRUPTED_DATA on 0).
    /// The high 64 bits of the UInt128 are the upper half: (v >> 64) narrowed to uint64_t.
    const uint64_t high64 = static_cast<uint64_t>(blob_hash >> 64);
    return high64 % gc_shards;
}

/// Accumulates per-shard `BlobDelta` lists from a stream of `RootOwnerEvent` blob-edge dispatches.
///
/// `ScatterScatter` partitions incoming `+1` (new-binding) and `-1` (old-binding) blob deltas into
/// `gc_shards` buckets keyed by `blobShard(blob_hash, gc_shards)`. The caller drives it by calling
/// `scatter` for every `RootOwnerEvent`'s blob-edge sets (in fold order), then calls `take` once to
/// drain the accumulated buckets into the per-shard `std::vector<BlobDelta>` vectors consumed by
/// `foldDeltasIntoGeneration`.
///
/// OWNER-MOVE DEFENSE (SabotageCrossShardDisplacement): when `old_blob_hashes` and `new_blob_hashes`
/// are both non-empty and equal (a promote at the same `ManifestRef`), NO deltas are emitted — the
/// event is a pure owner move that carries no blob changes. The fold barrier in `CasGc::fold` already
/// enforces this at the event level; `ShardScatter` applies the same rule defensively.
///
/// A `ShardScatter` is one-shot: `take` drains the internal state; calling `scatter` after `take`
/// is undefined behavior (the caller must construct a fresh instance per fold generation).
class ShardScatter
{
public:
    explicit ShardScatter(uint64_t gc_shards_);

    /// Emit `sign * +1` deltas for each hash in `blob_hashes` into the per-shard buckets. `sign`
    /// must be +1 (new-binding activation) or -1 (old-binding removal). Hashes are routed by
    /// `blobShard(hash, gc_shards)`.
    void emit(const std::vector<UInt128> & blob_hashes, int sign);

    /// Scatter one `RootOwnerEvent`'s old- and new-binding blob edge sets. Extracts blob hashes from
    /// `Blob`-placement `ManifestEntry` lists and emits `-1` / `+1` deltas respectively, UNLESS
    /// both sides refer to equal hashes in an owner-move event (SabotageCrossShardDisplacement guard:
    /// equal old/new sets produce no net delta).
    ///
    /// Callers must pass the ALREADY-DECODED entry lists — this function has no I/O and no backend
    /// access. The fold in `CasGc::fold` reads the manifest bodies before dispatching here; the
    /// reducer tests (Task 4) wire up the full round.
    void scatter(const std::vector<ManifestEntry> & old_entries,
                 const std::vector<ManifestEntry> & new_entries);

    /// Drain the accumulated per-shard delta vectors. The returned vector has exactly `gc_shards`
    /// elements (one `std::vector<BlobDelta>` per target shard, index == shard number). After
    /// `take` the internal buckets are empty; the object must not be used again.
    std::vector<std::vector<BlobDelta>> take();

private:
    uint64_t gc_shards;
    std::vector<std::vector<BlobDelta>> buckets;   /// buckets[shard] accumulates BlobDelta items
};

}
