#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <Common/scope_guard_safe.h>
#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

/// Txn-wide structural check, NOT a per-op precondition: if `ops` contains a
/// `RemoveNamespace`, it must be the last element, and every earlier op must be an exact
/// owner-removal `owner_transition` (`old_binding` set, `new_binding` empty). `CasRefLogCodec`
/// deliberately does not check this shape -- this is the one place that does.
void checkRemoveNamespaceOrdering(const std::vector<RefOp> & ops)
{
    const bool has_remove = std::any_of(ops.begin(), ops.end(),
        [](const RefOp & op) { return op.kind == RefOpKind::RemoveNamespace; });
    if (!has_remove)
        return;

    if (ops.empty() || ops.back().kind != RefOpKind::RemoveNamespace)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: remove_namespace must be the final operation of its transaction");

    for (size_t i = 0; i + 1 < ops.size(); ++i)
    {
        const RefOp & op = ops[i];
        const bool pure_removal = op.kind == RefOpKind::OwnerTransition
            && op.old_binding.has_value() && !op.new_binding.has_value();
        if (!pure_removal)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState: every operation before remove_namespace must be an exact owner removal");
    }
}

/// Installed test probe for the streaming-recovery memory invariant (see the header). Guarded by its
/// own mutex so an install/clear from a test thread cannot tear against a concurrent recovery.
std::mutex g_recovery_replay_memory_probe_mutex;
std::function<void(int64_t)> g_recovery_replay_memory_probe;

/// The four legal `owner_transition` shapes, decided purely from the (old_binding, new_binding)
/// optionals and their `RefOwnerKind`s -- no state read. `RefTableState::applyOwnerTransition` (the
/// writer/replay state machine) and `manifestEdgesOfTxn` (the GC fold's edge extractor) both switch
/// over this single classification instead of each carrying their own shape predicates, so a shape
/// neither consumer recognizes cannot silently acquire divergent meaning in one of them.
enum class OwnerTransitionShape : uint8_t
{
    AddPrecommit,      /// no old_binding, new_binding.kind == Precommit
    RemovePrecommit,   /// old_binding.kind == Precommit, no new_binding
    RemoveCommitted,   /// old_binding.kind == Committed, no new_binding
    Promote,           /// old_binding.kind == Precommit, new_binding.kind == Committed, SAME ref_name
                       /// and manifest_ref
};

/// Classify `op`'s (old_binding, new_binding) shape into one of the four legal transitions. Anything
/// else -- neither binding, old+new naming DIFFERENT manifests, a promote whose old/new ref_name
/// disagree, or any other kind combination -- throws `CORRUPTED_DATA` naming the offending combination
/// instead of falling through to a caller that would otherwise assign it accidental meaning.
[[nodiscard]] OwnerTransitionShape classifyOwnerTransitionShape(const RefOp & op)
{
    const bool has_old = op.old_binding.has_value();
    const bool has_new = op.new_binding.has_value();

    if (!has_old && has_new && op.new_binding->kind == RefOwnerKind::Precommit)
        return OwnerTransitionShape::AddPrecommit;

    if (has_old && !has_new && op.old_binding->kind == RefOwnerKind::Precommit)
        return OwnerTransitionShape::RemovePrecommit;

    if (has_old && !has_new && op.old_binding->kind == RefOwnerKind::Committed)
        return OwnerTransitionShape::RemoveCommitted;

    if (has_old && has_new && op.old_binding->kind == RefOwnerKind::Precommit
        && op.new_binding->kind == RefOwnerKind::Committed
        && op.old_binding->ref_name == op.new_binding->ref_name
        && op.old_binding->manifest_ref == op.new_binding->manifest_ref)
        return OwnerTransitionShape::Promote;

    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "owner_transition does not match any legal transition shape (has_old={}, old_kind={}, "
        "has_new={}, new_kind={})",
        has_old, has_old ? std::to_string(static_cast<uint8_t>(op.old_binding->kind)) : "n/a",
        has_new, has_new ? std::to_string(static_cast<uint8_t>(op.new_binding->kind)) : "n/a");
}

}

void setRecoveryReplayMemoryProbeForTest(std::function<void(int64_t delta_footprint_bytes)> probe)
{
    std::lock_guard lock(g_recovery_replay_memory_probe_mutex);
    g_recovery_replay_memory_probe = std::move(probe);
}

void reportReplayMemoryDelta(int64_t delta_footprint_bytes)
{
    /// Reads the installed probe under the mutex and calls it OUTSIDE the lock so the probe body may
    /// itself do arbitrary work. A no-op in production (no probe installed).
    std::function<void(int64_t)> probe;
    {
        std::lock_guard lock(g_recovery_replay_memory_probe_mutex);
        probe = g_recovery_replay_memory_probe;
    }
    if (probe)
        probe(delta_footprint_bytes);
}

