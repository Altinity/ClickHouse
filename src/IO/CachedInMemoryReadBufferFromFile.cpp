#include <Common/VectorWithMemoryTracking.h>
#include <IO/CachedInMemoryReadBufferFromFile.h>
#include <base/scope_guard.h>
#include <Common/PODArray.h>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>

namespace ProfileEvents
{
    extern const Event PageCacheReadBytes;
    extern const Event PageCacheReadThroughBytes;
    extern const Event PageCacheReadThroughBudgetSamples;
    extern const Event PageCacheReadThroughBudgetBytesSum;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int UNEXPECTED_END_OF_FILE;
    extern const int CANNOT_SEEK_THROUGH_FILE;
    extern const int SEEK_POSITION_OUT_OF_BOUND;
    extern const int LOGICAL_ERROR;
}

CachedInMemoryReadBufferFromFile::CachedInMemoryReadBufferFromFile(
    PageCacheFile cache_file_, PageCachePtr cache_, std::unique_ptr<ReadBufferFromFileBase> in_, const PageCacheSettings & settings_)
    : ReadBufferFromFileBase(0, nullptr, 0, in_->getFileSize())
    , cache_file(std::move(cache_file_))
    , cache_key_base_hash(cache_file.baseHash())
    , cache(cache_)
    , settings(settings_)
    , in(std::move(in_)), read_until_position(file_size.value())
    , inner_read_until_position(read_until_position)
{
}

bool CachedInMemoryReadBufferFromFile::innerSupportsReadAt() const
{
    std::call_once(inner_supports_read_at_init, [this]()
    {
        inner_supports_read_at = in->supportsReadAt();
    });
    return inner_supports_read_at;
}

String CachedInMemoryReadBufferFromFile::getFileName() const
{
    return cache_file.path;
}

String CachedInMemoryReadBufferFromFile::getInfoForLog()
{
    return "CachedInMemoryReadBufferFromFile(" + in->getInfoForLog() + ")";
}

bool CachedInMemoryReadBufferFromFile::isSeekCheap()
{
    /// Seek is cheap in the sense that seek()+nextImpl() is never much slower than ignore()+nextImpl()
    /// (which is what the caller cares about).
    return true;
}

off_t CachedInMemoryReadBufferFromFile::seek(off_t off, int whence)
{
    if (whence != SEEK_SET)
        throw Exception(ErrorCodes::CANNOT_SEEK_THROUGH_FILE, "Only SEEK_SET mode is allowed.");

    size_t offset = static_cast<size_t>(off);
    if (offset > file_size.value())
        throw Exception(ErrorCodes::SEEK_POSITION_OUT_OF_BOUND, "Seek position is out of bounds. Offset: {}", off);

    if (offset >= file_offset_of_buffer_end - working_buffer.size() && offset <= file_offset_of_buffer_end)
    {
        pos = working_buffer.end() - (file_offset_of_buffer_end - offset);
        chassert(getPosition() == off);
        return off;
    }

    resetWorkingBuffer();

    file_offset_of_buffer_end = offset;
    chunk.reset();

    chassert(getPosition() == off);
    return off;
}

off_t CachedInMemoryReadBufferFromFile::getPosition()
{
    return file_offset_of_buffer_end - available();
}

size_t CachedInMemoryReadBufferFromFile::getFileOffsetOfBufferEnd() const
{
    return file_offset_of_buffer_end;
}

void CachedInMemoryReadBufferFromFile::setReadUntilPosition(size_t position)
{
    read_until_position = std::min(position, file_size.value());
    if (position < static_cast<size_t>(getPosition()))
    {
        resetWorkingBuffer();
        chunk.reset();
    }
    else if (position < file_offset_of_buffer_end)
    {
        size_t diff = file_offset_of_buffer_end - position;
        working_buffer.resize(working_buffer.size() - diff);
        file_offset_of_buffer_end -= diff;
    }
}

void CachedInMemoryReadBufferFromFile::setReadUntilEnd()
{
    setReadUntilPosition(file_size.value());
}

