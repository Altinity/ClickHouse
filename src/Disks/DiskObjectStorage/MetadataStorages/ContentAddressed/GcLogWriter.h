#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// CA GC S2 — the coalesced gc/log delta WRITER (spec §5 "batching is a requirement", §5.1 epoch
/// protocol). Owned PER POOL by the metadata storage and shared with every transaction commit/drop.
///
/// A commit/drop hands the writer ONE logical `GcDelta` (op, event_id, part_id, ALL the part's resolved
/// blob pins). The writer:
///   1. SPLITS the delta by hash-prefix shard (shardForHash per pin) so each shard's log holds only its
///      own blobs; the `(part_id) edge` (§9) is buffered into the PART's home shard (shardForPartId), so
///      a manifest reference is counted in exactly one shard. The same `event_id` is reused across shards
///      (it is per (part_id, op)) — dedup is per-shard-per-fold, so reuse is correct.
///   2. Stamps each shard fragment with the shard's CURRENT epoch (read from gcCurrentEpochKey, default 0)
///      — §5.1 rule 2 (writer epoch read).
///   3. Buffers the fragments grouped by (shard, epoch) and, on a short size/time WINDOW, flushes ONE
///      coalesced object per (shard, window) to gcLogEventKey — group-commit, NOT one object per commit.
///   4. After a flush, RE-READS each shard's epoch; if it advanced past the one written, RE-APPENDS the
///      SAME logical fragment (same event_id) into the now-open epoch (bounded retry) — §5.1 rule 2. The
///      orphaned append in the closed epoch is a harmless leaked object (deduped by event_id on fold).
///
/// CA GC S4 (G1): the commit-vs-sweep `gc_lock` is DROPPED, so a GC epoch close CAN race a concurrent
/// append — rule 4's re-append path is now LIVE and is what makes the log complete under concurrent
/// appends (the §5.1 rule-2 carrier the lockless handshake relies on).
///
/// `cas_log_batch_size` (deltas per flushed object) is exposed so a later op-budget test can assert that
/// a burst of N commits coalesces into ⌈N/window⌉ objects, not N.
class GcLogWriter
{
public:
    /// `flush_max_deltas` / `flush_window` bound the group-commit window: a (shard, epoch) buffer flushes
    /// when it reaches `flush_max_deltas` fragments OR `flush_window` elapses since its first buffered
    /// fragment (whichever first). For S2 under the held lock the synchronous-flush-before-ref discipline
    /// (see flushShardForCommit) is what preserves I1/I6; the window is a load-shedding lever for the
    /// drop/async side.
    GcLogWriter(
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        size_t flush_max_deltas_ = 256,
        std::chrono::milliseconds flush_window_ = std::chrono::milliseconds(200));

    /// Enqueue one logical delta. Splits it by shard, stamps each fragment with the shard's current epoch,
    /// and buffers the fragments. Does NOT necessarily flush (group-commit) — call flushDueWindows or
    /// flushAll to push buffered fragments out. Returns the set of shards this delta touched.
    std::vector<ShardId> enqueue(const GcDelta & delta);

    /// I1/I6 commit-path helper. Enqueue the `+` AND synchronously flush every shard it touched BEFORE the
    /// caller writes the live ref, so `+ before ref` holds (for S2, with the lock held, a synchronous
    /// flush before the ref is the accepted discipline — the async/session-covers-gap path is S4). After
    /// the flush this also runs the §5.1 rule-2 re-append on each touched shard.
    ///
    /// CA GC S4 (§5.1 rule 3): returns the `(shard, epoch)` each fragment durably settled in AFTER the
    /// rule-2 re-append (the epoch a fold of that shard must fold to incorporate this `+`). The caller
    /// records these in its `WriteSession` so the session-until-folded reaper knows when the `+` is folded.
    std::vector<std::pair<ShardId, uint64_t>> appendAndFlushForCommit(const GcDelta & delta);

    /// Flush every (shard, epoch) buffer whose window has elapsed or whose size hit the cap. Then run the
    /// rule-2 re-append for any flushed shard. Cheap to call frequently (no-op if nothing is due).
    void flushDueWindows();

    /// Flush ALL buffered fragments regardless of window (used on the drop path and shutdown). Then run
    /// the rule-2 re-append for every flushed shard.
    void flushAll();