uint64_t decodedRefLogTxnFootprint(const RefLogTxn & txn)
{
    /// A deterministic proxy for the heap a decoded transaction keeps alive: the ns string, the op
    /// vector's element storage (ops count x per-op record size), and every owned ref-name string. Uses
    /// `size()` (not `capacity()`) so the value depends only on the decoded content, hence is identical
    /// across a streaming decode and a materialising control's decode of the same object.
    uint64_t bytes = txn.ns.size() + txn.ops.size() * sizeof(RefOp);
    for (const RefOp & op : txn.ops)
    {
        bytes += op.ref_name.size();
        if (op.old_binding)
            bytes += op.old_binding->ref_name.size();
        if (op.new_binding)
            bytes += op.new_binding->ref_name.size();
    }
    return bytes;
}

/// Member-wise swap; see the header for the install-region contract it exists for. Every member is
/// enumerated here by hand rather than swapped through a generated move, so a member added to the
/// class without a line here is a silent state-corruption bug -- the `static_assert`s below are the
/// type-level half of the guarantee (the macro at the call site proves the code path, these prove the
/// contract of the types), and `debugAssertBodyCounters` cross-checks the counters this swap carries.
void RefTableState::swap(RefTableState & other) noexcept
{
    static_assert(std::is_nothrow_swappable_v<RefLifecycle>);
    static_assert(std::is_nothrow_swappable_v<std::optional<RefTxnId>>);
    static_assert(std::is_nothrow_swappable_v<RefTxnId>);
    static_assert(std::is_nothrow_swappable_v<std::set<std::pair<String, ManifestRef>>>);
    static_assert(std::is_nothrow_swappable_v<uint64_t>);
    static_assert(noexcept(std::declval<RefCowMap &>().swap(std::declval<RefCowMap &>())));
    static_assert(noexcept(std::declval<RefCowManifestSet &>().swap(std::declval<RefCowManifestSet &>())));

    using std::swap;
    swap(lifecycle, other.lifecycle);
    swap(remove_txn_id, other.remove_txn_id);
    swap(greatest_applied, other.greatest_applied);
    committed.swap(other.committed);
    precommits.swap(other.precommits);
    owned_manifests.swap(other.owned_manifests);
    swap(snapshot_body_bytes, other.snapshot_body_bytes);
    swap(removal_body_bytes, other.removal_body_bytes);
}

/// True iff `manifest_ref` already names an existing committed row or precommit binding under ANY
/// ref_name (the add-precommit rule: "no conflicting owner may name the same manifest"). O(1) via
/// `owned_manifests`, a COW membership index (Pool/CasRefCowManifestSet.h) that every ownership-
/// changing arm below (and `stateFromSnapshot`) maintains in lock-step with `committed` and
/// `precommits`. The old linear scan lives on, in debug/sanitizer builds only, as
/// `debugAssertBodyCounters`'s cross-check that the index has not drifted from those two containers.
bool RefTableState::manifestAlreadyOwned(const ManifestRef & manifest_ref) const
{
    return owned_manifests.contains(manifest_ref);
}

