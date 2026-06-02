#pragma once
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <set>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// Enumerate the full set of LIVE part ids in a content-addressed pool: every published ref under
/// the pool's refs root (store/<server>/<uuid>/refs/<part>) names a part id (its payload). These
/// are the GC roots — a part id is live iff at least one ref points at it. Any list or read error
/// PROPAGATES so the caller aborts the sweep: a partial scan must never drive deletion (fail-close).
std::set<std::string> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix);

/// List the object keys of every object under a pool root prefix (e.g. partsPrefix / blobsPrefix).
/// Thin wrapper over object_storage->listObjects; errors propagate. Returned keys are the object
/// keys (matching the per-object key builders), suitable for set difference against reachable sets.
std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix);

}
