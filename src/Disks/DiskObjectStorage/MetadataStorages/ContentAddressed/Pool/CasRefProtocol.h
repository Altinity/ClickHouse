#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowManifestSet.h>
#include <base/types.h>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace DB::Cas
{

// Shared value types for the ref-ledger writer and the pure ref-log protocol helpers below. They live
// in this protocol header so the ledger can depend on the carriers without making the pool and ledger
// headers include one another.
/// Whether a root-shard mutation originates from the writer path (user-visible publish/drop/precommit)
/// or from GC/maintenance. Diagnostic-only (`toString`, event logging): recorded on the mutation item.
enum class RootMutationOrigin : uint8_t
{
    Writer,
    Gc,
};

/// The write-scope of one `appendRefOps` call (ref-append-lane batching): which part of the table the
/// call touches. The flat-combining batch builder admits at most ONE mutation per ref name into a
/// single flush (per-ref durable histories stay bit-identical to the unbatched protocol) and flushes
/// `WholeShard` calls SOLO (dropNamespace and anything touching multiple refs wholesale).
struct MutationScope
{
    enum class Kind : uint8_t { Ref, WholeShard };
    Kind kind = Kind::WholeShard;
    String ref_name;   /// set iff kind == Ref

    /// Creates a scope for a mutation that touches exactly one ref name. The name is moved into the
    /// scope because scopes are normally assembled as part of an append request.
    static MutationScope ref(String name) { return {Kind::Ref, std::move(name)}; }

    /// Creates a scope for a mutation that cannot safely share a batch with per-ref mutations.
    static MutationScope wholeShard() { return {Kind::WholeShard, {}}; }
};

/// Kind of mutation being applied, used in diagnostic logging and metrics. Does not affect behaviour.
enum class RootMutationKind : uint8_t
{
    Publish,
    Drop,
    Precommit,
    Promote,
    Abandon,
    UpdateRefPayload,
    DropNamespace,
    ReclaimPrecommit,
};

/// Human-readable name for `RootMutationOrigin` (diagnostic logging).
inline std::string_view toString(RootMutationOrigin origin)
{
    switch (origin)
    {
        case RootMutationOrigin::Writer: return "Writer";
        case RootMutationOrigin::Gc:     return "Gc";
    }
    return "Unknown";
}

/// Human-readable name for `RootMutationKind` (diagnostic logging).
inline std::string_view toString(RootMutationKind kind)
{
    switch (kind)
    {
        case RootMutationKind::Publish:           return "Publish";
        case RootMutationKind::Drop:              return "Drop";
        case RootMutationKind::Precommit:         return "Precommit";
        case RootMutationKind::Promote:           return "Promote";
        case RootMutationKind::Abandon:           return "Abandon";
        case RootMutationKind::UpdateRefPayload:  return "UpdateRefPayload";
        case RootMutationKind::DropNamespace:     return "DropNamespace";
        case RootMutationKind::ReclaimPrecommit:  return "ReclaimPrecommit";
    }
    return "Unknown";
}

/// The result of resolving a ref: its namespace-qualified manifest identity, the manifest size, and
/// the publication timestamp carried by the ref. A `Resolved` value does not own the manifest body.
struct Resolved
{
    /// The namespace-qualified identity of the part manifest this ref names. The owning RootNamespace
    /// + the ref's manifest_ref form the ManifestId (the ref carries no namespace itself — that comes
    /// from the owning root context).
    ManifestId manifest_id;
    uint64_t manifest_size = 0;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};

/// The non-identity portion of a committed-ref update. The ref's manifest identity is deliberately
/// absent: changing reachability must use an owner transition, while this carrier is only for updating
/// the publication timestamp (and the legacy opaque payload bytes) without changing the manifest edge.
/// In the current all-tree representation, per-part files are ordinary manifest entries rather than a
/// separate mutable-file map, so `published_at_ms` is the remaining metadata that can be restamped in
/// isolation.
struct RefPayloadUpdate
{
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};


/// Counts the committed refs and precommit bindings named by one namespace-removal transaction. The
/// transaction contains one exact owner-removal operation for each count before its final
/// `remove_namespace` operation; callers interested only in completion may ignore this summary.
struct DropNamespaceStats
{
    uint64_t committed_refs = 0;
    uint64_t precommits = 0;
};

/// Per-owner configuration passed by value to the ref ledger. It is a projection of the flat pool
/// configuration: `server_root_id` is used in ref-lane diagnostics, while boot time and wait-sleep
/// callbacks remain owned by the pool and are supplied separately because they describe live mount
/// state rather than ref-ledger policy.
struct RefLedgerConfig
{
    String server_root_id;
    uint64_t snapshot_log_count_threshold = 256;
    uint64_t snapshot_log_bytes_threshold = 1ULL << 20;
    uint64_t snapshot_publish_backoff_initial_ms = 200;
    uint64_t snapshot_publish_backoff_max_ms = 30000;
    uint64_t precommit_sweep_backoff_initial_ms = 200;
    uint64_t precommit_sweep_backoff_max_ms = 30000;
    uint64_t ref_table_cache_bytes = 256ULL << 20;
};

/// How `applyRefLogTxn` treats a call: how much admission re-checking it performs, AND -- welded to
/// that, not a separate axis -- what it does to `state` on a mid-transaction throw. The two concerns
/// always co-occur because both are derived from one caller intent. `LiveAppend` means "this is the
/// FIRST time this transaction is being validated, against a state that must survive a rejection": the
/// writer's append-time contract and every trial/shape-check preview. It re-checks every precondition,
/// including the cross-owner uniqueness check (O(1) via `owned_manifests` since E2), and applies
/// two-phase against a scratch copy so
/// `state` is byte-for-byte unchanged on any throw. `TrustedReplay` means "I am replaying
/// already-committed, already-validated history into a local state I own and discard on any error":
/// `replay`'s tail, and only `replay`'s tail (recovery, GC fold, fsck, protection views all inherit it
/// through `replay`). Because the transaction already passed `LiveAppend` validation when it was
/// durably appended, the cross-owner re-check is elided in release builds and kept as a `chassert` in
/// debug/sanitizer builds (same policy as `debugAssertBodyCounters`) -- every exact-binding
/// precondition (cheap, keyed) is still enforced in BOTH modes, so a corrupted log object still fails
/// closed either way. And because the state is thrown away on any error, `TrustedReplay` applies IN
/// PLACE with no scratch copy (E3 -- eliminates the per-transaction deep-copy of the replay tail's
/// unbounded COW overlays); a throw leaves `state` PARTIALLY APPLIED ("poisoned"), which is sound only
/// because its sole caller discards it on any throw. Welding the two axes into one enum keeps the
/// dangerous fourth combination -- trusted validation applied to a state that must survive a throw --
/// inexpressible. Do not pass `TrustedReplay` for a state that must survive a rejection.
enum class ApplyMode : uint8_t { LiveAppend, TrustedReplay };

/// The in-memory table state: `TableState = Replay(S_X.state, tail(X))`. This class, `applyRefLogTxn`, `snapshotOf`, and
/// `replay` are the ONE shared implementation of that equation -- used verbatim by the writer, its
/// own recovery path, `fsck`, and snapshot construction, so every consumer agrees on what a
/// transaction sequence means. "Namespace" and "table" name the same entity throughout this file
/// (and its callers): one `RefTableState` per `RootNamespace`.
///
/// Representation note (never-born vs removed): there is no separate "first birth" flag. Both
/// "never born" and "Removed" default `lifecycle` to `RefLifecycle::Removed`; they are told apart by
/// `remove_txn_id`: absent means the namespace has never completed a `remove_namespace` transaction
/// (either truly never born, or -- from this class's point of view -- indistinguishable from it,
/// which is fine because a `namespace_birth` op is legal from EITHER case and nothing else is legal
/// from either). Present means a real removal happened and recorded its `RefTxnId`. `committed` and
/// `precommits` are always empty while `lifecycle == Removed` (an invariant `applyRefLogTxn`
/// maintains: `remove_namespace` only fires once both are already empty, and no other operation is
/// legal until the next `namespace_birth`).
class RefTableState
{
public:
    RefTableState() = default;

    RefLifecycle getLifecycle() const { return lifecycle; }
    const std::optional<RefTxnId> & getRemoveTxnId() const { return remove_txn_id; }
    const RefTxnId & getGreatestApplied() const { return greatest_applied; }
    const RefCowMap & getCommitted() const { return committed; }
    const std::set<std::pair<String, ManifestRef>> & getPrecommits() const { return precommits; }
    uint64_t getSnapshotBodyBytes() const { return snapshot_body_bytes; }
    uint64_t getRemovalBodyBytes() const { return removal_body_bytes; }

    /// State-install point only (once per ref-log flush, never per batch item): folds the committed
    /// map's and the owned-manifest index's COW overlays into a fresh shared base each.
    void materializeCommitted() { committed.materialize(); owned_manifests.materialize(); }

private:
    RefLifecycle lifecycle = RefLifecycle::Removed;   /// see representation note above
    std::optional<RefTxnId> remove_txn_id;
    RefTxnId greatest_applied{};                       /// {0, 0} = no transaction applied yet

    RefCowMap committed;                                              /// keyed by ref_name
    std::set<std::pair<String, ManifestRef>> precommits;             /// (ref_name, manifest_ref)

    /// COW membership index of every `ManifestRef` with a current owner (a `committed` row or a
    /// `precommits` binding), maintained O(1) per applied op by every arm of `applyOwnerTransition`
    /// and `stateFromSnapshot` that changes ownership. Gives `manifestAlreadyOwned` O(1) instead of
    /// a linear scan over `committed` + `precommits`. See Pool/CasRefCowManifestSet.h.
    RefCowManifestSet owned_manifests;

    /// Running byte totals of the two admission-budget encodings' *bodies* (row/op lines only, no
    /// header/meta/trailer framing), maintained O(1) per applied op by `applyOp` and seeded by
    /// `stateFromSnapshot`. A pure function of `(committed, precommits)`: `admits` reads
    /// `framing + total` instead of re-encoding the whole table. See `admits`'s doc for why this is
    /// byte-exact rather than a drift-prone estimate.
    uint64_t snapshot_body_bytes = 0;   /// Σ committedRowEncodedSize + Σ precommitRowEncodedSize
    uint64_t removal_body_bytes  = 0;   /// Σ removalOpEncodedSize(one per committed + one per precommit)

    /// One operation's local preconditions and effect, shared by `applyRefLogTxn`'s per-op loop and by
    /// `admits`'s single-op preview. `txn_id` is only read by `RemoveNamespace` (it becomes the
    /// resulting `remove_txn_id`). `validation` is threaded down to `applyOwnerTransition`, the only
    /// arm that consults it. Was free `applyOpInPlace`.
    void applyOp(const RefOp & op, const RefTxnId & txn_id, ApplyMode validation);

    /// The `owner_transition` op kind: dispatches on the `(old_binding, new_binding)` shape to one of
    /// the four legal transitions (add precommit / remove precommit / remove committed / promote). Any
    /// other shape is not a recognized transition. `validation` is consulted only by the add-precommit
    /// arm's cross-owner uniqueness check. Was free.
    void applyOwnerTransition(const RefOp & op, ApplyMode validation);

    /// The `set_payload` op kind: the committed ref must still name `expected_manifest_ref`; replaces
    /// the opaque `payload` blob and `published_at_ms` without touching the manifest edge. Was free.
    void applySetPayload(const RefOp & op);

    /// True iff `manifest_ref` already names an existing committed row or precommit binding under ANY
    /// ref_name (the add-precommit rule: "no conflicting owner may name the same manifest"). Was free.
    bool manifestAlreadyOwned(const ManifestRef & manifest_ref) const;

#ifdef DEBUG_OR_SANITIZER_BUILD
    /// Debug/sanitizer-only: recompute both body totals from scratch and assert the incrementally
    /// maintained values match. Was free.
    void debugAssertBodyCounters() const;
#endif

    friend void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn, ApplyMode validation);
    friend RefTableState stateFromSnapshot(const RefTableSnapshot & snapshot);
    friend bool admits(const RefTableState & state, const RefOp & op,
                       uint64_t snapshot_budget, uint64_t removal_budget);
};

