#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
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

/// Dense block-framed sorted binary run: fixed-width CRC32C per block, sparse footer index. Since
/// codecs-v3 phase 5 the ONLY surviving kind is `ManifestEntries` — the stream embedded in a `CAPT`
/// part manifest (`CasManifestCodec`), read sequentially. The standalone source-edge `cas_run` object
/// moved to the sorted-NDJSON `Formats/CasRecordStreamFormat`; the dead `BlobDelta`/`SourceEdge`/
/// `TargetShardDelta` kinds are removed. Phase 6 deletes this whole file when it converts the embedded
/// manifest stream to text.
enum class RunKind : uint8_t
{
    ManifestEntries = 4,   /// value frozen; embedded part-manifest entry stream (phase-6-owned)
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
    RunKind kind = RunKind::ManifestEntries;
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

/// Sequential reader. `next` yields records in stored (sorted) order. (Random-access `seek` was deleted
/// in codecs-v3 phase 5 with its only caller; the surviving consumer — the embedded part-manifest
/// stream in `CasManifestCodec` — reads sequentially.)
///
/// Two modes, one interface (spec 2026-07-02 snapshot-streaming §RunFileReader modes):
///   - BORROWED: zero-copy over caller-owned bytes; the whole run is already resident (the caller
///     materialized it). Resident state is the caller's buffer plus one decoded block.
///   - STREAMING: reads a WRITE-ONCE run object off a `Backend` at O(one block) resident memory. Open
///     is exactly `head` + one tail ranged `get` (footer) + one body `getStream`. There is NO whole-run
///     member — the resident-memory proof is structural.
///
/// Fail-closed in BOTH modes: an absent key, a truncated/short stream, or ANY CRC failure throws
/// CORRUPTED_DATA — never a partial record.
class RunFileReader
{
public:
    /// Borrowed-memory mode: zero-copy over caller-owned bytes (the caller must keep them alive for
    /// the reader's lifetime). Replaces the old copying ReadBuffer constructor.
    explicit RunFileReader(std::string_view bytes);

    /// Streaming mode: head + tail-footer ranged get + body getStream; resident state is the footer
    /// index + ONE current block (<= kRunHardCapBlockSize). Throws CORRUPTED_DATA on an absent key,
    /// truncated stream, or any CRC failure.
    RunFileReader(Backend & backend_, const String & key_);

    bool next(String & key, String & payload);
    RunKind kind() const { return header.kind; }
    uint8_t keySchema() const { return header.key_schema; }

private:
    struct BlockIndexEntry
    {
        uint64_t block_offset = 0;
        String min_key;
        String max_key;
    };

    /// Parse the 13-byte header out of `head_bytes` (borrowed: the whole run; streaming: bytes drained
    /// from the front of the body stream) and record `header`. Fail-closed on a short/bad header.
    void parseHeader(std::string_view head_bytes);
    /// Parse + CRC-verify the footer over `footer_bytes`, whose LAST byte is the object's last byte;
    /// `footer_base` is the absolute file offset of `footer_bytes[0]` (0 in borrowed mode, the tail
    /// window start in streaming mode). Fills `index`, `total_count`, `data_end`.
    void loadFooter(std::string_view footer_bytes, uint64_t footer_base);
    bool loadBlock(size_t block_no);
    /// Decode one CRC-verified block frame (block_len prefix already consumed / bounded) from `frame`
    /// into the current-block cursor. `frame` starts at the `block_len` u32.
    void installBlockFrame(std::string_view frame, size_t block_no);
    /// Read exactly `n` bytes from `body_stream` into `out` or throw CORRUPTED_DATA (short stream).
    void readExactFromStream(String & out, size_t n);

    bool streaming = false;                     /// false => borrowed (reads `mem`); true => streaming
    std::string_view mem;                       /// borrowed mode: the whole run bytes (caller-owned)
    Backend * backend = nullptr;                /// streaming mode
    String object_key;                          ///   " the run object's key
    std::unique_ptr<ReadBuffer> body_stream;    ///   " forward stream, positioned just after the header
    uint64_t body_pos = 0;                      ///   " absolute file offset the stream is positioned at
    uint64_t object_size = 0;                   ///   " object size from head
    bool seeked = false;                        ///   " once true, ALL further blocks come via ranged get
    uint64_t data_end = 0;                      /// first footer byte (both modes)

    RunHeader header;
    std::vector<BlockIndexEntry> index;
    uint64_t total_count = 0;

    /// in-memory cursor over the currently loaded block
    String cur_block;
    size_t cur_block_pos = 0;
    uint32_t cur_block_records = 0;
    uint32_t cur_record_no = 0;
    size_t cur_block_idx = 0;
    bool block_loaded = false;
    bool exhausted = false;
};

/// (`RunMerger` — the k-way merge — deleted in codecs-v3 phase 5: no production caller ever existed
/// (the fold uses a hand-rolled two-cursor loop), and the source-edge runs it merged moved to
/// `Formats/CasRecordStreamFormat`.)

}
