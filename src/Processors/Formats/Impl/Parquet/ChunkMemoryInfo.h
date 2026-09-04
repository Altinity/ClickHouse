#pragma once
#include <Processors/Chunk.h>
#include <atomic>
#include <memory>

namespace DB::Parquet
{

/// Keeps a delivered chunk's bytes charged to the reader's Decoded pool until the pipeline drops
/// the chunk. The counter is shared with `ReadManager` so it outlives the reader.
///
/// A `Chunk` can be cloned (e.g. by the query result cache, or `CopyTransform` fanning a chunk out
/// to multiple downstream ports) -- `ChunkInfoCloneable::clone()` copy-constructs this class, and
/// each resulting copy independently charges and later uncharges `bytes`. This means a chunk that
/// gets cloned N times charges the pool N times for memory that may substantially overlap (cloned
/// `IColumn`s can share underlying buffers via `shared_ptr`/COW). That's a deliberate conservative
/// over-count, not a bug: it's simpler and safer than trying to track sharing, and it only ever
/// makes the reader more cautious about admitting new decode work, never less.
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

    /// Not assignable: the implicit assignment operator would overwrite `counter`/`bytes` without
    /// uncharging the old values or charging the new ones, silently corrupting the pool's accounting.
    ChunkMemoryInfo & operator=(const ChunkMemoryInfo &) = delete;

    ~ChunkMemoryInfo() override
    {
        counter->fetch_sub(ssize_t(bytes), std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<ssize_t>> counter;
    size_t bytes;
};

}
