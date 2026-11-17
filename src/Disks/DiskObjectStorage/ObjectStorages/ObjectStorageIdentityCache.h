#pragma once

#include <base/types.h>
#include <Common/CacheBase.h>

#include <optional>
#include <string_view>
#include <vector>

namespace DB
{

/// Object-storage "identity": the bits needed to open an object without a fresh HEAD - its size,
/// ETag, and (if multipart-uploaded) the byte offsets of its parts.
///
/// It exists to break the ordering that forces a HEAD before every open: object caches key on
/// (path, etag), so the etag must be known up front. Here the key is the store plus the path
/// (`makeObjectStorageIdentityCacheKey`) and the etag is stored in the value, learned once (via
/// GetObjectAttributes / a GET response) and reused. Multipart part offsets ride along so reads can
/// be aligned to part boundaries without re-probing.
struct ObjectStorageIdentity
{
    String etag;
    UInt64 size = 0;
    bool is_size_known = true;
    /// Cumulative start offset of each multipart part (part i covers [part_offsets[i], part_offsets[i+1])).
    /// Empty when unknown or the object is a single PUT.
    std::vector<UInt64> part_offsets;

    size_t memoryUsage() const
    {
        return sizeof(ObjectStorageIdentity) + etag.capacity() + part_offsets.capacity() * sizeof(UInt64);
    }
};

/// Approximate per-entry weight (bytes) for size-bounded eviction.
struct ObjectStorageIdentityWeight
{
    /// Extra bytes spent on the key string, hashmap node, list links, shared_ptr, etc.
    static constexpr size_t OVERHEAD = 128;

    size_t operator()(const ObjectStorageIdentity & identity) const
    {
        return identity.memoryUsage() + OVERHEAD;
    }
};

/// One object on one store. Bucket and path are not enough: two S3-compatible endpoints can reuse
/// the same bucket name. Fields are separated by NUL so a slash in the endpoint cannot alias another field.
inline String makeObjectStorageIdentityCacheKey(std::string_view endpoint, std::string_view bucket, std::string_view path)
{
    String key;
    key.reserve(endpoint.size() + bucket.size() + path.size() + 2);
    key.append(endpoint);
    key.push_back('\0');
    key.append(bucket);
    key.push_back('\0');
    key.append(path);
    return key;
}

/// Cache of object-storage identity, keyed by endpoint, bucket and object path, so opening an object
/// does not require a fresh HEAD / GetObjectAttributes on every read (a lake scan otherwise re-HEADs
/// every candidate file on every query). Backed by CacheBase (LRU/SLRU) for size-bounded eviction
/// and thread-safety.
///
/// It is owned by the global Context (see Context::getObjectStorageIdentityCache), sized from server
/// settings, and cleared by `SYSTEM DROP OBJECT STORAGE IDENTITY CACHE`. A write, copy, or delete by
/// this server removes the entry; the next read of that key requests the metadata again. Low-level
/// object-storage code reaches the cache via Context::getGlobalContextInstance() and must tolerate a
/// null pointer (no global context yet) by simply issuing the metadata request uncached.
class ObjectStorageIdentityCache
    : public CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>
{
    using Base = CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>;

public:
    ObjectStorageIdentityCache(const String & cache_policy, size_t max_size_in_bytes, double size_ratio);

    /// Look up an identity by `makeObjectStorageIdentityCacheKey`. Increments the hit/miss ProfileEvents.
    std::optional<ObjectStorageIdentity> tryGet(const String & key);

    /// Insert (or overwrite) the identity for a key.
    void set(const String & key, ObjectStorageIdentity identity);
};

using ObjectStorageIdentityCachePtr = std::shared_ptr<ObjectStorageIdentityCache>;

}
