#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefStateMachine.h>
#include <base/types.h>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace DB::Cas
{

/// Pure ref-log intake primitives for the GC round (spec §GC Round Algorithm). None of these read a
/// manifest body, a snapshot body, or `gc/state`: they turn a global `LIST cas/refs/` result and the
/// decoded bodies of new transactions into (a) the per-table log/snapshot/marker listing, (b) the
/// deterministic manifest-edge delta, and (c) the exact ref-object cleanup plan. The GC round
/// (`CasGc.cpp`) drives the manifest-body reads (`foldManifestEdges`), the fold barrier, the durable
/// cursor, and the batch deletions around these functions. Keeping the delta and cleanup logic pure
/// makes it directly unit-testable (`gtest_cas_ref_intake.cpp`) without a full round.

/// One `+1`/`-1` manifest edge emitted by one ref-log operation (spec §gc-step-produce-manifest-edge-delta).
/// `manifest_id` is namespace-qualified (equal `ManifestRef` tuples under two tables stay distinct).
/// The ordinals locate the exact operation and edge inside the transaction, giving the spec's
/// `event_id = {namespace, RefTxnId, operation_ordinal, edge_ordinal}` its determinism: replaying the
/// same logs yields byte-identical edges, so retry and competing GC attempts produce the same delta.
struct RefManifestEdge
{
    ManifestId manifest_id;
    int change = 0;              /// +1 activation | -1 removal
    RefOwnerKind owner_kind = RefOwnerKind::Committed;   /// kind of the binding that produced this edge:
                                 /// the `new_binding` kind for a `+1`, the `old_binding` kind for a `-1`.
                                 /// The GC fold needs it to classify a missing manifest body -- a removed
                                 /// precommit that never activated is skipped, every other missing body clamps.
    uint32_t op_ordinal = 0;     /// index of the op within its transaction
    uint32_t edge_ordinal = 0;   /// 0 = the removal edge, 1 = the activation edge of one op

    bool operator==(const RefManifestEdge &) const = default;
};

/// The manifest edges of ONE decoded transaction, in operation order, reading NO manifest body
/// (spec §gc-step-produce-manifest-edge-delta). Rules per operation:
///   - `owner_transition` with only a new binding (add owner)            => `+1` for `new.manifest_ref`
///   - `owner_transition` with only an old binding (remove owner)        => `-1` for `old.manifest_ref`
///   - `owner_transition` old+new naming the SAME manifest (promote)     => no edge (net zero)
///   - `owner_transition` old+new naming DIFFERENT manifests (replace)   => `-1` old then `+1` new
///   - `owner_transition` with neither binding                          => no edge (degenerate; a
///     transition shape the writer never produces and `applyRefLogTxn` rejects at replay)
///   - `namespace_birth` / `set_payload` / `remove_namespace`           => no edge
/// The `remove_namespace` operation changes lifecycle only; the exact owner removals that must precede
/// it in the same transaction already emit their own `-1` edges (spec §Remove Namespace).
std::vector<RefManifestEdge> manifestEdgesOfTxn(const RefLogTxn & txn);

/// The GC fold consumes a table's new transactions ONE log at a time, in ascending id order, emitting
/// each log's `manifestEdgesOfTxn` into `foldManifestEdges` and advancing the durable cursor per fully
/// folded log (mirroring the legacy per-event journal fold, including its clamp-on-missing-body barrier).
/// In-batch add+remove cancellation is therefore NOT done as a pre-fold net pass: the idempotent
/// `(blob, source_id)` in-degree set-merge already cancels a `+edge` and matching `-edge` folded into one
/// generation, and a pre-fold net pass would be unsafe -- a mid-batch clamp could split a cancelled pair
/// across the advanced cursor, folding a spurious `-1` in a later round. So there is deliberately no
/// `netManifestDelta` here.

/// The `remove_txn_id` of a transaction that ends its namespace's life (contains a `remove_namespace`
/// operation), or `nullopt`. The value equals `txn.txn_id`. The round routes it into the durable
/// `{namespace, remove_txn_id}` namespace-cleanup item (spec §Remove Namespace / §Step 6).
std::optional<RefTxnId> removalTxnId(const RefLogTxn & txn);

/// One table's surviving ref-object keys from this round's global `LIST`, split by kind and sorted
/// ascending (spec §Step 1). `_cleanup` markers are consulted only by the writer's recreation gate;
/// GC groups them so a later step can decide marker republication without a second scan.
struct RefTableListing
{
    std::vector<RefTxnId> logs;
    std::vector<RefTxnId> snapshots;
    std::vector<RefTxnId> cleanup_markers;

    bool operator==(const RefTableListing &) const = default;
};

/// Parse and group a global `LIST` of keys under `layout.casRefsPrefix()` by table (spec §Step 1). Every
/// key is expected to be one of the three ref-object kinds; a key under the ref prefix that
/// `Layout::parseRefObjectKey` does not recognize, or whose reconstructed namespace is malformed
/// (VERIFY-AT-T12: `parseRefObjectKey` does not re-validate the namespace, so this does), is a malformed
/// ref key and throws `CORRUPTED_DATA` -- the round catches it and aborts ref folding for the round
/// (spec §Step 2: a malformed key cannot produce a partial ref delta or authorize destructive work).
/// A key OUTSIDE `casRefsPrefix()` is ignored: the caller lists only the ref prefix, and a foreign key
/// is not this format's concern.
std::map<String, RefTableListing> groupRefKeys(const Layout & layout, const std::vector<String> & listed_keys);

/// The exact ref objects one round may delete for one table (spec §Step 6 / §Concurrent Startup And
/// Cleanup). Pure; acts only on keys THIS round's scan returned, so a covering snapshot is always
/// durable before any deletion it authorizes. A log `L` is deletable iff:
///   1. the newest observed snapshot `X` covers it (`L <= X`);
///   2. the durable `last_folded_ref_id` covers it (`L <= durable_cursor`);
///   3. it is not in `removal_logs_blocked` (a `remove_namespace` log whose namespace-cleanup item has
///      not durably reached `Completed`).
/// Snapshots with id `< X` are deletable. With no observed snapshot, `X` is undefined and no log is
/// coverage-deletable (an empty plan). The newest snapshot `X` itself is retained.
struct RefCleanupPlan
{
    std::vector<RefTxnId> deletable_logs;
    std::vector<RefTxnId> deletable_snapshots;

    bool operator==(const RefCleanupPlan &) const = default;
};
RefCleanupPlan planRefCleanup(const RefTableListing & listing, const RefTxnId & durable_cursor,
                              const std::set<RefTxnId> & removal_logs_blocked);

/// Recover one table's state via the shared recovery equation (spec §Startup And Recovery / §Table State):
/// one `LIST` of the table prefix, the newest valid snapshot, and replay of the later log tail through the
/// same state machine the writer uses. This is the read-only recovery `fsck`, offline repair, and GC's
/// disaster-recovery rebuild all consume, so every consumer agrees on what a table's ref state is.
/// Restart-on-vanish: a selected snapshot or tail body deleted between the `LIST` and its `GET` (a
/// concurrent GC cleanup published a covering newer snapshot) is not corruption -- recovery restarts with a
/// fresh `LIST`, bounded by `max_restarts`, after which the vanish becomes a `CORRUPTED_DATA` throw. A
/// never-touched namespace yields the empty (never-born) state. Different bytes under a deterministic key,
/// or an invalid body, are corruption and throw immediately.
RefTableState recoverRefTable(Backend & backend, const Layout & layout, const RootNamespace & ns,
                              unsigned max_restarts = 3);

}