/// The `owner_transition` op kind: dispatches on the `(old_binding,
/// new_binding)` shape to one of the four legal transitions (add precommit / remove precommit /
/// remove committed / promote). Any other shape is not a recognized transition.
void RefTableState::applyOwnerTransition(const RefOp & op)
{
    if (lifecycle != RefLifecycle::Live)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: owner_transition while namespace is not Live");

    /// Shape legality is decided once, by the shared classifier; everything below is the per-shape
    /// PRECONDITION check and effect, unchanged.
    switch (classifyOwnerTransitionShape(op))
    {
        /// Add precommit: no old_binding, a fresh Precommit new_binding.
        case OwnerTransitionShape::AddPrecommit:
        {
            const RefOwnerBinding & b = *op.new_binding;
            if (precommits.contains({b.ref_name, b.manifest_ref}))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: add precommit '{}' already exists for this exact manifest", b.ref_name);
            /// Cross-owner uniqueness runs UNCONDITIONALLY, in every apply strategy (writer append AND
            /// trusted replay). Since E2 this is an O(1) `owned_manifests` lookup, so the E1-era elision of
            /// it under trusted replay (which downgraded it to a debug-only `chassert`) bought nothing
            /// measurable while making a corrupted log/snapshot FAIL OPEN -- a double-owner input would drift
            /// the index and let ordinary later writes append invariant-violating durable history. Keeping it
            /// here is what makes replay fail CLOSED on a corrupted `manifest_ref` collision.
            if (manifestAlreadyOwned(b.manifest_ref))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: manifest already has a conflicting owner under another ref_name");
            precommits.emplace(b.ref_name, b.manifest_ref);
            snapshot_body_bytes += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
            removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
            owned_manifests.insert(b.manifest_ref);
            return;
        }

        /// Remove precommit: an exact Precommit old_binding, no new_binding.
        case OwnerTransitionShape::RemovePrecommit:
        {
            const RefOwnerBinding & b = *op.old_binding;
            if (precommits.erase({b.ref_name, b.manifest_ref}) == 0)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: exact precommit binding '{}' to remove is absent", b.ref_name);
            snapshot_body_bytes -= precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
            removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
            owned_manifests.erase(b.manifest_ref);
            return;
        }

        /// Remove committed ref: an exact Committed old_binding, no new_binding.
        case OwnerTransitionShape::RemoveCommitted:
        {
            const RefOwnerBinding & b = *op.old_binding;
            const auto it = committed.find(b.ref_name);
            if (it == committed.end() || !(it->second.manifest_ref == b.manifest_ref))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: exact committed binding '{}' to remove is absent", b.ref_name);
            const RefCommittedRow removed = it->second;
            committed.erase(it);
            snapshot_body_bytes -= committedRowEncodedSize(removed);
            removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Committed, removed.ref_name, removed.manifest_ref);
            owned_manifests.erase(removed.manifest_ref);
            return;
        }

        /// Promote: the SAME ref_name and manifest_ref move from Precommit to Committed
        /// in one atomic step; the resulting row's `published_at_ms` starts unset (installed by the
        /// companion set_published_at op in the same transaction, or a later one).
        case OwnerTransitionShape::Promote:
        {
            const RefOwnerBinding & b = *op.old_binding;
            if (precommits.erase({b.ref_name, b.manifest_ref}) == 0)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: exact precommit binding '{}' to promote is absent", b.ref_name);
            snapshot_body_bytes -= precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
            removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
            /// `owned_manifests` is deliberately left untouched by a promote: `b.manifest_ref` moves from
            /// precommit ownership to committed ownership without ever giving it up in between -- the
            /// same "there is no moment at which the manifest has no owner" invariant this function's
            /// header doc states for promote generally. The index tracks "does ANY owner currently name
            /// this manifest", not which kind, so an erase-then-insert pair here would be pure overhead.
            /// A DIFFERENT manifest already committed under this exact ref_name must be evicted by its
            /// own explicit owner_transition(old=Committed, new=None) first (an earlier op of this same
            /// transaction, or an earlier transaction) -- never silently here. `GC`'s manifest-edge delta
            /// is read off the transaction's explicit ops, not a
            /// before/after state diff; a promote that silently evicted a stale committed row would never
            /// emit that manifest's "-1" edge, leaking it as phantom-alive forever.
            if (committed.contains(b.ref_name))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: promote '{}' would silently displace a different already-committed "
                    "manifest -- remove it with an explicit owner_transition first", b.ref_name);
            RefCommittedRow row;
            row.ref_name = b.ref_name;
            row.manifest_ref = b.manifest_ref;
            snapshot_body_bytes += committedRowEncodedSize(row);
            removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
            committed.emplace(b.ref_name, std::move(row));
            return;
        }
    }
    /// Reachable only if a future `OwnerTransitionShape` enumerator is added without a matching `case`
    /// (mirrors `applyOp`'s exhaustive-switch-then-throw shape below) -- `-Wswitch`/`-Werror` catches that
    /// at compile time; this throw is the runtime backstop for builds without it.
    throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: unhandled owner_transition shape");
}

/// The `set_published_at` op kind: the committed ref must still name `expected_manifest_ref`; replaces
/// `published_at_ms` without touching the manifest edge.
void RefTableState::applySetPublishedAt(const RefOp & op)
{
    if (lifecycle != RefLifecycle::Live)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: set_published_at while namespace is not Live");

    const auto it = committed.find(op.ref_name);
    if (it == committed.end() || !(it->second.manifest_ref == op.expected_manifest_ref))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: set_published_at '{}' no longer names its expected_manifest_ref", op.ref_name);

    /// `RefCowMap`'s iterator is read-only (Pool/CasRefCowMap.h): a write always goes through
    /// `insert_or_assign`, never through the found iterator in place. Copy the row, apply the same
    /// field mutation the old in-place code did, and write the whole row back -- this IS the COW
    /// map's single-row copy-out, not a whole-table one.
    RefCommittedRow updated = it->second;
    const uint64_t old_row_bytes = committedRowEncodedSize(it->second);
    updated.published_at_ms = op.published_at_ms;
    snapshot_body_bytes -= old_row_bytes;
    snapshot_body_bytes += committedRowEncodedSize(updated);
    /// removal_body_bytes unchanged: set_published_at touches neither ref_name nor manifest_ref.
    committed.insert_or_assign(op.ref_name, std::move(updated));
}

