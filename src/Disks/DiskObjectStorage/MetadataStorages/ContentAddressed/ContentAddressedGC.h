#pragma once
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace DB::ContentAddressed
{

struct SweepStats
{
    size_t deleted_blobs = 0;
    size_t deleted_parts = 0;
};

/// Reachability garbage collector for one content-addressed pool (single process — see the M1 GC
/// safety invariants). It enumerates the live part ids (the published refs), computes the reachable
/// blob set from their footers, and deletes ONLY footers under parts/ and blobs under blobs/ that
/// have been continuously unreferenced for at least `grace` seconds. Refs, table-level files and
/// generic disk files are owned by the table and never touched here.
///
/// `first_unreachable` is the across-sweeps timer state (grace is measured from the first sweep that
/// found an object unreferenced, NOT from object age); reachable-again clears an object's timer.
/// It is held in memory for the process lifetime (M1): on restart it resets, which only makes grace
/// conservative (never premature deletion).
class ContentAddressedGC
{
public:
    ContentAddressedGC(ObjectStoragePtr object_storage_, std::string key_prefix_);

    /// Run one sweep. Deletes nothing if any step before the removal throws (fail-close): a missing
    /// footer for a live ref (B18), or any list/read error, aborts the sweep with the pool intact.
    SweepStats runSweepOnce(int64_t now, int64_t grace);

private:
    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
    std::unordered_map<std::string, int64_t> first_unreachable;
};

}
