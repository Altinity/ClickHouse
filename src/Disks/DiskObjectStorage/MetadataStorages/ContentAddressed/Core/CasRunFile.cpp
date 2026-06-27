#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/copyData.h>
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

RunFileReader::RunFileReader(ReadBuffer & in_) : in(in_)
{
    /// Materialize the whole run up front (this layer reads from in-memory backends), header
    /// included, so block offsets stored in the footer (absolute from file start) index `full`
    /// directly. Parse the header out of the materialized bytes.
    {
        WriteBufferFromString tmp(full);
        copyData(in, tmp);
        tmp.finalize();
    }

    auto le16at = [&](size_t off) -> uint16_t
    {
        return static_cast<uint16_t>(static_cast<uint8_t>(full[off])
            | (static_cast<uint16_t>(static_cast<uint8_t>(full[off + 1])) << 8));
    };
    auto le32hdr = [&](size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };

    /// Header: magic[4], format_version u16, kind u8, key_schema u8, codec u8, block_size u32 = 13 bytes.
    if (full.size() < 13)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated header");
    if (std::string_view(full.data(), 4) != "CARN")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad magic");
    std::memcpy(header.magic, full.data(), 4);
    header.format_version = le16at(4);
    checkCompatibility(header.format_version, "RunFile");
    header.kind = static_cast<RunKind>(static_cast<uint8_t>(full[6]));
    header.key_schema = static_cast<uint8_t>(full[7]);
    header.codec = static_cast<uint8_t>(full[8]);
    header.block_size = le32hdr(9);
    if (header.codec != kRunCodecNone)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: unsupported codec {}", header.codec);
    loadFooter();
}

void RunFileReader::loadFooter()
{
    /// SECURITY: every length/offset read here comes from UNTRUSTED bytes. We must bound-check the
    /// trailer and verify the footer CRC BEFORE trusting any in-footer length, then still bound-check
    /// every offset against the footer body while parsing (defense in depth). Under libc++ release
    /// hardening `operator[]` is not bounds-checked (heap over-read = UB) and `substr(pos, n)` with
    /// `pos > size()` throws std::out_of_range — which decodeGuarded does NOT catch. So: no unchecked
    /// `operator[]`/`substr` on untrusted offsets; everything maps to CORRUPTED_DATA (a DB::Exception).

    /// `requireBytes(pos, n)`: assert that [pos, pos+n) lies fully inside `full`, fail-closed otherwise.
    auto requireBytes = [&](size_t pos, size_t n)
    {
        if (pos > full.size() || n > full.size() - pos)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer field out of bounds");
    };
    auto le32at = [&](size_t off) -> uint32_t
    {
        requireBytes(off, 4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };
    auto le64at = [&](size_t off) -> uint64_t
    {
        requireBytes(off, 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };

    /// 1. Bound-check the trailer and locate the footer body. The smallest possible footer body is
    /// block_count(u32) + total_count(u64) + footer_crc(u32) = 16 bytes; footer_len counts the body
    /// plus its own trailing u32, so footer_len >= 16 + 4 = 20. The footer must also start at or after
    /// the 13-byte header.
    constexpr size_t kHeaderLen = 13;
    constexpr size_t kMinFooterBody = 4 + 8 + 4;            /// block_count + total_count + footer_crc
    constexpr size_t kMinFooterLen = kMinFooterBody + 4;    /// + the trailing footer_len u32 itself
    if (full.size() < 4)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated");
    const uint32_t footer_len = le32at(full.size() - 4);
    if (footer_len < kMinFooterLen || footer_len > full.size())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad footer_len {}", footer_len);
    const size_t footer_start = full.size() - footer_len;   /// start of footer body
    if (footer_start < kHeaderLen)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer overlaps header");
    const size_t footer_body_end = full.size() - 4;         /// excludes the trailer u32
    const size_t crc_pos = footer_body_end - 4;             /// the stored footer crc u32

    /// 2. Verify the footer CRC FIRST, over the now-known, fully-present byte range
    /// [footer_start, crc_pos). Only after this passes do we trust the in-footer length fields.
    const uint32_t want_crc = le32at(crc_pos);
    const std::string_view footer_body(full.data() + footer_start, crc_pos - footer_start);
    if (crc32cOf(footer_body) != want_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer crc mismatch");

    /// 3. Parse the CRC-validated footer region. Still bound-check every offset/length against the
    /// footer body (defense in depth): a CRC-valid-but-self-inconsistent footer must fail closed, not
    /// over-read. Reads must stay within [footer_start, crc_pos).
    auto requireInFooterBody = [&](size_t pos, size_t n)
    {
        if (pos > crc_pos || n > crc_pos - pos)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer entry out of bounds");
    };

    size_t pos = footer_start;
    const uint32_t block_count = le32at(pos); pos += 4;
    index.clear();
    index.reserve(std::min<size_t>(block_count, 64));       /// cap reservation on untrusted count
    for (uint32_t b = 0; b < block_count; ++b)
    {
        BlockIndexEntry e;
        requireInFooterBody(pos, 8); e.block_offset = le64at(pos); pos += 8;
        requireInFooterBody(pos, 4); uint32_t mn = le32at(pos); pos += 4;
        requireInFooterBody(pos, mn); e.min_key.assign(full.data() + pos, mn); pos += mn;
        requireInFooterBody(pos, 4); uint32_t mx = le32at(pos); pos += 4;
        requireInFooterBody(pos, mx); e.max_key.assign(full.data() + pos, mx); pos += mx;
        index.push_back(std::move(e));
    }
    requireInFooterBody(pos, 8); total_count = le64at(pos); pos += 8;
    /// The block index must consume exactly the footer body up to the stored crc (no trailing slack).
    if (pos != crc_pos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer length inconsistent");
}

bool RunFileReader::loadBlock(size_t block_no)
{
    if (block_no >= index.size())
    {
        exhausted = true;
        return false;
    }
    /// Block offsets came from the CRC-validated footer, but harden anyway (N4): every read is
    /// bound-checked against `full`, so a self-inconsistent block fails closed, never over-reads.
    auto requireBytes = [&](size_t pos, size_t n)
    {
        if (pos > full.size() || n > full.size() - pos)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block field out of bounds");
    };
    auto le32at = [&](size_t off) -> uint32_t
    {
        requireBytes(off, 4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };

    size_t off = index[block_no].block_offset;
    const uint32_t block_len = le32at(off); off += 4;
    requireBytes(off, block_len);                          /// the whole block body must be present
    const size_t block_end = off + block_len;
    const uint32_t rec_count = le32at(off); off += 4;
    uint32_t mn = le32at(off); off += 4; requireBytes(off, mn); off += mn;   /// skip min_key
    uint32_t mx = le32at(off); off += 4; requireBytes(off, mx); off += mx;   /// skip max_key
    const uint32_t stored_crc = le32at(off); off += 4;
    if (off > block_end)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block header exceeds block");
    const std::string_view payload(full.data() + off, block_end - off);
    if (crc32cOf(payload) != stored_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block crc mismatch");

    cur_block.assign(payload.data(), payload.size());
    cur_block_pos = 0;
    cur_block_records = rec_count;
    cur_record_no = 0;
    cur_block_idx = block_no;
    block_loaded = true;
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

void RunFileReader::seek(std::string_view key)
{
    exhausted = false;
    if (index.empty())
    {
        exhausted = true;
        return;
    }
    /// Find the last block whose min_key <= key (sparse index); start scanning there.
    size_t target = 0;
    for (size_t b = 0; b < index.size(); ++b)
    {
        if (index[b].min_key <= key)
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
        if (k >= key)
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