bool CachedInMemoryReadBufferFromFile::nextImpl()
{
    chassert(read_until_position <= file_size.value());
    if (file_offset_of_buffer_end >= read_until_position)
        return false;

    size_t block_size = settings.block_size;

    if (chunk != nullptr)
    {
        chassert(chunk->range.hash(cache_key_base_hash) == cache_range.hash(cache_key_base_hash));
        if (file_offset_of_buffer_end < cache_range.offset || file_offset_of_buffer_end >= cache_range.offset + block_size)
            chunk.reset();
    }

    if (chunk == nullptr)
    {
        cache_range.offset = file_offset_of_buffer_end / block_size * block_size;
        cache_range.size = std::min(block_size, file_size.value() - cache_range.offset);

        chunk = cache->getOrSet(cache_file, cache_range, settings.read_if_exists_otherwise_bypass, settings.random_eviction_for_tests, [&](auto cell)
        {
            Buffer prev_in_buffer = in->internalBuffer();
            SCOPE_EXIT({ in->set(prev_in_buffer.begin(), prev_in_buffer.size()); });

            size_t pos = 0;
            while (pos < cache_range.size)
            {
                char * piece_start = cell->data() + pos;
                size_t piece_size = cache_range.size - pos;
                in->set(piece_start, piece_size);
                if (pos == 0)
                {
                    /// Do in->setReadUntilPosition if needed.
                    /// If the next few blocks are likely cache misses, include them too, to reduce
                    /// the number of requests (usually `in` makes a new HTTP request after each
                    /// nontrivial seek or setReadUntilPosition call).
                    /// Use aligned groups of blocks (rather than sliding window) to work better
                    /// with distributed cache.
                    size_t lookahead_bytes = block_size * std::max<size_t>(1, settings.lookahead_blocks);
                    size_t lookahead_block_end = std::min({
                        file_size.value(),
                        (cache_range.offset / lookahead_bytes + 1) * lookahead_bytes,
                        (read_until_position + block_size - 1) / block_size * block_size});

                    if (inner_read_until_position < cache_range.offset + cache_range.size ||
                        inner_read_until_position > lookahead_block_end)
                    {
                        PageCacheByteRange probe = cache_range;
                        do
                        {
                            probe.offset += probe.size;
                            probe.size = std::min(block_size, file_size.value() - probe.offset);
                            chassert(probe.offset <= lookahead_block_end);
                        }
                        while (probe.offset < lookahead_block_end
                            && !cache->contains(
                                probe.hash(cache_key_base_hash),
                                settings.random_eviction_for_tests));
                        inner_read_until_position = probe.offset;
                        in->setReadUntilPosition(inner_read_until_position);
                    }

                    in->seek(cache_range.offset, SEEK_SET);
                }
                else
                    chassert(!in->available());

                if (in->eof())
                    throw Exception(ErrorCodes::UNEXPECTED_END_OF_FILE, "File {} ended after {} bytes, but we expected {}",
                        getFileName(), cache_range.offset + pos, file_size.value());

                chassert(in->position() >= piece_start && in->buffer().end() <= piece_start + piece_size);
                chassert(in->getPosition() == static_cast<off_t>(cache_range.offset + pos));

                size_t n = in->available();
                chassert(n);
                if (in->position() != piece_start)
                    memmove(piece_start, in->position(), n);
                in->position() += n;
                pos += n;
            }

            return cell;
        });
    }

    nextimpl_working_buffer_offset = file_offset_of_buffer_end - cache_range.offset;
    working_buffer = Buffer(
        chunk->data(),
        chunk->data() + std::min(chunk->size(), read_until_position - cache_range.offset));
    pos = working_buffer.begin() + nextimpl_working_buffer_offset;

    if (!internal_buffer.empty())
    {
        /// We were given an external buffer to read into. We currently don't allow this as it would
        /// require unnecessary memcpy.
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CachedInMemoryReadBufferFromFile doesn't support using external buffer");
    }

    size_t size = available();
    file_offset_of_buffer_end += size;
    ProfileEvents::increment(ProfileEvents::PageCacheReadBytes, size);

    return true;
}

