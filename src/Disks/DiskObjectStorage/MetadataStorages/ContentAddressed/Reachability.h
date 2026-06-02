#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DB::ContentAddressed
{

using PartManifestResolver = std::function<PartManifest(const PartId & part_id)>;

/// Reachable blob object-key set from the live roots (refs -> part manifests -> blob object keys).
/// A manifest stores the BARE content hash in each `BlobEntry.key` (the production write path records
/// `BlobEntry{blob_hash, size, blob_hash}`), while the GC sweep enumerates FULL object keys under
/// `blobsPrefix(key_prefix)`. To make the two comparable the reachable set is built with the SAME
/// `blobKey(key_prefix, bare_hash)` fan-out the read path uses, so reachable == full blob object key.
/// The return type is `std::set<BlobObjectKey>`: the sweep can ONLY compare it against listed blobs
/// after wrapping those into `BlobObjectKey` too, so a bare-hash-vs-object-key mismatch cannot compile.
std::set<BlobObjectKey> markReachableBlobs(
    const std::string & key_prefix, const std::set<PartId> & live_part_ids, const PartManifestResolver & resolve);

struct SweepResult
{
    std::vector<std::string> to_delete;
    std::unordered_map<std::string, int64_t> first_unreachable; /// updated timers (cleared for reachable-again)
};

/// `grace` is measured from first loss of reachability (NOT object age). Reachable-again clears the timer.
/// Operates on raw object-key strings — the GC has already reduced both blob and part keys to the
/// same unreferenced-object-key space before calling this, so no typed distinction is needed here.
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace);

}
