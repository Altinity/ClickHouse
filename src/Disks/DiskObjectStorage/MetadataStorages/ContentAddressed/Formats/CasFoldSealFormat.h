#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// A reference to one write-once run object and its whole-object checksum. A retry can compare the
/// checksum with the bytes already sealed before it adopts or consumes the run.
///
/// `shard` and `generation` are required on `blob_target_runs`. An idle shard can carry a parent's run
/// into a newer seal, even though the object remains under the older generation's key namespace. The
/// explicit fields therefore preserve both the shard association and the physical generation without
/// making consumers parse a storage key. Run references for objects that are always local to the current
/// generation may leave these fields at their defaults because their consumers resolve them by key.
struct RunRef
{
    String key;
    UInt128 checksum{};
    uint64_t shard = 0;        /// gc-shard this run belongs to (REQUIRED for blob_target_runs)
    uint64_t generation = 0;   /// generation whose key namespace physically holds the object (for retention)
    bool operator==(const RunRef &) const = default;
};

/// Records what the current round did for one namespace and shard. `classification` is a persisted byte:
/// 0 means absent, 1 means unchanged, 2 means all records through the observed cursor were folded, and 4
/// means folding was clamped below the ref-log cursor. A clamped entry must be read again even if its
/// manifest token is unchanged, because an unfolded event may become foldable in the next round.
/// `folded_token` is the manifest token observed when the entry was processed.
struct ShardCoverage
{
    uint8_t classification = 0;
    Token folded_token;

    /// The greatest `RefTxnId` whose owner changes have contributed their manifest-edge deltas. There is
    /// one ref-log stream per namespace, so this cursor is stored in the namespace's shard-0 entry.
    /// `{0, 0}` means that no transaction has been folded yet. A clamp leaves the cursor below the
    /// offending transaction so the complete transaction is retried rather than partially applied.
    RefTxnId last_folded_ref_id{};

    bool operator==(const ShardCoverage &) const = default;
};

/// Per-shard summary of condemned rows carried in the sealed source-edge run. It lets graduation and
/// pure reference-carry decisions inspect the seal without reading a run. Every newly written seal has
/// an entry for every shard in `0..gc_shards-1`: a folding shard computes its entry from its remaining
/// condemned rows, while a pure-carry shard copies the parent's entry. Missing entries are invalid and
/// must not be interpreted as zero.
struct CondemnedSummary
{
    uint64_t condemned_total = 0;   /// count of `kCondemned` rows in this shard's sealed run
    uint64_t pending_total = 0;     /// how many of those are `delete_pending` (a graduation is due)
    uint64_t oldest_nonpending_condemn_round = UINT64_MAX;   /// min condemn_round over non-pending; UINT64_MAX = none
    bool operator==(const CondemnedSummary &) const = default;
};

/// Durable state of the physical cleanup for a removed namespace's `@cas@` metadata. `Pending` remains
/// in the seal while enumerate-and-delete passes may still find objects. `Completed` is terminal for the
/// physical pass: the namespace was observed empty, after which the cleanup marker and `Removed` snapshot
/// can be published idempotently before a later `namespace_birth` recreates the namespace.
enum class RefNsCleanupState : uint8_t
{
    Pending = 1,
    Completed = 2,
};

/// One namespace-cleanup item keyed by `{ns, remove_txn_id}`. It is carried forward in each fold seal
/// until the `Completed` state has been durably observed and its completion artifacts are present.
struct RefNsCleanupItem
{
    RootNamespace ns;
    RefTxnId remove_txn_id;
    RefNsCleanupState state = RefNsCleanupState::Pending;
    bool operator==(const RefNsCleanupItem &) const = default;
};

/// The write-once fold seal for one GC generation at
/// `<prefix>/gc/gen/<generation>/attempt/<attempt>/fold_seal`. It is the generation's durable coverage
/// record: it stores cursors and run references, not one record per edge, manifest, or candidate. A retry
/// and the next round use the adopted seal to determine what was folded and which parent runs can be
/// carried forward. Its run references and cleanup items are also the durable inputs to retention and
/// namespace cleanup. Manifest cleanup is intentionally not represented here: those cleanups execute
/// inline from the in-memory cleanup map, and no durable cleanup-run reader exists.
struct CasFoldSeal
{
    uint64_t generation = 0;
    uint64_t parent_generation = 0;
    std::map<String, ShardCoverage> per_ns_shard;   /// "ns/shard" -> coverage
    std::vector<RunRef> blob_target_runs;           /// the blob in-degree run segments this gen sealed
    std::map<uint64_t, CondemnedSummary> condemned_summary;   /// gc-shard -> summary; TOTAL over gc_shards
    /// Namespace-cleanup items, keyed by "<ns>\n<remove-txn-render>". Carried forward until their
    /// completion artifacts are observed. Empty on a pool that has never removed a namespace.
    std::map<String, RefNsCleanupItem> ns_cleanup_items;
    bool operator==(const CasFoldSeal &) const = default;
};

/// Encodes a fold seal as a strict, raw text control object. The header and meta lines are followed by
/// tagged records in the fixed `cov`/`btr`/`cnd`/`nsc` order and a record-count trailer. Map iteration and
/// run references are sorted so retries produce byte-identical output for write-once adoption.
String encodeFoldSeal(const CasFoldSeal & seal);

/// Decodes and validates a fold seal, rejecting unknown fields, malformed records, trailing bytes, and
/// a trailer count that differs from the records read. Invalid persisted data raises `CORRUPTED_DATA`.
CasFoldSeal decodeFoldSeal(std::string_view data);

}
