#pragma once
#include <Processors/Chunk.h>
#include <atomic>
#include <memory>

namespace DB::Parquet
{

/// Keeps a delivered chunk's bytes charged to the reader's Decoded pool until the pipeline drops
/// the chunk. The counter is shared with `ReadManager` so it outlives the reader.
class ChunkMemoryInfo : public ChunkInfoCloneable<ChunkMemoryInfo>
{
public:
    ChunkMemoryInfo(std::shared_ptr<std::atomic<ssize_t>> counter_, size_t bytes_)
        : counter(std::move(counter_)), bytes(bytes_)
    {
        counter->fetch_add(ssize_t(bytes), std::memory_order_relaxed);
    }

    ChunkMemoryInfo(const ChunkMemoryInfo & other) : counter(other.counter), bytes(other.bytes)
    {
        counter->fetch_add(ssize_t(bytes), std::memory_order_relaxed);
    }

    ~ChunkMemoryInfo() override
    {
        counter->fetch_sub(ssize_t(bytes), std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<ssize_t>> counter;
    size_t bytes;
};

}
