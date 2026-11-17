#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIdentityCache.h>

#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>

#include <chrono>
#include <limits>

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

namespace
{

UInt64 nowNs()
{
    return static_cast<UInt64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool isExpired(const ObjectStorageIdentity & identity, UInt64 ttl_seconds)
{
    constexpr UInt64 ns_per_second = 1'000'000'000ULL;
    if (identity.cached_at_ns == 0)
        return true;
    if (ttl_seconds > std::numeric_limits<UInt64>::max() / ns_per_second)
        return false;
    return nowNs() - identity.cached_at_ns > ttl_seconds * ns_per_second;
}

}

ObjectStorageIdentityCache::ObjectStorageIdentityCache(
    const String & cache_policy, size_t max_size_in_bytes, double size_ratio, UInt64 ttl_seconds_)
    : Base(
        cache_policy,
        CurrentMetrics::ObjectStorageIdentityCacheBytes,
        CurrentMetrics::ObjectStorageIdentityCacheCells,
        max_size_in_bytes,
        /*max_count*/ 0,
        size_ratio)
    , ttl_seconds(ttl_seconds_)
{
}

void ObjectStorageIdentityCache::setTTLSeconds(UInt64 ttl_seconds_)
{
    ttl_seconds.store(ttl_seconds_, std::memory_order_relaxed);
}

std::optional<ObjectStorageIdentity> ObjectStorageIdentityCache::tryGet(const String & key)
{
    auto mapped = Base::get(key);
    if (!mapped)
    {
        ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheMisses);
        return std::nullopt;
    }

    const UInt64 ttl = ttl_seconds.load(std::memory_order_relaxed);
    if (ttl > 0 && isExpired(*mapped, ttl))
    {
        /// Drop only this expired resident. A write may have replaced it since `get` returned.
        const UInt64 cached_at_ns = mapped->cached_at_ns;
        Base::removeIfMatches(key, [cached_at_ns](const Base::MappedPtr & current)
        {
            return current->cached_at_ns == cached_at_ns;
        });
        ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheMisses);
        return std::nullopt;
    }

    ProfileEvents::increment(ProfileEvents::ObjectStorageIdentityCacheHits);
    return *mapped;
}

UInt64 ObjectStorageIdentityCache::epochOf(const String & key) const
{
    std::lock_guard lock(epoch_mutex);
    const auto it = epoch_by_key.find(key);
    if (it == epoch_by_key.end())
        return 0;
    return it->second;
}

void ObjectStorageIdentityCache::setIfEpoch(const String & key, ObjectStorageIdentity identity, UInt64 epoch)
{
    std::lock_guard lock(epoch_mutex);
    const auto it = epoch_by_key.find(key);
    const UInt64 current = it == epoch_by_key.end() ? 0 : it->second;
    if (current != epoch)
        return;
    store(key, std::move(identity));
}

void ObjectStorageIdentityCache::replace(const String & key, ObjectStorageIdentity identity)
{
    std::lock_guard lock(epoch_mutex);
    ++epoch_by_key[key];
    store(key, std::move(identity));
}

void ObjectStorageIdentityCache::invalidate(const String & key)
{
    std::lock_guard lock(epoch_mutex);
    ++epoch_by_key[key];
    Base::remove(key);
}

void ObjectStorageIdentityCache::clear()
{
    std::lock_guard lock(epoch_mutex);
    epoch_by_key.clear();
    Base::clear();
}

void ObjectStorageIdentityCache::store(const String & key, ObjectStorageIdentity identity)
{
    identity.cached_at_ns = nowNs();
    Base::set(key, std::make_shared<ObjectStorageIdentity>(std::move(identity)));
}

}
