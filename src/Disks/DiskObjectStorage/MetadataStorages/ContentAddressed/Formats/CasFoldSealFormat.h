#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
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
/// Other outcome RunRefs (if any) are per-generation-local and MAY leave the defaults (0/0) — those
/// consumers still key by construction and never resolve through the shard/generation here.
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
///   2 = Folded (records up to the shard_version were folded), 4 = Clamped (the fold was
///   barrier/anomaly-clamped below the ref-log's current cursor: unfolded events exist or may become
///   foldable, so the token-diff Skip is FORBIDDEN — the next round must re-read this shard regardless
///   of an unchanged token).
/// folded_token is the shard's observed manifest token at fold time.
struct ShardCoverage
{
    uint8_t classification = 0;
    Token folded_token;

    /// Snapshot+log ref model (spec §GC State): the durable `last_folded_ref_id` for this table -- the
    /// greatest `RefTxnId` whose owner changes have already contributed their manifest-edge delta. Keyed
    /// per table (one ref-log stream per namespace, shard fixed at 0). {0,0} = nothing folded yet.
    RefTxnId last_folded_ref_id{};

    bool operator==(const ShardCoverage &) const = default;
};

/// Per-gc-shard condemned-in-snapshot summary (retired-in-snapshot T4, spec §2.2). Distilled from the
/// `kCondemned` rows this generation sealed for one gc-shard so `graduationDue` and the pure ref-carry
/// decision are ZERO-I/O (they read only the seal, never a run). It is TOTAL over `0..gc_shards-1` in
/// every newly written seal (folding shards compute it from their `still_retired`; pure-carry shards copy
/// the parent's entry verbatim). A "missing" shard entry is never treated as zero — consumers fail closed.
struct CondemnedSummary
{
    uint64_t condemned_total = 0;   /// count of `kCondemned` rows in this shard's sealed run
    uint64_t pending_total = 0;     /// how many of those are `delete_pending` (a graduation is due)
    uint64_t oldest_nonpending_condemn_round = UINT64_MAX;   /// min condemn_round over non-pending; UINT64_MAX = none
    bool operator==(const CondemnedSummary &) const = default;
};

/// Durable state of one namespace-cleanup item (spec §Clean Old Ref Objects). Phase 1 protocol state
/// added by the snapshot+log ref design: the two-state record of physically reclaiming a removed
/// namespace's `@cas@` metadata. `Pending` while enumerate-and-delete passes run; terminal `Completed`
/// once a pass observed nothing left, after which the `_cleanup` marker and `Removed` snapshot are
/// (idempotently) published and a later `namespace_birth` may recreate the namespace.
enum class RefNsCleanupState : uint8_t
{
    Pending = 1,
    Completed = 2,
};

/// One namespace-cleanup item keyed by `{ns, remove_txn_id}` (spec §Remove Namespace / §Step 6). Carried
/// forward in the fold seal each generation until `Completed`.
struct RefNsCleanupItem
{
    RootNamespace ns;
    RefTxnId remove_txn_id;
    RefNsCleanupState state = RefNsCleanupState::Pending;
    bool operator==(const RefNsCleanupItem &) const = default;
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
    /// (`part_manifest_cleanup` removed in codecs-v3 phase 5: the cleanup RUN object had no reader —
    /// manifest cleanups execute inline from the in-memory `mf_cleanup` map — so the durable bundle and
    /// this seal record were dead weight.)
    std::map<uint64_t, CondemnedSummary> condemned_summary;   /// gc-shard -> summary; TOTAL over gc_shards
    /// Namespace-cleanup items (spec §Step 6), keyed by "<ns>\n<remove-txn-render>". Carried forward each
    /// generation until Completed. Empty on a pool that has never removed a namespace.
    std::map<String, RefNsCleanupItem> ns_cleanup_items;
    bool operator==(const CasFoldSeal &) const = default;
};

/// v3 text (spec §control-plane, DETERMINISTIC — PinnedRaw): header line + a meta line
/// {"g","pg"} + tagged record lines (fixed section order: cov/btr/cnd/nsc, each sorted) + {"n":count}
/// trailer. Byte-reproducible for the putDeterministicArtifact retry-equality contract.
String encodeFoldSeal(const CasFoldSeal & seal);
CasFoldSeal decodeFoldSeal(std::string_view data);

}