/// The inverse of `snapshotOf`: state from a snapshot's rows. `replay` may receive a hand-built
/// `RefTableSnapshot` that never passed through `decodeRefTableSnapshot`, so this round-trips it
/// through the codec's own `encodeRefTableSnapshot`/`decodeRefTableSnapshot` rather than
/// re-implementing a second, independently-maintained copy of its validation (sortedness, no
/// duplicates, canonical names, nonzero ids, `manifest_ref` field validity, lifecycle/remove_txn_id
/// coupling) that could silently miss a case. Concretely: a hand-built snapshot with two committed
/// rows sharing one `ref_name` would otherwise DROP the second row via `RefCowMap::emplace` below
/// (same no-overwrite-on-existing-key semantics as `std::map::emplace`) -- the same phantom-alive
/// class of bug as a promote's silent displacement (see `applyOwnerTransition` above), just reached
/// through snapshot loading instead of a transaction.
///
/// Promoted from `CasRefProtocol.cpp`'s anonymous namespace to the public protocol API: the ONE
/// validated way to construct a state from rows -- tests and benchmarks use it instead of poking
/// fields.
RefTableState stateFromSnapshot(const RefTableSnapshot & snapshot);

/// Applies the COMPLETE transaction to `state`, or throws `CORRUPTED_DATA` (each transition shape
/// below has exactly one precondition enforced here). The two txn-wide preconditions
/// (strictly-increasing `txn_id`, `remove_namespace` ordering) are checked before any mutation, so
/// they never leave `state` half-applied under either strategy below.
///
/// Apply strategy is selected by `validation` (see `ApplyMode`):
///  - `LiveAppend` (writer append-time + previews): two-phase against a scratch copy; every op is validated
///    and applied, in array order, to the scratch first, and `state` is replaced by it only once the
///    WHOLE transaction has succeeded. A throw anywhere leaves `state` byte-for-byte unchanged, and no
///    intra-transaction intermediate state (e.g. a manifest with its precommit already gone but its
///    committed binding not yet installed) is ever observable to a caller -- matching the promote
///    rule: "There is no moment at which the manifest has no owner."
///  - `TrustedReplay` (replay only): applies IN PLACE with no scratch copy (E3 -- eliminates the
///    per-transaction deep-copy of the replay tail's unbounded COW overlays). A throw leaves `state`
///    PARTIALLY APPLIED; this is sound only because `replay`, its sole caller, discards the state on
///    any throw (its result reaches a caller only on full success). See `ApplyMode`'s doc.
///
/// Enforced preconditions:
///  - `txn.txn_id` must be strictly greater than `state.greatest_applied`
///    ("only strict increase is required").
///  - `remove_namespace`, if present, must be the transaction's FINAL operation, and every earlier
///    operation must be an exact owner-removal `owner_transition` (`old_binding` set, `new_binding`
///    empty). The codec does not check this shape; this is the one place
///    that does.
///  - `namespace_birth`: legal only while `lifecycle != Live` (never-born or `Removed`). (Gating
///    recreation on the `_cleanup` marker for a completed removal is the writer's recovery-time
///    responsibility since the marker is not part of `RefTableState`.)
///  - `owner_transition` add (no `old_binding`, `new_binding.kind == Precommit`): namespace must be
///    `Live`; the exact `(ref_name, manifest_ref)` pair must be absent from `precommits`; AND no
///    existing committed row or precommit binding, under ANY ref_name, may already name the same
///    `manifest_ref` ("no conflicting owner may name the same manifest" -- this
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
///    manifest-edge delta is read off each transaction's explicit
///    ops, not a before/after state diff, so a silent displacement would never emit the evicted
///    manifest's "-1" edge -- it would leak as phantom-alive forever. On success the precommit is
///    replaced by a committed row with an EMPTY payload (the initial payload arrives
///    via a separate `set_payload` op, in the same transaction or a later one).
///  - Any other `old_binding`/`new_binding` combination is not a recognized transition shape.
///  - `set_payload`: namespace must be `Live`; the committed ref named by `ref_name` must still name
///    `expected_manifest_ref`.
///  - `remove_namespace`: namespace must be `Live` and both `committed` and `precommits` must already
///    be empty at this point in the (in-array-order) replay -- which is only true if the transaction's
///    earlier removal ops actually named every owner.
///  - Any operation other than `namespace_birth` while `lifecycle == Removed` is rejected:
///    "Any operation other than a valid later namespace_birth while state is
///    Removed is corruption."
///
/// `CORRUPTED_DATA` throughout: every rejection above uses the same "is corruption" framing,
/// extended uniformly to every precondition in this section, matching how
/// `CasRefLogCodec`/`CasRefSnapshotCodec` already use `CORRUPTED_DATA` for "this data does not
/// correspond to a valid state" one layer down (wire shape rather than transition legality). Recovery
/// and `fsck` -- the primary callers replaying persisted logs -- want exactly that fail-closed
/// framing; a writer that wants a friendlier user-facing rejection for an ordinary attempted mutation
/// (e.g. "ref already exists") checks its own business state before ever building the op.
///
/// `validation` (default `ApplyMode::LiveAppend`, the writer's append-time contract): pass
/// `ApplyMode::TrustedReplay` only when `txn` already passed `LiveAppend` validation at the time it was
/// durably appended -- `replay`'s tail is the one caller that does. Every OTHER caller (the writer's
/// own trial/shape-check previews and its post-PUT state install, all in `CasRefLedger.cpp`) keeps the
/// default `LiveAppend`, because those calls are the FIRST time the transaction is validated, not a replay of
/// already-validated history.
void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn, ApplyMode validation = ApplyMode::LiveAppend);

