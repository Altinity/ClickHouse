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
    extern const int NOT_IMPLEMENTED;
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
        writeBinaryLittleEndian(e.file_hash, out);
        writeBinaryLittleEndian(e.file_size, out);

        switch (e.placement)
        {
            case Placement::Inline:
                writeBinaryLittleEndian(static_cast<uint32_t>(e.inline_bytes.size()), out);
                writeString(e.inline_bytes, out);
                break;
            case Placement::Blob:
                break;
            case Placement::PackSlice:
                writeBinaryLittleEndian(e.pack_hash, out);
                writeBinaryLittleEndian(e.pack_offset, out);
                writeBinaryLittleEndian(e.pack_length, out);
                break;
            case Placement::Subtree:
                break;
        }
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
        if (version > TREE_VERSION)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS tree: unsupported version {}", version);

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
            if (placement < 1 || placement > 4)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: unknown placement {}", placement);
            e.placement = static_cast<Placement>(placement);

            readBinaryLittleEndian(e.file_hash, in);
            readBinaryLittleEndian(e.file_size, in);

            switch (e.placement)
            {
                case Placement::Inline:
                {
                    uint32_t len = 0;
                    readBinaryLittleEndian(len, in);
                    e.inline_bytes = readFixedBytes(in, len);
                    break;
                }
                case Placement::Blob:
                    break;
                case Placement::PackSlice:
                    readBinaryLittleEndian(e.pack_hash, in);
                    readBinaryLittleEndian(e.pack_offset, in);
                    readBinaryLittleEndian(e.pack_length, in);
                    break;
                case Placement::Subtree:
                    break;
            }

            entries.push_back(std::move(e));
        }

        return entries;
    });
}

TreeId treeIdFor(const String & encoded)
{
    const auto hash = CityHash_v1_0_2::CityHash128(encoded.data(), encoded.size());
    return TreeId(getHexUIntLowercase(hash));
}

}
