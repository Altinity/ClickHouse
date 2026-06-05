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
    /// The `(part_id) edge` (§9) lands in the part's home shard, derived from the part_id hex prefix
    /// exactly as a blob's shard is derived from its hash prefix (shardForHash). Reuse the same helper by
    /// reinterpreting the part_id as a BlobHash for the purpose of prefix-bit extraction — both are
    /// lowercase-hex digests, so the partition is identical and deterministic.
    return shardForHash(BlobHash(part_id.string()));
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

std::map<ShardId, GcLogWriter::Fragment> GcLogWriter::splitDeltaByShard(const GcDelta & delta)
{
    /// Each pin to its hash-prefix shard; the (part_id) edge to the part's home shard (always present,
    /// even for a blob-less part — the manifest reference must still be counted). The same event_id is
    /// reused across shards (it is per (part_id, op); dedup is per-shard-per-fold, so reuse is correct).
    std::map<ShardId, Fragment> by_shard;
    auto fragment_for = [&](ShardId shard) -> Fragment &
    {
        auto [it, inserted] = by_shard.try_emplace(shard);
        if (inserted)
        {
            it->second.delta.op = delta.op;
            it->second.delta.event_id = delta.event_id;
            it->second.delta.part_id = delta.part_id;
            /// CA GC S3 (#1 fix): carry the resolved manifest generation onto every shard fragment. The
            /// fold only applies the (part_id) edge on the home shard (shardForPartId guard), so a fragment
            /// that does not own the edge carries an unused mg — harmless; the home-shard fragment now keys
            /// the manifest at its real `mg` instead of 0.
            it->second.delta.manifest_generation = delta.manifest_generation;
        }
        return it->second;
    };
    /// CA GC S3 (#1 fix): each pin goes to its hash-prefix shard CARRYING its resolved generation (parallel
    /// to delta.pins). An empty pin_generations (an S2 delta, or one read from an older log object) takes
    /// every g as 0, matching the codec/fold default — so the fold keys CountKey{Blob,H,g} at the real g.
    for (size_t i = 0; i < delta.pins.size(); ++i)
    {
        Fragment & fragment = fragment_for(shardForHash(delta.pins[i]));
        fragment.delta.pins.push_back(delta.pins[i]);
        fragment.delta.pin_generations.push_back(i < delta.pin_generations.size() ? delta.pin_generations[i] : 0);
    }
    fragment_for(shardForPartId(delta.part_id)).carries_part_edge = true;
    return by_shard;
}

std::vector<ShardId> GcLogWriter::enqueue(const GcDelta & delta)
{
    auto by_shard = splitDeltaByShard(delta);

    std::vector<ShardId> result;
    result.reserve(by_shard.size());
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto now = std::chrono::steady_clock::now();
        for (auto & [shard, fragment] : by_shard)
        {
            const uint64_t epoch = readShardEpoch(shard);
            auto & buffer = buffers[{shard, epoch}];
            if (buffer.fragments.empty())
                buffer.opened_at = now;
            buffer.fragments.push_back(std::move(fragment));
            result.push_back(shard);
        }
    }
    return result;
}

void GcLogWriter::flushBufferLocked(ShardId shard, uint64_t epoch, Buffer & buffer)
{
    if (buffer.fragments.empty())
        return;

    /// Coalesce every buffered fragment into ONE GcLogBatch (group-commit — §5). One object per
    /// (shard, window); cas_log_batch_size = the number of deltas in it.
    GcLogBatch batch;
    batch.deltas.reserve(buffer.fragments.size());
    for (auto & fragment : buffer.fragments)
        batch.deltas.push_back(std::move(fragment.delta));

    /// The object is named by the FIRST fragment's event_id (deterministic, stable). Two windows for the
    /// same (shard, epoch) would each carry their own first-event_id, so they never clobber each other;
    /// and a re-append of the SAME logical batch (rule 2) reuses the same first event_id, so it is the
    /// idempotent dedup target. (The body still carries every delta's own event_id for per-delta dedup.)
    const std::string & object_event_id = batch.deltas.front().event_id;
    const GcLogObjectKey object_key = gcLogEventKey(key_prefix, epoch, shard, object_event_id);
    const std::string bytes = batch.serialize();

    /// Clear the buffer BEFORE the (throwing) write: batch.deltas already MOVED every fragment's delta out,
    /// so buffer.fragments now holds moved-from zombies. If the write throws, leaving them in place would
    /// have the next flush serialize junk gc/log entries. The data we are about to write is captured in
    /// `bytes`; on a throw the caller's #2 fail-closed path re-logs from the durable failed-delta record.
    buffer.fragments.clear();

    auto out = object_storage->writeObject(StoredObject(object_key.string()), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();

    last_batch_size.store(batch.deltas.size(), std::memory_order_relaxed);
}

uint64_t GcLogWriter::reappendIfAdvancedLocked(ShardId shard, uint64_t written_epoch, const std::vector<Fragment> & retained)
{
    /// §5.1 rule 2: after flushing into `written_epoch`, re-read the shard epoch; WHILE it has advanced
    /// PAST the epoch we wrote, re-buffer the SAME fragments (same event_ids) into the now-open epoch and
    /// re-flush — so the straggler is re-logged rather than lost. The orphaned append in the closed epoch
    /// is a harmless leaked object (deduped by event_id on fold). Bounded retry. CA GC S4 (G1): with the
    /// commit-vs-sweep `gc_lock` dropped, a GC epoch close CAN now race a commit's append, so this loop is
    /// LIVE — it is the §5.1 rule-2 carrier that makes the log complete under concurrent appends (a `+` that
    /// lands as its epoch is closed is re-appended into the now-open epoch, so the fold never loses it).
    ///
    /// CA GC S4: returns the FINAL epoch the fragments durably settled in (the highest epoch we wrote into,
    /// = `written_epoch` if no advance occurred). The caller threads this into the session's `delta_epochs`
    /// so the folded watermark is checked against the epoch the `+` actually landed in.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint64_t current = readShardEpoch(shard);
        if (current <= written_epoch)
            return written_epoch;
        auto & open_buffer = buffers[{shard, current}];
        if (open_buffer.fragments.empty())
            open_buffer.opened_at = std::chrono::steady_clock::now();
        for (const auto & f : retained)
            open_buffer.fragments.push_back(f);
        flushBufferLocked(shard, current, open_buffer);
        written_epoch = current;
    }
    return written_epoch;
}

