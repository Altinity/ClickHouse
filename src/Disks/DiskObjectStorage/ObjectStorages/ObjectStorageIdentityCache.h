#pragma once

#include <base/types.h>
#include <Common/CacheBase.h>

#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace DB
{

/// Object-storage "identity": the bits needed to open an object without a fresh HEAD - its size,
/// ETag and Last-Modified.
///
/// It exists to break the ordering that forces a HEAD before every open: object caches key on
/// (path, etag), so the etag must be known up front. Here the key is the store plus the path
/// (`makeObjectStorageIdentityCacheKey`) and the etag is stored in the value, learned once (via a
/// HEAD or a GET response) and reused. Every entry comes from a HEAD, which always returns
/// Last-Modified, so `is_last_modified_known` only means "the object has a modification time".
struct ObjectStorageIdentity
{
    String etag;
    UInt64 size = 0;
    bool is_size_known = true;
    /// Seconds since epoch. Meaningful only when `is_last_modified_known`.
    Int64 last_modified = 0;
    bool is_last_modified_known = false;
    /// SipHash of the access key, secret, session token and extra request headers that authenticated the HEAD. Not a secret.
    /// A hit is only valid for the same principal: the STS mock used in tests returns one access key
    /// with a different secret per role session, so the access key alone does not separate them,
    /// and per-query `headers(...)` carry credentials that are not part of the AWS credentials at all.
    UInt64 credential_hash = 0;

    /// Capacity of the composite key the entry is stored under. Set by `setIfEpoch`, so the weight counts it.
    /// Not part of the object's identity.
    size_t key_capacity = 0;

    size_t memoryUsage() const
    {
        return sizeof(ObjectStorageIdentity) + etag.capacity() + key_capacity;
    }
};

/// Approximate per-entry weight (bytes) for size-bounded eviction.
struct ObjectStorageIdentityWeight
{
    /// Extra bytes spent on the hashmap node, list links, shared_ptr, etc. The key bytes are counted in the value.
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

/// Drop the identity cached for this endpoint, bucket, and path. No-op when the global context or the
/// cache is not set. The next read of the path requests the metadata again.
void invalidateObjectStorageIdentityCache(std::string_view endpoint, std::string_view bucket, std::string_view path);

/// Cache of object-storage identity, keyed by endpoint, bucket and object path, so opening an object
/// does not require a fresh HEAD on every read (a lake scan otherwise re-HEADs
/// every candidate file on every query). Backed by CacheBase (LRU/SLRU) for size-bounded eviction
/// and thread-safety.
///
/// It is owned by the global Context (see Context::getObjectStorageIdentityCache), sized from server
/// settings, and cleared by `SYSTEM DROP OBJECT STORAGE IDENTITY CACHE`. A write, copy, or delete by
/// this server, and a read that finds the object replaced or missing, removes the entry and advances
/// the epoch. A metadata fetch takes `beginFetch` before its request and stores its result only if
/// that epoch is unchanged and no fetch of the same key started later, so an older observation cannot
/// put a stale identity back. The next read of that key requests the metadata again.
/// Low-level object-storage code reaches the cache via Context::getGlobalContextInstance() and must
/// tolerate a null pointer (no global context yet) by simply issuing the metadata request uncached.
class ObjectStorageIdentityCache
    : public CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>
{
    using Base = CacheBase<String, ObjectStorageIdentity, std::hash<String>, ObjectStorageIdentityWeight>;

public:
    ObjectStorageIdentityCache(const String & cache_policy, size_t max_size_in_bytes, double size_ratio);

    /// Look up an identity by `makeObjectStorageIdentityCacheKey` for the principal `credential_hash`.
    /// An entry learned by another principal is a miss and is left in place. Increments the hit/miss ProfileEvents.
    std::optional<ObjectStorageIdentity> tryGet(const String & key, UInt64 credential_hash);

    /// Epoch at the start of a metadata fetch, and a sequence that orders that fetch against others.
    struct Read
    {
        UInt64 epoch = 0;
        UInt64 seq = 0;
    };

    /// Call before the metadata request and pass the result to `setIfEpoch`.
    Read beginFetch();

    /// Store `identity` only when `read.epoch` is still current and no later fetch of `key` has stored.
    /// "Has stored" is remembered after the entry is evicted, so cache pressure cannot let an older fetch
    /// overwrite a newer observation.
    void setIfEpoch(const String & key, ObjectStorageIdentity identity, Read read);

    /// Advance the epoch and drop the entry. A fetch already in flight will not put the old identity back.
    void invalidate(const String & key);

    /// Drop every entry and advance the epoch. Overrides `CacheBase::clear`, so a call through a
    /// `CacheBase` pointer also advances the epoch and a fetch in flight across the drop cannot store afterwards.
    void clear() override;

private:
    mutable std::mutex epoch_mutex;
    /// Advanced by every `invalidate` and `clear`. Guarded by `epoch_mutex`. It is never reset, so a
    /// fetch in flight can never match a later epoch by accident.
    UInt64 epoch = 0;
    /// Advanced by every `beginFetch`. Guarded by `epoch_mutex`.
    UInt64 next_fetch_seq = 0;
    /// The highest sequence that stored an entry, per hash slot of the key. Unlike the entry itself, it survives
    /// eviction. Two keys can share a slot, which only makes `setIfEpoch` skip a store that was safe to do:
    /// the next read of that key requests the metadata again. Guarded by `epoch_mutex`.
    static constexpr size_t FETCH_SEQ_SLOTS = 4096;
    std::array<UInt64, FETCH_SEQ_SLOTS> last_stored_seq{};
};

using ObjectStorageIdentityCachePtr = std::shared_ptr<ObjectStorageIdentityCache>;

}
