#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace DB::Cas
{

/// The in-memory table state (spec §Responsibility Boundary / §Table State):
/// `TableState = Replay(S_X.state, tail(X))`. This struct, `applyRefLogTxn`, `snapshotOf`, and
/// `replay` are the ONE shared implementation of that equation -- used verbatim by the writer, its
/// own recovery path, `fsck`, and snapshot construction, so every consumer agrees on what a
/// transaction sequence means.
///
/// Representation note (never-born vs removed): there is no separate "first birth" flag. Both
/// "never born" and "Removed" default `lifecycle` to `RefLifecycle::Removed`; they are told apart by
/// `remove_txn_id`: absent means the namespace has never completed a `remove_namespace` transaction
/// (either truly never born, or -- from this struct's point of view -- indistinguishable from it,
/// which is fine because a `namespace_birth` op is legal from EITHER case and nothing else is legal
/// from either). Present means a real removal happened and recorded its `RefTxnId`. `committed` and
/// `precommits` are always empty while `lifecycle == Removed` (an invariant `applyRefLogTxn`
/// maintains: `remove_namespace` only fires once both are already empty, and no other operation is
/// legal until the next `namespace_birth`).
struct RefTableState
{
    RefLifecycle lifecycle = RefLifecycle::Removed;   /// see representation note above
    std::optional<RefTxnId> remove_txn_id;
    RefTxnId greatest_applied{};                       /// {0, 0} = no transaction applied yet

    std::map<String, RefCommittedRow> committed;                     /// keyed by ref_name
    std::set<std::pair<String, ManifestRef>> precommits;             /// (ref_name, manifest_ref)
};

/// Applies the COMPLETE transaction to `state`, or throws `CORRUPTED_DATA` and leaves `state`
/// byte-for-byte unchanged (spec §State Transitions: every subsection there is one precondition
/// enforced here). Two-phase: every op is validated and applied, in array order, against a scratch
/// copy first; `state` is replaced by the scratch only once the whole transaction has succeeded, so
/// no intra-transaction intermediate state (e.g. a manifest with its precommit already gone but its
/// committed binding not yet installed) is ever observable to a caller -- matching spec §Promote:
/// "There is no moment at which the manifest has no owner."
///
/// Enforced preconditions:
///  - `txn.txn_id` must be strictly greater than `state.greatest_applied` (spec §Ordered Ref
///    Transaction Identifier: "only strict increase is required").
///  - `remove_namespace`, if present, must be the transaction's FINAL operation, and every earlier
///    operation must be an exact owner-removal `owner_transition` (`old_binding` set, `new_binding`
///    empty) -- spec §Remove Namespace. The codec does not check this shape; this is the one place
///    that does.
///  - `namespace_birth`: legal only while `lifecycle != Live` (never-born or `Removed`); spec
///    §Namespace Birth. (Gating recreation on the `_cleanup` marker for a completed removal is the
///    writer's recovery-time responsibility -- Task 10/11 -- since the marker is not part of
///    `RefTableState`.)
///  - `owner_transition` add (no `old_binding`, `new_binding.kind == Precommit`): namespace must be
///    `Live`; the exact `(ref_name, manifest_ref)` pair must be absent from `precommits`; AND no
///    existing committed row or precommit binding, under ANY ref_name, may already name the same
///    `manifest_ref` (spec §Add Precommit: "no conflicting owner may name the same manifest" -- this
///    is what lets `GC`'s `+1/-1` manifest-edge delta treat one manifest as ever having at most one
///    owner). The build-tuple-is-locally-active-build half of that same sentence is the writer's own
///    concern -- `RefTableState` has no notion of "active builds".
///  - `owner_transition` remove (an `old_binding`, no `new_binding`): the exact binding (Precommit or
///    Committed, matching `ref_name` and `manifest_ref`) must exist.
///  - `owner_transition` promote (`old_binding.kind == Precommit`, `new_binding.kind == Committed`,
///    same `ref_name` and `manifest_ref` on both sides): the exact precommit must exist, AND
///    `ref_name` must not already name a DIFFERENT committed manifest -- that stale row must be
///    evicted by its own explicit `owner_transition(old=Committed, new=None)` first (an earlier op of
///    the same transaction, so the two together read as one atomic replace, or an earlier
///    transaction). Promote never displaces an existing committed row implicitly: `GC`'s
///    manifest-edge delta (spec §Produce Manifest-Edge Delta) is read off each transaction's explicit
///    ops, not a before/after state diff, so a silent displacement would never emit the evicted
///    manifest's "-1" edge -- it would leak as phantom-alive forever. On success the precommit is
///    replaced by a committed row with an EMPTY payload (spec §Promote: the initial payload arrives
///    via a separate `set_payload` op, in the same transaction or a later one).
///  - Any other `old_binding`/`new_binding` combination is not a recognized transition shape.
///  - `set_payload`: namespace must be `Live`; the committed ref named by `ref_name` must still name
///    `expected_manifest_ref` (spec §Update Payload).
///  - `remove_namespace`: namespace must be `Live` and both `committed` and `precommits` must already
///    be empty at this point in the (in-array-order) replay -- which is only true if the transaction's
///    earlier removal ops actually named every owner (spec §Remove Namespace).
///  - Any operation other than `namespace_birth` while `lifecycle == Removed` is rejected -- spec
///    §Remove Namespace: "Any operation other than a valid later namespace_birth while state is
///    Removed is corruption."
///
/// `CORRUPTED_DATA` throughout: every rejection above is the spec's own "is corruption" framing
/// (§Remove Namespace) extended uniformly to every other precondition in this section, matching how
/// `CasRefLogCodec`/`CasRefSnapshotCodec` already use `CORRUPTED_DATA` for "this data does not
/// correspond to a valid state" one layer down (wire shape rather than transition legality). Recovery
/// and `fsck` -- the primary callers replaying persisted logs -- want exactly that fail-closed
/// framing; a writer that wants a friendlier user-facing rejection for an ordinary attempted mutation
/// (e.g. "ref already exists") checks its own business state before ever building the op.
void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn);

