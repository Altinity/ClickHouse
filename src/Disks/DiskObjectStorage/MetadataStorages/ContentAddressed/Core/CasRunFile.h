#pragma once
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Dense block-framed sorted binary run (spec §Backpressure And Journal Encoding). This is the hot
/// data-plane format: no per-record protobuf tags, no varints in the framing scalars, fixed-width
/// CRC32C per block, sparse footer index. `payload` is specialized by `kind`; the key schema is fixed
/// per kind (blob_hash; (blob_hash, source_id); (target_shard, blob_hash); etc.). The format is
/// deterministic so a write-once run is byte-reproducible for resume/adoption (OQ5).
enum class RunKind : uint8_t
{
    BlobDelta = 2,
    SourceEdge = 3,
    ManifestEntries = 4,
    TargetShardDelta = 5,
};

/// Block sizing (OQ5). Target a large sequential block; hard-cap any single block.
constexpr uint32_t kRunTargetBlockSize = 256u * 1024u;
constexpr uint32_t kRunHardCapBlockSize = 1024u * 1024u;
constexpr uint16_t kRunFormatVersion = 1;
/// codec 0 = no compression (the only codec in Phase 1a; hashes are high-entropy).
constexpr uint8_t kRunCodecNone = 0;

struct RunHeader
{
    char magic[4] = {'C', 'A', 'R', 'N'};
    uint16_t format_version = kRunFormatVersion;
    RunKind kind = RunKind::BlobDelta;
    uint8_t key_schema = 0;     /// fixed per kind; meaning owned by the producer
    uint8_t codec = kRunCodecNone;
    uint32_t block_size = kRunTargetBlockSize;
};

/// Streaming writer. Keys must be appended in non-decreasing order (the caller sorts). Memory is
/// bounded by one in-flight block (<= hard cap) plus the footer index. `finish` must be called
/// exactly once; it flushes the last block and writes the footer.
class RunFileWriter
{
public:
    RunFileWriter(WriteBuffer & out_, RunHeader header_);
    void append(std::string_view key, std::string_view payload);
    void finish();

private:
    struct BlockIndexEntry
    {
        uint64_t block_offset = 0;
        String min_key;
        String max_key;
    };

    void flushBlock();

    WriteBuffer & out;
    RunHeader header;
    uint64_t bytes_written = 0;          /// running file offset (header + sealed blocks)
    String block_payload;                /// in-flight DataBlock payload
    uint32_t block_records = 0;
    String block_min_key;
    String block_max_key;
    String prev_key;
    bool have_prev_key = false;
    std::vector<BlockIndexEntry> index;
    uint64_t total_count = 0;
    bool finished = false;
};

/// Streaming reader. `next` yields records in stored (sorted) order. `seek(key)` repositions the
/// cursor to the first record whose key >= `key`, using the sparse footer index to skip whole blocks
/// (one ranged read region per touched block).
class RunFileReader
{
public:
    explicit RunFileReader(ReadBuffer & in_);
    bool next(String & key, String & payload);
    void seek(std::string_view key);
    RunKind kind() const { return header.kind; }
    uint8_t keySchema() const { return header.key_schema; }

private:
    struct BlockIndexEntry
    {
        uint64_t block_offset = 0;
        String min_key;
        String max_key;
    };

    void loadFooter();
    bool loadBlock(size_t block_no);

    ReadBuffer & in;
    RunHeader header;
    std::vector<BlockIndexEntry> index;
    uint64_t total_count = 0;

    String full;   /// the materialized run bytes (this layer reads in-memory backends)

    /// in-memory cursor over the currently loaded block
    String cur_block;
    size_t cur_block_pos = 0;
    uint32_t cur_block_records = 0;
    uint32_t cur_record_no = 0;
    size_t cur_block_idx = 0;
    bool block_loaded = false;
    bool exhausted = false;
};

/// K-way merge over several sorted `RunFileReader`s. `next` advances all readers positioned at the
/// smallest key and returns that key together with EVERY payload stored for it (across all inputs and
/// across duplicate-key records within one input). Inputs must share a key ordering (they do: keys are
/// byte-compared).
///
/// Memory: the O(inputs * block_size) bound applies once block-ranged reads replace the Phase-1a
/// whole-run `full` materialization. TODAY each `RunFileReader` materializes its entire run into
/// `full`, so resident memory is O(sum of run sizes); the merge front itself is O(inputs * block_size).
class RunMerger
{
public:
    explicit RunMerger(std::vector<std::unique_ptr<RunFileReader>> readers_);
    bool next(String & key, std::vector<String> & payloads_for_key);

private:
    struct Front
    {
        size_t reader_idx = 0;
        String key;
        String payload;
        bool valid = false;
    };

    void pull(size_t reader_idx);

    std::vector<std::unique_ptr<RunFileReader>> readers;
    std::vector<Front> fronts;   /// one current front record per reader
};

}
