#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint8_t ROOT_VERSION = 1;

void writeString16(String & out, const String & s)
{
    writeLE16(out, static_cast<uint16_t>(s.size()));
    out += s;
}

void writeString32(String & out, const String & s)
{
    writeLE32(out, static_cast<uint32_t>(s.size()));
    out += s;
}

}

String encodeRootShard(const RootShard & root)
{
    String out;
    out += "CARS";
    writeU8(out, ROOT_VERSION);

    writeLE64(out, root.shard_version);
    writeLE64(out, root.fence_round);

    writeLE32(out, static_cast<uint32_t>(root.refs.size()));
    for (const auto & [name, payload] : root.refs)
    {
        writeString16(out, name);
        writeU128LE(out, payload.tree_id);
        writeLE64(out, payload.tree_size);

        writeLE16(out, static_cast<uint16_t>(payload.mutable_files.size()));
        for (const auto & [k, v] : payload.mutable_files)
        {
            writeString16(out, k);
            writeString32(out, v);
        }
    }

    writeLE32(out, static_cast<uint32_t>(root.journal.size()));
    for (const auto & rec : root.journal)
    {
        writeU8(out, static_cast<uint8_t>(rec.op));
        writeString16(out, rec.ref_name);
        writeU128LE(out, rec.tree_id);
        writeLE64(out, rec.at_version);
    }

    return out;
}

RootShard decodeRootShard(std::string_view data)
{
    ByteReader r(data);

    const String magic = r.readBytes(4);
    if (magic != "CARS")
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS root shard: bad magic");

    const uint8_t version = r.readU8();
    if (version > ROOT_VERSION)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS root shard: unsupported version {}", version);

    RootShard root;
    root.shard_version = r.readLE64();
    root.fence_round = r.readLE64();

    const uint32_t ref_count = r.readLE32();
    for (uint32_t i = 0; i < ref_count; ++i)
    {
        const uint16_t name_len = r.readLE16();
        const String name = r.readBytes(name_len);

        RefPayload payload;
        payload.tree_id = r.readU128LE();
        payload.tree_size = r.readLE64();

        const uint16_t mutable_count = r.readLE16();
        for (uint16_t j = 0; j < mutable_count; ++j)
        {
            const uint16_t klen = r.readLE16();
            const String k = r.readBytes(klen);
            const uint32_t vlen = r.readLE32();
            const String v = r.readBytes(vlen);
            payload.mutable_files[k] = v;
        }

        root.refs[name] = std::move(payload);
    }

    const uint32_t journal_count = r.readLE32();
    for (uint32_t i = 0; i < journal_count; ++i)
    {
        JournalRecord rec;
        const uint8_t op = r.readU8();
        if (op != static_cast<uint8_t>(JournalRecord::Op::Add)
            && op != static_cast<uint8_t>(JournalRecord::Op::Remove))
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid journal op {}", op);
        rec.op = static_cast<JournalRecord::Op>(op);

        const uint16_t name_len = r.readLE16();
        rec.ref_name = r.readBytes(name_len);
        rec.tree_id = r.readU128LE();
        rec.at_version = r.readLE64();

        root.journal.push_back(std::move(rec));
    }

    return root;
}

}
