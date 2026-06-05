#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/RefPayload.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace ContentAddressed
{

std::string serializeRefPayload(const PartId & part_id)
{
    /// MAGIC(4) + version(1) + part_id (length-prefixed). Explicit little-endian via the shared codec.
    std::string out;
    WriteBufferFromString buf(out);
    FormatHeader{kRefPayloadMagic, kRefPayloadVersion}.write(buf);
    writeStringBinary(part_id.string(), buf);
    buf.finalize();
    return out;
}

PartId partIdFromRefPayload(const std::string & payload)
{
    ReadBufferFromString buf(payload);
    FormatHeader::readAndValidate(buf, kRefPayloadMagic, kRefPayloadVersion, "ref payload");
    std::string part_id;
    readStringBinary(part_id, buf);
    if (part_id.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: ref payload has no part id");
    /// v1 holds only the part id. A future version that appends fields (B1's part header, per-ref
    /// mutable state) bumps the version byte, which `readAndValidate` rejects fail-closed in this
    /// build; that is the intended forward-compat gate (never misinterpret a newer payload).
    return PartId(std::move(part_id));
}

}

}