/// The canonical snapshot of `state` under `ns`: `committed` sorted by
/// bytewise `ref_name` (guaranteed by `RefCowMap`'s sorted merge-iteration order,
/// `Pool/CasRefCowMap.h` -- the same ordering `std::map<String, ...>` gave before it, by design)
/// and `precommits` sorted by `(ref_name, manifest_ref)` (guaranteed by
/// `std::set<std::pair<String, ManifestRef>>`'s iteration order, since `ManifestRef::operator<`
/// matches the tuple order `CasRefSnapshotCodec` itself sorts by). `snapshot_id` is
/// `state.greatest_applied`; a `Removed` state produces zero rows plus `remove_txn_id`, per spec.
/// Does not itself enforce that the result is encodable (a never-born state's `snapshot_id` is
/// `{0, 0}`, which `encodeRefTableSnapshot` already rejects) -- that check already lives in the
/// codec and need not be duplicated here.
RefTableSnapshot snapshotOf(const RefTableState & state, const String & ns);

/// `TableState = Replay(S_X.state, tail(X))` in one call: starts from `snapshot`
/// (or the empty/never-born state when absent) and applies every transaction in `tail`, in order, via
/// `applyRefLogTxn`. A given `snapshot` is revalidated in full -- sortedness, no duplicates, canonical
/// names, nonzero ids, `manifest_ref` field validity, and lifecycle/remove_txn_id/row-emptiness
/// coupling, i.e. everything `CasRefSnapshotCodec` already enforces -- because `replay` may be handed
/// a hand-built `RefTableSnapshot` that never passed through `decodeRefTableSnapshot` (`fsck`, most
/// notably). Every entry of `tail` must also share one `ns` -- with `snapshot`'s `ns` when a snapshot
/// is given, otherwise with each other. A mismatch (of either kind) throws `CORRUPTED_DATA`: silently
/// accepting a malformed snapshot or replaying transactions from the wrong table would produce a
/// wrong-but-plausible-looking state, exactly the class of bug this equation exists to make
/// impossible.
RefTableState replay(const std::optional<RefTableSnapshot> & snapshot, std::span<const RefLogTxn> tail);

