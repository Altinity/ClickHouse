#pragma once
#include <base/types.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// Content-defined chunking (CDC) for large CAS part files.
///
/// A whole-file blob only deduplicates against a byte-identical file. A merge that re-emits most of
/// its input bytes therefore publishes an entirely new blob, which is the storage cost measured in
/// Altinity/ClickHouse#2314. Splitting the stream at boundaries chosen by CONTENT rather than by
/// offset lets the unchanged runs land in chunks that already exist in the pool, so only the changed
/// runs cost new objects. Boundaries follow the content, so inserting or removing bytes anywhere in
/// the file re-aligns after at most one chunk instead of shifting every later boundary.
///
/// The rolling hash is a Gear hash (`h = (h << 1) + table[byte]`, the FastCDC shape): the left shift
/// retires a byte's contribution after 64 steps, so the hash depends on the trailing 64 bytes without
/// an explicit window buffer. A boundary is declared when the top `cutBits()` bits are all zero,
/// which for uniformly distributed content happens once per `avg_bytes`.
///
/// Chunk boundaries are NOT a persisted format. A manifest records the resulting chunk list
/// explicitly, so changing these parameters (or this algorithm) only changes the dedup hit rate of
/// files written afterwards; it can never make an already-written manifest unreadable. The gear table
/// is nonetheless derived from a pinned seed by `constexpr` integer arithmetic, so every build on
/// every platform cuts identically -- two replicas writing the same bytes must agree on boundaries or
/// they deduplicate nothing.
struct ChunkerParams
{
    /// No boundary is declared before this many bytes, which bounds the per-object request and
    /// manifest-entry overhead a pathological input can cause.
    uint64_t min_bytes = 1ull << 20;    /// 1 MiB
    /// The mean interval between hash-selected boundaries, i.e. the period of the cut predicate --
    /// NOT the realised mean chunk size. Because no boundary is accepted below `min_bytes`, the
    /// realised mean is approximately `min_bytes + avg_bytes`, truncated by `max_bytes`: the
    /// defaults below cut at about 5 MB on incompressible content. Only the bit width is used (see
    /// `cutBits`), so the effective value is the enclosing power of two; mask-based CDC cannot
    /// express an arbitrary period.
    uint64_t avg_bytes = 4ull << 20;    /// 4 MiB
    /// A boundary is forced here even if the hash never matches, which bounds the bytes a single
    /// re-upload can cost and keeps ranged reads of one chunk bounded.
    uint64_t max_bytes = 16ull << 20;   /// 16 MiB

    /// Throws `BAD_ARGUMENTS` unless `1 <= min_bytes <= avg_bytes <= max_bytes` and `cutBits()` lands
    /// in `[1, 63]`. Called once where the disk settings are parsed, so a misconfigured disk fails at
    /// mount rather than producing degenerate chunking on the write path.
    void validate() const;

    /// The number of leading hash bits required to be zero for a boundary: the bit width of
    /// `avg_bytes` less one, so an `avg_bytes` of 2^22 cuts with probability 2^-22 per byte.
    uint32_t cutBits() const;
};

/// What one `feed` call consumed, and whether it ended on a chunk boundary.
///
/// `boundary` is a separate flag rather than being implied by `taken < data.size()`, and that is
/// load-bearing: a boundary can fall exactly at the end of the fed span, in which case `taken ==
/// data.size()`. Conflating that with "no boundary found" would drop the boundary, and the rolling
/// hash would have moved past it by the next call -- so cut positions would depend on how the caller
/// happened to slice its input. Write-buffer sizes vary with adaptive sizing and per-column
/// settings, so that would make two writers of identical bytes disagree on boundaries and
/// deduplicate nothing.
struct ChunkerFeedResult
{
    /// Bytes from the front of `data` that belong to the current chunk.
    size_t taken = 0;
    /// A chunk boundary falls exactly after `taken` bytes.
    bool boundary = false;
};

/// Streaming boundary detector. It inspects bytes and reports where chunks end; it never buffers,
/// copies, or owns payload, so the caller keeps full control of where the bytes are written.
///
/// One instance handles one file. The caller loops: `feed` the next span, write the prefix it
/// consumed, and when it reports a boundary, close that chunk, call `startChunk`, and feed the rest.
class ContentChunker
{
public:
    explicit ContentChunker(ChunkerParams params_);

    /// Consume the next span of the file, reporting how many leading bytes belong to the current
    /// chunk and whether a boundary falls right after them. Bytes beyond `taken` have NOT been seen
    /// by the rolling hash, so re-feeding them after `startChunk` is correct.
    ChunkerFeedResult feed(std::string_view data);

    /// Start a new chunk: clears the rolling hash and the current chunk length. The hash is reset
    /// rather than carried across the boundary so a chunk's cut position depends only on its own
    /// bytes -- that is what makes an unchanged run reproduce the same boundaries regardless of what
    /// preceded it in the file, and therefore what makes dedup work at all.
    void startChunk();

    /// Bytes accumulated into the current chunk since the last `startChunk`.
    uint64_t currentChunkBytes() const { return chunk_len; }

private:
    ChunkerParams params;
    uint32_t cut_bits;
    uint64_t hash = 0;
    uint64_t chunk_len = 0;
};

}