/// The canonical snapshot of `state` under `ns` (spec §Snapshot Format): `committed` sorted by
/// bytewise `ref_name` (guaranteed by `std::map<String, ...>`'s iteration order) and `precommits`
/// sorted by `(ref_name, manifest_ref)` (guaranteed by `std::set<std::pair<String, ManifestRef>>`'s
/// iteration order, since `ManifestRef::operator<` matches the tuple order `CasRefSnapshotCodec`
/// itself sorts by). `snapshot_id` is `state.greatest_applied`; a `Removed` state produces zero rows
/// plus `remove_txn_id`, per spec. Does not itself enforce that the result is encodable (a
/// never-born state's `snapshot_id` is `{0, 0}`, which `encodeRefTableSnapshot` already rejects) --
/// that check already lives in the codec and need not be duplicated here.
RefTableSnapshot snapshotOf(const RefTableState & state, const String & ns);

/// `TableState = Replay(S_X.state, tail(X))` (spec §Table State) in one call: starts from `snapshot`
/// (or the empty/never-born state when absent) and applies every transaction in `tail`, in order,
/// via `applyRefLogTxn`. Validates ns/lifecycle coupling: a given `snapshot`'s `lifecycle` must be
/// consistent with its `remove_txn_id`/row emptiness (the same coupling `CasRefSnapshotCodec` already
/// checks -- re-checked here because `replay` may be handed a hand-built `RefTableSnapshot` that never
/// passed through `decodeRefTableSnapshot`), and every entry of `tail` must share one `ns` -- with
/// `snapshot`'s `ns` when a snapshot is given, otherwise with each other. A mismatch throws
/// `CORRUPTED_DATA`: replaying transactions from the wrong table onto this table's snapshot would
/// silently produce a wrong-but-plausible-looking state, exactly the class of bug this equation exists
/// to make impossible.
RefTableState replay(const std::optional<RefTableSnapshot> & snapshot, std::span<const RefLogTxn> tail);

/// Admission budget (spec §Snapshot Format): true iff applying `op` to a COPY of `state` (via the same
/// per-operation validator `applyRefLogTxn` uses -- an `op` that is not itself a legal transition
/// throws exactly as `applyRefLogTxn` would, since `admits` answers "would this legal op still fit
/// the budget", not "is this op legal") keeps BOTH the resulting table snapshot and the resulting
/// hypothetical complete-removal transaction within their respective byte budgets.
///
/// `RefTableState` carries no `ns` (it is per-table but not one of this struct's fields), so both
/// hypothetical encodings are measured with an empty `ns`. `ns` is constant for one table for its
/// entire lifetime, so a caller computes its own table's `4 + ns.size()` overhead once (it is repeated
/// exactly once in a snapshot body and once in a removal-transaction body -- see `CasRefSnapshotCodec`
/// / `CasRefLogCodec`'s wire layout) and pre-subtracts it, together with its own safety margin, from
/// the raw `ref_snapshot_max_bytes` / `ref_removal_max_bytes` hard limits before calling `admits`.
///
/// Implementation choice: sizes are computed non-incrementally, by literally building the hypothetical
/// post-state's snapshot and removal transaction and calling `encodeRefTableSnapshot` /
/// `encodeRefLogTxn` on them, rather than maintaining a separate incremental byte estimate. This can
/// never drift from what those encoders actually produce (there is nothing to keep in sync), at the
/// cost of doing real encode work per candidate operation -- acceptable per spec §Snapshot Format,
/// since the state machine is explicitly not the hot path for Phase 1.
bool admits(const RefTableState & state, const RefOp & op, uint64_t snapshot_budget, uint64_t removal_budget);

}