/// The exact encoded size of `state`'s canonical snapshot (`encodeRefTableSnapshot(snapshotOf(state,
/// "")).size()`), computed in O(1) from the running body counter plus O(1) framing instead of a full
/// re-encode. Used by `admits` and directly property-tested against the real encoder.
uint64_t encodedSnapshotBudgetSize(const RefTableState & state);

/// The exact encoded size of `state`'s hypothetical whole-namespace removal transaction, computed in
/// O(1) from the running body counter plus O(1) framing. Used by `admits`.
uint64_t encodedRemovalBudgetSize(const RefTableState & state);

/// Admission budget: true iff applying `op` to a COPY of `state` (via the same
/// per-operation validator `applyRefLogTxn` uses -- an `op` that is not itself a legal transition
/// throws exactly as `applyRefLogTxn` would, since `admits` answers "would this legal op still fit
/// the budget", not "is this op legal") keeps BOTH the resulting table snapshot and the resulting
/// hypothetical complete-removal transaction within their respective byte budgets.
///
/// `RefTableState` carries no `ns` (it is per-table but not one of this class's fields), so both
/// hypothetical encodings are measured with an empty `ns`. `ns` is constant for one table for its
/// entire lifetime, so a caller computes its own table's `ns.size()` overhead once (the wire layout's
/// `u32` length prefix itself is present in BOTH the empty-`ns` measurement here and the real encoding,
/// so it cancels -- only the `ns` bytes themselves are the delta; repeated exactly once in a snapshot
/// body and once in a removal-transaction body, see `CasRefSnapshotCodec` / `CasRefLogCodec`'s wire
/// layout) and pre-subtracts it, together with its own safety margin, from the raw
/// `ref_snapshot_max_bytes` / `ref_removal_max_bytes` hard limits before calling `admits`.
///
/// Implementation: sizes are computed incrementally. `RefTableState` carries running body-byte totals
/// (`snapshot_body_bytes` / `removal_body_bytes`) maintained O(1) per applied op by `RefTableState::applyOp`;
/// `admits` applies `op` to a scratch copy and reads `framing + total` via `encodedSnapshotBudgetSize`
/// / `encodedRemovalBudgetSize`, making the whole check O(touched rows) instead of O(table size). This
/// is byte-exact rather than a drift-prone estimate: both budget encodings are pure per-row sums, the
/// per-row contributions come from the same codec primitives the full encoders use, and a
/// debug/sanitizer-only recompute-and-compare `chassert` (`RefTableState::debugAssertBodyCounters`)
/// reasserts equality on every applied transaction and every `admits` preview.
bool admits(const RefTableState & state, const RefOp & op, uint64_t snapshot_budget, uint64_t removal_budget);

