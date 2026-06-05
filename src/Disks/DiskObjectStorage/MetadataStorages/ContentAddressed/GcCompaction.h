#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// CA GC S2 — the streaming, log-structured epoch COMPACTION (spec §5, §5.1, §9).
///
/// The reverse index (key -> referrer count) is log-structured like an LSM tree: a durable sorted SNAPSHOT
/// run (`gc/snap/<padded-epoch>.<shard>`) plus a tail of small `+`/`-` DELTA objects appended on commit/drop
/// under `gc/log/<epoch>.<shard>/`. Compacting one GC epoch, per shard:
///
///   1. CLOSE the epoch — a plain fenced PUT `gc/current_epoch/<shard> = E+1` (the per-shard epoch has a
///      SINGLE writer, the shard's GC leader, so the existing fence lease serializes it; NO CAS — §5.1
///      rule 1). Only AFTER the close do we LIST + fold E, so a writer that read E either lands its `+` in E
///      (folded here) or re-appends into E+1 (§5.1 rule 2). The LIST therefore sees a stable, complete epoch.
///   2. LIST `gc/log/E.<shard>/`, decode every coalesced delta object, and EXTERNAL-SORT the deltas by key
///      (the unified `(kind, identity)` count key — a `(H)` pin or the `(part_id)` edge). Memory is bounded
///      by the merge frontier, not the blob count: large frontiers spill the sorted delta run to scratch.
///   3. Streaming MERGE-JOIN the sorted deltas against the sorted snapshot run: walk both inputs in lockstep,
///      DEDUP by `event_id` while folding (a re-appended duplicate collapses to one count — §5.1), sum counts
///      per key, STREAM the new `gc/snap/<E+1>.<shard>` as you go, and EMIT any key whose running count
///      reaches 0 as a CANDIDATE in the same pass. Candidates fall out of the merge — no decrement-to-zero
///      queue.
///   4. After the new snapshot lands, RECLAIM the old `gc/snap/E.<shard>` + every `gc/log/E.<shard>/` object
///      (the epoch was already closed in step 1). Idempotent under a leader crash: a half-advanced epoch
///      re-runs cleanly (the PUTs are idempotent; a missing old object is a no-op).
///
/// SAFETY: the compaction NEVER deletes a blob/part itself. It only emits count-0 CANDIDATES; the caller
/// (`ContentAddressedGC::runSweepOnce`) still applies the unchanged `grace` ageing, the fence guard, the
/// re-validate-under-lock, and the existing `removeObjectsIfExist` delete primitive. The full `parts/`+
/// `blobs/` scan survives as the explicit reconciliation fallback. The under-count danger (a live key
/// emitted as a count-0 candidate) is contained by I1/I6 (`+` before ref) + grace + the reconciliation
/// fallback + the S1 drift validator cross-check.
class GcCompaction
{
public:
    /// `padded_epoch_width` only mirrors gcSnapKey's zero-pad so the snapshot LIST is in numeric order; it
    /// is not configurable here (it follows the layout). The compaction is constructed per pool over the
    /// SAME object storage + key prefix the GC sweep holds, so the read and write sides cannot disagree.
    GcCompaction(ObjectStoragePtr object_storage_, std::string key_prefix_);

    /// A unified count key: either a blob `(H)` pin or the `(part_id)` edge (§9). Both identities are
    /// lowercase-hex digests, so the `kind` byte keeps the two key spaces from colliding while the sorted
    /// merge treats them uniformly. Sorts by (kind, identity).
    enum class KeyKind : uint8_t
    {
        Blob = 1, /// a content blob `(H)` — the candidate's full key is blobKey(key_prefix, identity)
        Part = 2, /// a part-manifest `(part_id)` edge — the candidate's full key is partKey(key_prefix, identity)
    };

    struct CountKey
    {
        KeyKind kind = KeyKind::Blob;
        std::string identity; /// the bare hex digest (BlobHash or PartId string)

        auto operator<=>(const CountKey &) const = default;
        bool operator==(const CountKey &) const = default;
    };

    /// One emitted count-0 candidate: the unified key plus its FULL object key in the pool layout (the
    /// caller compares this directly against the parts/ + blobs/ keyspace, exactly as the legacy scan did).
    struct Candidate
    {
        CountKey key;
        std::string object_key; /// blobKey(...).string() or partKey(...).string()
    };