void CachedInMemoryReadBufferFromFile::updateReadStats(size_t read_bytes, uint64_t first_byte_us, uint64_t total_us) const
{
    /// Only reads whose transport reported progress mid-transfer separate latency from bandwidth; the
    /// rest give one duration and nothing to attribute it to, so they are skipped rather than folded in
    /// as an absurd bandwidth sample (the same rule the Parquet prefetcher's fitting uses).
    if (first_byte_us == 0 || total_us <= first_byte_us || read_bytes == 0)
        return;

    constexpr double alpha = 0.2;
    const double bandwidth_sample = static_cast<double>(read_bytes) / static_cast<double>(total_us - first_byte_us);

    std::lock_guard lock(read_stats_mutex);
    if (stat_samples == 0)
    {
        stat_rtt_us = static_cast<double>(first_byte_us);
        stat_bandwidth_bytes_per_us = bandwidth_sample;
    }
    else
    {
        stat_rtt_us = stat_rtt_us * (1 - alpha) + static_cast<double>(first_byte_us) * alpha;
        stat_bandwidth_bytes_per_us = stat_bandwidth_bytes_per_us * (1 - alpha) + bandwidth_sample * alpha;
    }
    stat_bandwidth_peak_bytes_per_us = std::max(stat_bandwidth_peak_bytes_per_us, stat_bandwidth_bytes_per_us);
    ++stat_samples;
}

size_t CachedInMemoryReadBufferFromFile::readThroughBudgetBytes() const
{
    if (size_t override_bytes = read_through_budget_override.load(std::memory_order_relaxed))
        return override_bytes;

    double rtt_us, bandwidth;
    size_t samples;
    {
        std::lock_guard lock(read_stats_mutex);
        rtt_us = stat_rtt_us;
        bandwidth = stat_bandwidth_bytes_per_us;
        samples = stat_samples;
    }
    /// No evidence yet: don't spend bytes on a guess. The first reads of a scan pay the un-bridged cost
    /// and pay for the fit.
    if (samples < 4 || bandwidth <= 0 || rtt_us <= 0)
        return 0;

    /// Reading through an island of cached blocks costs `island / bandwidth` of transfer and saves one
    /// round trip, so the break-even island is `bandwidth * rtt`: the bytes that move in the time the
    /// avoided request would have spent waiting. Both terms are measured on this buffer's own source
    /// reads, so the budget follows the storage (a 30 ms / 100 MiB/s object store gives ~3 MiB, a local
    /// disk gives almost nothing).
    ///
    /// This bounds the *time* the extra bytes cost on one stream, which is not the whole story when
    /// many streams share a saturated link -- there the bytes are taken from the other streams while
    /// the saved round trip helps only this one. Measuring that from inside the cache does not work:
    /// per-read bandwidth already includes the sharing, and comparing it to the peak seen in the same
    /// query yields ~1 (measured: the fitted budget sat at its cap at 4, 16 and 64 concurrent reads
    /// alike). So the byte cost is bounded directly instead, by the caller-visible quantity that
    /// matters -- see `max_waste_fraction` in `populateBlockRange`.
    constexpr double max_budget_bytes = 8.0 * 1024 * 1024;
    const double budget = std::min(bandwidth * rtt_us, max_budget_bytes);

    ProfileEvents::increment(ProfileEvents::PageCacheReadThroughBudgetSamples);
    ProfileEvents::increment(ProfileEvents::PageCacheReadThroughBudgetBytesSum, static_cast<size_t>(std::max(0.0, budget)));
    return static_cast<size_t>(std::max(0.0, budget));
}