/// Pure ref-log intake primitives for a GC round. None of these read a
/// manifest body, a snapshot body, or `gc/state`: they turn a global `LIST cas/refs/` result and the
/// decoded bodies of new transactions into (a) the per-table log/snapshot/marker listing, (b) the
/// deterministic manifest-edge delta, and (c) the exact ref-object cleanup plan. The GC round
/// (`CasGc.cpp`) drives the manifest-body reads (`foldManifestEdges`), the fold barrier, the durable
/// cursor, and the batch deletions around these functions. Keeping the delta and cleanup logic pure
/// makes it directly unit-testable (`gtest_cas_ref_intake.cpp`) without a full round.

/// One `+1`/`-1` manifest edge emitted by one ref-log operation.
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

/// The manifest edges of ONE decoded transaction, in operation order, reading NO manifest body.
/// Rules per operation:
///   - `owner_transition` with only a new binding (add owner)            => `+1` for `new.manifest_ref`
///   - `owner_transition` with only an old binding (remove owner)        => `-1` for `old.manifest_ref`
///   - `owner_transition` old+new naming the SAME manifest (promote)     => no edge (net zero)
///   - `owner_transition` old+new naming DIFFERENT manifests (replace)   => `-1` old then `+1` new
///   - `owner_transition` with neither binding                          => no edge (degenerate; a
///     transition shape the writer never produces and `applyRefLogTxn` rejects at replay)
///   - `namespace_birth` / `set_payload` / `remove_namespace`           => no edge
/// The `remove_namespace` operation changes lifecycle only; the exact owner removals that must precede
/// it in the same transaction already emit their own `-1` edges.
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
/// `{namespace, remove_txn_id}` namespace-cleanup item.
std::optional<RefTxnId> removalTxnId(const RefLogTxn & txn);

