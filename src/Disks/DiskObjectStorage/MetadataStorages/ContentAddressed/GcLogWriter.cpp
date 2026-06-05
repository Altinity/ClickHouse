#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <utility>

namespace DB::ContentAddressed
{

GcLogWriter::GcLogWriter(
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    size_t flush_max_deltas_,
    std::chrono::milliseconds flush_window_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , flush_max_deltas(flush_max_deltas_)
    , flush_window(flush_window_)
{
}

ShardId GcLogWriter::shardForPartId(const PartId & part_id)
{
    /// Delegate to the canonical PoolPaths free function so the nibble-fold logic lives in exactly one
    /// place. `PoolPaths::shardForPartId` is the single source of truth for both the sealed-index keying
    /// (Task 14/15) and the log-writer's (part_id) edge sharding (§9) — they must always agree.
    return DB::ContentAddressed::shardForPartId(part_id);
}

uint64_t GcLogWriter::readShardEpoch(ShardId shard) const
{
    /// §5.1 rule 2 (writer epoch read): the per-shard epoch is the value a writer stamps its delta with.
    /// Absent => 0 (no shard has ever been compacted yet). The value is the decimal epoch the fenced
    /// leader wrote via a plain PUT; a malformed/empty object degrades to 0 (the open epoch since the
    /// beginning of time) which only widens the window the writer logs into — never an under-count.
    const std::string key = gcCurrentEpochKey(key_prefix, shard);
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
        return 0;
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    if (content.empty())
        return 0;
    try
    {
        return std::stoull(content);
    }
    catch (...)
    {
        return 0;
    }
}

uint64_t GcLogWriter::refreshShardEpoch(ShardId shard) const
{
    /// HEAD+GET the authoritative epoch (outside any lock) and update the cache. Returns the fresh value.
    const uint64_t epoch = readShardEpoch(shard);
    std::lock_guard<std::mutex> guard(epoch_cache_mtx);
    auto & cached = epoch_cache[shard];
    if (epoch > cached)
        cached = epoch; /// monotonic: never let a racing lower read move the cache backwards.
    return cached;
}

uint64_t GcLogWriter::cachedShardEpoch(ShardId shard) const
{
    {
        std::lock_guard<std::mutex> guard(epoch_cache_mtx);
        if (auto it = epoch_cache.find(shard); it != epoch_cache.end())
            return it->second;
    }
    /// Cold miss: refresh once (the warm-path HEAD+GET is gone — subsequent commits hit the cache; the
    /// re-append's refresh keeps it current when a fold advances the epoch).
    return refreshShardEpoch(shard);
}

std::map<ShardId, GcLogWriter::Fragment> GcLogWriter::splitDeltaByShard(const GcDelta & delta)
{
    /// Each pin to its hash-prefix shard; the (part_id) edge to the part's home shard (always present, even
    /// for a blob-less part — the manifest reference must still be counted). The same event_id is reused
    /// across shards (it is per (part_id, op); dedup is per-shard-per-fold, so reuse is correct).
    std::map<ShardId, Fragment> by_shard;
    auto fragment_for = [&](ShardId shard) -> Fragment &
    {
        auto [it, inserted] = by_shard.try_emplace(shard);
        if (inserted)
        {
            it->second.delta.op = delta.op;
            it->second.delta.event_id = delta.event_id;
            it->second.delta.part_id = delta.part_id;
            /// Carry the resolved manifest generation onto every fragment. Only the home-shard fragment
            /// applies the (part_id) edge (shardForPartId guard in the fold), so on the others the `mg` is
            /// unused — harmless; the home-shard fragment keys the manifest at its real `mg` instead of 0.
            it->second.delta.manifest_generation = delta.manifest_generation;
        }
        return it->second;
    };
    /// Each pin carries its resolved generation (parallel to delta.pins). An empty pin_generations (an S2
    /// delta, or one read from an older log object) takes every g as 0, matching the codec/fold default —
    /// so the fold keys CountKey{Blob, H, g} at the real g.
    for (size_t i = 0; i < delta.pins.size(); ++i)
    {
        Fragment & fragment = fragment_for(shardForHash(delta.pins[i]));
        fragment.delta.pins.push_back(delta.pins[i]);
        fragment.delta.pin_generations.push_back(i < delta.pin_generations.size() ? delta.pin_generations[i] : 0);
    }
    fragment_for(shardForPartId(delta.part_id)).carries_part_edge = true;
    return by_shard;
}

void GcLogWriter::enqueue(const GcDelta & delta)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto & [shard, fragment] : splitDeltaByShard(delta))
    {
        const uint64_t epoch = cachedShardEpoch(shard);
        std::lock_guard<std::mutex> lock(mtx);
        auto & buffer = buffers[{shard, epoch}];
        if (buffer.fragments.empty())
            buffer.opened_at = now;
        buffer.fragments.push_back(std::move(fragment));
    }
}

std::optional<GcLogWriter::PendingWrite> GcLogWriter::drainBufferLocked(ShardId shard, uint64_t epoch, Buffer & buffer)
{
    /// Under `mtx`: coalesce the buffered fragments into ONE GcLogBatch, compute the deterministic object key
    /// (named by the first fragment's event_id), and CLEAR the buffer. No S3 I/O here — the serialized bytes
    /// are returned for a lock-free write so the mutex is never held across a PUT (G1).
    if (buffer.fragments.empty())
        return std::nullopt;
    GcLogBatch batch;
    batch.deltas.reserve(buffer.fragments.size());
    for (auto & fragment : buffer.fragments)
        batch.deltas.push_back(std::move(fragment.delta));
    const std::string & object_event_id = batch.deltas.front().event_id;
    const GcLogObjectKey object_key = gcLogEventKey(key_prefix, epoch, shard, object_event_id);
    PendingWrite pending;
    pending.object_key = object_key.string();
    pending.bytes = batch.serialize();
    pending.delta_count = batch.deltas.size();
    buffer.fragments.clear();
    return pending;
}