VectorWithMemoryTracking<PageCache::MappedPtr> CachedInMemoryReadBufferFromFile::populateBlockRange(size_t offset, size_t n, const std::function<bool(PageCache::MappedPtr &)> & block_callback) const
{
    if (n == 0 || offset >= file_size.value())
        return {};

    size_t block_size = settings.block_size;
    /// Compute end_offset without overflow: clamp n so that offset + n <= file_size.
    size_t end_offset = offset + std::min(n, file_size.value() - offset);

    size_t first_block_start = offset / block_size * block_size;
    size_t num_blocks = (end_offset - first_block_start + block_size - 1) / block_size;

    bool detached_if_missing = settings.read_if_exists_otherwise_bypass;
    bool inject_eviction = settings.random_eviction_for_tests;

    /// Phase 1: probe cache for all blocks, record hits.
    VectorWithMemoryTracking<PageCache::MappedPtr> cells(num_blocks);
    PageCacheByteRange block_range;
    for (size_t i = 0; i < num_blocks; ++i)
    {
        block_range.offset = first_block_start + i * block_size;
        block_range.size = std::min(block_size, file_size.value() - block_range.offset);
        cells[i] = cache->get(block_range.hash(cache_key_base_hash), inject_eviction);
    }

    /// How much of this request is missing: the denominator for the read-through waste bound below.
    /// Taken over the whole request rather than the run being built, because a run cannot accumulate
    /// misses before it is allowed to bridge -- judging the bound on the run alone never permits the
    /// first island and disables the read-through completely (measured).
    size_t total_missing_blocks = 0;
    for (size_t i = 0; i < num_blocks; ++i)
        if (!cells[i])
            ++total_missing_blocks;
    size_t read_through_blocks_used = 0;

    /// Phase 2: fill missing blocks, coalescing consecutive misses into single reads.
    ///
    /// On object storage, each `in->readBigAt` is a separate HTTP request, so reading one
    /// block at a time turns a cold scan into one request per block (~15k for a 14 GB file
    /// at 1 MiB blocks). Coalescing consecutive misses into a single request amortizes that
    /// overhead.
    ///
    /// The coalesced read uses a temporary buffer, capped at `page_cache_max_coalesced_bytes` to
    /// bound transient memory under parallel cold reads. A run longer than the cap is split.
    /// Single-block misses bypass the buffer and read directly into the cache cell.
    const size_t max_blocks_per_fetch = std::max<size_t>(1, settings.max_coalesced_bytes / block_size);

    /// A run of missing blocks may also read *through* a short island of blocks that are already
    /// cached, so that two runs separated by such an island cost one request instead of two. The
    /// bytes covering the island are fetched and thrown away (the cached cells are never
    /// overwritten), which is worth it only while those bytes take less time to transfer than the
    /// round trip they save -- about one bandwidth-delay product, the same reasoning as the Parquet
    /// reader's `input_format_parquet_coalesce_gap_bytes`. Without it, a half-cached file read at
    /// block granularity degenerates into one request per missing block: the alternating pattern a
    /// partially warm cache produces is exactly the worst case.
    /// Derived from this buffer's own fitted bandwidth and round-trip time, so it adapts to the
    /// storage and to how contended the link currently is; zero (no samples yet, or a saturated pipe)
    /// means no read-through, i.e. the behaviour before it existed.
    const size_t max_read_through_blocks = readThroughBudgetBytes() / block_size;

    size_t i = 0;
    while (i < num_blocks)
    {
        if (cells[i])
        {
            if (block_callback && block_callback(cells[i]))
                return cells;
            ++i;
            continue;
        }

        /// Grow the run: every missing block extends it; a cached block extends it only while the
        /// read-through budget lasts, and only counts once the run reaches another missing block
        /// (a run never ends on a cached block -- reading those bytes would save nothing).
        /// Two bounds on the bytes fetched for cached blocks: the time model above (one round trip's
        /// worth, per island) and a hard ceiling on the waste relative to the bytes the run actually
        /// needs. The second is what protects a saturated link, where the extra bytes are the scarce
        /// resource rather than the round trips: without it, a half-cached file nearly doubles the bytes
        /// read (measured) and that loses whenever concurrency has already hidden the latency.
        /// Bytes fetched for cached blocks, as a share of the bytes the request is missing; set by the
        /// caller from its concurrency (see `setReadThroughWastePermille`).
        const size_t max_waste_blocks = total_missing_blocks * read_through_waste_permille.load(std::memory_order_relaxed) / 1000;
        const size_t miss_begin = i;
        size_t miss_end = i + 1;
        size_t bridged = 0;         // consecutive cached blocks under consideration
        size_t bridged_total = 0;   // cached blocks already merged into this run
        for (size_t j = i + 1; j < num_blocks && (j - miss_begin) < max_blocks_per_fetch; ++j)
        {
            if (!cells[j])
            {
                miss_end = j + 1;
                bridged_total += bridged;
                bridged = 0;
                continue;
            }
            if (max_read_through_blocks == 0 || bridged + 1 > max_read_through_blocks
                || read_through_blocks_used + bridged_total + bridged + 1 > max_waste_blocks)
                break;
            ++bridged;
        }
        i = miss_end;
        read_through_blocks_used += bridged_total;

        if (miss_end - miss_begin == 1)
        {
            /// Single-block miss: read directly into the cache cell (no temp buffer).
            block_range.offset = first_block_start + miss_begin * block_size;
            block_range.size = std::min(block_size, file_size.value() - block_range.offset);
            UInt128 key_hash = block_range.hash(cache_key_base_hash);

            cells[miss_begin] = cache->getOrSet(
                cache_file, block_range, detached_if_missing, inject_eviction,
                [&](const auto & c)
                {
                    Stopwatch watch;
                    uint64_t first_byte_us = 0;
                    size_t bytes_read = in->readBigAt(c->data(), block_range.size, block_range.offset,
                        /// `false` means "keep going": a `true` return cancels the read.
                        [&](size_t) { if (first_byte_us == 0) first_byte_us = watch.elapsedMicroseconds(); return false; });
                    updateReadStats(bytes_read, first_byte_us, watch.elapsedMicroseconds());
                    if (bytes_read < block_range.size)
                        throw Exception(ErrorCodes::UNEXPECTED_END_OF_FILE, "File {} ended after {} bytes, but we expected {}",
                            cache_file.path, block_range.offset + bytes_read, file_size.value());
                },
                key_hash);
        }
        else
        {
            /// Multi-block miss: fetch the whole run with one `readBigAt`, then distribute into cells.
            const size_t range_start = first_block_start + miss_begin * block_size;
            const size_t range_end = std::min(first_block_start + miss_end * block_size, file_size.value());
            const size_t range_size = range_end - range_start;

            PODArray<char> buf(range_size);
            Stopwatch watch;
            uint64_t first_byte_us = 0;
            size_t bytes_read = in->readBigAt(buf.data(), range_size, range_start,
                /// `false` means "keep going": a `true` return cancels the read.
                        [&](size_t) { if (first_byte_us == 0) first_byte_us = watch.elapsedMicroseconds(); return false; });
            updateReadStats(bytes_read, first_byte_us, watch.elapsedMicroseconds());
            if (bytes_read < range_size)
                throw Exception(ErrorCodes::UNEXPECTED_END_OF_FILE, "File {} ended after {} bytes, but we expected {}",
                    cache_file.path, range_start + bytes_read, file_size.value());

            for (size_t j = miss_begin; j < miss_end; ++j)
            {
                block_range.offset = first_block_start + j * block_size;
                block_range.size = std::min(block_size, file_size.value() - block_range.offset);

                /// A block bridged by the read-through already has its cell; the bytes just fetched
                /// for it are the price of the merge, not something to write anywhere.
                if (cells[j])
                {
                    ProfileEvents::increment(ProfileEvents::PageCacheReadThroughBytes, block_range.size);
                    continue;
                }

                const size_t buf_offset = block_range.offset - range_start;
                UInt128 key_hash = block_range.hash(cache_key_base_hash);

                cells[j] = cache->getOrSet(
                    cache_file, block_range, detached_if_missing, inject_eviction,
                    [&](const auto & c)
                    {
                        memcpy(c->data(), buf.data() + buf_offset, block_range.size);
                    },
                    key_hash);
            }
        }

        for (size_t j = miss_begin; j < miss_end; ++j)
        {
            if (block_callback && block_callback(cells[j]))
                return cells;
        }
    }

    return cells;
}