/// One table's surviving ref-object keys from this round's global `LIST`, split by kind and sorted
/// ascending. `_cleanup` markers are consulted only by the writer's recreation gate;
/// GC groups them so a later step can decide marker republication without a second scan.
struct RefTableListing
{
    std::vector<RefTxnId> logs;
    std::vector<RefTxnId> snapshots;
    std::vector<RefTxnId> cleanup_markers;

    bool operator==(const RefTableListing &) const = default;
};

/// Parse and group a global `LIST` of keys under `layout.casRefsPrefix()` by table. Every
/// key is expected to be one of the three ref-object kinds; a key under the ref prefix that
/// `Layout::parseRefObjectKey` does not recognize, or whose reconstructed namespace is malformed
/// (`parseRefObjectKey` does not re-validate the namespace, so this does), is a malformed
/// ref key and throws `CORRUPTED_DATA` -- the round catches it and aborts ref folding for the round
/// (a malformed key cannot produce a partial ref delta or authorize destructive work).
/// A key OUTSIDE `casRefsPrefix()` is ignored: the caller lists only the ref prefix, and a foreign key
/// is not this format's concern.
std::map<String, RefTableListing> groupRefKeys(const Layout & layout, const std::vector<String> & listed_keys);

/// The exact ref objects one round may delete for one table.
/// Pure; acts only on keys THIS round's scan returned, so a covering snapshot is always
/// durable before any deletion it authorizes. A log `L` is deletable iff:
///   1. the newest observed snapshot `X` covers it (`L <= X`);
///   2. the durable `last_folded_ref_id` covers it (`L <= durable_cursor`);
///   3. it is not in `removal_logs_blocked` (a `remove_namespace` log whose namespace-cleanup item has
///      not durably reached `Completed`).
/// Snapshots with id `< X` are deletable. With no observed snapshot, `X` is undefined and no log is
/// coverage-deletable (an empty plan). The newest snapshot `X` itself is retained.
///
/// `completed_removal_snapshot` (optional) is the identifier of a `Removed` snapshot that the caller has
/// already made durable THIS round via `putIfAbsent` for a namespace-cleanup item that reached
/// `Completed`. It participates in `X` as a covering
/// snapshot exactly as if the round's scan had returned it -- the caller guarantees it is durable before
/// any delete this plan authorizes, preserving the "covering snapshot durable before deletion" invariant.
/// It is never itself scheduled for deletion (it is not in `listing.snapshots` on the round it is first
/// published; on a later round it appears in `listing.snapshots` as the newest and is retained there).
struct RefCleanupPlan
{
    std::vector<RefTxnId> deletable_logs;
    std::vector<RefTxnId> deletable_snapshots;

