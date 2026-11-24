#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace DB::Cas
{

/// Blob-target shard for a blob identity.
///
/// Deterministic and total over all hashes: the blob-target sharding axis uses the high 64 bits of
/// the 128-bit blob hash modulo `gc_shards`. The high bits are taken rather than the low bits so
/// that blobs whose hashes differ only in the low 64 bits — an adversarial corner case — still
/// spread across shards. `CityHash128` output has high entropy in BOTH halves, so both choices
/// are equivalent for organic workloads; high bits are the documented canonical choice.
///
/// Properties:
///   - Deterministic/stable: same inputs always yield the same shard.
///   - Total: result is in [0, gc_shards).
///   - Single-shard equivalence: gc_shards == 1 always returns 0.
///
/// The argument is the complete `BlobRef`, so callers cannot silently discard its algorithm while
/// routing. The shard number deliberately uses only `ref.digest`: distribution comes from the
/// digest bytes, while the algorithm remains part of the identity carried through the GC pipeline.
inline uint64_t blobShard(const BlobRef & ref, uint64_t gc_shards)
{
    /// gc_shards >= 1 is enforced by GcState decode (CORRUPTED_DATA on 0).
    /// BE-u64 of bytes[0:8] — an EXPLICIT big-endian read, bit-identical to the old
    /// `static_cast<uint64_t>(blob_hash >> 64)` for every 128-bit digest (`fromU128` writes the
    /// UInt128 big-endian into bytes[0:16], so bytes[0:8] IS the old high 64 bits). MUST stay an
    /// explicit big-endian read, never a native-endian memcpy (would silently reshard on an LE host).
    uint64_t high64 = 0;
    for (int i = 0; i < 8; ++i)
        high64 = (high64 << 8) | ref.digest.bytes[static_cast<size_t>(i)];
    return high64 % gc_shards;
}

/// Route a part-manifest cleanup bundle to a worker by its namespace-qualified `ManifestId`. Workers
/// own disjoint ranges; routing by `ManifestRef` alone would merge cleanup work from two namespaces
/// that happen to reuse the same reference components. `gc_shards == 1` routes every `ManifestId`
/// to owner shard 0.
///
/// The hash mixes both the `root_namespace` string and the three `ManifestRef` components — the
/// same mixing used by `std::hash<ManifestId>`. Two `ManifestId`s that share the same `ManifestRef`
/// but carry different namespaces produce independent hash values and may route to different shards.
///
/// Properties:
///   - Deterministic/stable: same inputs always yield the same shard.
///   - Total: result is in [0, gc_shards).
///   - Single-shard equivalence: gc_shards == 1 always returns 0.
uint64_t manifestCleanupShard(const ManifestId & id, uint64_t gc_shards);

/// Role split of a sharded GC round (`gc_shards > 1`):
///
///   - COORDINATOR (exactly one per round — the lease holder): owns input-seal, round-visibility,
///     the single GLOBAL fence (over all LIST-discovered shards), and generation-advance. These
///     steps span the whole fence universe and must NOT be sharded: a publish into one root shard
///     can protect a blob assigned to ANY target shard, so an independent per-reducer fence is
///     unsafe. `Gc::fence` therefore stays the single coordinator fence over the entire universe.
///
///   - REDUCERS / CLEANUP WORKERS (one per disjoint shard): own ONLY their shard's blob-target reduce
///     (`foldDeltasIntoGeneration` for that shard) or part-manifest cleanup (`manifestCleanupShard`).
///     Their key namespaces are disjoint, so two replicas may reduce DIFFERENT shards concurrently.
///     Reducer work needs NO lease: the lease is work-dedup only (see `CasGcScheduler`), not a
///     coordination primitive.

}