void GcLogWriter::writePending(const PendingWrite & pending)
{
    /// Lock-free: the S3 PUT. Two writers targeting the same (shard, epoch, first-event_id) write identical
    /// bytes to the same key (idempotent — the batch is named by the first event_id and re-appends reuse it).
    auto out = object_storage->writeObject(StoredObject(pending.object_key), WriteMode::Rewrite);
    out->write(pending.bytes.data(), pending.bytes.size());
    out->finalize();
    last_batch_size.store(pending.delta_count, std::memory_order_relaxed);
}

uint64_t GcLogWriter::reappendIfAdvanced(ShardId shard, uint64_t written_epoch, const std::vector<Fragment> & retained)
{
    /// §5.1 rule 2: while the shard epoch has advanced PAST the epoch we wrote, re-buffer the SAME fragments
    /// (same event_ids — deduped on fold) into the now-open epoch and re-flush, so a straggler is re-logged
    /// rather than lost. CA GC S4 (#3, G1): the epoch refresh (HEAD+GET) and the re-flush PUT run OUTSIDE the
    /// lock; `mtx` is taken only to move the fragments into the buffer and drain them. Bounded retry.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint64_t current = refreshShardEpoch(shard); /// HEAD+GET outside the lock; updates the cache.
        if (current <= written_epoch)
            return written_epoch;
        std::optional<PendingWrite> pending;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & open_buffer = buffers[{shard, current}];
            if (open_buffer.fragments.empty())
                open_buffer.opened_at = std::chrono::steady_clock::now();
            for (const auto & f : retained)
                open_buffer.fragments.push_back(f);
            pending = drainBufferLocked(shard, current, open_buffer);
        }
        if (pending)
            writePending(*pending);
        written_epoch = current;
    }
    return written_epoch;
}

std::vector<std::pair<ShardId, uint64_t>> GcLogWriter::appendAndFlushForCommit(const GcDelta & delta)
{
    auto by_shard = splitDeltaByShard(delta);

    std::vector<std::pair<ShardId, uint64_t>> settled;
    settled.reserve(by_shard.size());
    for (auto & [shard, fragment] : by_shard)
    {
        /// Read the open epoch from the cache (no HEAD+GET on the warm path). Buffer the fragment + drain
        /// under `mtx`; do the PUT outside it (G1: the mutex is never held across S3 I/O).
        const uint64_t epoch = cachedShardEpoch(shard);
        std::optional<PendingWrite> pending;
        std::vector<Fragment> retained;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & buffer = buffers[{shard, epoch}];
            if (buffer.fragments.empty())
                buffer.opened_at = std::chrono::steady_clock::now();
            buffer.fragments.push_back(std::move(fragment));
            retained = buffer.fragments; /// copy for the rule-2 re-append BEFORE the drain clears the buffer.
            pending = drainBufferLocked(shard, epoch, buffer);
        }
        if (pending)
            writePending(*pending);
        /// CA GC S4 (#3): the rule-2 re-append re-reads the epoch and re-drains OUTSIDE the per-shard lock
        /// window above (it takes its own short lock per attempt). Returns the final settled epoch.
        const uint64_t final_epoch = reappendIfAdvanced(shard, epoch, retained);
        settled.emplace_back(shard, final_epoch);
    }
    return settled;
}

void GcLogWriter::flushShardEpochs(const std::vector<ShardEpoch> & shard_epochs)
{
    /// For each (shard, epoch): drain under `mtx` and write outside it, plus the rule-2 re-append (also
    /// lock-free I/O). No S3 PUT is ever held under the lock (G1). The caller snapshots the keys under `mtx`
    /// first because the re-append may insert new buffer entries (we must not iterate the live map).
    for (const auto & shard_epoch : shard_epochs)
    {
        std::optional<PendingWrite> pending;
        std::vector<Fragment> retained;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto & buffer = buffers[shard_epoch];
            if (buffer.fragments.empty())
                continue;
            retained = buffer.fragments;
            pending = drainBufferLocked(shard_epoch.first, shard_epoch.second, buffer);
        }
        if (pending)
            writePending(*pending);
        reappendIfAdvanced(shard_epoch.first, shard_epoch.second, retained);
    }
}

void GcLogWriter::flushDueWindows()
{
    /// Flush only the (shard, epoch) buffers that are size- or window-due.
    std::vector<ShardEpoch> due;
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto now = std::chrono::steady_clock::now();
        for (auto & [shard_epoch, buffer] : buffers)
        {
            if (buffer.fragments.empty())
                continue;
            const bool size_due = buffer.fragments.size() >= flush_max_deltas;
            const bool time_due = (now - buffer.opened_at) >= flush_window;
            if (size_due || time_due)
                due.push_back(shard_epoch);
        }
    }
    flushShardEpochs(due);
}

void GcLogWriter::flushAll()
{
    /// Flush every non-empty buffer regardless of window (the drop path and shutdown).
    std::vector<ShardEpoch> all;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto & [shard_epoch, buffer] : buffers)
            if (!buffer.fragments.empty())
                all.push_back(shard_epoch);
    }
    flushShardEpochs(all);
}

}
