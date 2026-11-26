#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIdentityCache.h>

#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>
#include <Common/LockMemoryExceptionInThread.h>
#include <Interpreters/Context.h>

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

void invalidateObjectStorageIdentityCache(std::string_view endpoint, std::string_view bucket, std::string_view path)
{
    /// Callers run this after an object-changing request has already succeeded, and some of them do not sit in a
    /// `SCOPE_EXIT_MEMORY`. Building the key allocates, so without this lock a query at its memory limit would throw
    /// here and leave the previous identity cached.
    LockMemoryExceptionInThread lock_memory_tracker(VariableContext::Global);

    auto global_context = Context::getGlobalContextInstance();
    if (!global_context)
        return;

    auto cache = global_context->getObjectStorageIdentityCache();
    if (!cache)
        return;

    cache->invalidate(makeObjectStorageIdentityCacheKey(endpoint, bucket, path));
}

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

std::optional<ObjectStorageIdentity> ObjectStorageIdentityCache::tryGet(const String & key, UInt64 credential_hash)
{
    if (auto mapped = Base::get(key); mapped && mapped->credential_hash == credential_hash)
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
    /// A later fetch of this key has already stored, even if its entry was evicted since. A slot shared with another key
    /// can only cause a spurious skip.
    UInt64 & last_seq = last_stored_seq[std::hash<String>()(key) % FETCH_SEQ_SLOTS];
    if (last_seq > read.seq)
        return;
    last_seq = read.seq;
    /// `Base::set` takes the cache mutex after `epoch_mutex`, the same order as `remove` and `clear`.
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
