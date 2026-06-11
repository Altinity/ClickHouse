#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
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

    String out;
    out += "CATR";
    writeU8(out, TREE_VERSION);
    writeLE32(out, static_cast<uint32_t>(entries.size()));

    for (const auto & e : entries)
    {
        writeLE16(out, static_cast<uint16_t>(e.name.size()));
        out += e.name;
        writeU8(out, static_cast<uint8_t>(e.placement));
        writeU128LE(out, e.file_hash);
        writeLE64(out, e.file_size);

        switch (e.placement)
        {
            case Placement::Inline:
                writeLE32(out, static_cast<uint32_t>(e.inline_bytes.size()));
                out += e.inline_bytes;
                break;
            case Placement::Blob:
                break;
            case Placement::PackSlice:
                writeU128LE(out, e.pack_hash);
                writeLE64(out, e.pack_offset);
                writeLE64(out, e.pack_length);
                break;
            case Placement::Subtree:
                break;
        }
    }

    return out;
}

std::vector<TreeEntry> decodeTree(std::string_view data)
{
    ByteReader r(data);

    const String magic = r.readBytes(4);
    if (magic != "CATR")
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: bad magic");

    const uint8_t version = r.readU8();
    if (version > TREE_VERSION)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS tree: unsupported version {}", version);

    const uint32_t count = r.readLE32();

    std::vector<TreeEntry> entries;
    entries.reserve(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        TreeEntry e;
        const uint16_t name_len = r.readLE16();
        e.name = r.readBytes(name_len);

        const uint8_t placement = r.readU8();
        if (placement < 1 || placement > 4)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: unknown placement {}", placement);
        e.placement = static_cast<Placement>(placement);

        e.file_hash = r.readU128LE();
        e.file_size = r.readLE64();

        switch (e.placement)
        {
            case Placement::Inline:
            {
                const uint32_t len = r.readLE32();
                e.inline_bytes = r.readBytes(len);
                break;
            }
            case Placement::Blob:
                break;
            case Placement::PackSlice:
                e.pack_hash = r.readU128LE();
                e.pack_offset = r.readLE64();
                e.pack_length = r.readLE64();
                break;
            case Placement::Subtree:
                break;
        }

        entries.push_back(std::move(e));
    }

    return entries;
}

TreeId treeIdFor(const String & encoded)
{
    const auto hash = CityHash_v1_0_2::CityHash128(encoded.data(), encoded.size());
    return TreeId(getHexUIntLowercase(hash));
}

}
