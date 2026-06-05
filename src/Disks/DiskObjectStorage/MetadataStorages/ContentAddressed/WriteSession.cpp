#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>

namespace DB::ContentAddressed
{

std::string WriteSession::serialize() const
{
    /// MAGIC(4) + encoding version(1) + body, on the shared codec. The body carries the owning server
    /// id (length-prefixed string), the lease deadline and fence token (fixed-width little-endian u64),
    /// the part id (length-prefixed string, same shape as the manifest's blob hash), then a varint count
    /// and that many pending blob hashes (each length-prefixed). All explicitly little-endian so the
    /// object is byte-identical regardless of the writer's architecture (cross-arch determinism).
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, ENCODING_VERSION}.write(buf);
    DB::writeStringBinary(server_id, buf);
    DB::writeBinaryLittleEndian(lease_deadline_unix, buf);
    DB::writeBinaryLittleEndian(fence_token, buf);
    DB::writeStringBinary(part_id.string(), buf);
    DB::writeVarUInt(pending.size(), buf);
    for (const auto & hash : pending)
        DB::writeStringBinary(hash.string(), buf);
    /// CA GC S4 (v2): the foldedness state. `committed` as a single byte, then a varint count and that many
    /// `(shard, epoch)` pairs (each fixed-width little-endian) the `+` fragments durably settled in.
    DB::writeBinaryLittleEndian(static_cast<uint8_t>(committed ? 1 : 0), buf);
    DB::writeVarUInt(delta_epochs.size(), buf);
    for (const auto & [shard, epoch] : delta_epochs)
    {
        DB::writeBinaryLittleEndian(static_cast<UInt32>(shard), buf);
        DB::writeBinaryLittleEndian(epoch, buf);
    }
    buf.finalize();
    return out;
}

WriteSession WriteSession::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, ENCODING_VERSION, "write session");

    WriteSession session;
    DB::readStringBinary(session.server_id, buf);
    DB::readBinaryLittleEndian(session.lease_deadline_unix, buf);
    DB::readBinaryLittleEndian(session.fence_token, buf);
    std::string part_id;
    DB::readStringBinary(part_id, buf);
    session.part_id = PartId(std::move(part_id));
    uint64_t n = 0;
    DB::readVarUInt(n, buf);
    session.pending.reserve(n);
    for (uint64_t i = 0; i < n; ++i)
    {
        std::string hash;
        DB::readStringBinary(hash, buf);
        session.pending.emplace_back(std::move(hash));
    }
    /// CA GC S4 (v2): the foldedness state.
    uint8_t committed_raw = 0;
    DB::readBinaryLittleEndian(committed_raw, buf);
    session.committed = committed_raw != 0;
    uint64_t m = 0;
    DB::readVarUInt(m, buf);
    session.delta_epochs.reserve(m);
    for (uint64_t i = 0; i < m; ++i)
    {
        UInt32 shard = 0;
        UInt64 epoch = 0;
        DB::readBinaryLittleEndian(shard, buf);
        DB::readBinaryLittleEndian(epoch, buf);
        session.delta_epochs.emplace_back(static_cast<ShardId>(shard), epoch);
    }
    return session;
}

}
