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

    PartManifest manifest;
    uint64_t blob_count = 0;
    DB::readVarUInt(blob_count, buf);
    for (uint64_t i = 0; i < blob_count; ++i)
    {
        std::string logical_file;
        DB::readStringBinary(logical_file, buf);
        BlobEntry entry;
        std::string hash;
        DB::readStringBinary(hash, buf);
        entry.key = BlobHash(std::move(hash));
        DB::readBinaryLittleEndian(entry.size, buf);
        DB::readStringBinary(entry.checksum, buf);
        manifest.blobs[logical_file] = std::move(entry);
    }
    uint64_t inlined_count = 0;
    DB::readVarUInt(inlined_count, buf);
    for (uint64_t i = 0; i < inlined_count; ++i)
    {
        std::string logical_file;
        DB::readStringBinary(logical_file, buf);
        DB::readStringBinary(manifest.inlined[logical_file], buf);
    }
    return manifest;
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
    /// CA GC S3 (#6, version 2): the resolved manifest generation, then the (blob-hash -> g) map.
    DB::writeVarUInt(manifest_generation, buf);
    DB::writeVarUInt(pin_generations.size(), buf);
    for (const auto & [hash, g] : pin_generations)
    {
        DB::writeStringBinary(hash, buf);
        DB::writeVarUInt(g, buf);
    }
    buf.finalize();
    return out;
}

RefSidecar RefSidecar::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, VERSION, "ref sidecar");
    RefSidecar sidecar;
    uint64_t file_count = 0;
    DB::readVarUInt(file_count, buf);
    for (uint64_t i = 0; i < file_count; ++i)
    {
        std::string file_name;
        DB::readStringBinary(file_name, buf);
        DB::readStringBinary(sidecar.files[file_name], buf);
    }
    /// CA GC S3 (#6, version 2): the resolved generations (always present in a v2 object).
    DB::readVarUInt(sidecar.manifest_generation, buf);
    uint64_t pin_count = 0;
    DB::readVarUInt(pin_count, buf);
    for (uint64_t i = 0; i < pin_count; ++i)
    {
        std::string hash;
        DB::readStringBinary(hash, buf);
        uint64_t generation = 0;
        DB::readVarUInt(generation, buf);
        sidecar.pin_generations.emplace(std::move(hash), generation);
    }
    return sidecar;
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
