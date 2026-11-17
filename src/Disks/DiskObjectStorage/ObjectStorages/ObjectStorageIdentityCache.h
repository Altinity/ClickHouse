#pragma once

#include <base/types.h>
#include <Common/CacheBase.h>
#include <Common/UnorderedMapWithMemoryTracking.h>

#include <atomic>
#include <mutex>
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
    /// `steady_clock` nanoseconds when this entry was stored. Zero is already expired when a TTL is set.
    UInt64 cached_at_ns = 0;

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
/// the same bucket name, and an explicit region can select a different store behind one endpoint.
/// Fields are separated by NUL so a slash in the endpoint cannot alias another field.
inline String makeObjectStorageIdentityCacheKey(
    std::string_view endpoint,
    std::string_view region,
    std::string_view bucket,
    std::string_view path)
{
    String key;
    key.reserve(endpoint.size() + region.size() + bucket.size() + path.size() + 3);
    key.append(endpoint);
    key.push_back('\0');
    key.append(region);
    key.push_back('\0');
    key.append(bucket);
    key.push_back('\0');
    key.append(path);
    return key;
}

/// Cache of object-storage identity, keyed by endpoint, region, bucket and object path, so opening
/// an object does not require a fresh HEAD / GetObjectAttributes on every read (a lake scan otherwise
/// re-HEADs every candidate file on every query). Backed by CacheBase (LRU/SLRU) for size-bounded
/// eviction and thread-safety.
///
/// It is owned by the global Context (see Context::getObjectStorageIdentityCache), sized from server
/// settings, and cleared by `SYSTEM DROP OBJECT STORAGE IDENTITY CACHE`. Entries expire after
/// `object_storage_identity_cache_ttl` seconds (zero disables expiry). A write by this server stores
/// the new size and ETag, and a copy or delete drops the entry, so a replaced object is not read
/// with its previous size and ETag. Low-level object-storage code reaches the cache via Context::getGlobalContextInstance()
/// and must tolerate a null pointer (no global context yet) by simply issuing the metadata request
/// uncached.
class ObjectStorageIdentityCache
    : public CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>
{
    using Base = CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>;

public:
    ObjectStorageIdentityCache(const String & cache_policy, size_t max_size_in_bytes, double size_ratio, UInt64 ttl_seconds);

    void setTTLSeconds(UInt64 ttl_seconds);

    /// Look up an identity by `makeObjectStorageIdentityCacheKey`. An expired entry is a miss.
    /// Increments the hit/miss ProfileEvents.
    std::optional<ObjectStorageIdentity> tryGet(const String & key);

    /// Generation of `key`. A fetch captures this before the request and passes it to `setIfEpoch`,
    /// so a write that lands during the request cannot be overwritten by that fetch's old identity.
    /// Zero when the key has never been invalidated.
    UInt64 epochOf(const String & key) const;

    /// Store `identity` only when `epoch` is still the generation `epochOf` returned.
    void setIfEpoch(const String & key, ObjectStorageIdentity identity, UInt64 epoch);

    /// Bump the generation and store `identity`. Used after this server has replaced the object.
    void replace(const String & key, ObjectStorageIdentity identity);

    /// Bump the generation and drop the entry. A fetch already in flight will not put the old identity back.
    void invalidate(const String & key);

    /// Drop every entry and generation. Hides `CacheBase::clear` so generations cannot outlive the entries.
    void clear();

private:
    /// `epoch_mutex` must be held.
    void store(const String & key, ObjectStorageIdentity identity);

    mutable std::mutex epoch_mutex;
    /// Generations of keys this server has replaced or deleted. Absent means generation 0.
    /// Guarded by `epoch_mutex`.
    UnorderedMapWithMemoryTracking<String, UInt64> epoch_by_key;
    std::atomic<UInt64> ttl_seconds;
};

using ObjectStorageIdentityCachePtr = std::shared_ptr<ObjectStorageIdentityCache>;

}