/// One operation's local preconditions and effect, shared by
/// `applyRefLogTxn`'s per-op loop and by `admits`'s single-op preview. `txn_id` is only read by
/// `RemoveNamespace` (it becomes the resulting `remove_txn_id`). Validation is identical regardless of
/// which apply strategy reached here, so this takes no mode.
void RefTableState::applyOp(const RefOp & op, const RefTxnId & txn_id)
{
    switch (op.kind)
    {
        case RefOpKind::NamespaceBirth:
        {
            /// Namespace birth: legal from never-born or Removed, never from Live. Gating
            /// recreation on the `_cleanup` marker is the writer's recovery-time responsibility;
            /// the marker is not part of `RefTableState`.
            if (lifecycle == RefLifecycle::Live)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: namespace_birth while already Live");
            lifecycle = RefLifecycle::Live;
            remove_txn_id.reset();
            return;
        }
        case RefOpKind::OwnerTransition:
            applyOwnerTransition(op);
            return;
        case RefOpKind::SetPublishedAt:
            applySetPublishedAt(op);
            return;
        case RefOpKind::RemoveNamespace:
        {
            /// Remove namespace: requires Live and both owner sets already empty -- true only
            /// if this transaction's earlier ops (checked by `checkRemoveNamespaceOrdering`) actually
            /// named every owner that existed when the transaction started.
            if (lifecycle != RefLifecycle::Live)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: remove_namespace while not Live");
            if (!committed.empty() || !precommits.empty())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: remove_namespace with nonempty owner sets");
            /// `committed`/`precommits` empty implies `owned_manifests` empty too -- every entry in
            /// the index is put there by an ownership change to one of those two containers. A
            /// mismatch here means the index has drifted, not that this transaction is invalid.
            chassert(owned_manifests.size() == 0);
            lifecycle = RefLifecycle::Removed;
            remove_txn_id = txn_id;
            return;
        }
        case RefOpKind::EpochSeal:
            /// Stage A task 1 scope boundary: the codec now accepts `EpochSeal` (grammar +
            /// round trip only), but the apply layer's seal handling -- INV-2's contextual grammar
            /// and the slot-occupy integration -- is wired by a later task. NOT reachable from any
            /// writer in this build (nothing yet mints an `EpochSeal` op), but IS reachable from a
            /// foreign or hand-crafted `_log` object replayed through `RefTableState::replay` -- the
            /// decoder now accepts one, so a storage-controlled body can drive this switch. That is
            /// exactly why this throws `NOT_IMPLEMENTED` rather than `LOGICAL_ERROR`: the latter aborts
            /// under sanitizer/CI abort-on-logical-error settings, which would let storage-controlled
            /// input crash the server; this fails loudly without that hazard.
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "RefTableState: EpochSeal apply is not yet implemented");
    }
    /// Reachable only through a hand-corrupted RefOpKind (mirrors CasRefLogCodec.cpp's
    /// exhaustive-switch-then-throw shape); every named enumerator returns above.
    throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: unknown op kind {}", static_cast<uint8_t>(op.kind));
}

RefTableState stateFromSnapshot(const RefTableSnapshot & snapshot)
{
    const String bytes = encodeRefTableSnapshot(snapshot);
    const RefTableSnapshot validated = decodeRefTableSnapshot(bytes, snapshot.ns, snapshot.snapshot_id);

    RefTableState state;
    state.lifecycle = validated.lifecycle;
    state.remove_txn_id = validated.remove_txn_id;
    state.greatest_applied = validated.snapshot_id;
    /// The codec validated sortedness and no-duplicate ref_name/(ref_name, manifest_ref), but NEVER
    /// cross-owner `manifest_ref` uniqueness -- a snapshot naming one manifest under two owners
    /// (committed/committed, committed/precommit, or precommit/precommit) is semantically corrupt and
    /// this is the one place that rejects it. The check runs before each `owned_manifests.insert` so it
    /// reports "corrupt snapshot data" rather than the container's "index drifted = code bug" framing,
    /// which would be the wrong diagnosis for malformed persisted data.
    for (const RefCommittedRow & row : validated.committed)
    {
        if (state.owned_manifests.contains(row.manifest_ref))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "stateFromSnapshot: snapshot names one manifest under two owners (committed ref '{}')", row.ref_name);
        state.committed.emplace(row.ref_name, row);
        state.snapshot_body_bytes += committedRowEncodedSize(row);
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
        state.owned_manifests.insert(row.manifest_ref);
    }
    for (const RefOwnerBinding & b : validated.precommits)
    {
        if (state.owned_manifests.contains(b.manifest_ref))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "stateFromSnapshot: snapshot names one manifest under two owners (precommit ref '{}')", b.ref_name);
        state.precommits.emplace(b.ref_name, b.manifest_ref);
        state.snapshot_body_bytes += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
        state.owned_manifests.insert(b.manifest_ref);
    }
    return state;
}