    /// The result of folding one shard's epoch.
    struct CompactionResult
    {
        uint64_t folded_epoch = 0; /// the epoch E that was closed and folded
        uint64_t new_epoch = 0; /// E + 1 (the now-open epoch)
        std::vector<Candidate> candidates; /// every key whose running count reached 0
        /// The folded snapshot (key -> count) — the authoritative reverse index after this epoch. Exposed
        /// so the caller's S1 drift validator can cross-check the in-memory pre-filter against it.
        std::map<CountKey, int64_t> folded_counts;
    };

    /// Compact ONE shard's currently-open epoch end-to-end (close -> list -> sort -> merge-join -> advance).
    /// Returns the fold result (candidates + folded counts). Reads `gc/current_epoch/<shard>` to learn the
    /// open epoch E. SAFETY: the caller must hold the fence (it passes whether it still leads via
    /// `fence_still_mine`); the close PUT and the reclaim are skipped if leadership was lost, so a paused
    /// leader never advances an epoch under a successor.
    CompactionResult compactShard(ShardId shard, const std::function<bool()> & fence_still_mine);

    /// The result of a §9 rebuild / catch-up fold (read-only — it neither advances the epoch nor reclaims).
    struct RebuildResult
    {
        /// The reverse index (positive counts only) recomputed from snapshot + un-folded log, byte-equivalent
        /// to what the next fold's `folded_counts` would hold. Seeds / validates the leader's reverse view.
        std::map<CountKey, int64_t> counts;
        /// Keys already at count 0 in the rebuilt view (every reference netted away) — the candidates a
        /// catch-up leader can hand straight to the grace + re-validate + delete tail without re-folding.
        std::vector<Candidate> candidates;
    };

    /// §9 rebuild / catch-up: recompute the reverse counts for a shard from the LATEST `gc/snap` run plus
    /// every un-folded `gc/log/<epoch>.<shard>/` (epochs at or after the snapshot's), WITHOUT any blob
    /// `LIST` (the snapshot + log are sufficient — spec §9). Used on leader startup / catch-up to rebind the
    /// reverse index after a restart. READ-ONLY: it does not close, advance, reclaim, or write the snapshot.
    /// Returns nullopt iff BOTH the snapshot and the log are entirely absent (total log+snap loss) — the
    /// caller then falls back to the heavy `runReconciliationScan` (which rebinds reachability from live
    /// refs + manifests, the only source left when the log truth is gone).
    std::optional<RebuildResult> rebuildFromSnapshotAndLog(ShardId shard);

private:
    /// Read the current open epoch for a shard (gcCurrentEpochKey, default 0 if absent or malformed —
    /// degrading to 0 only widens the window the fold covers, never an under-count).
    uint64_t readShardEpoch(ShardId shard) const;

    /// Plain fenced PUT gcCurrentEpochKey(shard) = epoch (§5.1 rule 1, single-writer-per-shard, no CAS).
    void writeShardEpoch(ShardId shard, uint64_t epoch);

    /// Decode a sorted (key -> count) run from the snapshot object for (epoch, shard). Returns an empty map
    /// if the snapshot is absent (epoch 0 has no predecessor snapshot — the count base is empty).
    std::map<CountKey, int64_t> readSnapshot(uint64_t epoch, ShardId shard) const;

    /// Serialize + PUT the sorted (key -> count) run as gcSnapKey(epoch, shard). Only keys with a POSITIVE
    /// count are persisted (a 0/negative count is a reclaimed or net-zero key and carries no live reference,
    /// so it does not belong in the durable reverse index — and re-persisting it would let it linger).
    void writeSnapshot(uint64_t epoch, ShardId shard, const std::map<CountKey, int64_t> & counts);

    /// LIST + decode every coalesced delta object under gcLogPrefix(epoch, shard). The returned deltas are
    /// NOT yet sorted or deduped (the merge-join does both). `event_id` is preserved per delta for dedup.
    std::vector<GcDelta> listAndDecodeDeltas(uint64_t epoch, ShardId shard) const;

    /// Reclaim (delete) the old snapshot + every log object for a folded, already-closed epoch.
    void reclaimFoldedEpoch(uint64_t epoch, ShardId shard);

    /// The latest snapshot epoch present for a shard (the highest <padded-epoch> under gc/snap/ whose shard
    /// suffix matches). Returns nullopt if no snapshot exists for the shard. Used by rebuild.
    std::optional<uint64_t> latestSnapshotEpoch(ShardId shard) const;

    /// The set of log epochs present for a shard (every <epoch> under gc/log/<epoch>.<shard>/), sorted
    /// ascending. Used by rebuild to fold every un-folded epoch over the snapshot.
    std::vector<uint64_t> logEpochs(ShardId shard) const;

    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
};

}
