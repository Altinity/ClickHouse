#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <crc32c/crc32c.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

void putLE32(String & s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putLenPrefixed(String & s, std::string_view bytes)
{
    putLE32(s, static_cast<uint32_t>(bytes.size()));
    s.append(bytes.data(), bytes.size());
}

uint32_t crc32cOf(std::string_view bytes)
{
    return crc32c::Crc32c(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

}

RunFileWriter::RunFileWriter(WriteBuffer & out_, RunHeader header_) : out(out_), header(header_)
{
    /// RunHeader: magic[4], format_version u16, kind u8, key_schema u8, codec u8, block_size u32.
    out.write(header.magic, 4);
    writeBinaryLittleEndian(header.format_version, out);
    writeBinaryLittleEndian(static_cast<uint8_t>(header.kind), out);
    writeBinaryLittleEndian(header.key_schema, out);
    writeBinaryLittleEndian(header.codec, out);
    writeBinaryLittleEndian(header.block_size, out);
    bytes_written = 4 + 2 + 1 + 1 + 1 + 4;
}

void RunFileWriter::append(std::string_view key, std::string_view payload)
{
    if (finished)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: append after finish");
    if (have_prev_key && key < prev_key)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: keys must be non-decreasing");

    /// Encoded record size: key_len u32 + key + payload_len u32 + payload.
    const size_t rec_size = 4 + key.size() + 4 + payload.size();

    /// Seal the current block first if adding this record would exceed the target and the block is
    /// non-empty (deterministic boundary). A single oversize record (> hard cap) still gets its own
    /// block - we never split a record.
    if (block_records > 0 && block_payload.size() + rec_size > header.block_size)
        flushBlock();

    if (block_records == 0)
        block_min_key.assign(key.begin(), key.end());
    block_max_key.assign(key.begin(), key.end());

    putLE32(block_payload, static_cast<uint32_t>(key.size()));
    block_payload.append(key.data(), key.size());
    putLE32(block_payload, static_cast<uint32_t>(payload.size()));
    block_payload.append(payload.data(), payload.size());
    ++block_records;
    ++total_count;

    prev_key.assign(key.begin(), key.end());
    have_prev_key = true;

    if (block_payload.size() >= kRunHardCapBlockSize)
        flushBlock();
}

void RunFileWriter::flushBlock()
{
    if (block_records == 0)
        return;

    BlockIndexEntry idx;
    idx.block_offset = bytes_written;
    idx.min_key = block_min_key;
    idx.max_key = block_max_key;
    index.push_back(std::move(idx));

    /// DataBlock: block_len u32, record_count u32, min_key(len-prefixed), max_key(len-prefixed),
    /// crc32c u32, payload. block_len is the byte length of (record_count..payload) inclusive.
    String block_head;
    putLE32(block_head, block_records);
    putLenPrefixed(block_head, block_min_key);
    putLenPrefixed(block_head, block_max_key);
    putLE32(block_head, crc32cOf(block_payload));
    const uint32_t block_len = static_cast<uint32_t>(block_head.size() + block_payload.size());

    writeBinaryLittleEndian(block_len, out);
    out.write(block_head.data(), block_head.size());
    out.write(block_payload.data(), block_payload.size());
    bytes_written += 4 + block_len;

    block_payload.clear();
    block_records = 0;
    block_min_key.clear();
    block_max_key.clear();
}

void RunFileWriter::finish()
{
    if (finished)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: finish twice");
    flushBlock();

    /// RunFooter: block_count u32, per block { block_offset u64, min_key(lp), max_key(lp) },
    /// total_count u64, footer_crc32c u32, footer_len u32 trailer.
    String footer;
    putLE32(footer, static_cast<uint32_t>(index.size()));
    for (const auto & e : index)
    {
        for (int i = 0; i < 8; ++i)
            footer.push_back(static_cast<char>((e.block_offset >> (8 * i)) & 0xFF));
        putLenPrefixed(footer, e.min_key);
        putLenPrefixed(footer, e.max_key);
    }
    for (int i = 0; i < 8; ++i)
        footer.push_back(static_cast<char>((total_count >> (8 * i)) & 0xFF));
    putLE32(footer, crc32cOf(footer));
    const uint32_t footer_len = static_cast<uint32_t>(footer.size() + 4);   /// + the trailer itself

    out.write(footer.data(), footer.size());
    writeBinaryLittleEndian(footer_len, out);
    finished = true;
}

/// ---- reader ----

namespace
{

constexpr size_t kHeaderLen = 13;
/// Footer allowance: the tail ranged get must cover the entire footer. A footer body holds the block
/// index (min/max keys are 32 bytes for edge runs, small for others) plus the fixed scalars; 64 KiB
/// is the design budget (Global Constraints). We read min(size, kRunHardCapBlockSize + 64KB) suffix so
/// the SAME read that carries the footer can, for a small object, carry the whole run.
constexpr size_t kFooterAllowance = 64u * 1024u;

/// Bounded little-endian scalar reads over an UNTRUSTED byte view. `require`/`le32`/`le64` map every
/// out-of-bounds access to CORRUPTED_DATA (never an unchecked operator[] — under libc++ hardening that
/// would be a heap over-read, and substr past end throws std::out_of_range which is not fail-closed).
uint32_t le32of(std::string_view s, size_t off)
{
    if (off > s.size() || 4 > s.size() - off)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: field out of bounds");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
    return v;
}

uint64_t le64of(std::string_view s, size_t off)
{
    if (off > s.size() || 8 > s.size() - off)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: field out of bounds");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
    return v;
}

}

RunFileReader::RunFileReader(std::string_view bytes) : mem(bytes)
{
    /// Borrowed mode: the whole run is resident (caller-owned). Block offsets stored in the footer are
    /// absolute from file start, so they index `mem` directly. Parse the header, then the footer.
    parseHeader(mem);
    loadFooter(mem, /*footer_base=*/0);
}

RunFileReader::RunFileReader(Backend & backend_, const String & key_)
    : streaming(true), backend(&backend_), object_key(key_)
{
    /// STREAMING open = exactly three backend requests (Global Constraints request-profile gate):
    ///   1. head       — size + existence (absent => CORRUPTED_DATA "run object absent").
    ///   2. get(tail)  — the suffix window that carries the footer (and, for a small object, the whole
    ///                   run); bounded to kRunHardCapBlockSize + footer allowance so no single request
    ///                   exceeds the resident-memory bound.
    ///   3. getStream  — the body, positioned (after we drain the 13-byte header) at the first block.
    /// No separate header get: the header is drained from the front of the body stream. A `seek` later
    /// adds one ranged get per touched block; the pure-linear fold path never seeks.
    const HeadResult h = backend->head(object_key);
    if (!h.exists)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: run object absent: {}", object_key);
    object_size = h.size;

    const size_t tail_window = std::min<size_t>(object_size, kRunHardCapBlockSize + kFooterAllowance);
    const uint64_t tail_base = object_size - tail_window;
    auto tail = backend->get(object_key, Range{.offset = tail_base, .length = tail_window});
    if (!tail)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: run object vanished mid-open: {}", object_key);
    if (tail->bytes.size() != tail_window)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: short tail read for {}", object_key);

    /// The tail probe may be SMALLER than the footer of a large run: the sparse index grows with the
    /// block count (offset u64 + two length-prefixed boundary keys per block), so a multi-GB run's
    /// footer exceeds any fixed probe (~13k blocks already overflow this one). Read the footer_len
    /// trailer from the probe and, when the footer does not fit, re-read the EXACT footer window with
    /// one more ranged get — the open profile becomes 4 requests for such runs, and the resident bound
    /// stays footer + one block (the index IS part of the reader's documented resident state).
    if (tail->bytes.size() < 4)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated (no footer_len trailer): {}", object_key);
    const auto trailer_le32 = [&](size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(tail->bytes[off + i])) << (8 * i);
        return v;
    };
    const uint32_t footer_len = trailer_le32(tail->bytes.size() - 4);
    if (footer_len > object_size)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad footer_len {} for {}", footer_len, object_key);
    if (footer_len > tail->bytes.size())
    {
        const uint64_t footer_base = object_size - footer_len;
        auto footer = backend->get(object_key, Range{.offset = footer_base, .length = footer_len});
        if (!footer || footer->bytes.size() != footer_len)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: short footer read for {}", object_key);
        loadFooter(footer->bytes, footer_base);
    }
    else
        loadFooter(tail->bytes, tail_base);

    /// Open the forward body stream at offset 0 and drain the 13-byte header from it, leaving the
    /// stream positioned at the first block. Bounding the stream to `data_end` keeps it to the run body
    /// (the footer never streams). getStream over a WRITE-ONCE run: the token is irrelevant here (the
    /// object never mutates under us); we only consume bytes.
    auto sr = backend->getStream(object_key, Range{.offset = 0, .length = data_end});
    if (!sr)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: run object vanished mid-open: {}", object_key);
    body_stream = std::move(sr->stream);
    String head_bytes;
    readExactFromStream(head_bytes, kHeaderLen);
    parseHeader(head_bytes);
    body_pos = kHeaderLen;
}

void RunFileReader::parseHeader(std::string_view head_bytes)
{
    /// Header: magic[4], format_version u16, kind u8, key_schema u8, codec u8, block_size u32 = 13 bytes.
    if (head_bytes.size() < kHeaderLen)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated header");
    if (head_bytes.substr(0, 4) != "CARN")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad magic");
    std::memcpy(header.magic, head_bytes.data(), 4);
    header.format_version = static_cast<uint16_t>(static_cast<uint8_t>(head_bytes[4])
        | (static_cast<uint16_t>(static_cast<uint8_t>(head_bytes[5])) << 8));
    checkCompatibility(header.format_version, "RunFile");
    header.kind = static_cast<RunKind>(static_cast<uint8_t>(head_bytes[6]));
    header.key_schema = static_cast<uint8_t>(head_bytes[7]);
    header.codec = static_cast<uint8_t>(head_bytes[8]);
    header.block_size = le32of(head_bytes, 9);
    if (header.codec != kRunCodecNone)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: unsupported codec {}", header.codec);
}

void RunFileReader::readExactFromStream(String & out, size_t n)
{
    out.clear();
    out.resize(n);
    size_t got = 0;
    while (got < n)
    {
        const size_t r = body_stream->read(out.data() + got, n - got);
        if (r == 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated stream (short read)");
        got += r;
    }
}

void RunFileReader::loadFooter(std::string_view footer_bytes, uint64_t footer_base)
{
    /// SECURITY: every length/offset read here comes from UNTRUSTED bytes. Bound-check the trailer and
    /// verify the footer CRC BEFORE trusting any in-footer length, then still bound-check every offset
    /// against the footer body while parsing (defense in depth). `footer_bytes` ends at the object's
    /// last byte; `footer_base` is the absolute file offset of `footer_bytes[0]` (0 in borrowed mode,
    /// the tail-window start in streaming mode). The stored per-block `block_offset` values are ABSOLUTE
    /// file offsets, so we keep them as-is — the streaming path ranged-gets them directly.

    /// 1. Bound-check the trailer and locate the footer body. Min footer body = block_count(u32) +
    /// total_count(u64) + footer_crc(u32) = 16 bytes; footer_len counts the body plus its own trailing
    /// u32, so footer_len >= 20. The footer must start at or after the 13-byte header (absolute).
    constexpr size_t kMinFooterBody = 4 + 8 + 4;
    constexpr size_t kMinFooterLen = kMinFooterBody + 4;
    if (footer_bytes.size() < 4)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated");
    const uint32_t footer_len = le32of(footer_bytes, footer_bytes.size() - 4);
    /// footer_len is measured against the WHOLE object; in streaming mode `footer_bytes` is only a
    /// suffix, so the footer must fit inside the window we read (and the object).
    if (footer_len < kMinFooterLen || footer_len > footer_bytes.size()
        || footer_len > footer_base + footer_bytes.size())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad footer_len {}", footer_len);
    const size_t footer_start = footer_bytes.size() - footer_len;    /// start of footer body (in view)
    const uint64_t footer_start_abs = footer_base + footer_start;
    if (footer_start_abs < kHeaderLen)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer overlaps header");
    const size_t crc_pos = footer_bytes.size() - 4 - 4;              /// the stored footer crc u32 (in view)

    /// 2. Verify the footer CRC FIRST, over [footer_start, crc_pos). Only then trust in-footer lengths.
    const uint32_t want_crc = le32of(footer_bytes, crc_pos);
    const std::string_view footer_body(footer_bytes.data() + footer_start, crc_pos - footer_start);
    if (crc32cOf(footer_body) != want_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer crc mismatch");

    /// 3. Parse the CRC-validated footer. Still bound-check every offset/length against the footer body
    /// (defense in depth): a CRC-valid-but-self-inconsistent footer must fail closed, not over-read.
    auto requireInFooterBody = [&](size_t pos, size_t n)
    {
        if (pos > crc_pos || n > crc_pos - pos)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer entry out of bounds");
    };

    size_t pos = footer_start;
    const uint32_t block_count = le32of(footer_bytes, pos); pos += 4;
    index.clear();
    index.reserve(std::min<size_t>(block_count, 64));       /// cap reservation on untrusted count
    for (uint32_t b = 0; b < block_count; ++b)
    {
        BlockIndexEntry e;
        requireInFooterBody(pos, 8); e.block_offset = le64of(footer_bytes, pos); pos += 8;
        requireInFooterBody(pos, 4); uint32_t mn = le32of(footer_bytes, pos); pos += 4;
        requireInFooterBody(pos, mn); e.min_key.assign(footer_bytes.data() + pos, mn); pos += mn;
        requireInFooterBody(pos, 4); uint32_t mx = le32of(footer_bytes, pos); pos += 4;
        requireInFooterBody(pos, mx); e.max_key.assign(footer_bytes.data() + pos, mx); pos += mx;
        index.push_back(std::move(e));
    }
    requireInFooterBody(pos, 8); total_count = le64of(footer_bytes, pos); pos += 8;
    /// The block index must consume exactly the footer body up to the stored crc (no trailing slack).
    if (pos != crc_pos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer length inconsistent");

    /// `data_end` = first footer byte (absolute). The run body is [kHeaderLen, data_end).
    data_end = footer_start_abs;
}

void RunFileReader::installBlockFrame(std::string_view frame, size_t block_no)
{
    /// `frame` starts at the block_len u32. Bound-check every field against `frame`, so a
    /// self-inconsistent block fails closed, never over-reads (N4 hardening).
    size_t off = 0;
    const uint32_t block_len = le32of(frame, off); off += 4;
    if (block_len > frame.size() - off)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    const size_t block_end = off + block_len;
    const uint32_t rec_count = le32of(frame, off); off += 4;
    uint32_t mn = le32of(frame, off); off += 4;
    if (mn > frame.size() - off) throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    off += mn;   /// skip min_key
    uint32_t mx = le32of(frame, off); off += 4;
    if (mx > frame.size() - off) throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    off += mx;   /// skip max_key
    const uint32_t stored_crc = le32of(frame, off); off += 4;
    if (off > block_end)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block header exceeds block");
    const std::string_view payload(frame.data() + off, block_end - off);
    if (crc32cOf(payload) != stored_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block crc mismatch");

    cur_block.assign(payload.data(), payload.size());
    cur_block_pos = 0;
    cur_block_records = rec_count;
    cur_record_no = 0;
    cur_block_idx = block_no;
    block_loaded = true;
}

bool RunFileReader::loadBlock(size_t block_no)
{
    if (block_no >= index.size())
    {
        exhausted = true;
        return false;
    }

    const uint64_t block_off = index[block_no].block_offset;
    /// The frame spans [block_off, next_block_off) — the next index entry's offset, or `data_end` for
    /// the last block. This is the exact frame length (block_len prefix + body).
    const uint64_t frame_end = (block_no + 1 < index.size()) ? index[block_no + 1].block_offset : data_end;
    if (block_off < kHeaderLen || frame_end > data_end || block_off >= frame_end)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block offset out of bounds");
    const size_t frame_len = static_cast<size_t>(frame_end - block_off);

    if (!streaming)
    {
        /// Borrowed: the frame is a window into `mem`.
        if (block_off > mem.size() || frame_len > mem.size() - block_off)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
        installBlockFrame(mem.substr(block_off, frame_len), block_no);
        return true;
    }

    /// Streaming. Sequential-from-stream while we have not seeked AND the stream is exactly at this
    /// frame; otherwise (after any seek, or a non-contiguous jump) a ranged get for this one frame.
    String frame;
    if (!seeked && body_pos == block_off)
    {
        readExactFromStream(frame, frame_len);
        body_pos += frame_len;
    }
    else
    {
        auto got = backend->get(object_key, Range{.offset = block_off, .length = frame_len});
        if (!got || got->bytes.size() != frame_len)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: short ranged block read for {}", object_key);
        frame = std::move(got->bytes);
    }
    installBlockFrame(frame, block_no);
    return true;
}

bool RunFileReader::next(String & key, String & payload)
{
    if (exhausted)
        return false;
    if (!block_loaded && !loadBlock(0))
        return false;
    while (cur_record_no >= cur_block_records)
    {
        if (!loadBlock(cur_block_idx + 1))
            return false;
    }
    auto le32at = [&](const String & s, size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
        return v;
    };
    uint32_t klen = le32at(cur_block, cur_block_pos); cur_block_pos += 4;
    key = cur_block.substr(cur_block_pos, klen); cur_block_pos += klen;
    uint32_t plen = le32at(cur_block, cur_block_pos); cur_block_pos += 4;
    payload = cur_block.substr(cur_block_pos, plen); cur_block_pos += plen;
    ++cur_record_no;
    return true;
}

void RunFileReader::seek(std::string_view seek_key)
{
    exhausted = false;
    if (index.empty())
    {
        exhausted = true;
        return;
    }
    /// After any seek the linear stream is no longer authoritative: ALL subsequent blocks (this one and
    /// every block reached by later next()s) come via ranged gets (streaming mode). The pure-linear
    /// fold path never seeks, so this never regresses the 3-request open profile.
    seeked = true;
    /// Find the last block whose min_key <= key (sparse index); start scanning there.
    size_t target = 0;
    for (size_t b = 0; b < index.size(); ++b)
    {
        if (index[b].min_key <= seek_key)
            target = b;
        else
            break;
    }
    loadBlock(target);
    /// Advance within the block to the first record with stored_key >= key.
    String k, p;
    size_t save_pos = cur_block_pos;
    uint32_t save_rec = cur_record_no;
    while (cur_record_no < cur_block_records)
    {
        save_pos = cur_block_pos;
        save_rec = cur_record_no;
        if (!next(k, p))
            return;
        if (k >= seek_key)
        {
            /// rewind one record so the next next() re-yields it
            cur_block_pos = save_pos;
            cur_record_no = save_rec;
            return;
        }
    }
}

/// ---- merger ----

RunMerger::RunMerger(std::vector<std::unique_ptr<RunFileReader>> readers_) : readers(std::move(readers_))
{
    fronts.resize(readers.size());
    for (size_t i = 0; i < readers.size(); ++i)
        pull(i);
}

void RunMerger::pull(size_t reader_idx)
{
    Front & f = fronts[reader_idx];
    f.reader_idx = reader_idx;
    f.valid = readers[reader_idx]->next(f.key, f.payload);
}

bool RunMerger::next(String & key, std::vector<String> & payloads_for_key)
{
    /// Find the smallest valid front key.
    bool any = false;
    String min_key;
    for (const auto & f : fronts)
    {
        if (!f.valid)
            continue;
        if (!any || f.key < min_key)
        {
            min_key = f.key;
            any = true;
        }
    }
    if (!any)
        return false;

    key = min_key;
    payloads_for_key.clear();
    /// Drain every front (and every consecutive duplicate-key record per reader) equal to min_key.
    for (size_t i = 0; i < fronts.size(); ++i)
    {
        while (fronts[i].valid && fronts[i].key == min_key)
        {
            payloads_for_key.push_back(fronts[i].payload);
            pull(i);
        }
    }
    return true;
}

}