#ifdef DEBUG_OR_SANITIZER_BUILD
/// Debug/sanitizer-only: recompute both body totals from scratch and assert the incrementally
/// maintained values match. This is what makes the incremental counters *provably* byte-exact rather
/// than a drift-prone estimate -- the concern the old non-incremental admits() cited. O(N); compiled
/// only in debug and sanitizer builds (`DEBUG_OR_SANITIZER_BUILD`, the same condition `chassert` fires
/// under), so an ASan/TSan run exercises it too, not just a debug build.
///
/// Also rebuilds the expected `owned_manifests` membership by scanning `committed` + `precommits`
/// (the same linear walk `manifestAlreadyOwned` used to do directly) and cross-checks it against the
/// COW index: every scanned manifest must be present in the index, and the index's total size must
/// equal the number of rows scanned -- together those two checks catch both a missing entry and a
/// stale/extra one, which a size-only or membership-only check could each miss on their own.
void RefTableState::debugAssertBodyCounters() const
{
    uint64_t snap = 0;
    uint64_t rem = 0;
    size_t owned_scanned = 0;
    for (const auto [name, row] : committed)
    {
        snap += committedRowEncodedSize(row);
        rem  += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
        chassert(owned_manifests.contains(row.manifest_ref));
        ++owned_scanned;
    }
    for (const auto & [name, mref] : precommits)
    {
        snap += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, name, mref});
        rem  += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);
        chassert(owned_manifests.contains(mref));
        ++owned_scanned;
    }
    chassert(snapshot_body_bytes == snap);
    chassert(removal_body_bytes == rem);
    chassert(owned_manifests.size() == owned_scanned);
}
#endif

void RefTableState::applyTxnInPlace(const RefLogTxn & txn)
{
    /// Txn-wide preconditions first, before any mutation.
    if (!(greatest_applied < txn.txn_id))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: txn_id {}-{} is not strictly greater than the greatest applied {}-{}",
            txn.txn_id.writer_epoch, txn.txn_id.ref_sequence,
            greatest_applied.writer_epoch, greatest_applied.ref_sequence);

    checkRemoveNamespaceOrdering(txn.ops);

    /// Apply every op, in array order, IN PLACE. A throw leaves `*this` PARTIALLY APPLIED ("poisoned").
    /// This is the poisoning strategy (E3, no scratch copy): sound ONLY on a state the caller discards
    /// on any throw. The public `applyRefLogTxn` reaches it only through a scratch copy (turning it into
    /// the strong guarantee); `replay` reaches it directly on its own local, discard-on-throw state,
    /// which is what eliminates the per-transaction deep-copy of the replay tail's unbounded COW
    /// overlays -- a K-transaction replay over an N-row base drops from O(K*N) to O(K + N).
    for (const RefOp & op : txn.ops)
        applyOp(op, txn.txn_id);
    greatest_applied = txn.txn_id;
#ifdef DEBUG_OR_SANITIZER_BUILD
    /// Reached only on success, where `*this` is fully applied and its incremental body counters and
    /// owned-manifest index are consistent -- the invariant this cross-check defends.
    debugAssertBodyCounters();
#endif
}

void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn)
{
    /// The one public apply entry point, ALWAYS the strong exception guarantee: validate and apply the
    /// whole transaction against a scratch copy; replace `state` only once the whole transaction
    /// succeeds, so a throw anywhere leaves `state` byte-for-byte unchanged and no intra-transaction
    /// intermediate state is ever observable. This copy is cheap on every caller: each applies against a
    /// materialized (empty-overlay) live state or a small bounded-overlay batch scratch, never the
    /// unbounded replay tail (that is `replay`'s job, and it uses the private in-place strategy directly
    /// on its own discard-on-throw local state).
    RefTableState scratch = state;
    scratch.applyTxnInPlace(txn);
    state = std::move(scratch);
}

RefTableSnapshot snapshotOf(const RefTableState & state, const String & ns)
{
    RefTableSnapshot snapshot;
    snapshot.ns = ns;
    snapshot.snapshot_id = state.getGreatestApplied();
    snapshot.lifecycle = state.getLifecycle();
    snapshot.remove_txn_id = state.getRemoveTxnId();

    snapshot.committed.reserve(state.getCommitted().size());
    for (const auto [name, row] : state.getCommitted())
        snapshot.committed.push_back(row);   /// RefCowMap iterates sorted by ref_name (Pool/CasRefCowMap.h)

    snapshot.precommits.reserve(state.getPrecommits().size());
    for (const auto & [name, manifest_ref] : state.getPrecommits())
        snapshot.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, name, manifest_ref});
        /// std::set<std::pair<String, ManifestRef>> iterates sorted by (ref_name, manifest_ref),
        /// matching CasRefSnapshotCodec's required precommit sort order exactly.

    return snapshot;
}

