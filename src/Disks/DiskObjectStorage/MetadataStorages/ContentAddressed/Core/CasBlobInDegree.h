#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <vector>

namespace DB::Cas
{

/// A single +1/-1 source-edge update to a blob's in-degree, pre-merge. `delta` is always +1 or -1.
struct BlobDelta
{
    UInt128 blob_hash{};
    int64_t delta = 0;
};

/// A blob whose merged in-degree reached exactly zero — a retire candidate.
struct BlobCandidate
{
    UInt128 hash{};
};

/// Merge the prior generation's blob in-degree run for `shard` (absent => empty/zero baseline) with
/// `scattered` deltas, producing the new generation's write-once run under
/// blobTargetRunKey(new_generation, shard, 0). Streaming: prior run + scattered deltas are sorted by
/// `blob_hash` and merged via `RunMerger`; memory is O(inputs * block_size). The merged per-blob
/// counter must never go below zero (CORRUPTED_DATA — an undercount that would over-delete). A blob
/// whose count transitions to exactly 0 this generation is written as an explicit 0-row so
/// `zeroInDegree` can stream it; rows that are still 0 from a prior generation are dropped. Appends the
/// produced run's `RunRef` (key + footer checksum) to `out_runs` for the fold seal.
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t new_generation, uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs);

/// Stream the sealed in-degree run for (generation, shard) and return every blob written at in-degree 0
/// (the candidates that transitioned to zero in this generation).
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t shard);

/// The in-degree of one blob in the sealed (generation, shard) run: 0 when the blob is absent from the
/// run (or written as an explicit transitioned-to-0 row), else its count. Used by the recheck's
/// per-candidate spare/delete decision and by tests.
int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t shard, const UInt128 & blob_hash);

}
