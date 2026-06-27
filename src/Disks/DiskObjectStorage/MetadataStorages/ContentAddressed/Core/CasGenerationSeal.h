#pragma once
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
struct RunRef
{
    String key;
    UInt128 checksum{};
    bool operator==(const RunRef &) const = default;
};

/// What a round did to ONE (namespace, shard). classification is a small enum byte:
///   0 = Absent (shard not present / fresh-pool), 1 = Unchanged (token matched persisted; skipped),
///   2 = Folded (records in (folded_cursor, shard_version] were folded), 3 = Minted (fence-only).
/// folded_token is the shard's observed manifest token at fold time; folded_cursor is the position
/// the fold folded TO (the shard_version). Together they make `SabotageCutOverclaim` defensible: the
/// recheck can prove the cursor never ran past the sealed deltas.
struct ShardCoverage
{
    uint8_t classification = 0;
    Token folded_token;
    uint64_t folded_cursor = 0;
    bool operator==(const ShardCoverage &) const = default;
};

/// The FOLD seal for one GC generation — write-once at <prefix>/gc/gen/<generation>/fold_seal (spec
/// rev. 15 §Visibility-Split: "fold output is sealed in a write-once CasFoldSeal"). Coarse: there is no
/// object per edge/manifest/candidate. Records exactly what `fold` folded; fence/recheck/delete/trim
/// do NOT touch this object — they write the separate `CasCompletionSeal`.
struct CasFoldSeal
{
    uint64_t generation = 0;
    uint64_t parent_generation = 0;
    std::map<String, ShardCoverage> per_ns_shard;   /// "ns/shard" -> coverage
    std::vector<RunRef> blob_target_runs;           /// the blob in-degree run segments this gen sealed
    std::vector<RunRef> part_manifest_cleanup;      /// the part-manifest cleanup bundles this gen sealed
    bool operator==(const CasFoldSeal &) const = default;
};

/// The COMPLETION seal for one GC generation — write-once at <prefix>/gc/gen/<generation>/completion_seal
/// (spec rev. 15 §Visibility-Split). Records what fence/recheck/delete/trim completed; its presence is
/// the durable "this generation is done" marker the resume rule reads (completion_seal => done).
/// `adoptable` is the gate the internal reducer products + generation adoption are held behind, distinct
/// from the retired-token view (published earlier behind the retire barrier — gc/state.round /
/// `ViewableRound`).
struct CasCompletionSeal
{
    uint64_t generation = 0;
    std::map<String, uint64_t> fence_positions;     /// "ns/shard" (+ "_registry") -> fenced version
    std::vector<RunRef> blob_target_runs;           /// the completion (fold-through-fence) gen's in-degree runs
    std::vector<RunRef> delete_outcomes;            /// the outcome-log segments this gen wrote
    std::map<String, uint64_t> trim_cursors;        /// "ns/shard" -> the cursor trim ran to
    bool adoptable = false;                         /// gen adoption gated on this (see §Visibility-Split)
    /// M1: per-(ns,shard) fold cursor coverage carried forward from the round's fold seal at recheck. The
    /// next round's fold reads its parent cursor from the LATEST seal at snap_generation; after a completed
    /// round that is THIS completion seal (its fold_seal lives at the parent generation), so the cursor must
    /// be recoverable here with NO dependence on trim having run (else re-fold from 0 => blob double-count).
    std::map<String, ShardCoverage> folded_cursors; /// "ns/shard" -> coverage (mirrors CasFoldSeal::per_ns_shard)
    bool operator==(const CasCompletionSeal &) const = default;
};

String encodeFoldSeal(const CasFoldSeal & seal);
CasFoldSeal decodeFoldSeal(std::string_view data);

String encodeCompletionSeal(const CasCompletionSeal & seal);
CasCompletionSeal decodeCompletionSeal(std::string_view data);

}