RefTableState replay(const std::optional<RefTableSnapshot> & snapshot, std::span<const RefLogTxn> tail)
{
    RefTableState state = snapshot ? stateFromSnapshot(*snapshot) : RefTableState{};

    const String * expected_ns = snapshot ? &snapshot->ns : nullptr;
    for (const RefLogTxn & txn : tail)
    {
        if (expected_ns && txn.ns != *expected_ns)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState::replay: transaction ns '{}' does not match the table's ns '{}'",
                txn.ns, *expected_ns);
        expected_ns = &txn.ns;
        /// The one place the poisoning in-place apply strategy is reached (E3): `state` is `replay`'s own
        /// local, returned only after the WHOLE tail succeeds, so a mid-tail throw destroys it during
        /// unwinding and no consumer ever observes a poisoned state. `applyTxnInPlace` still runs the
        /// FULL validation `applyRefLogTxn` does -- including the cross-owner uniqueness check, which is
        /// O(1) and no longer elided (post-consult) -- so a corrupted/collision-bearing tail fails closed
        /// here rather than silently drifting the index.
        state.applyTxnInPlace(txn);
    }
    return state;
}

RefReplayBuilder::RefReplayBuilder(std::optional<RefTableSnapshot> base, uint64_t base_encoded_bytes)
{
    if (base)
    {
        result.newest_snapshot_id = base->snapshot_id;
        result.sealed_from = base->sealed_from;
        result.base_snapshot_bytes = base_encoded_bytes;
        expected_ns = base->ns;
        candidate = stateFromSnapshot(*base);   /// full snapshot revalidation, exactly as `replay`
    }
}

void RefReplayBuilder::applyOne(RefLogTxn && txn, uint64_t encoded_bytes)
{
    /// The streaming-recovery memory probe is driven by the CALLER's loop (around GET->decode->this
    /// call->discard), not here: the alive decoded-transaction set is a property of how the loop holds
    /// its transactions, which `applyOne` -- seeing one at a time regardless -- cannot observe. See
    /// `reportReplayMemoryDelta` / `decodedRefLogTxnFootprint`.
    if (expected_ns && txn.ns != *expected_ns)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefReplayBuilder: transaction ns '{}' does not match the table's ns '{}'",
            txn.ns, *expected_ns);
    expected_ns = txn.ns;
    /// The same private in-place poisoning path `replay` uses (E3): `candidate` is this builder's own
    /// state, discarded on any throw (the builder is destroyed during unwinding), so a mid-tail
    /// corruption fails closed here and no consumer ever observes a poisoned candidate. NOT the public
    /// scratch-copying `applyRefLogTxn`, which would deep-copy the growing candidate once per
    /// transaction and reintroduce the O(K*N) cost `replay` was written to avoid.
    candidate.applyTxnInPlace(txn);
    ++result.tail_count;
    result.tail_bytes += encoded_bytes;
}

RecoveryResult RefReplayBuilder::finish() &&
{
    /// Matches `replay`: the candidate is returned WITHOUT `materializeCommitted` -- the writer's
    /// recovery folds the COW overlays once on the result before installing it; the read-only consumers
    /// (orphan sweep, fsck oracle) do not need the fold at all.
    result.state = std::move(candidate);
    return std::move(result);
}

uint64_t encodedSnapshotBudgetSize(const RefTableState & state)
{
    /// snapshotOf uses snapshot_id = state.greatest_applied, empty ns, sealed_from unset, and the
    /// state's own lifecycle/remove_txn_id -- match that framing exactly, then add the running body sum.
    const uint64_t rows = state.getCommitted().size() + state.getPrecommits().size();
    return snapshotFramingSize("", state.getGreatestApplied(), state.getLifecycle(),
                               state.getRemoveTxnId(), /*sealed_from*/std::nullopt, rows)
        + state.getSnapshotBodyBytes();
}

uint64_t encodedRemovalBudgetSize(const RefTableState & state)
{
    /// The hypothetical whole-namespace removal transaction uses a fixed {1,1} preview id, empty ns,
    /// and one removal op per owner (committed + precommit) plus a terminal remove_namespace op -- so
    /// op_count = committed + precommits + 1.
    static constexpr RefTxnId kPreviewTxnId{1, 1};
    const uint64_t rows = state.getCommitted().size() + state.getPrecommits().size();
    return removalFramingSize("", kPreviewTxnId, rows + 1) + state.getRemovalBodyBytes();
}

bool admits(const RefTableState & state, const RefOp & op, uint64_t snapshot_budget, uint64_t removal_budget)
{
    /// A fixed nonzero placeholder id: this previews `op` in isolation and the scratch state is
    /// discarded immediately after reading its (incrementally maintained) budget sizes.
    static constexpr RefTxnId kPreviewTxnId{1, 1};

    /// Previews an op that has not yet been validated or durably appended anywhere, so it gets the full
    /// append-time check (the same `applyOp` the writer's apply path runs -- validation is strategy-
    /// independent).
    RefTableState scratch = state;
    scratch.applyOp(op, kPreviewTxnId);   // throws exactly as before if `op` is not a legal transition
#ifdef DEBUG_OR_SANITIZER_BUILD
    scratch.debugAssertBodyCounters();
#endif

    if (encodedSnapshotBudgetSize(scratch) > snapshot_budget)
        return false;
    return encodedRemovalBudgetSize(scratch) <= removal_budget;
}

