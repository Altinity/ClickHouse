#pragma once

#include <base/types.h>
#include <Common/CacheBase.h>
#include <Common/UnorderedMapWithMemoryTracking.h>

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

/// What a metadata fetch observed before its request. `key` counts the writes, copies, and deletes of
/// one object. `clear` counts the times the whole cache was dropped: `clear` also forgets every `key`
/// counter, so without it a key would look untouched again and a fetch in flight across the drop
/// would store the identity it fetched before the overwrite.
struct ObjectStorageIdentityEpoch
{
    UInt64 clear = 0;
    UInt64 key = 0;

    bool operator==(const ObjectStorageIdentityEpoch &) const = default;
};

/// Cache of object-storage identity, keyed by endpoint, bucket and object path, so opening an object
/// does not require a fresh HEAD / GetObjectAttributes on every read (a lake scan otherwise re-HEADs
/// every candidate file on every query). Backed by CacheBase (LRU/SLRU) for size-bounded eviction
/// and thread-safety.
///
/// It is owned by the global Context (see Context::getObjectStorageIdentityCache), sized from server
/// settings, and cleared by `SYSTEM DROP OBJECT STORAGE IDENTITY CACHE`. A write, copy, or delete by
/// this server bumps that key's generation and removes the entry, and `SYSTEM DROP` bumps a cache-wide
/// generation. The per-key generations are kept only up to the cache's configured size. Past that, the
/// map is dropped and the cache-wide generation is bumped, which rejects every fetch already in flight.
/// A fetch stores its result only when both generations are still the ones it
/// observed before the request, so a read that started earlier cannot put the old identity back. The
/// next read of that key requests the metadata again. Low-level
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

    /// Generation of `key`. A fetch captures this before the request and passes it to `setIfEpoch`.
    ObjectStorageIdentityEpoch epochOf(const String & key) const;

    /// Store `identity` only when `epoch` is still the generation `epochOf` returned.
    void setIfEpoch(const String & key, ObjectStorageIdentity identity, ObjectStorageIdentityEpoch epoch);

    /// Bump the generation and drop the entry. A fetch already in flight will not put the old identity back.
    void invalidate(const String & key);

    /// Drop every entry and per-key generation, and bump the cache-wide generation. Hides
    /// `CacheBase::clear` so a fetch in flight across the drop cannot store afterwards.
    void clear();

private:
    /// Bytes of one per-key generation record: the key, its counter, and a hashmap node.
    static size_t generationRecordBytes(const String & key);

    /// `epoch_mutex` must be held. Forget every per-key generation and bump the cache-wide generation.
    void forgetPerKeyGenerations();

    /// `epoch_mutex` must be held. When `extra` bytes would take the generation map past the cache's
    /// configured size, forget the per-key generations first. One new record is then kept even if that
    /// record alone is larger than the configured size.
    void reserveGenerationBytes(size_t extra);

    mutable std::mutex epoch_mutex;
    /// Times the per-key generations were forgotten, by `clear` or because the map reached its size.
    /// Guarded by `epoch_mutex`.
    UInt64 clear_epoch = 0;
    /// Generations of keys this server has replaced or deleted. Absent means generation 0.
    /// Guarded by `epoch_mutex`. A key stays here so a fetch that started earlier can see the bump,
    /// until the map reaches the cache's configured size.
    UnorderedMapWithMemoryTracking<String, UInt64> epoch_by_key;
    /// `epoch_by_key` accounted with `generationRecordBytes`. Guarded by `epoch_mutex`.
    size_t generation_bytes = 0;
};

using ObjectStorageIdentityCachePtr = std::shared_ptr<ObjectStorageIdentityCache>;

}
