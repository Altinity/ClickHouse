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

ObjectStorageIdentityEpoch ObjectStorageIdentityCache::epochOf(const String & key) const
{
    std::lock_guard lock(epoch_mutex);
    const auto it = epoch_by_key.find(key);
    return {clear_epoch, it == epoch_by_key.end() ? 0 : it->second};
}

void ObjectStorageIdentityCache::setIfEpoch(const String & key, ObjectStorageIdentity identity, ObjectStorageIdentityEpoch epoch)
{
    std::lock_guard lock(epoch_mutex);
    const auto it = epoch_by_key.find(key);
    const ObjectStorageIdentityEpoch current{clear_epoch, it == epoch_by_key.end() ? 0 : it->second};
    if (current != epoch)
        return;
    Base::set(key, std::make_shared<ObjectStorageIdentity>(std::move(identity)));
}

size_t ObjectStorageIdentityCache::generationRecordBytes(const String & key)
{
    return sizeof(String) + key.size() + sizeof(UInt64) + 64;
}

void ObjectStorageIdentityCache::forgetPerKeyGenerations()
{
    ++clear_epoch;
    epoch_by_key.clear();
    generation_bytes = 0;
}

void ObjectStorageIdentityCache::reserveGenerationBytes(size_t extra)
{
    const size_t budget = Base::maxSizeInBytes();
    if (generation_bytes <= budget && extra <= budget - generation_bytes)
        return;
    forgetPerKeyGenerations();
}

void ObjectStorageIdentityCache::invalidate(const String & key)
{
    std::lock_guard lock(epoch_mutex);
    const auto it = epoch_by_key.find(key);
    if (it == epoch_by_key.end())
    {
        const size_t extra = generationRecordBytes(key);
        reserveGenerationBytes(extra);
        epoch_by_key.emplace(key, 1);
        generation_bytes += extra;
    }
    else
    {
        ++it->second;
    }
    Base::remove(key);
}

void ObjectStorageIdentityCache::clear()
{
    std::lock_guard lock(epoch_mutex);
    forgetPerKeyGenerations();
    Base::clear();
}

}
