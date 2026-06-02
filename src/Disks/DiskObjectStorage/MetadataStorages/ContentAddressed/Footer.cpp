#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <cstring>
#include <stdexcept>

namespace DB::ContentAddressed
{

static void putU64(std::string & b, uint64_t v) { char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); }
static void putStr(std::string & b, const std::string & s) { putU64(b, s.size()); b.append(s); }

static uint64_t getU64(const std::string & b, size_t & p)
{
    if (p + 8 > b.size()) throw std::runtime_error("CAS footer truncated (u64)");
    uint64_t v; std::memcpy(&v, b.data() + p, 8); p += 8; return v;
}
static std::string getStr(const std::string & b, size_t & p)
{
    uint64_t n = getU64(b, p);
    if (p + n > b.size()) throw std::runtime_error("CAS footer truncated (str)");
    std::string s = b.substr(p, n); p += n; return s;
}

std::string Footer::serialize() const
{
    std::string b(MAGIC, sizeof(MAGIC));
    putU64(b, blobs.size());
    for (const auto & [k, v] : blobs) { putStr(b, k); putStr(b, v.key); putU64(b, v.size); putStr(b, v.checksum); }
    putU64(b, inlined.size());
    for (const auto & [k, v] : inlined) { putStr(b, k); putStr(b, v); }
    return b;
}

Footer Footer::deserialize(const std::string & bytes)
{
    if (bytes.size() < sizeof(MAGIC) || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        throw std::runtime_error("CAS footer: bad magic");
    Footer f;
    size_t p = sizeof(MAGIC);
    uint64_t nb = getU64(bytes, p);
    for (uint64_t i = 0; i < nb; ++i) { auto k = getStr(bytes, p); BlobEntry e; e.key = getStr(bytes, p); e.size = getU64(bytes, p); e.checksum = getStr(bytes, p); f.blobs[k] = std::move(e); }
    uint64_t ni = getU64(bytes, p);
    for (uint64_t i = 0; i < ni; ++i) { auto k = getStr(bytes, p); f.inlined[k] = getStr(bytes, p); }
    return f;
}

}
