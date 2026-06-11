#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
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

void writeString16(WriteBuffer & out, const String & s)
{
    writeBinaryLittleEndian(static_cast<uint16_t>(s.size()), out);
    writeString(s, out);
}

void writeString32(WriteBuffer & out, const String & s)
{
    writeBinaryLittleEndian(static_cast<uint32_t>(s.size()), out);
    writeString(s, out);
}

String readString16(ReadBuffer & in)
{
    uint16_t len = 0;
    readBinaryLittleEndian(len, in);
    return readFixedBytes(in, len);
}

String readString32(ReadBuffer & in)
{
    uint32_t len = 0;
    readBinaryLittleEndian(len, in);
    return readFixedBytes(in, len);
}

}

String encodeRootShard(const RootShard & root)
{
    WriteBufferFromOwnString out;
    writeString("CARS", out);
    writeBinaryLittleEndian(ROOT_VERSION, out);

    writeBinaryLittleEndian(root.shard_version, out);
    writeBinaryLittleEndian(root.fence_round, out);

    writeBinaryLittleEndian(static_cast<uint32_t>(root.refs.size()), out);
    for (const auto & [name, payload] : root.refs)
    {
        writeString16(out, name);
        writeBinaryLittleEndian(payload.tree_id, out);
        writeBinaryLittleEndian(payload.tree_size, out);

        writeBinaryLittleEndian(static_cast<uint16_t>(payload.mutable_files.size()), out);
        for (const auto & [k, v] : payload.mutable_files)
        {
            writeString16(out, k);
            writeString32(out, v);
        }
    }

    writeBinaryLittleEndian(static_cast<uint32_t>(root.journal.size()), out);
    for (const auto & rec : root.journal)
    {
        writeBinaryLittleEndian(static_cast<uint8_t>(rec.op), out);
        writeString16(out, rec.ref_name);
        writeBinaryLittleEndian(rec.tree_id, out);
        writeBinaryLittleEndian(rec.at_version, out);
    }

    return std::move(out.str());
}

RootShard decodeRootShard(std::string_view data)
{
    return decodeGuarded("root shard", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        if (readFixedBytes(in, 4) != "CARS")
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS root shard: bad magic");

        uint8_t version = 0;
        readBinaryLittleEndian(version, in);
        if (version > ROOT_VERSION)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "CAS root shard: unsupported version {}", version);

        RootShard root;
        readBinaryLittleEndian(root.shard_version, in);
        readBinaryLittleEndian(root.fence_round, in);

        uint32_t ref_count = 0;
        readBinaryLittleEndian(ref_count, in);
        for (uint32_t i = 0; i < ref_count; ++i)
        {
            String name = readString16(in);

            RefPayload payload;
            readBinaryLittleEndian(payload.tree_id, in);
            readBinaryLittleEndian(payload.tree_size, in);

            uint16_t mutable_count = 0;
            readBinaryLittleEndian(mutable_count, in);
            for (uint16_t j = 0; j < mutable_count; ++j)
            {
                String k = readString16(in);
                payload.mutable_files[k] = readString32(in);
            }

            root.refs[name] = std::move(payload);
        }

        uint32_t journal_count = 0;
        readBinaryLittleEndian(journal_count, in);
        for (uint32_t i = 0; i < journal_count; ++i)
        {
            JournalRecord rec;
            uint8_t op = 0;
            readBinaryLittleEndian(op, in);
            if (op != static_cast<uint8_t>(JournalRecord::Op::Add)
                && op != static_cast<uint8_t>(JournalRecord::Op::Remove))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid journal op {}", op);
            rec.op = static_cast<JournalRecord::Op>(op);

            rec.ref_name = readString16(in);
            readBinaryLittleEndian(rec.tree_id, in);
            readBinaryLittleEndian(rec.at_version, in);

            root.journal.push_back(std::move(rec));
        }

        return root;
    });
}

}
