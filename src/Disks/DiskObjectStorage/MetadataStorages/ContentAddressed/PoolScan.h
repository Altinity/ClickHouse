#pragma once
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <set>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// Resolve a ref object payload into the part id it names. The write path stores the part id verbatim
/// (a 32-char lowercase-hex string, see computePartId). Per B22(c) we tolerate a possible leading
/// version byte and trailing whitespace/newline by extracting the longest run of lowercase-hex
/// characters: an unversioned payload is unchanged, a future versioned one drops its marker byte.
/// An empty hex run is corruption (a published ref must name a part) and throws CORRUPTED_DATA
/// fail-close. This is the SINGLE ref-payload parser: both the GC live-set scan and the read path
/// resolve a ref through it, so they cannot disagree on the part id by construction (B28).
std::string partIdFromRefPayload(const std::string & payload);

/// Enumerate the full set of LIVE part ids in a content-addressed pool: every published ref under
/// the pool's refs root (the store/server/uuid/refs/part layout) names a part id (its payload). These
/// are the GC roots — a part id is live iff at least one ref points at it. Any list or read error
/// PROPAGATES so the caller aborts the sweep: a partial scan must never drive deletion (fail-close).
std::set<std::string> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix);

/// List the object keys of every object under a pool root prefix (e.g. partsPrefix / blobsPrefix).
/// Thin wrapper over object_storage->listObjects; errors propagate. Returned keys are the object
/// keys (matching the per-object key builders), suitable for set difference against reachable sets.
std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix);

}
