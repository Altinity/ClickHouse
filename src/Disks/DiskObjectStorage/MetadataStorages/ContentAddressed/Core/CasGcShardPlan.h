#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
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

/// Per-shard in-degree reducer for phase 4 of the sharded GC fold (spec §Phase4).
///
/// `ShardReducer` owns exactly ONE target shard (`shard` in [0, `gc_shards`)). It accepts the
/// shard's slice of blob deltas (from `ShardScatter::take()[shard]`) and merges them into a
/// per-shard `CasBlobInDegree` generation run via `foldDeltasIntoGeneration`.
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

    /// True iff this reducer owns `blob_hash` — i.e. `blobShard(blob_hash, gc_shards) == shard`.
    bool owns(const UInt128 & blob_hash) const;

    /// Merge `shard_deltas` (the `ShardScatter::take()[shard]` slice for this shard) into a
    /// new in-degree generation for this shard. Writes the sealed run under
    /// `blobTargetRunKey(new_generation, shard, 0)` via `backend`, appends its `RunRef` to
    /// `out_runs`, and returns the `RunRef`. The call is idempotent (write-once via `putIfAbsent`).
    ///
    /// `prior_generation` is the generation whose per-shard run forms the baseline (0 = fresh pool).
    /// `new_generation` must be > `prior_generation`.
    ///
    /// PRECONDITION: every `BlobDelta` in `shard_deltas` must be owned by this reducer
    /// (`blobShard(d.blob_hash, gc_shards) == shard`).  Violations are caught at the
    /// `foldDeltasIntoGeneration` layer (an undercount would be CORRUPTED_DATA).
    std::vector<RunRef> reduce(Backend & backend, const Layout & layout,
                               uint64_t prior_generation, uint64_t new_generation, uint64_t attempt,
                               std::vector<BlobDelta> shard_deltas);

private:
    uint64_t shard;
    uint64_t gc_shards;
};

/// Coordinator policy for the sharded GC round (spec §Phase4, §Sharding Model).
///
/// In a sharded round (`gc_shards > 1`) the work splits into two roles:
///
///   - COORDINATOR (exactly one per round — the lease holder): owns registry-fence, input-seal,
///     round-visibility, the single GLOBAL fence, and generation-advance. These steps span the whole
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