    bool operator==(const RefCleanupPlan &) const = default;
};
RefCleanupPlan planRefCleanup(const RefTableListing & listing, const RefTxnId & durable_cursor,
                              const std::set<RefTxnId> & removal_logs_blocked,
                              std::optional<RefTxnId> completed_removal_snapshot = std::nullopt);

/// The result of `recoverRefTableDetailed`: the replayed table state plus the two facts the late-log
/// detector needs about the snapshot recovery actually selected: its id and, when it is a
/// recovery seal, the `sealed_from` upper bound of what that recovery's `LIST` actually observed. Both
/// are `nullopt` exactly when recovery found no snapshot at all; `sealed_from` alone is `nullopt`
/// whenever the selected snapshot is an ordinary published one (only a seal ever sets it).
struct RecoveredRefTable
{
    RefTableState state;
    std::optional<RefTxnId> newest_snapshot_id;
    std::optional<RefTxnId> sealed_from;
};

/// Recover one table's state via the shared recovery equation:
/// one `LIST` of the table prefix, the newest valid snapshot, and replay of the later log tail through the
/// same state machine the writer uses. This is the read-only recovery `fsck`, offline repair, and GC's
/// disaster-recovery rebuild all consume, so every consumer agrees on what a table's ref state is.
/// Restart-on-vanish: a selected snapshot or tail body deleted between the `LIST` and its `GET` (a
/// concurrent GC cleanup published a covering newer snapshot) is not corruption -- recovery restarts with a
/// fresh `LIST`, bounded by `max_restarts`, after which the vanish becomes a `CORRUPTED_DATA` throw. A
/// never-touched namespace yields the empty (never-born) state. Different bytes under a deterministic key,
/// or an invalid body, are corruption and throw immediately.
///
/// `on_page_fetched`, if set, fires once per physical `backend.list` page across every restart attempt --
/// a GC-owned caller's hook for a page-level ProfileEvents counter; fsck/offline-repair callers leave it
/// unset.
RecoveredRefTable recoverRefTableDetailed(Backend & backend, const Layout & layout, const RootNamespace & ns,
                                          const std::function<void()> & on_page_fetched = {},
                                          unsigned max_restarts = 3);

/// Thin wrapper over `recoverRefTableDetailed` for the consumers that only need the replayed state
/// (fsck, offline repair, the writer's own recovery-on-open, and the GC fold's owner-set builder).
RefTableState recoverRefTable(Backend & backend, const Layout & layout, const RootNamespace & ns,
                              const std::function<void()> & on_page_fetched = {},
                              unsigned max_restarts = 3);

}
