#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Optional progress sink for runFsck: called periodically during the listing and reachability
/// walk so a long scan over a large/slow pool is visibly progressing (not hung). `phase` names the
/// current step; `objects`/`pages` are running counts. Default {} = no progress (existing callers).
using FsckProgress = std::function<void(std::string_view phase, uint64_t objects, uint64_t pages)>;

enum class FsckClass : uint8_t
{
    Reachable,     /// reachable from a live ref AND present in the object store
    Dangling,      /// reachable from a live ref but the object is MISSING — INV-NO-LOSS violation
    Unreachable,   /// present but not reachable from any ref (in-grace debris or a leak)
};

struct FsckObject
{
    String key;
    ObjectKind kind = ObjectKind::Blob;
    uint64_t size = 0;                   /// on-disk object size (0 when dangling)
    FsckClass cls = FsckClass::Reachable;
    std::vector<String> reachable_from;  /// "ns/ref" labels (populated for reachable/dangling when detail)
};

struct FsckReport
{
    uint64_t reachable = 0;
    uint64_t dangling = 0;
    uint64_t unreachable = 0;

    uint64_t physical_bytes = 0;
    uint64_t referenced_logical_bytes = 0;
    uint64_t total_blob_refs = 0;
    uint64_t distinct_blobs = 0;

    std::vector<FsckObject> objects;

    double dedupRatio() const { return distinct_blobs ? double(total_blob_refs) / double(distinct_blobs) : 0.0; }
    bool clean() const { return dangling == 0; }
};

/// Independent reachability check: recompute reachability from the authoritative refs (NEVER from
/// gc/snap) and diff against a raw object listing. Read-only. `detail` populates per-object rows.
/// `deadline`, if set, bounds the WHOLE scan: it is checked between list pages and reachability
/// refs, throwing `TIMEOUT_EXCEEDED` if exceeded (a slow-but-progressing scan surfaces a clear
/// error instead of an opaque hang). A single LIST page stuck in S3-client retries is bounded
/// separately by the disk's S3 retry/timeout settings, not here.
FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress = {},
                   std::optional<std::chrono::steady_clock::time_point> deadline = {});

}
