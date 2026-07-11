#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>

namespace DB::Cas
{

/// THE blob identity (mixed-algo pools, design 2026-07-11 §2): the PAIR of the hash algo and the
/// digest. A bare digest is NOT a blob identity anywhere -- `ch128` and `xxh3` digests are both
/// 16-byte, so the same digest value under two algos names two DIFFERENT objects. BlobRef is
/// constructed ONLY where algo and digest are born together (the write mint / the hasher) or read
/// together (a durable form: settlement key, blob path, manifest entry, envelope). Every other
/// site COPIES BlobRefs -- never assemble one from an algo and a digest obtained separately.
struct BlobRef
{
    BlobHashAlgo algo = BlobHashAlgo::CityHash128;
    BlobDigest digest{};

    auto operator<=>(const BlobRef &) const = default;
    bool operator==(const BlobRef &) const = default;
};

/// Hasher for unordered_map/unordered_set keys (in-process only, not a content address).
struct BlobRefHash
{
    size_t operator()(const BlobRef & r) const noexcept
    {
        size_t h = BlobDigestHash{}(r.digest);
        h ^= static_cast<size_t>(r.algo) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// The ONE way to obtain a digest codec for an algo (replaces the deleted pool-wide
/// `DigestCodec(PoolMeta)`): width follows the algo, never a pool-level assumption.
inline DigestCodec codecFor(BlobHashAlgo algo)
{
    return DigestCodec(blobHashLenFor(algo));
}

/// Bare lowercase hex of the digest at the algo's width -- for OBJECT KEY construction only
/// (the algo lives in the key's path segment `blobs/<algo>/...`).
inline String blobHexOf(const BlobRef & r)
{
    return codecFor(r.algo).toHex(r.digest);
}

/// Human/log identity: "<algoName>:<hex>", e.g. "sha256:ab12...". Rendered ids must never be a
/// bare hex (ambiguous across algos) -- events, inspect JSON and error messages use this.
inline String blobIdOf(const BlobRef & r)
{
    return String(blobHashAlgoName(r.algo)) + ":" + blobHexOf(r);
}

}
