#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <vector>

namespace DB::Cas
{

/// Deterministic 16-byte id of a source edge (ManifestId, path). Distinctness only — not reconstructable.
UInt128 sourceEdgeId(const ManifestId & id, const String & path);
/// 32-byte run key = blob_hash(16 BE) ++ source_id(16 BE); lexicographic == (blob_hash, source_id) order.
String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id);
bool parseSrcEdgeRunKey(const String & key, UInt128 & blob_hash, UInt128 & source_id);

/// Write-once for a DETERMINISTIC artifact (same inputs => byte-identical bytes): the blob in-degree
/// runs AND the fold/completion seals. `putIfAbsent`; on a `PreconditionFailed` the key is already
/// occupied — `get` it and compare bytes: byte-equal means our own deterministic replay (adopt, no-op),
/// divergent bytes are impossible under correct operation and we fail closed with `CORRUPTED_DATA`
/// rather than let a divergent artifact disagree with the adopted snapshot. This is the spec's
/// "deterministic artifacts: byte-equal-or-CORRUPTED_DATA" rule (spec §strict-put-if-absent). It is
/// NOT for the observation-bearing artifacts (retired set, outcome log) — those carry HEAD-observed
/// tokens that two observers may legitimately differ on and keep first-durable-write-wins semantics.
void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes);

/// One source-edge update pre-merge: the edge (blob_hash, source_id), and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    UInt128 blob_hash{};
    UInt128 source_id{};
    bool remove = false;
};

/// A blob whose active source-edge set became empty this generation — a retire candidate.
struct BlobCandidate
{
    UInt128 hash{};
};

/// Merge the prior generation's blob source-edge run for `shard` (absent => empty/zero baseline) with
/// `scattered` deltas, producing the new generation's write-once run under
/// blobTargetRunKey(new_generation, attempt, shard, 0). Streaming: prior run + scattered deltas are sorted by
/// (blob_hash, source_id) and merged via two-cursor scan; the source-edge set is idempotent under re-fold
/// (identical (blob_hash, source_id) pairs deduplicate). A blob whose active edge set transitions to
/// exactly empty this generation is written as an explicit zero-marker row so `zeroInDegree` can stream
/// it; prior-generation zero markers are dropped. Appends the produced run's `RunRef` (key + footer
/// checksum) to `out_runs` for the fold seal.
/// `prior_attempt` is the attempt under which the PARENT generation's run was sealed (the prior round's
/// adopted `snap_attempt`); `attempt` is the attempt under which THIS generation's run is written (the
/// folding leader's `lease.seq`). They differ whenever the round that produced `prior_generation` adopted
/// a different attempt than the current fold mints — keeping them distinct lets the fold read the correct
/// parent baseline while writing under its own attempt.
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t prior_attempt,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs);

/// Stream the sealed in-degree run for (generation, attempt, shard) and return every blob written at
/// in-degree 0 (the candidates that transitioned to zero in this generation).
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t attempt, uint64_t shard);

/// The in-degree of one blob in the sealed (generation, attempt, shard) run: 0 when the blob is absent
/// from the run (or written as an explicit transitioned-to-0 row), else its count. Used by the recheck's
/// per-candidate spare/delete decision and by tests.
int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t attempt, uint64_t shard, const UInt128 & blob_hash);

}
