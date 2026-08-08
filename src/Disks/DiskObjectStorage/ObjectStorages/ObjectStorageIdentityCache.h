#pragma once

#include <base/types.h>

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB
{

/// EXPERIMENTAL, process-wide cache of object-storage "identity": the bits needed to open an object
/// without a fresh HEAD - its size, ETag, and (if multipart-uploaded) the byte offsets of its parts.
///
/// It exists to break the ordering that forces a HEAD before every open: object caches key on
/// (path, etag), so the etag must be known up front. Here we key by path and store the etag in the
/// value, learned once (via GetObjectAttributes / a GET response) and reused. Multipart part offsets
/// ride along so reads can be aligned to part boundaries without re-probing.
///
/// This is a deliberately minimal singleton (plain mutex + map with a coarse size cap), meant for
/// measuring the win on this research branch. Productionizing should promote it to a Context-managed
/// CacheBase with proper eviction, server settings, and SYSTEM DROP support.
struct ObjectStorageIdentity
{
    String etag;
    UInt64 size = 0;
    bool is_size_known = true;
    /// Cumulative start offset of each multipart part (part i covers [part_offsets[i], part_offsets[i+1])).
    /// Empty when unknown or the object is a single PUT.
    std::vector<UInt64> part_offsets;
};

class ObjectStorageIdentityCache
{
public:
    static ObjectStorageIdentityCache & instance();

    std::optional<ObjectStorageIdentity> tryGet(const String & key) const;
    void set(const String & key, ObjectStorageIdentity identity);
    void clear();

private:
    /// Coarse cap: on overflow the whole map is cleared (experimental; a real cache would use LRU).
    static constexpr size_t max_entries = 1'000'000;

    mutable std::mutex mutex;
    std::unordered_map<String, ObjectStorageIdentity> map;
};

}
