#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/SipHash.h>
#include <base/hex.h>

namespace DB::ContentAddressed
{

std::string PartManifest::serialize() const
{
    /// MAGIC(4) + version(1) + body. Counts are varints; sizes are fixed-width little-endian u64;
    /// names and bytes are length-prefixed strings (a varint length + the raw bytes), all explicitly
    /// little-endian so the object is byte-identical regardless of the writer's architecture.
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, VERSION}.write(buf);
    DB::writeVarUInt(blobs.size(), buf);
    for (const auto & [k, v] : blobs)
    {
        DB::writeStringBinary(k, buf);
        DB::writeStringBinary(v.key.string(), buf);
        DB::writeBinaryLittleEndian(v.size, buf);
        DB::writeStringBinary(v.checksum, buf);
    }
    DB::writeVarUInt(inlined.size(), buf);
    for (const auto & [k, v] : inlined)
    {
        DB::writeStringBinary(k, buf);
        DB::writeStringBinary(v, buf);
    }
    buf.finalize();
    return out;
}

PartManifest PartManifest::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, VERSION, "manifest");

    PartManifest f;
    uint64_t nb = 0;
    DB::readVarUInt(nb, buf);
    for (uint64_t i = 0; i < nb; ++i)
    {
        std::string k;
        DB::readStringBinary(k, buf);
        BlobEntry e;
        std::string key;
        DB::readStringBinary(key, buf);
        e.key = BlobHash(std::move(key));
        DB::readBinaryLittleEndian(e.size, buf);
        DB::readStringBinary(e.checksum, buf);
        f.blobs[k] = std::move(e);
    }
    uint64_t ni = 0;
    DB::readVarUInt(ni, buf);
    for (uint64_t i = 0; i < ni; ++i)
    {
        std::string k;
        DB::readStringBinary(k, buf);
        DB::readStringBinary(f.inlined[k], buf);
    }
    return f;
}

std::string RefSidecar::serialize() const
{
    /// MAGIC(4) + version(1) + body, on the shared codec: a varint count then (name, bytes) length-
    /// prefixed string pairs, all explicitly little-endian (same shape as PartManifest's inlined map).
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, VERSION}.write(buf);
    DB::writeVarUInt(files.size(), buf);
    for (const auto & [k, v] : files)
    {
        DB::writeStringBinary(k, buf);
        DB::writeStringBinary(v, buf);
    }
    buf.finalize();
    return out;
}

RefSidecar RefSidecar::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, VERSION, "ref sidecar");
    RefSidecar s;
    uint64_t n = 0;
    DB::readVarUInt(n, buf);
    for (uint64_t i = 0; i < n; ++i)
    {
        std::string k;
        DB::readStringBinary(k, buf);
        DB::readStringBinary(s.files[k], buf);
    }
    return s;
}

PartId computePartId(const std::map<std::string, BlobEntry> & blobs)
{
    /// std::map iterates in sorted key order, so the (logical_file, checksum) stream is canonical.
    /// Mutable per-part files (isMutablePerPartFile) are excluded so two parts with identical column
    /// data but a different uuid / txn / metadata version still resolve to the same part_id (dedup);
    /// those files live per-ref in the sidecar, never in the shared manifest (single source of truth).
    SipHash hash;
    for (const auto & [file, blob] : blobs)
    {
        if (isMutablePerPartFile(file))
            continue;
        hash.update(file);
        hash.update(blob.checksum);
    }
    return PartId(getHexUIntLowercase(hash.get128()));
}

}