    /// The number of deltas in the most-recently-flushed coalesced object (cas_log_batch_size). 0 before
    /// the first flush. Used by the op-budget asserts (§11/§12.3).
    size_t lastBatchSize() const { return last_batch_size.load(std::memory_order_relaxed); }

    /// The home shard of a part's `(part_id) edge` (§9): derived from the part_id's hex prefix exactly as
    /// shardForHash derives a blob's shard, so the edge lands in one deterministic shard.
    static ShardId shardForPartId(const PartId & part_id);

private:
    /// One shard fragment of a logical delta: the part_id, op, event_id, and ONLY this shard's pins, plus
    /// whether this shard owns the (part_id) edge.
    struct Fragment
    {
        GcDelta delta; /// pins narrowed to this shard
        bool carries_part_edge = false; /// true iff this is the part's home shard (§9 manifest edge)
    };

    /// A per-(shard, epoch) buffer of fragments awaiting a flush, with the timestamp of its first entry
    /// (the window clock).
    struct Buffer
    {
        std::vector<Fragment> fragments;
        std::chrono::steady_clock::time_point opened_at;
    };

    using ShardEpoch = std::pair<ShardId, uint64_t>;

    /// Split a logical delta into per-shard fragments: each pin to its hash-prefix shard; the (part_id)
    /// edge to the part's home shard (always present). The same event_id is reused across shards (it is
    /// per (part_id, op); dedup is per-shard-per-fold).
    static std::map<ShardId, Fragment> splitDeltaByShard(const GcDelta & delta);

    /// Read gcCurrentEpochKey(shard) (default 0 if absent). The single-writer-per-shard fenced PUT writes
    /// it; here it is just a read to stamp the delta's epoch (§5.1 rule 2).
    uint64_t readShardEpoch(ShardId shard) const;

    /// CA GC S4 (#3): the cached open epoch for a shard (refreshes once on a cold miss). NOT under `mtx`.
    uint64_t cachedShardEpoch(ShardId shard) const;
    /// CA GC S4 (#3): HEAD+GET the authoritative epoch (outside any lock) and monotonically update the cache.
    uint64_t refreshShardEpoch(ShardId shard) const;

    /// CA GC S4 (#3): a drained buffer ready to be written WITHOUT the lock. Produced under `mtx`
    /// (drainBufferLocked), consumed lock-free (writePending).
    struct PendingWrite
    {
        std::string object_key;
        std::string bytes;
        size_t delta_count = 0;
    };
    std::optional<PendingWrite> drainBufferLocked(ShardId shard, uint64_t epoch, Buffer & buffer);
    void writePending(const PendingWrite & pending);

    /// §5.1 rule 2: re-read the shard epoch; while it has advanced past `written_epoch`, re-buffer the
    /// `retained` fragments (same event_ids) under the now-open epoch and re-flush (bounded retry). CA GC
    /// S4 (#3, G1): the epoch refresh (HEAD+GET) and the re-flush PUT run OUTSIDE the lock; `mtx` is taken
    /// only to move the fragments into the buffer and drain them. Returns the FINAL epoch the fragments
    /// durably settled in (= `written_epoch` if no advance occurred) — CA GC S4.
    uint64_t reappendIfAdvanced(ShardId shard, uint64_t written_epoch, const std::vector<Fragment> & retained);

    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
    const size_t flush_max_deltas;
    const std::chrono::milliseconds flush_window;

    mutable std::mutex mtx;
    std::map<ShardEpoch, Buffer> buffers;

    /// CA GC S4 (#3, G1): cache of shard -> last-observed open epoch, so the hot append path does not do a
    /// HEAD+GET under the lock on every commit. The epoch advances at most once per GC round (a fold); a
    /// stale-low cache only widens the window the writer logs into (the re-append catches the advance), and
    /// a stale-high cache cannot happen (the cache is only ever refreshed from object storage, monotonically).
    /// Guarded by `epoch_cache_mtx` (distinct from `mtx`, which guards the buffers), so an epoch refresh
    /// never blocks a concurrent buffer move.
    mutable std::mutex epoch_cache_mtx;
    mutable std::map<ShardId, uint64_t> epoch_cache;

    std::atomic<size_t> last_batch_size{0};
};

}
