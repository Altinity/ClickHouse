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

void ObjectStorageIdentityCache::set(const String & key, ObjectStorageIdentity identity)
{
    Base::set(key, std::make_shared<ObjectStorageIdentity>(std::move(identity)));
}

}