std::vector<RefManifestEdge> manifestEdgesOfTxn(const RefLogTxn & txn)
{
    std::vector<RefManifestEdge> edges;
    edges.reserve(txn.ops.size());   /// every recognized op contributes at most one edge
    const RootNamespace ns{txn.ns};

    for (uint32_t op_ordinal = 0; op_ordinal < txn.ops.size(); ++op_ordinal)
    {
        const RefOp & op = txn.ops[op_ordinal];
        if (op.kind != RefOpKind::OwnerTransition)
            continue;

        /// Shape legality is decided once, by the shared classifier -- the same one
        /// `RefTableState::applyOwnerTransition` dispatches on -- so an unrecognized shape throws here
        /// instead of silently acquiring accidental edge meaning (e.g. an old+new pair naming different
        /// manifests, which the state machine never admits, used to read as a tolerated "replace").
        switch (classifyOwnerTransitionShape(op))
        {
            case OwnerTransitionShape::AddPrecommit:
                edges.push_back(RefManifestEdge{
                    ManifestId{ns, op.new_binding->manifest_ref}, +1, op.new_binding->kind, op_ordinal, 1});
                continue;
            case OwnerTransitionShape::RemovePrecommit:
            case OwnerTransitionShape::RemoveCommitted:
                edges.push_back(RefManifestEdge{
                    ManifestId{ns, op.old_binding->manifest_ref}, -1, op.old_binding->kind, op_ordinal, 0});
                continue;
            case OwnerTransitionShape::Promote:
                /// Same-manifest owner move: the manifest keeps an owner the whole time, so there is no
                /// net edge.
                continue;
        }
        /// Reachable only if a future `OwnerTransitionShape` enumerator is added without a matching
        /// `case` -- `-Wswitch`/`-Werror` catches that at compile time; this throw is the runtime
        /// backstop for builds without it.
        throw Exception(ErrorCodes::CORRUPTED_DATA, "manifestEdgesOfTxn: unhandled owner_transition shape");
    }

    return edges;
}

std::optional<RefTxnId> removalTxnId(const RefLogTxn & txn)
{
    for (const RefOp & op : txn.ops)
        if (op.kind == RefOpKind::RemoveNamespace)
            return txn.txn_id;
    return std::nullopt;
}

std::map<String, RefTableListing> groupRefKeys(const Layout & layout, const std::vector<String> & listed_keys)
{
    const String base = layout.casRefsPrefix();
    std::map<String, RefTableListing> out;

    for (const String & key : listed_keys)
    {
        if (!key.starts_with(base))
            continue;

        const auto parsed = layout.parseRefObjectKey(key);
        if (!parsed)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "groupRefKeys: key '{}' under the ref prefix is not a valid ref object -- aborting ref folding", key);

        /// `parseRefObjectKey` reconstructs the namespace from the key WITHOUT checking
        /// its shape. This is the first production consumer of that namespace, so re-validate it; a
        /// malformed namespace throws (BAD_ARGUMENTS), which the round treats as a malformed key.
        layout.validateNamespace(parsed->ns);

        RefTableListing & table = out[parsed->ns.string()];
        switch (parsed->kind)
        {
            case RefObjectKind::Log:
                table.logs.push_back(parsed->txn_id);
                break;
            case RefObjectKind::Snap:
                table.snapshots.push_back(parsed->txn_id);
                break;
            case RefObjectKind::Cleanup:
                table.cleanup_markers.push_back(parsed->txn_id);
                break;
        }
    }

    for (auto & [ns, table] : out)
    {
        std::sort(table.logs.begin(), table.logs.end());
        std::sort(table.snapshots.begin(), table.snapshots.end());
        std::sort(table.cleanup_markers.begin(), table.cleanup_markers.end());
    }

    return out;
}