std::vector<std::pair<ShardId, uint64_t>> GcLogWriter::appendAndFlushForCommit(const GcDelta & delta)
{
    /// I1/I6 commit-path discipline: the `+` must be durably enqueued BEFORE the live ref. For S2 (the
    /// gc_lock is held across commit + sweep) a synchronous flush before the ref is the accepted
    /// discipline; the async/session-covers-gap path is S4 and is NOT done here.
    ///
    /// Split + buffer the delta, then synchronously flush every (shard, epoch) buffer that now holds a
    /// fragment of THIS delta — draining anything else queued for those (shard, epoch)s along with it.
    auto by_shard = splitDeltaByShard(delta);

    std::vector<std::pair<ShardId, uint64_t>> settled;
    settled.reserve(by_shard.size());
    std::lock_guard<std::mutex> lock(mtx);
    for (auto & [shard, fragment] : by_shard)
    {
        const uint64_t epoch = readShardEpoch(shard);
        auto & buffer = buffers[{shard, epoch}];
        if (buffer.fragments.empty())
            buffer.opened_at = std::chrono::steady_clock::now();
        buffer.fragments.push_back(std::move(fragment));

        /// Retain a copy of the batch for the rule-2 re-append BEFORE flushBufferLocked clears the buffer.
        const std::vector<Fragment> retained = buffer.fragments;
        flushBufferLocked(shard, epoch, buffer);
        /// CA GC S4: capture the FINAL epoch the fragment settled in (after any rule-2 re-append) so the
        /// caller can record `(shard, final_epoch)` in its session for the folded-watermark check.
        const uint64_t final_epoch = reappendIfAdvancedLocked(shard, epoch, retained);
        settled.emplace_back(shard, final_epoch);
    }
    return settled;
}

void GcLogWriter::flushDueWindows()
{
    std::lock_guard<std::mutex> lock(mtx);
    const auto now = std::chrono::steady_clock::now();
    /// Collect the due (shard, epoch) keys first; flushing mutates `buffers` (re-append may insert new
    /// (shard, epoch) entries), so iterate over a snapshot of keys rather than the live map.
    std::vector<ShardEpoch> due;
    for (auto & [shard_epoch, buffer] : buffers)
    {
        if (buffer.fragments.empty())
            continue;
        const bool size_due = buffer.fragments.size() >= flush_max_deltas;
        const bool time_due = (now - buffer.opened_at) >= flush_window;
        if (size_due || time_due)
            due.push_back(shard_epoch);
    }
    for (const auto & shard_epoch : due)
    {
        auto & buffer = buffers[shard_epoch];
        if (buffer.fragments.empty())
            continue;
        const std::vector<Fragment> retained = buffer.fragments;
        flushBufferLocked(shard_epoch.first, shard_epoch.second, buffer);
        reappendIfAdvancedLocked(shard_epoch.first, shard_epoch.second, retained);
    }
}

void GcLogWriter::flushAll()
{
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<ShardEpoch> all;
    for (auto & [shard_epoch, buffer] : buffers)
        if (!buffer.fragments.empty())
            all.push_back(shard_epoch);
    for (const auto & shard_epoch : all)
    {
        auto & buffer = buffers[shard_epoch];
        if (buffer.fragments.empty())
            continue;
        const std::vector<Fragment> retained = buffer.fragments;
        flushBufferLocked(shard_epoch.first, shard_epoch.second, buffer);
        reappendIfAdvancedLocked(shard_epoch.first, shard_epoch.second, retained);
    }
}

}