size_t CachedInMemoryReadBufferFromFile::readBigAt(char * to, size_t n, size_t offset, const std::function<bool(size_t m)> & progress_callback) const
{
    if (n == 0 || offset >= file_size.value())
        return 0;

    size_t end_offset = offset + std::min(n, file_size.value() - offset);

    size_t bytes_copied = 0;
    auto cells = populateBlockRange(
        offset, n,
        [&](PageCache::MappedPtr & cell)
        {
            size_t block_start = cell->range.offset;
            size_t block_data_size = cell->range.size;
            size_t offset_in_block = (offset > block_start) ? offset - block_start : 0;
            size_t to_copy = std::min(block_data_size - offset_in_block, end_offset - (offset + bytes_copied));

            memcpy(to + bytes_copied, cell->data() + offset_in_block, to_copy);
            bytes_copied += to_copy;

            ProfileEvents::increment(ProfileEvents::PageCacheReadBytes, to_copy);

            if (progress_callback)
                return progress_callback(bytes_copied);
            return false;
        });

    return bytes_copied;
}

VectorWithMemoryTracking<SeekableReadBuffer::CachedRegion> CachedInMemoryReadBufferFromFile::readBigAtRetainCells(size_t n, size_t offset) const
{
    if (n == 0 || offset >= file_size.value())
        return {};

    size_t block_size = settings.block_size;
    size_t end_offset = offset + std::min(n, file_size.value() - offset);
    size_t first_block_start = offset / block_size * block_size;

    auto cells = populateBlockRange(offset, n);

    VectorWithMemoryTracking<CachedRegion> regions;
    size_t current_offset = offset;
    for (size_t i = 0; i < cells.size() && current_offset < end_offset; ++i)
    {
        size_t block_start = first_block_start + i * block_size;
        size_t block_data_size = std::min(block_size, file_size.value() - block_start);
        size_t offset_in_block = (current_offset > block_start) ? current_offset - block_start : 0;
        size_t usable = std::min(block_data_size - offset_in_block, end_offset - current_offset);

        const char * data_ptr = cells[i]->data() + offset_in_block;
        regions.push_back(CachedRegion{
            .handle = std::move(cells[i]),
            .data = data_ptr,
            .size = usable,
            .file_offset = current_offset,
        });

        current_offset += usable;
        ProfileEvents::increment(ProfileEvents::PageCacheReadBytes, usable);
    }

    return regions;
}