RefCleanupPlan planRefCleanup(const RefTableListing & listing, const RefTxnId & durable_cursor,
                              const std::set<RefTxnId> & removal_logs_blocked,
                              std::optional<RefTxnId> completed_removal_snapshot)
{
    RefCleanupPlan plan;

    /// The coverage boundary `X` is the newest snapshot known to be durable: the newest observed in this
    /// round's scan, and -- for a namespace-cleanup item that reached `Completed` this round -- the
    /// `Removed` snapshot the caller just made durable. With
    /// neither there is no boundary, so no log is coverage-deletable and no older snapshot exists to delete.
    std::optional<RefTxnId> newest_snapshot;
    if (!listing.snapshots.empty())
        newest_snapshot = listing.snapshots.back();
    if (completed_removal_snapshot && (!newest_snapshot || *newest_snapshot < *completed_removal_snapshot))
        newest_snapshot = completed_removal_snapshot;
    if (!newest_snapshot)
        return plan;

    for (const RefTxnId & log_id : listing.logs)
    {
        if (*newest_snapshot < log_id)     /// L > X: not covered by any durable snapshot
            continue;
        if (durable_cursor < log_id)       /// L > cursor: its edge delta is not yet durable
            continue;
        if (removal_logs_blocked.contains(log_id))   /// remove_namespace log whose item is not Completed
            continue;
        plan.deletable_logs.push_back(log_id);
    }

    /// Only snapshots the scan actually returned are deletion candidates; a `completed_removal_snapshot`
    /// first published this round is not in `listing.snapshots`, so it is never scheduled for deletion,
    /// and once it later appears in the scan it is the newest and is retained here.
    for (const RefTxnId & snapshot_id : listing.snapshots)
        if (snapshot_id < *newest_snapshot)
            plan.deletable_snapshots.push_back(snapshot_id);

    return plan;
}

RecoveredRefTable recoverRefTableDetailed(Backend & backend, const Layout & layout, const RootNamespace & ns,
                                          const std::function<void()> & on_page_fetched, unsigned max_restarts)
{
    for (unsigned attempt = 0;; ++attempt)
    {
        /// One LIST of the table prefix; classify keys into logs and snapshots.
        std::vector<RefTxnId> logs;
        std::vector<RefTxnId> snapshots;
        String cursor;
        for (;;)
        {
            const ListPage page = backend.list(layout.refsNamespacePrefix(ns), cursor, 1000);
            if (on_page_fetched)
                on_page_fetched();
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = layout.parseRefObjectKey(lk.key);
                if (parsed && parsed->ns == ns)
                {
                    if (parsed->kind == RefObjectKind::Log)
                        logs.push_back(parsed->txn_id);
                    else if (parsed->kind == RefObjectKind::Snap)
                        snapshots.push_back(parsed->txn_id);
                }
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
        std::sort(logs.begin(), logs.end());
        std::sort(snapshots.begin(), snapshots.end());

        bool vanished = false;

        /// Newest snapshot, if any.
        std::optional<RefTableSnapshot> snapshot;
        std::optional<RefTxnId> snapshot_id;
        if (!snapshots.empty())
        {
            snapshot_id = snapshots.back();
            const auto got = backend.get(layout.refSnapshotKey(ns, *snapshot_id));
            if (!got)
                vanished = true;
            else
                snapshot = decodeRefTableSnapshot(openObject(FormatId::RefSnapshot, got->bytes), ns.string(), *snapshot_id);
        }

        /// Stream every log with id greater than the selected snapshot, in id order, through one
        /// builder: GET -> decode -> applyOne -> discard, holding at most a single decoded transaction
        /// resident (unlike the retired whole-tail vector, which held every decoded transaction at once).
        RefReplayBuilder builder(std::move(snapshot));
        if (!vanished)
            for (const RefTxnId & id : logs)
            {
                if (snapshot_id && !(*snapshot_id < id))
                    continue;   /// id <= snapshot: already included in the snapshot
                const auto got = backend.get(layout.refLogKey(ns, id));
                if (!got)
                {
                    vanished = true;
                    break;
                }
                RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
                /// Account this decoded transaction's resident footprint to the memory probe for exactly
                /// the span it is held -- this iteration. The streaming loop holds one at a time, so the
                /// probe's peak stays within a single transaction (a whole-tail materialiser would hold
                /// the entire tail, and the probe would see it). No-op in production.
                const int64_t footprint = static_cast<int64_t>(decodedRefLogTxnFootprint(txn));
                reportReplayMemoryDelta(footprint);
                SCOPE_EXIT({ reportReplayMemoryDelta(-footprint); });
                builder.applyOne(std::move(txn), got->bytes.size());
            }

        if (vanished)
        {
            if (attempt < max_restarts)
                continue;   /// a selected object vanished; restart with a fresh LIST (the builder's candidate is discarded)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "recoverRefTable: table {} kept losing a selected snapshot/tail object across {} restarts",
                ns.string(), max_restarts);
        }

        RecoveryResult result = std::move(builder).finish();
        return RecoveredRefTable{std::move(result.state), result.newest_snapshot_id, result.sealed_from};
    }
}

RefTableState recoverRefTable(Backend & backend, const Layout & layout, const RootNamespace & ns,
                              const std::function<void()> & on_page_fetched, unsigned max_restarts)
{
    return recoverRefTableDetailed(backend, layout, ns, on_page_fetched, max_restarts).state;
}

}
