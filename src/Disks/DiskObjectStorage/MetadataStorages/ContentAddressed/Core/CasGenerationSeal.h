#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// A reference to one write-once run object plus its content checksum (RunFooter checksum), so a
/// resuming round can verify it adopted the exact bytes a prior attempt sealed (spec §Resume).
///
/// `shard` and `generation` (2026-07-02 snapshot-streaming, T0) let consumers resolve a run through the
/// seal's refs instead of constructing `blobTargetRunKey`: with reference-parent carry a run sealed for
/// generation G may physically live under an OLDER generation's key namespace, so the key alone no longer
/// implies the generation, and per-shard association must not parse the key path. `blob_target_runs`
/// refs SET both at every write point (the fold's `foldDeltasIntoGeneration` and the ref-carry copy).
/// `part_manifest_cleanup` and other outcome RunRefs are per-generation-local and MAY leave the defaults
/// (0/0) — those consumers still key by construction and never resolve through the shard/generation here.
struct RunRef
{
    String key;
    UInt128 checksum{};
    uint64_t shard = 0;        /// gc-shard this run belongs to (REQUIRED for blob_target_runs)
    uint64_t generation = 0;   /// generation whose key namespace physically holds the object (for retention)
    bool operator==(const RunRef &) const = default;
};

/// What a round did to ONE (namespace, shard). classification is a small enum byte:
///   0 = Absent (shard not present / fresh-pool), 1 = Unchanged (token matched persisted; skipped),
///   2 = Folded (records in (folded_cursor, shard_version] were folded), 3 = Minted (fence-only,
///   retired concept), 4 = Clamped (the fold was barrier/anomaly-clamped below the journal end:
///   unfolded events exist or may become foldable, so the token-diff Skip is FORBIDDEN — the next
///   round must re-read this shard regardless of an unchanged token).
/// folded_token is the shard's observed manifest token at fold time; folded_cursor is the position
/// the fold folded TO (the shard_version). Together they make `SabotageCutOverclaim` defensible: the
/// recheck can prove the cursor never ran past the sealed deltas.
struct ShardCoverage
{
    uint8_t classification = 0;
    Token folded_token;
    uint64_t folded_cursor = 0;
    ShardIncarnation incarnation;  /// incarnation the cursor was sealed against; {0,0} = unstamped

    /// The lexicographically-minimal {writer_epoch, build_sequence} among this shard's LIVE precommit
    /// owner bindings (un-promoted, un-removed) at fold time; has_live_precommit == false when there are
    /// none. Consumed by computeDiscoverDecisions to force a re-fold once the watermark proves this
    /// precommit dead, so reclaimAbandonedPrecommit runs even on an otherwise token-stable (Skip) shard.
    bool has_live_precommit = false;
    uint64_t min_live_precommit_writer_epoch = 0;
    uint64_t min_live_precommit_build_sequence = 0;

    bool operator==(const ShardCoverage &) const = default;
};

/// The FOLD seal for one GC generation — write-once at <prefix>/gc/gen/<generation>/attempt/<attempt>/fold_seal.
/// The one-pass ack-floor round records everything it folded here (coarse: no object per
/// edge/manifest/candidate). This is the sole per-generation coverage record — the resume rule and the
/// next round's parent-cursor read both key off it.
struct CasFoldSeal
{
    uint64_t generation = 0;
    uint64_t parent_generation = 0;
    std::map<String, ShardCoverage> per_ns_shard;   /// "ns/shard" -> coverage
    std::vector<RunRef> blob_target_runs;           /// the blob in-degree run segments this gen sealed
    std::vector<RunRef> part_manifest_cleanup;      /// the part-manifest cleanup bundles this gen sealed
    bool operator==(const CasFoldSeal &) const = default;
};

String encodeFoldSeal(const CasFoldSeal & seal);
CasFoldSeal decodeFoldSeal(std::string_view data);

}
