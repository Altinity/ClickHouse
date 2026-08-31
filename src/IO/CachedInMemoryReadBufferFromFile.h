#pragma once

#include <Common/VectorWithMemoryTracking.h>
#include <atomic>
#include <mutex>

#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadSettings.h>
#include <Common/PageCache.h>

namespace DB
{

class CachedInMemoryReadBufferFromFile : public ReadBufferFromFileBase
{
public:
    /// `in_` must support using external buffer. I.e. we assign its internal_buffer before
    /// each in_->next() call and expect the read data to be put into that buffer.
    /// `in_` should be seekable and should be able to read the whole file from 0 to in_->getFileSize();
    /// in particular, don't call setReadUntilPosition() on `in_` directly, call
    /// CachedInMemoryReadBufferFromFile::setReadUntilPosition().
    CachedInMemoryReadBufferFromFile(PageCacheFile cache_file_, PageCachePtr cache_, std::unique_ptr<ReadBufferFromFileBase> in_, const PageCacheSettings & settings_);

    String getFileName() const override;
    String getInfoForLog() override;
    bool isSeekCheap() override;

    bool isContentCached(size_t offset, size_t size) override;

    off_t seek(off_t off, int whence) override;
    off_t getPosition() override;
    size_t getFileOffsetOfBufferEnd() const override;
    bool supportsRightBoundedReads() const override { return true; }
    void setReadUntilPosition(size_t position) override;
    void setReadUntilEnd() override;

    size_t readBigAt(char * to, size_t n, size_t offset, const std::function<bool(size_t m)> & progress_callback) const override;
    bool supportsReadAt() override { return innerSupportsReadAt(); }

    VectorWithMemoryTracking<CachedRegion> readBigAtRetainCells(size_t n, size_t offset) const override;
    bool supportsReadAtRetainCells() const override { return innerSupportsReadAt(); }

    /// How many bytes of already-cached blocks `populateBlockRange` may fetch and throw away in order to
    /// join two runs of missing blocks into one request. Derived from this buffer's own fitted bandwidth
    /// and round-trip time (see `readThroughBudgetBytes`), so it needs no configuration; a non-zero
    /// value set here overrides the derivation, and `0` (the default) restores it.
    void setReadThroughBudgetOverrideBytes(size_t budget_bytes) { read_through_budget_override.store(budget_bytes, std::memory_order_relaxed); }

    /// How many bytes the read-through may waste, as a permille of the bytes the request is missing.
    /// The time model (`bandwidth * rtt`) bounds what one island may cost; this bounds what the whole
    /// request may spend, and it is the part that depends on the caller: extra bytes are cheap while
    /// the reader is latency-bound (few concurrent reads, idle pipe) and expensive once it is
    /// bandwidth-bound (many concurrent reads saturating the link), which only the caller can know.
    /// Default 250 permille; `DB::Parquet::Prefetcher` sets it from its IO pool size.
    void setReadThroughWastePermille(size_t permille) { read_through_waste_permille.store(permille, std::memory_order_relaxed); }

    /// Whether every cache block covering [offset, offset + n) is present in the cache right now.
    /// Unlike `isContentCached`, this touches no buffer state (no seek, no `chunk` population), so it
    /// follows the same thread-safety rules as `readBigAt`: concurrent calls are allowed. Advisory
    /// only -- a block can be evicted between this call and the read that follows it.
    bool isBigRangeCached(size_t offset, size_t n) const;
    bool isRangeLocal(size_t offset, size_t n) const override { return isBigRangeCached(offset, n); }

    PageCache::MappedPtr getPageCacheCell() const { return chunk; }
    PageCachePtr getPageCache() const { return cache; }

private:
    const PageCacheFile cache_file;
    PageCacheByteRange cache_range; // offset is offset of `chunk` start
    SipHash cache_key_base_hash;
    PageCachePtr cache;
    PageCacheSettings settings;
    std::unique_ptr<ReadBufferFromFileBase> in;

    size_t file_offset_of_buffer_end = 0;
    size_t read_until_position;
    /// From the latest call to in->setReadUntilPosition.
    size_t inner_read_until_position;

    PageCache::MappedPtr chunk;

    /// See setReadThroughBudgetOverrideBytes.
    std::atomic<size_t> read_through_budget_override {0};
    /// See setReadThroughWastePermille.
    std::atomic<size_t> read_through_waste_permille {250};

    /// Fitted cost of a source read, from this buffer's own `in->readBigAt` calls: `rtt_us` is the time
    /// to the first byte, `bandwidth_bytes_per_us` the rate after it, `bandwidth_peak_bytes_per_us` the
    /// best rate seen. Guarded by `read_stats_mutex`; updated once per source read (rare relative to
    /// cache hits).
    mutable std::mutex read_stats_mutex;
    mutable double stat_rtt_us = 0;
    mutable double stat_bandwidth_bytes_per_us = 0;
    mutable double stat_bandwidth_peak_bytes_per_us = 0;
    mutable size_t stat_samples = 0;

    /// Bytes of already-cached blocks a miss run may read through, from the fitted stats above.
    size_t readThroughBudgetBytes() const;
    /// Fold one completed source read into the fitted stats.
    void updateReadStats(size_t read_bytes, uint64_t first_byte_us, uint64_t total_us) const;

    /// Lazy: `in->supportsReadAt` may do HTTP/fstat, so don't probe in the ctor.
    /// `call_once` also keeps the probe from racing with parallel `readBigAt` calls.
    mutable std::once_flag inner_supports_read_at_init;
    mutable bool inner_supports_read_at = false;
    bool innerSupportsReadAt() const;

    /// Ensures all cache blocks covering [offset, offset+n) are populated.
    /// Returns a vector of MappedPtr, one per block. Each missing block is read
    /// individually via `readBigAt` directly into its cache cell.
    /// `block_callback` is called after reading each cell, in sequence.
    /// The callback may move the cell out; then the returned vector will have nullptr.
    /// If `block_callback` returns true, reading stops.
    VectorWithMemoryTracking<PageCache::MappedPtr> populateBlockRange(size_t offset, size_t n, const std::function<bool(PageCache::MappedPtr &)> & block_callback = nullptr) const;

    bool nextImpl() override;
};

}
