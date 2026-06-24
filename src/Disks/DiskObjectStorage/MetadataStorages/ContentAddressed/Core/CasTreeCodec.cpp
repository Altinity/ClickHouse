#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/hex.h>
#include <city.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

void sortAndCheckDuplicateNames(std::vector<TreeEntry> & entries)
{
    std::sort(entries.begin(), entries.end(),
        [](const TreeEntry & a, const TreeEntry & b) { return a.name < b.name; });
    for (size_t i = 1; i < entries.size(); ++i)
        if (entries[i].name == entries[i - 1].name)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CAS tree: duplicate entry name '{}'", entries[i].name);
}

}

String encodeTree(std::vector<TreeEntry> entries)
{
    sortAndCheckDuplicateNames(entries);

    /// First pass: assign each Inline entry a contiguous (offset,length) within the data section.
    String data;
    {
        WriteBufferFromString data_buf(data);
        for (const auto & e : entries)
            if (e.placement == Placement::Inline)
                writeString(e.inline_bytes, data_buf);
        data_buf.finalize();
    }

    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(static_cast<uint32_t>(entries.size()), out);   /// entry_count

    uint64_t running_offset = 0;
    for (const auto & e : entries)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(e.name.size()), out);
        writeString(e.name, out);
        writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), out);
        writeU128LE(out, e.file_hash);
        writeBinaryLittleEndian(e.file_size, out);
        if (e.placement == Placement::Inline)
        {
            writeBinaryLittleEndian(running_offset, out);                  /// data_offset
            writeBinaryLittleEndian(static_cast<uint64_t>(e.inline_bytes.size()), out); /// data_length
            running_offset += e.inline_bytes.size();
        }
    }

    writeString(data, out);                                                /// DATA section
    return std::move(out.str());
}

std::vector<TreeEntry> decodeTree(std::string_view data)
{
    return decodeGuarded("tree", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        uint32_t count = 0;
        readBinaryLittleEndian(count, in);

        struct Pending { size_t offset; size_t length; };
        std::vector<TreeEntry> entries;
        std::vector<Pending> inline_slices;          /// index-aligned with Inline entries

        /// A count larger than the remaining buffer could possibly encode is corruption. Bound the
        /// reserve so a junk count can't trigger a huge allocation (std::bad_alloc escapes decodeGuarded).
        constexpr size_t MIN_ENTRY_BYTES = 27;   /// name_len(2)+placement(1)+file_hash(16)+file_size(8)
        if (static_cast<uint64_t>(count) > in.available() / MIN_ENTRY_BYTES + 1)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CAS tree: entry count {} exceeds what the {}-byte buffer can encode", count, in.available());
        entries.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            TreeEntry e;
            uint16_t name_len = 0;
            readBinaryLittleEndian(name_len, in);
            e.name = readFixedBytes(in, name_len);

            uint8_t placement = 0;
            readBinaryLittleEndian(placement, in);
            if (placement != static_cast<uint8_t>(Placement::Inline)
                && placement != static_cast<uint8_t>(Placement::Blob)
                && placement != static_cast<uint8_t>(Placement::Subtree))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: unknown placement {}", placement);
            e.placement = static_cast<Placement>(placement);

            e.file_hash = readU128LE(in);
            readBinaryLittleEndian(e.file_size, in);

            if (e.placement == Placement::Inline)
            {
                uint64_t off = 0;
                uint64_t len = 0;
                readBinaryLittleEndian(off, in);
                readBinaryLittleEndian(len, in);
                inline_slices.push_back({static_cast<size_t>(off), static_cast<size_t>(len)});
            }
            entries.push_back(std::move(e));
        }

        /// The DATA section is the remainder. Slice each Inline entry's bytes out of it; an out-of-range
        /// (offset+length) is corruption.
        const size_t data_start = in.count();
        const std::string_view data_section = data.substr(data_start);
        size_t slice_idx = 0;
        for (auto & e : entries)
        {
            if (e.placement != Placement::Inline)
                continue;
            const Pending & p = inline_slices[slice_idx++];
            if (p.offset > data_section.size() || p.length > data_section.size() - p.offset)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                    "CAS tree: inline slice [{}, {}) overruns data section of {} bytes",
                    p.offset, p.offset + p.length, data_section.size());
            e.inline_bytes = String(data_section.substr(p.offset, p.length));
        }

        return entries;
    });
}

TreeId merkleTreeId(std::vector<TreeEntry> entries)
{
    sortAndCheckDuplicateNames(entries);

    /// Canonical, frozen Merkle input. node_kind domain-separates a file leaf from a subtree node so a
    /// blob hash and a child tree id under the same name can never collide (RFC 6962-style separation).
    WriteBufferFromOwnString buf;
    writeString("CAMT", buf);                                       /// domain tag
    writeBinaryLittleEndian(static_cast<uint8_t>(1), buf);          /// Merkle rule version (in-hash only)
    writeBinaryLittleEndian(static_cast<uint32_t>(entries.size()), buf);
    for (const auto & e : entries)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(e.name.size()), buf);
        writeString(e.name, buf);
        /// Frozen Merkle rule v1: Inline and Blob are both file leaves (node_kind=0); only Subtree is 1.
        /// A future non-subtree Placement must still map to 0 here, or bump the rule version byte above.
        const uint8_t node_kind = (e.placement == Placement::Subtree) ? 1 : 0;
        writeBinaryLittleEndian(node_kind, buf);
        writeU128LE(buf, e.file_hash);
    }

    const auto hash = CityHash_v1_0_2::CityHash128(buf.str().data(), buf.str().size());
    return TreeId(getHexUIntLowercase(hash));
}

}
