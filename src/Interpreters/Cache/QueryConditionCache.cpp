#include <Interpreters/Cache/QueryConditionCache.h>
#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>
#include <Common/SipHash.h>
#include <Common/logger_useful.h>
#include <IO/WriteHelpers.h>

namespace ProfileEvents
{
    extern const Event QueryConditionCacheHits;
    extern const Event QueryConditionCacheMisses;
    extern const Event QueryConditionCacheLayoutMismatch;
}

namespace CurrentMetrics
{
    extern const Metric QueryConditionCacheBytes;
    extern const Metric QueryConditionCacheEntries;
}

namespace DB
{

bool QueryConditionCache::Key::operator==(const Key & other) const
{
    return table_id == other.table_id
        && part_name == other.part_name
        && condition_hash == other.condition_hash
        && marks_count == other.marks_count
        && has_final_mark == other.has_final_mark;
}

size_t QueryConditionCache::KeyHasher::operator()(const Key & key) const
{
    SipHash hash;
    hash.update(key.table_id);
    hash.update(key.part_name);
    hash.update(key.condition_hash);
    hash.update(key.marks_count);
    hash.update(key.has_final_mark);
    return hash.get64();
}

size_t QueryConditionCache::EntryWeight::operator()(const Entry & entry) const
{
    /// Estimate the memory size of `std::vector<bool>` (it uses bit-packing internally)
    size_t memory = (entry.matching_marks.capacity() + 7) / 8; /// round up to bytes.
    return memory + sizeof(decltype(entry.matching_marks));
}

QueryConditionCache::QueryConditionCache(const String & cache_policy, size_t max_size_in_bytes, double size_ratio)
    : cache(cache_policy, CurrentMetrics::QueryConditionCacheBytes, CurrentMetrics::QueryConditionCacheEntries, max_size_in_bytes, 0, size_ratio)
{
}

void QueryConditionCache::write(
    const UUID & table_id, const String & part_name, UInt64 condition_hash, const String & condition,
    const MarkRanges & mark_ranges, size_t marks_count, bool has_final_mark)
{
    if (has_final_mark && marks_count == 0)
    {
        ProfileEvents::increment(ProfileEvents::QueryConditionCacheLayoutMismatch);
        LOG_ERROR(
            logger,
            "Query condition cache write for table_id: {}, part_name: {}, condition_hash: {}: "
            "has_final_mark is set but marks_count is 0, which is not a valid mark layout. "
            "Skipping cache update for this entry. See Altinity/ClickHouse#2342.",
            table_id, part_name, condition_hash);
        return;
    }

    for (const auto & mark_range : mark_ranges)
    {
        if (mark_range.begin > mark_range.end || mark_range.end > marks_count)
        {
            ProfileEvents::increment(ProfileEvents::QueryConditionCacheLayoutMismatch);
            LOG_ERROR(
                logger,
                "Query condition cache write for table_id: {}, part_name: {}, condition_hash: {}: "
                "invalid mark range [{}, {}) for entry with {} marks. Skipping cache update for this "
                "entry. See Altinity/ClickHouse#2342.",
                table_id, part_name, condition_hash, mark_range.begin, mark_range.end, marks_count);
            return;
        }
    }

    /// NOTE(#2342): marks_count and has_final_mark are now part of Key (see QueryConditionCache.h).
    /// Before this change, Key was {table_id, part_name, condition_hash} only. A part_name can be
    /// reused after its underlying data (and therefore its mark layout) has changed -- e.g. after a
    /// mutation, a merge that produces a part with a name collision under certain replay scenarios,
    /// or (per the linked issue) some other part-lifecycle path we have not fully identified.
    /// getOrSet() below could then return a pre-existing Entry sized for the OLD layout, which the
    /// code proceeded to index using the NEW marks_count/mark_ranges. On std::vector<bool>'s
    /// bit-packed storage this is an out-of-bounds *write*, not just a read -- see the (*) fill()
    /// and matching_marks[marks_count - 1] writes below. Keying on the layout as well eliminates the
    /// possibility of this mismatch entirely; the alternative (validate-before-use, kept as a defense
    /// in depth below) only stops the write after the mismatched Entry has already been returned.
    Key key = {table_id, part_name, condition_hash, marks_count, has_final_mark, condition};

    auto load_func = [&](){ return std::make_shared<Entry>(marks_count); };
    auto [entry, inserted] = cache.getOrSet(key, load_func);

    /// Try to avoid acquiring the RW lock below (*) by early-ing out. Matters for systems with lots of cores.
    {
        std::shared_lock shared_lock(entry->mutex); /// cheap

        /// NOTE(#2342): defense in depth. With marks_count in the Key, this branch should now be
        /// unreachable in normal operation -- cache.getOrSet() cannot return an Entry keyed to a
        /// different marks_count than the one just constructed. If this ever fires, it means either
        /// (a) a hash collision between two distinct Keys (extremely unlikely with SipHash64 over
        ///     table_id + part_name + condition_hash + marks_count + has_final_mark), or
        /// (b) a residual bug in Key equality/hashing, or
        /// (c) the underlying corruption in #2342 has already occurred elsewhere and this Entry's
        ///     bookkeeping is not trustworthy regardless of what the key comparison says.
        /// Silently returning (skip the cache write) is the only safe response here -- do NOT throw
        /// from inside the query pipeline for what may be a caching-layer inconsistency, and do NOT
        /// proceed to index a vector we now know is a mismatched size. This is intentionally the
        /// last line of defense, not the fix: the fix is the Key change above.
        chassert(entry->matching_marks.size() == marks_count);
        if (entry->matching_marks.size() != marks_count)
        {
            ProfileEvents::increment(ProfileEvents::QueryConditionCacheLayoutMismatch);
            LOG_ERROR(
                logger,
                "Query condition cache layout mismatch on write for table_id: {}, part_name: {}, "
                "condition_hash: {}: entry has {} marks, current task has {} marks (has_final_mark: {}). "
                "Skipping cache update for this entry. See Altinity/ClickHouse#2342.",
                table_id, part_name, condition_hash, entry->matching_marks.size(), marks_count, has_final_mark);
            return;
        }

        bool need_not_update_marks = true;
        for (const auto & mark_range : mark_ranges)
        {
            /// If the bits are already in the desired state (false), we don't need to update them.
            need_not_update_marks = std::all_of(entry->matching_marks.begin() + mark_range.begin,
                                                entry->matching_marks.begin() + mark_range.end,
                                                [](auto b) { return b == false; });
            if (!need_not_update_marks)
                break;
        }

        /// Do we either have no final mark or final mark is already in the desired state?
        bool need_not_update_final_mark = !has_final_mark || entry->matching_marks[marks_count - 1] == false;

        if (need_not_update_marks && need_not_update_final_mark)
            return;
    }

    {
        std::lock_guard lock(entry->mutex); /// (*)

        /// NOTE(#2342): same defense-in-depth check as above, now under the exclusive lock. Kept
        /// deliberately duplicated rather than factored into a helper: the shared_lock check above
        /// is an optimization (avoid taking the exclusive lock), and the entry could theoretically
        /// change between releasing the shared lock and acquiring the exclusive one in some future
        /// refactor. Re-checking here costs nothing and removes that assumption.
        chassert(entry->matching_marks.size() == marks_count);
        if (entry->matching_marks.size() != marks_count)
        {
            ProfileEvents::increment(ProfileEvents::QueryConditionCacheLayoutMismatch);
            LOG_ERROR(
                logger,
                "Query condition cache layout mismatch on write (exclusive path) for table_id: {}, "
                "part_name: {}, condition_hash: {}: entry has {} marks, current task has {} marks "
                "(has_final_mark: {}). Skipping cache update for this entry. See Altinity/ClickHouse#2342.",
                table_id, part_name, condition_hash, entry->matching_marks.size(), marks_count, has_final_mark);
            return;
        }

        /// The input mark ranges are the areas which the scan can skip later on.
        for (const auto & mark_range : mark_ranges)
            std::fill(entry->matching_marks.begin() + mark_range.begin, entry->matching_marks.begin() + mark_range.end, false);

        if (has_final_mark)
            entry->matching_marks[marks_count - 1] = false;
    }

    LOG_TEST(
        logger,
        "{} entry for table_id: {}, part_name: {}, condition_hash: {}, condition: {}, marks_count: {}, has_final_mark: {}",
        inserted ? "Inserted" : "Updated",
        table_id,
        part_name,
        condition_hash,
        condition,
        marks_count,
        has_final_mark);
}

std::optional<QueryConditionCache::MatchingMarks> QueryConditionCache::read(
    const UUID & table_id, const String & part_name, UInt64 condition_hash, size_t marks_count, bool has_final_mark)
{
    Key key = {table_id, part_name, condition_hash, marks_count, has_final_mark, ""};

    if (auto entry = cache.get(key))
    {
        std::shared_lock lock(entry->mutex);

        /// NOTE(#2342): defense in depth, mirrors the write-path checks above. On the read path a
        /// mismatch is comparatively low-risk -- the caller only reads matching_marks, it does not
        /// index into it with the *caller's* marks_count anywhere we've audited (see
        /// MergeTreeDataSelectExecutor::filterPartsByQueryConditionCache, which indexes with
        /// mark_range.begin/end drawn from part_with_ranges.ranges, i.e. the part's own state at scan
        /// time, not from this returned vector's size). We still guard here because we have NOT
        /// confirmed every caller across all supported versions respects that invariant, and because
        /// returning a mismatched-size MatchingMarks to a caller that isn't expecting one is exactly
        /// the kind of latent hazard this whole issue is about. Treat as a cache miss on mismatch.
        chassert(entry->matching_marks.size() == marks_count);
        if (entry->matching_marks.size() != marks_count)
        {
            ProfileEvents::increment(ProfileEvents::QueryConditionCacheLayoutMismatch);
            LOG_ERROR(
                logger,
                "Query condition cache layout mismatch on read for table_id: {}, part_name: {}, "
                "condition_hash: {}: entry has {} marks, requested {} marks (has_final_mark: {}). "
                "Treating as a cache miss. See Altinity/ClickHouse#2342.",
                table_id, part_name, condition_hash, entry->matching_marks.size(), marks_count, has_final_mark);
            ProfileEvents::increment(ProfileEvents::QueryConditionCacheMisses);
            return {};
        }

        LOG_TEST(
            logger,
            "Read entry for table_uuid: {}, part: {}, condition_hash: {}",
            table_id,
            part_name,
            condition_hash);

        ProfileEvents::increment(ProfileEvents::QueryConditionCacheHits);
        return {entry->matching_marks};
    }
    else
    {
        ProfileEvents::increment(ProfileEvents::QueryConditionCacheMisses);

        LOG_TEST(
            logger,
            "Could not find entry for table_uuid: {}, part: {}, condition_hash: {}",
            table_id,
            part_name,
            condition_hash);

        return {};
    }

}

std::vector<QueryConditionCache::Cache::KeyMapped> QueryConditionCache::dump() const
{
    return cache.dump();
}

void QueryConditionCache::clear()
{
    cache.clear();
}

void QueryConditionCache::setMaxSizeInBytes(size_t max_size_in_bytes)
{
    cache.setMaxSizeInBytes(max_size_in_bytes);
}

size_t QueryConditionCache::maxSizeInBytes() const
{
    return cache.maxSizeInBytes();
}

QueryConditionCache::Entry::Entry(size_t mark_count)
    : matching_marks(mark_count, true) /// by default, all marks potentially are potential matches, i.e. we can't skip them
{
}

}
