#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIdentityCache.h>

#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
    extern const Event ObjectStorageIdentityCacheHits;
    extern const Event ObjectStorageIdentityCacheMisses;
}

namespace DB
{

ObjectStorageIdentityCache & ObjectStorageIdentityCache::instance()
{
    static ObjectStorageIdentityCache cache;
    return cache;
}

std::optional<ObjectStorageIdentity> ObjectStorageIdentityCache::tryGet(const String & key) const
{
    std::lock_guard lock(mutex);
    auto it = map.find(key);
    if (it == map.end())
    {
        ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheMisses);
        return std::nullopt;
    }
    ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheHits);
    return it->second;
}

void ObjectStorageIdentityCache::set(const String & key, ObjectStorageIdentity identity)
{
    std::lock_guard lock(mutex);
    if (map.size() >= max_entries)
        map.clear();
    map[key] = std::move(identity);
}

void ObjectStorageIdentityCache::clear()
{
    std::lock_guard lock(mutex);
    map.clear();
}

}
