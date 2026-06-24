#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPlacement.h>
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
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint8_t TREE_VERSION = 1;

}

String encodeTree(std::vector<TreeEntry> entries)
{
    std::sort(entries.begin(), entries.end(),
        [](const TreeEntry & a, const TreeEntry & b) { return a.name < b.name; });

    for (size_t i = 1; i < entries.size(); ++i)
    {
        if (entries[i].name == entries[i - 1].name)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CAS tree: duplicate entry name '{}'", entries[i].name);
    }

    WriteBufferFromOwnString out;
    writeString("CATR", out);
    writeBinaryLittleEndian(TREE_VERSION, out);
    writeBinaryLittleEndian(static_cast<uint32_t>(entries.size()), out);

    for (const auto & e : entries)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(e.name.size()), out);
        writeString(e.name, out);
        writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), out);
        writeU128LE(out, e.file_hash);
        writeBinaryLittleEndian(e.file_size, out);

        visitPlacement(e.placement,
            [&] {   /// Inline: write inline byte-count then the bytes
                writeBinaryLittleEndian(static_cast<uint32_t>(e.inline_bytes.size()), out);
                writeString(e.inline_bytes, out);
            },
            [&] {},   /// Blob: no extra fields
            [&] {}    /// Subtree: no extra fields
        );
    }

    return std::move(out.str());
}

std::vector<TreeEntry> decodeTree(std::string_view data)
{
    return decodeGuarded("tree", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        if (readFixedBytes(in, 4) != "CATR")
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: bad magic");

        uint8_t version = 0;
        readBinaryLittleEndian(version, in);
        checkVersion(TREE_VERSION, version, "tree");

        uint32_t count = 0;
        readBinaryLittleEndian(count, in);

        std::vector<TreeEntry> entries;
        entries.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            TreeEntry e;
            uint16_t name_len = 0;
            readBinaryLittleEndian(name_len, in);
            e.name = readFixedBytes(in, name_len);

            uint8_t placement = 0;
            readBinaryLittleEndian(placement, in);
            /// Only Inline(1), Blob(2), Subtree(3) are valid; any other value can never legitimately
            /// exist and must fail closed.
            if (placement != static_cast<uint8_t>(Placement::Inline)
                && placement != static_cast<uint8_t>(Placement::Blob)
                && placement != static_cast<uint8_t>(Placement::Subtree))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: unknown placement {}", placement);
            e.placement = static_cast<Placement>(placement);

            e.file_hash = readU128LE(in);
            readBinaryLittleEndian(e.file_size, in);

            visitPlacement(e.placement,
                [&] {   /// Inline: read byte-count then the bytes
                    uint32_t len = 0;
                    readBinaryLittleEndian(len, in);
                    e.inline_bytes = readFixedBytes(in, len);
                },
                [&] {},   /// Blob: no extra fields
                [&] {}    /// Subtree: no extra fields
            );

            entries.push_back(std::move(e));
        }

        return entries;
    });
}

TreeId merkleTreeId(std::vector<TreeEntry> entries)
{
    std::sort(entries.begin(), entries.end(),
        [](const TreeEntry & a, const TreeEntry & b) { return a.name < b.name; });

    for (size_t i = 1; i < entries.size(); ++i)
    {
        if (entries[i].name == entries[i - 1].name)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CAS tree: duplicate entry name '{}'", entries[i].name);
    }

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
        const uint8_t node_kind = (e.placement == Placement::Subtree) ? 1 : 0;   /// 0 = file, 1 = subtree
        writeBinaryLittleEndian(node_kind, buf);
        writeU128LE(buf, e.file_hash);
    }
    buf.finalize();

    const auto hash = CityHash_v1_0_2::CityHash128(buf.str().data(), buf.str().size());
    return TreeId(getHexUIntLowercase(hash));
}

}
