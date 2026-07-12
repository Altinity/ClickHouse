#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <functional>
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
/// NOTE: this is a SEPARATE sharding axis from the root-shard axis used by `Store::shardOf` (which
/// applies `CityHash64(ref_name) % root_shards` to string ref names). The two axes are intentionally
/// different: root shards partition the ref-log by ref name; blob-target shards partition the
/// in-degree reducer work by blob hash.
///
/// Properties:
///   - Deterministic/stable: same inputs always yield the same shard.
///   - Total: result is in [0, gc_shards).
///   - Single-shard equivalence: gc_shards == 1 always returns 0.
///
/// Takes a `BlobRef` (Phase 3 T3 — widened from the bare `BlobDigest` overload) so the identity
/// pair travels through the sharding axis too, even though the shard number ITSELF is derived from
/// `ref.digest` alone (deliberately ignoring `ref.algo`): distribution comes from the digest bytes,
/// and taking the whole `BlobRef` prevents a caller from silently discarding the algo half of the
/// identity before routing.
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
/// own disjoint ranges; routing by `ManifestRef` alone would merge two namespaces' cleanup work
/// (Phase 0 `SabotageKeyByRefNotId`). `gc_shards == 1` routes every `ManifestId` to owner shard 0.
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

/// Per-shard in-degree reducer for phase 4 of the sharded GC fold (spec §Phase4).
///
/// `ShardReducer` owns exactly ONE target shard (`shard` in [0, `gc_shards`)). It accepts the
/// caller's per-shard slice of `BlobDelta`s — produced by `foldManifestEdges` and bucketed by
/// `blobShard` — and merges them into a per-shard `CasBlobInDegree` generation run via
/// `foldDeltasIntoGeneration`.
///
/// Ownership invariant: a reducer touches ONLY blobs it owns — i.e. `blobShard(h, gc_shards) == shard`.
/// Two reducers for DIFFERENT shards may run concurrently; their key namespaces are disjoint
/// (`blobTargetRunKey(gen, shard0, seq)` vs `blobTargetRunKey(gen, shard1, seq)`).
///
/// The `reduce` method delegates to `foldDeltasIntoGeneration` (the same path the non-sharded fold
/// uses with `shard == 0`), so `gc_shards == 1` with `shard == 0` reproduces the non-sharded fold
/// byte-for-byte (Task 7 compatibility requirement).
///
/// NOTE on durable writes: `reduce` writes the per-shard in-degree run directly via `backend`
/// (under `blobTargetRunKey(new_generation, shard, 0)`), exactly as `foldDeltasIntoGeneration`
/// does. Returning the durable write here (rather than an in-memory map) keeps the round driver
/// stateless: it simply constructs a `ShardReducer` per shard, calls `reduce`, and the sealed
/// run is already present for `zeroInDegree` / `inDegreeInGeneration` consumers. An in-memory
/// return value is unnecessary because the backend is directly queryable; tests use
/// `inDegreeInGeneration` over an `InMemoryBackend`.
class ShardReducer
{
public:
    /// Construct a reducer that owns `shard` (in [0, `gc_shards`)).
    ShardReducer(uint64_t shard_, uint64_t gc_shards_);

    /// True iff this reducer owns `ref` — i.e. `blobShard(ref, gc_shards) == shard`.
    bool owns(const BlobRef & ref) const;

    /// Merge `shard_deltas` (the caller's per-shard `BlobDelta` slice produced by `foldManifestEdges`
    /// and bucketed by `blobShard`) into a new in-degree generation for this shard. Writes the sealed
    /// run under `blobTargetRunKey(new_generation, shard, 0)` via `backend`, appends its `RunRef` to
    /// `out_runs`, and returns the `RunRef`. The call is idempotent (write-once via `putIfAbsent`).
    ///
    /// `prior_runs` are the parent generation's run segments for this shard, resolved BY THE CALLER from
    /// the parent fold seal's `blob_target_runs` filtered to `shard` (2026-07-02 T0). An empty vector is
    /// the fresh-pool / empty baseline.
    ///
    /// PRECONDITION: every `BlobDelta` in `shard_deltas` must be owned by this reducer
    /// (`blobShard(d.ref, gc_shards) == shard`). This is a caller contract; there is no
    /// underflow throw backstopping it — pass a misbucketed delta and the fold silently misroutes it.
    std::vector<RunRef> reduce(Backend & backend, const Layout & layout,
                               const std::vector<RunRef> & prior_runs,
                               uint64_t new_generation, uint64_t attempt,
                               std::vector<BlobDelta> shard_deltas,
                               uint64_t current_round = 0, uint64_t condemn_round = 0,
                               const std::function<std::optional<HeadResult>(const BlobRef &)> & head_blob = {},
                               const std::function<std::optional<HeadResult>(const BlobRef &)> & peek_head = {},
                               RetiredMergeResult * out_retired = nullptr,
                               bool suppress_destructive = false);

private:
    uint64_t shard;
    uint64_t gc_shards;
};

/// Coordinator policy for the sharded GC round (spec §Phase4, §Sharding Model).
///
/// In a sharded round (`gc_shards > 1`) the work splits into two roles:
///
///   - COORDINATOR (exactly one per round — the lease holder): owns input-seal, round-visibility,
///     the single GLOBAL fence (over all LIST-discovered shards), and generation-advance. These steps span the whole
///     fence universe and must NOT be sharded: a publish into one root shard can protect blobs in ANY
///     target shard, so an independent per-reducer fence is unsafe (Task 1 `SabotageReducerOwnsFence`).
///     `Gc::fence` therefore stays the single coordinator fence over the entire universe.
///
///   - REDUCERS / CLEANUP WORKERS (one per disjoint shard): own ONLY their shard's blob-target reduce
///     (`ShardReducer`) or part-manifest cleanup (`manifestCleanupShard`). Their key namespaces are
///     disjoint, so two replicas may reduce DIFFERENT shards concurrently. Reducer work needs NO lease:
///     the lease is work-dedup only (see `CasGcScheduler`), not a coordination primitive.
///
/// `CoordinatorPlan` is a tiny policy object that encodes these invariants for callers and tests; it
/// holds no durable state and performs no I/O. The booleans are constant for any `gc_shards >= 1`.
class CoordinatorPlan
{
public:
    explicit CoordinatorPlan(uint64_t gc_shards_) : gc_shards(gc_shards_) {}

    /// One global fence covers the whole fence universe — never one fence per shard.
    bool hasSingleGlobalFence() const { return true; }

    /// A per-reducer fence is unsafe (cross-shard protection); reducers never fence on their own.
    bool allowsPerShardFence() const { return false; }

    /// Reducer work is lease-free: the lease is work-dedup only, not a reduce gate.
    bool requiresLeaseForReduce() const { return false; }

    /// Input-seal (round visibility) is a coordinator-only step.
    bool requiresCoordinatorForSeal() const { return true; }

    /// The global fence is a coordinator-only step.
    bool requiresCoordinatorForFence() const { return true; }

    uint64_t shardCount() const { return gc_shards; }

private:
    uint64_t gc_shards;
};

}
