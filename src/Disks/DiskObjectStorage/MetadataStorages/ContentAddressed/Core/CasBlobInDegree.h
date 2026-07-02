#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
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
/// Outcome of the retired (third) merge cursor (ack-floor redesign, spec 2026-07-02): the same
/// streaming pass that folds edges settles every prior retired entry and detects new candidates.
struct RetiredMergeResult
{
    std::vector<RetiredEntry> still_retired;   /// carried + newly-condemned + newly-PENDING entries (the next list)
    std::vector<RetiredEntry> graduated;       /// newly floor-passed this pass — published pending, deleted NEXT pass
    std::vector<RetiredEntry> spared;          /// in-degree recovered — entry dropped
    std::vector<RetiredEntry> redelete;        /// pending in the PRIOR list — execute deleteExact pre-CAS, drop
};

/// Three-cursor extension (ack-floor redesign): `prior_retired` (Blob entries ONLY, sorted by hash
/// ascending) rides the same per-blob close-out as the two existing cursors. Settlement rules, in
/// order, per entry for blob `h` with post-merge in-degree `d`:
///   delete_pending (prior pass)          -> redelete if d = 0 (the caller executes the exact-token
///                                           delete pre-CAS and the entry drops); d > 0 for a pending
///                                           entry is structurally impossible — spared + loud log;
///   d > 0                                -> spared (recovery wins even past the floor);
///   d = 0 and condemn_round < min_ack    -> graduated: REPUBLISHED as delete_pending (two-phase
///                                           graduation, Task-9 amendment) — deleted the NEXT pass;
///   d = 0 otherwise                      -> still_retired, carried byte-unchanged.
/// A blob that transitions to zero THIS pass with no prior entry is condemned: `head_blob` captures
/// the exact incarnation token/size (absent object or empty head_blob -> nothing to delete, skipped)
/// and the entry is minted at `condemn_round`. Entries for blobs the merged stream never visits
/// (no edges, no deltas) settle at in-degree 0 by definition. The retired cursor never changes the
/// snapshot run bytes. Defaults keep the legacy call sites behavior-identical (empty retired,
/// min_ack 0 => nothing graduates, no head_blob => nothing condemned).
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t prior_attempt,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              const std::vector<RetiredEntry> & prior_retired = {},
                              uint64_t min_ack = 0, uint64_t condemn_round = 0,
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & head_blob = {},
                              RetiredMergeResult * out_retired = nullptr);

/// Stream the sealed in-degree run for (generation, attempt, shard) and return every blob written at
/// in-degree 0 (the candidates that transitioned to zero in this generation).
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t attempt, uint64_t shard);

/// The in-degree of one blob in the sealed (generation, attempt, shard) run: 0 when the blob is absent
/// from the run (or written as an explicit transitioned-to-0 row), else its count. Used by
/// `Gc::previewDeletes` and by tests (the round itself settles candidates inside the three-cursor
/// merge; there is no per-candidate point query anymore).
int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t attempt, uint64_t shard, const UInt128 & blob_hash);

}
