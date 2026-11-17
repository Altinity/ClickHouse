#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIdentityCache.h>

#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>

namespace ProfileEvents
{
    extern const Event ObjectStorageIdentityCacheHits;
    extern const Event ObjectStorageIdentityCacheMisses;
}

namespace CurrentMetrics
{
    extern const Metric ObjectStorageIdentityCacheBytes;
    extern const Metric ObjectStorageIdentityCacheCells;
}

namespace DB
{

ObjectStorageIdentityCache::ObjectStorageIdentityCache(const String & cache_policy, size_t max_size_in_bytes, double size_ratio)
    : Base(
        cache_policy,
        CurrentMetrics::ObjectStorageIdentityCacheBytes,
        CurrentMetrics::ObjectStorageIdentityCacheCells,
        max_size_in_bytes,
        /*max_count*/ 0,
        size_ratio)
{
}

std::optional<ObjectStorageIdentity> ObjectStorageIdentityCache::tryGet(const String & key)
{
    if (auto mapped = Base::get(key))
    {
        ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheHits);
        return *mapped;
    }

    ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheMisses);
    return std::nullopt;
}

ObjectStorageIdentityCache::Read ObjectStorageIdentityCache::beginFetch()
{
    std::lock_guard lock(epoch_mutex);
    return Read{epoch, ++next_fetch_seq};
}

void ObjectStorageIdentityCache::setIfEpoch(const String & key, ObjectStorageIdentity identity, Read read)
{
    std::lock_guard lock(epoch_mutex);
    if (epoch != read.epoch)
        return;
    /// `Base::get` takes the cache mutex after `epoch_mutex`, the same order as `set` and `remove`.
    if (auto existing = Base::get(key))
    {
        if (existing->fetch_seq > read.seq)
            return;
        /// A HEAD does not return multipart part offsets. Keep the ones already stored when this
        /// fetch saw the same ETag. A different ETag is a different object, so its offsets are dropped.
        if (identity.part_offsets.empty() && !existing->part_offsets.empty() && identity.etag == existing->etag)
            identity.part_offsets = existing->part_offsets;
    }
    identity.fetch_seq = read.seq;
    Base::set(key, std::make_shared<ObjectStorageIdentity>(std::move(identity)));
}

void ObjectStorageIdentityCache::invalidate(const String & key)
{
    std::lock_guard lock(epoch_mutex);
    ++epoch;
    Base::remove(key);
}

void ObjectStorageIdentityCache::clear()
{
    std::lock_guard lock(epoch_mutex);
    ++epoch;
    Base::clear();
}

}
