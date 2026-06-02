#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Common/Exception.h>
#include <Common/SipHash.h>
#include <base/hex.h>
#include <cstring>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::ContentAddressed
{

/// On-object format: integers are serialized in host byte order (little-endian targets only).
static void putU64(std::string & b, uint64_t v)
{
    char t[8];
    std::memcpy(t, &v, 8);
    b.append(t, 8);
}

static void putStr(std::string & b, const std::string & s)
{
    putU64(b, s.size());
    b.append(s);
}

/// On-object format: integers are deserialized in host byte order (little-endian targets only).
/// Overflow-safe length checks rely on the invariant p <= b.size() holding after each read.
static uint64_t getU64(const std::string & b, size_t & p)
{
    if (b.size() - p < 8)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS manifest truncated (u64)");
    uint64_t v;
    std::memcpy(&v, b.data() + p, 8);
    p += 8;
    return v;
}

static std::string getStr(const std::string & b, size_t & p)
{
    uint64_t n = getU64(b, p);
    if (n > b.size() - p)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS manifest truncated (str)");
    std::string s = b.substr(p, n);
    p += n;
    return s;
}

std::string PartManifest::serialize() const
{
    std::string b(MAGIC, sizeof(MAGIC));
    putU64(b, blobs.size());
    for (const auto & [k, v] : blobs)
    {
        putStr(b, k);
        putStr(b, v.key.string());
        putU64(b, v.size);
        putStr(b, v.checksum);
    }
    putU64(b, inlined.size());
    for (const auto & [k, v] : inlined)
    {
        putStr(b, k);
        putStr(b, v);
    }
    return b;
}

PartManifest PartManifest::deserialize(const std::string & bytes)
{
    if (bytes.size() < sizeof(MAGIC) || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS manifest: bad magic");
    PartManifest f;
    size_t p = sizeof(MAGIC);
    uint64_t nb = getU64(bytes, p);
    for (uint64_t i = 0; i < nb; ++i)
    {
        auto k = getStr(bytes, p);
        BlobEntry e;
        e.key = BlobHash(getStr(bytes, p));
        e.size = getU64(bytes, p);
        e.checksum = getStr(bytes, p);
        f.blobs[k] = std::move(e);
    }
    uint64_t ni = getU64(bytes, p);
    for (uint64_t i = 0; i < ni; ++i)
    {
        auto k = getStr(bytes, p);
        f.inlined[k] = getStr(bytes, p);
    }
    return f;
}

std::string RefSidecar::serialize() const
{
    std::string b(MAGIC, sizeof(MAGIC));
    putU64(b, VERSION);
    putU64(b, files.size());
    for (const auto & [k, v] : files)
    {
        putStr(b, k);
        putStr(b, v);
    }
    return b;
}

RefSidecar RefSidecar::deserialize(const std::string & bytes)
{
    if (bytes.size() < sizeof(MAGIC) || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref sidecar: bad magic");
    size_t p = sizeof(MAGIC);
    const uint64_t version = getU64(bytes, p);
    if (version != VERSION)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref sidecar: unknown version {}", version);
    RefSidecar s;
    const uint64_t n = getU64(bytes, p);
    for (uint64_t i = 0; i < n; ++i)
    {
        auto k = getStr(bytes, p);
        s.files[k] = getStr(bytes, p);
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