bool CachedInMemoryReadBufferFromFile::isBigRangeCached(size_t offset, size_t n) const
{
    if (n == 0)
        return true;
    if (!file_size.has_value() || offset >= file_size.value())
        return false;

    const size_t block_size = settings.block_size;
    const size_t end_offset = offset + std::min(n, file_size.value() - offset);
    const size_t first_block_start = offset / block_size * block_size;
    const size_t num_blocks = (end_offset - first_block_start + block_size - 1) / block_size;

    PageCacheByteRange block_range;
    for (size_t i = 0; i < num_blocks; ++i)
    {
        block_range.offset = first_block_start + i * block_size;
        block_range.size = std::min(block_size, file_size.value() - block_range.offset);
        if (!cache->contains(block_range.hash(cache_key_base_hash), settings.random_eviction_for_tests))
            return false;
    }
    return true;
}

bool CachedInMemoryReadBufferFromFile::isContentCached(size_t offset, size_t /*size*/)
{
    /// Usually this is called immediately after seek()ing to `offset`.

    if (!working_buffer.empty())
    {
        chassert(chunk);
        return chunk->range.offset <= offset && chunk->range.offset + chunk->range.size > offset;
    }

    size_t block_size = settings.block_size;
    cache_range.offset = offset / block_size * block_size;
    cache_range.size = std::min(block_size, file_size.value() - cache_range.offset);

    /// Use get() instead of contains() to populate `chunk`, so the subsequent nextImpl() call
    /// can reuse it without a second cache lookup.
    UInt128 key_hash = cache_range.hash(cache_key_base_hash);
    chunk = cache->get(key_hash, settings.random_eviction_for_tests);

    return chunk != nullptr;
}

}
