#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <Common/scope_guard_safe.h>
#include <algorithm>
#include <limits>
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

CatalogLifeIndex::CatalogLifeIndex(const RefCatalog & catalog)
{
    for (const CatalogEntry & entry : catalog.entries)
    {
        const NamespaceLifePhysicalId life_id = entry.incarnation;
        if (auto ambiguous_it = ambiguous_names.find(life_id); ambiguous_it != ambiguous_names.end())
        {
            ambiguous_it->second.push_back(entry.ns.string());
            continue;
        }

        const auto [it, inserted] = unique_lives.emplace(
            life_id, NamespaceLifeId::fromCatalogEntry(entry.ns, life_id));
        if (inserted)
            continue;

        std::vector<String> names;
        names.push_back(it->second.ns.string());
        names.push_back(entry.ns.string());
        unique_lives.erase(it);
        ambiguous_names.emplace(life_id, std::move(names));
    }
}

bool CatalogLifeIndex::isAmbiguous(NamespaceLifePhysicalId life_id) const
{
    return ambiguous_names.contains(life_id);
}

std::optional<NamespaceLifeId> CatalogLifeIndex::resolve(NamespaceLifePhysicalId life_id) const
{
    if (const auto ambiguous_it = ambiguous_names.find(life_id); ambiguous_it != ambiguous_names.end())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS ref catalog: life_id {} is shared by current namespaces '{}' and '{}' -- both rows are unresolvable",
            renderIncarnation(life_id), ambiguous_it->second[0], ambiguous_it->second[1]);
    if (const auto it = unique_lives.find(life_id); it != unique_lives.end())
        return it->second;
    return std::nullopt;
}

void CatalogLifeIndex::throwIfAmbiguous(std::string_view consumer) const
{
    if (ambiguous_names.empty())
        return;
    const auto & [life_id, names] = *ambiguous_names.begin();
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "{}: catalog life_id {} is shared by current namespaces '{}' and '{}' -- refusing a decision from an ambiguous cut",
        consumer, renderIncarnation(life_id), names[0], names[1]);
}

bool catalogStillNamesLife(const RefCatalog & catalog, const NamespaceLifeId & life)
{
    return std::any_of(catalog.entries.begin(), catalog.entries.end(), [&](const CatalogEntry & entry)
    {
        return entry.ns == life.ns && entry.incarnation == life.incarnation
            && (entry.state == NsState::Live || entry.state == NsState::Removing);
    });
}

std::optional<RefTxnId> refChainSuccessor(const RefLogTxn & txn)
{
    if (removalTxnId(txn))
        return std::nullopt;
    if (const auto transition = refEpochTransitionOf(txn))
        return RefTxnId{transition->next_epoch, 1};
    if (txn.txn_id.ref_sequence == std::numeric_limits<uint64_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref chain: log id {}-{} has no representable successor",
            txn.txn_id.writer_epoch, txn.txn_id.ref_sequence);
    return RefTxnId{txn.txn_id.writer_epoch, txn.txn_id.ref_sequence + 1};
}

std::optional<RefEpochTransition> refEpochTransitionOf(const RefLogTxn & txn)
{
    if (!refLogTxnIsEpochSeal(txn))
        return std::nullopt;
    if (txn.next_epoch)
        return RefEpochTransition{txn.txn_id, *txn.next_epoch};
    if (txn.txn_id.writer_epoch == std::numeric_limits<uint64_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref chain: epoch seal {}-{} has no representable successor",
            txn.txn_id.writer_epoch, txn.txn_id.ref_sequence);
    return RefEpochTransition{txn.txn_id, txn.txn_id.writer_epoch + 1};
}

void validateRefChainLink(const std::optional<RefEpochTransition> & open, const RefLogTxn & next)
{
    const RefTxnId & id = next.txn_id;
    if (!open)
    {
        if (next.prev_epoch_seal)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref chain: {}-{} carries prev_epoch_seal {}-{}, but the record before it is not a tombstone",
                id.writer_epoch, id.ref_sequence, next.prev_epoch_seal->writer_epoch, next.prev_epoch_seal->ref_sequence);
        return;
    }
    if (id != RefTxnId{open->next_epoch, 1})
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS ref chain: {}-{} follows tombstone {}-{}, whose successor is {}-1",
            id.writer_epoch, id.ref_sequence, open->seal_id.writer_epoch, open->seal_id.ref_sequence, open->next_epoch);
    if (next.prev_epoch_seal != open->seal_id)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS ref chain: {}-{} follows tombstone {}-{} but its prev_epoch_seal does not name it",
            id.writer_epoch, id.ref_sequence, open->seal_id.writer_epoch, open->seal_id.ref_sequence);
}

bool ckptTokenMoved(CasOperation & op, const Layout & layout, const NamespaceLifeId & life, const Etag & token)
{
    const std::optional<Object> current = op.read(layout.refCkptKey(life), Retry::standard());
    return !current || !sameVersion(current->etag, token);
}

bool failureJudgedAgainstCkpt(int error_code)
{
    return error_code == ErrorCodes::CORRUPTED_DATA;
}

bool restartsOnMovedBase(int error_code, CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                         const Etag & token)
{
    return failureJudgedAgainstCkpt(error_code) && ckptTokenMoved(op, layout, life, token);
}

RefTxnId RefTableState::chainNextId(uint64_t life_epoch) const
{
    if (greatest_applied == RefTxnId{})
        return RefTxnId{life_epoch, 1};
    if (open_transition)
        return RefTxnId{open_transition->next_epoch, 1};
    if (greatest_applied.ref_sequence == std::numeric_limits<uint64_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref chain: log id {}-{} has no representable successor",
            greatest_applied.writer_epoch, greatest_applied.ref_sequence);
    return RefTxnId{greatest_applied.writer_epoch, greatest_applied.ref_sequence + 1};
}

RefNextSlot nextRefSlot(const RefTableState & state, uint64_t life_epoch)
{
    const RefTxnId id = state.chainNextId(life_epoch);
    const auto & open = state.getOpenTransition();
    if (open && id.ref_sequence == 1)
        return RefNextSlot{id, open->seal_id};
    return RefNextSlot{id, std::nullopt};
}

namespace
{

/// Txn-wide structural check, NOT a per-op precondition: if `ops` contains a
/// `RemoveNamespace`, it must be the last element, and every earlier op must be an exact
/// owner-removal `owner_transition` (`old_binding` set, `new_binding` empty). The sole other legal
/// form is `[NamespaceBirth, RemoveNamespace]`: a cataloged life may own `_ckpt`/`_files` without ever
/// having emitted a ref transaction, and its empty birth+terminal must be one durable removal record.
/// `CasRefLogCodec` deliberately does not check this shape -- this is the one place that does.
void checkRemoveNamespaceOrdering(const std::vector<RefOp> & ops)
{
    const bool has_remove = std::any_of(ops.begin(), ops.end(),
        [](const RefOp & op) { return op.kind == RefOpKind::RemoveNamespace; });
    if (!has_remove)
        return;

    if (ops.empty() || ops.back().kind != RefOpKind::RemoveNamespace)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: remove_namespace must be the final operation of its transaction");

    if (ops.size() == 2 && ops.front().kind == RefOpKind::NamespaceBirth)
        return;

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
    static_assert(std::is_nothrow_swappable_v<std::optional<RefEpochTransition>>);
    static_assert(std::is_nothrow_swappable_v<std::set<std::pair<String, ManifestRef>>>);
    static_assert(std::is_nothrow_swappable_v<uint64_t>);
    static_assert(noexcept(std::declval<RefCowMap &>().swap(std::declval<RefCowMap &>())));
    static_assert(noexcept(std::declval<RefCowManifestSet &>().swap(std::declval<RefCowManifestSet &>())));

    using std::swap;
    swap(lifecycle, other.lifecycle);
    swap(remove_txn_id, other.remove_txn_id);
    swap(greatest_applied, other.greatest_applied);
    swap(open_transition, other.open_transition);
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
/// new_binding)` shape to one of the five legal transitions (add precommit / remove precommit /
/// remove committed / promote / move). Any other shape is not a recognized transition.
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

        /// Move: a committed row changes its ref_name, keeping its manifest_ref and published_at_ms.
        case OwnerTransitionShape::Move:
        {
            const RefOwnerBinding & from = *op.old_binding;
            const RefOwnerBinding & to = *op.new_binding;
            const auto it = committed.find(from.ref_name);
            if (it == committed.end() || !(it->second.manifest_ref == from.manifest_ref))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: exact committed binding '{}' to move is absent", from.ref_name);
            if (committed.contains(to.ref_name))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: move destination '{}' is already committed", to.ref_name);
            /// Precommits are keyed by (ref_name, manifest_ref); any precommit under the destination name
            /// means another build is about to claim it.
            if (hasLivePrecommitFor(to.ref_name))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: move destination '{}' has a live precommit", to.ref_name);
            /// `RefCowMap` has no node handle, so the destination is inserted before the source is
            /// erased: an allocation failure in between leaves a duplicate row, never a lost one.
            RefCommittedRow row = it->second;
            RefCommittedRow moved = row;
            moved.ref_name = to.ref_name;
            committed.emplace(to.ref_name, std::move(moved));
            snapshot_body_bytes -= committedRowEncodedSize(row);
            removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
            row.ref_name = to.ref_name;
            snapshot_body_bytes += committedRowEncodedSize(row);
            removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
            committed.erase(from.ref_name);
            /// `owned_manifests` is untouched: the manifest keeps its single owner across the rename.
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
            /// A life is born once. After its terminal record the walk reads no further key, so a birth
            /// there would be state no reader reaches.
            if (lifecycle == RefLifecycle::Live)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: namespace_birth while already Live");
            if (remove_txn_id)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: namespace_birth after the terminal record");
            lifecycle = RefLifecycle::Live;
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
            chassert(owned_manifests.empty());
            lifecycle = RefLifecycle::Removed;
            remove_txn_id = txn_id;
            return;
        }
        case RefOpKind::EpochSeal:
            /// A seal carries no table content: it only occupies its slot and moves the chain to the
            /// next epoch. A never-born stream gets one like any first write of an epoch; only a
            /// terminal stream has no successor to link.
            if (remove_txn_id)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableState: epoch_seal at {}-{} after the terminal record",
                    txn_id.writer_epoch, txn_id.ref_sequence);
            return;
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
    /// A persisted snapshot is a materialization of a live stream only. `RefTableState` defaults to
    /// `Removed`, so this assignment is deliberately explicit rather than relying on construction
    /// defaults that have the opposite meaning.
    state.lifecycle = RefLifecycle::Live;
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

    /// The chain is dense: a reader that applies every record from the base to the first absent key
    /// has the whole stream only if each record sits exactly at its predecessor's successor.
    if (isTerminal())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: txn_id {}-{} follows the terminal record {}-{}",
            txn.txn_id.writer_epoch, txn.txn_id.ref_sequence,
            greatest_applied.writer_epoch, greatest_applied.ref_sequence);
    if (greatest_applied == RefTxnId{})
    {
        if (txn.txn_id.ref_sequence != 1)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState: the first record of a stream is at sequence 1, not {}-{}",
                txn.txn_id.writer_epoch, txn.txn_id.ref_sequence);
    }
    else if (const RefTxnId expected = chainNextId(txn.txn_id.writer_epoch); txn.txn_id != expected)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableState: txn_id {}-{} does not continue the ref-log chain -- the greatest applied id "
            "is {}-{}, so the only successor is {}-{}",
            txn.txn_id.writer_epoch, txn.txn_id.ref_sequence,
            greatest_applied.writer_epoch, greatest_applied.ref_sequence,
            expected.writer_epoch, expected.ref_sequence);

    validateRefChainLink(open_transition, txn);

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
    open_transition = refEpochTransitionOf(txn);
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
    if (state.getLifecycle() != RefLifecycle::Live)
        throw Exception(
            ErrorCodes::CORRUPTED_DATA,
            "snapshotOf: terminal namespace state is not snapshot-serializable");

    RefTableSnapshot snapshot;
    snapshot.ns = ns;
    snapshot.snapshot_id = state.getGreatestApplied();

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
    /// (orphan sweep and fsck) do not need the fold at all.
    result.state = std::move(candidate);
    return std::move(result);
}

uint64_t encodedSnapshotBudgetSize(const RefTableState & state)
{
    /// `snapshotOf` uses `snapshot_id = state.greatest_applied` and an empty namespace here. Snapshot
    /// lifecycle is fixed to live on the wire, so the framing depends only on those fields and the row
    /// count.
    const uint64_t rows = state.getCommitted().size() + state.getPrecommits().size();
    return snapshotFramingSize("", state.getGreatestApplied(), rows)
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
            case OwnerTransitionShape::Move:
                continue;   /// ownership transfer, no edge
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

std::map<NamespaceLifePhysicalId, RefTableListing> groupRefKeys(
    const Layout & layout, const std::vector<String> & listed_keys)
{
    const String base = layout.casRefsPrefix();
    std::map<NamespaceLifePhysicalId, RefTableListing> out;

    for (const String & key : listed_keys)
    {
        if (!key.starts_with(base))
            continue;

        const auto parsed = layout.parseRefObjectKey(key);
        if (!parsed)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "groupRefKeys: key '{}' under the ref prefix is not a valid ref object -- aborting ref folding", key);

        RefTableListing & table = out[parsed->life_id];
        switch (parsed->kind)
        {
            case RefObjectKind::Log:
                table.logs.push_back(parsed->txn_id);
                break;
            case RefObjectKind::Snap:
                table.snapshots.push_back(parsed->txn_id);
                break;
        }
    }

    for (auto & [life_id, table] : out)
    {
        std::sort(table.logs.begin(), table.logs.end());
        std::sort(table.snapshots.begin(), table.snapshots.end());
    }

    return out;
}

RefCleanupPlan planRefCleanup(const RefTableListing & listing, const RefTxnId & durable_cursor,
                              std::optional<RefTxnId> checkpoint,
                              std::optional<RefTxnId> retained_log_proof)
{
    RefCleanupPlan plan;

    /// A physical `_snap` listed after its PUT but before the `_ckpt` CAS is not a recovery base.
    /// The caller supplies `checkpoint` only after `readCheckpointSnapshotBase` has exact-read the
    /// same-id non-seal `_log` and `_snap`. With no such validated triple cleanup has no coverage
    /// authority and deliberately leaks every listed object.
    if (!checkpoint)
        return plan;

    for (const RefTxnId & log_id : listing.logs)
    {
        if (durable_cursor < log_id)       /// L > cursor: its edge delta is not yet durable
            continue;
        if (*checkpoint <= log_id)         /// the exact witness and its successors remain
            continue;
        if (retained_log_proof == log_id)  /// a later-epoch base still needs its predecessor seal
            continue;
        plan.deletable_logs.push_back(log_id);
    }

    /// The checkpoint's same-id snapshot is the recovery base, so only strictly older listed snapshots
    /// are deletion candidates.
    for (const RefTxnId & snapshot_id : listing.snapshots)
        if (snapshot_id < *checkpoint)
            plan.deletable_snapshots.push_back(snapshot_id);

    return plan;
}

namespace
{
/// Read-ahead depth of a walk with no frontier: it overlaps a short tail, and bounds the keys past the end
/// fetched for nothing in each epoch the walk ends or leaves.
constexpr uint64_t kHintDepthWithoutFrontier = 4;
}

RefChainWalk walkRefChain(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                          const RefChainOrigin & origin, const RefChainVisitor & visit, KeyReader * reader)
{
    const String & ns = life.ns.string();
    RefChainWalk walk;
    walk.open_transition = origin.open_transition;
    RefTxnId id = origin.first_key;

    /// Hints are taken in order and never cross an epoch, so the outstanding ones are the keys of the
    /// current epoch from `hinted_from` up to one window.
    std::optional<RefTxnId> hinted_from;
    const auto discard_hints = [&]
    {
        if (reader && hinted_from)
            discardRefLogHintsOfEpoch(*reader, layout, life, *hinted_from,
                                      RefTxnId{hinted_from->writer_epoch, std::numeric_limits<uint64_t>::max()});
        hinted_from.reset();
    };
    SCOPE_EXIT_MEMORY_SAFE({ discard_hints(); });

    const auto base_moved = [&] { return ckptTokenMoved(op, layout, life, *origin.ckpt_token); };
    /// A failure of the chain itself is corruption only if `_ckpt` did not move: a moved base means
    /// cleanup may have emptied the keys this walk read.
    const auto fail = [&](const String & what) -> RefChainWalk
    {
        if (origin.ckpt_token && base_moved())
        {
            walk.end = RefChainEnd::BaseMoved;
            return walk;
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref chain of namespace '{}': {}", ns, what);
    };
    const auto confirm_end = [&](RefChainEnd end) -> RefChainWalk
    {
        walk.end = end;
        if (origin.ckpt_token && origin.confirm_base_after_end && base_moved())
            walk.end = RefChainEnd::BaseMoved;
        return walk;
    };
    const auto render = [](const RefTxnId & x) { return fmt::format("{}-{}", x.writer_epoch, x.ref_sequence); };

    while (true)
    {
        /// Read ahead within the epoch. Under an old frontier up to it: the logs there are known to exist.
        /// Without one the end is unknown, so only a few keys ahead: a hint past the end is a wasted GET.
        const RefTxnId epoch_end{id.writer_epoch, std::numeric_limits<uint64_t>::max()};
        std::optional<RefTxnId> hint_limit;
        if (!origin.old_frontier)
            hint_limit = id.ref_sequence > epoch_end.ref_sequence - kHintDepthWithoutFrontier
                ? epoch_end : RefTxnId{id.writer_epoch, id.ref_sequence + kHintDepthWithoutFrontier};
        else if (id < *origin.old_frontier)
            hint_limit = std::min(*origin.old_frontier, epoch_end);
        if (reader && reader->window() > 0 && hint_limit && id.ref_sequence < std::numeric_limits<uint64_t>::max())
        {
            const RefTxnId from{id.writer_epoch, id.ref_sequence + 1};
            hintRefLogsWithinEpoch(*reader, layout, life, from, *hint_limit);
            if (!hinted_from)
                hinted_from = from;
        }
        const String key = layout.refLogKey(life, id);
        const std::optional<Object> got = reader ? reader->take(key) : op.read(key, Retry::standard());
        if (hinted_from && !(id < *hinted_from))
            hinted_from = RefTxnId{id.writer_epoch, id.ref_sequence + 1};

        if (!got)
        {
            walk.next_key = id;
            if (origin.old_frontier && !(*origin.old_frontier < id))
                return fail("log " + render(id) + " is absent at or below the checkpoint frontier "
                    + render(*origin.old_frontier));
            return confirm_end(RefChainEnd::AbsentKey);
        }

        RefLogTxn txn;
        std::optional<RefTxnId> next;
        std::optional<RefEpochTransition> transition;
        try
        {
            txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns, id);
            validateRefChainLink(walk.open_transition, txn);
            next = refChainSuccessor(txn);
            transition = refEpochTransitionOf(txn);
        }
        catch (const Exception & e)
        {
            if (origin.ckpt_token && restartsOnMovedBase(e.code(), op, layout, life, *origin.ckpt_token))
            {
                walk.end = RefChainEnd::BaseMoved;
                return walk;
            }
            throw;
        }

        if (origin.old_frontier && id < *origin.old_frontier)
        {
            if (!next)
                return fail("terminal record " + render(id) + " lies below the checkpoint frontier "
                    + render(*origin.old_frontier));
            if (*origin.old_frontier < *next)
                return fail("the checkpoint frontier " + render(*origin.old_frontier) + " is not on the chain between "
                    + render(id) + " and " + render(*next));
        }

        ++walk.records;
        const bool keep_walking = visit(std::move(txn), got->bytes.size());
        walk.open_transition = transition;
        if (!next)
        {
            walk.next_key = id;
            return confirm_end(RefChainEnd::Terminal);
        }
        if (!keep_walking)
        {
            walk.end = RefChainEnd::Stopped;
            walk.next_key = *next;
            return walk;
        }
        if (next->writer_epoch != id.writer_epoch)
            discard_hints();
        id = *next;
    }
}

RefCursorResume resolveRefCursorSuccessor(CasOperation & op, const Layout & layout, const NamespaceLifeId & life,
                                          const RefTxnId & cursor, const std::optional<RefTxnId> & snapshot_id)
{
    const String & ns = life.ns.string();
    RefCursorResume result;
    const auto hold = [&](String why)
    {
        result.kind = RefCursorResumeKind::Hold;
        result.detail = std::move(why);
        return result;
    };
    const auto key_at = [&](RefTxnId next, std::optional<RefEpochTransition> open)
    {
        result.kind = RefCursorResumeKind::Key;
        result.next_key = next;
        result.open_transition = open;
        return result;
    };
    /// Absent: `nullopt` with `undecodable` empty. A body that does not decode is reported, never thrown:
    /// the caller holds rather than guessing the record's kind.
    String undecodable;
    const auto read_record = [&](const RefTxnId & id) -> std::optional<RefLogTxn>
    {
        const std::optional<Object> got = op.read(layout.refLogKey(life, id), Retry::standard());
        if (!got)
        {
            ++result.absent_probes;
            return std::nullopt;
        }
        ++result.body_gets;
        try
        {
            return decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns, id);
        }
        catch (const Exception & e)
        {
            undecodable = e.message();
            return std::nullopt;
        }
    };
    const auto ordinary_successor = [&]
    {
        if (cursor.ref_sequence == std::numeric_limits<uint64_t>::max())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref chain: cursor {}-{} has no representable successor",
                cursor.writer_epoch, cursor.ref_sequence);
        return RefTxnId{cursor.writer_epoch, cursor.ref_sequence + 1};
    };

    /// Cleanup deletes only below the snapshot id, so a cursor at or above it still holds its own record.
    if (!snapshot_id || !(cursor < *snapshot_id))
    {
        const std::optional<RefLogTxn> got = read_record(cursor);
        if (!got)
            return hold(undecodable.empty() ? String("the record at the cursor is absent")
                                            : "the record at the cursor is undecodable: " + undecodable);
        const std::optional<RefTxnId> next = refChainSuccessor(*got);
        if (!next)
        {
            result.kind = RefCursorResumeKind::Terminal;
            return result;
        }
        return key_at(*next, refEpochTransitionOf(*got));
    }

    /// Below the snapshot the cursor's key may hold a stray, so it is not read. Back-chain by sequence-1
    /// backlinks from the snapshot's epoch: each backlink epoch is strictly lower than its record's, so
    /// the loop ends at or below the cursor's epoch.
    uint64_t epoch = snapshot_id->writer_epoch;
    while (true)
    {
        if (epoch == cursor.writer_epoch)
            return key_at(ordinary_successor(), std::nullopt);
        const RefTxnId head_id{epoch, 1};
        const std::optional<RefLogTxn> head = read_record(head_id);
        if (!head)
            return hold(undecodable.empty() ? fmt::format("the head {}-1 is absent", epoch)
                                            : fmt::format("the head {}-1 is undecodable: {}", epoch, undecodable));
        if (!head->prev_epoch_seal)
            return hold(fmt::format("the head {}-1 carries no backlink", epoch));
        const RefTxnId backlink = *head->prev_epoch_seal;
        if (backlink == cursor)
            return key_at(head_id, RefEpochTransition{cursor, epoch});
        if (backlink.writer_epoch < cursor.writer_epoch
            || (backlink.writer_epoch == cursor.writer_epoch && backlink < cursor))
            return hold(fmt::format("the head {}-1 links {}-{}, below the cursor", epoch,
                backlink.writer_epoch, backlink.ref_sequence));
        if (backlink.writer_epoch == cursor.writer_epoch)
            return key_at(ordinary_successor(), std::nullopt);
        epoch = backlink.writer_epoch;
    }
}

CheckpointSnapshotBase readCheckpointSnapshotBase(
    CasOperation & op, const Layout & layout, const NamespaceLifeId & life, const RefCkpt & checkpoint)
{
    const RootNamespace & ns = life.ns;
    if (!checkpoint.checkpoint_snapshot_id)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery for namespace '{}': checkpoint has no snapshot base",
            ns.string());
    }
    if (!checkpoint.life_epoch)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery for namespace '{}': checkpoint-named snapshot base has no life_epoch context",
            ns.string());
    }
    const RefTxnId snapshot_id = *checkpoint.checkpoint_snapshot_id;
    const auto log = op.read(layout.refLogKey(life, snapshot_id), Retry::standard());
    if (!log)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} has no matching log under "
            "the supplied immutable lifecycle authority",
            ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence);
    }

    const RefLogTxn base_txn = decodeRefLogTxn(
        openObject(FormatId::RefLog, log->bytes), ns.string(), snapshot_id);
    if (refLogTxnIsEpochSeal(base_txn))
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} names an EpochSeal, "
            "not a snapshot base",
            ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence);
    }
    validateEpochSealGrammarContextual(base_txn, *checkpoint.life_epoch);
    std::optional<RefTxnId> predecessor_seal_id;
    if (base_txn.prev_epoch_seal)
    {
        predecessor_seal_id = *base_txn.prev_epoch_seal;
        const auto predecessor = op.read(layout.refLogKey(life, *predecessor_seal_id), Retry::standard());
        if (!predecessor)
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} refers to absent "
                "previous epoch seal {}-{}",
                ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence,
                predecessor_seal_id->writer_epoch, predecessor_seal_id->ref_sequence);
        }
        const RefLogTxn predecessor_txn = decodeRefLogTxn(
            openObject(FormatId::RefLog, predecessor->bytes), ns.string(), *predecessor_seal_id);
        if (!refLogTxnIsEpochSeal(predecessor_txn))
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} refers to non-seal "
                "transaction {}-{}",
                ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence,
                predecessor_seal_id->writer_epoch, predecessor_seal_id->ref_sequence);
        }
        if (refEpochTransitionOf(predecessor_txn)->next_epoch != snapshot_id.writer_epoch)
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} refers to seal {}-{}, "
                "whose successor lies in epoch {}",
                ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence,
                predecessor_seal_id->writer_epoch, predecessor_seal_id->ref_sequence,
                refEpochTransitionOf(predecessor_txn)->next_epoch);
        }
    }

    const auto snapshot = op.read(layout.refSnapshotKey(life, snapshot_id), Retry::standard());
    if (!snapshot)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS recovery for namespace '{}': checkpoint-named base snapshot {}-{} is absent under the supplied "
            "immutable lifecycle authority",
            ns.string(), snapshot_id.writer_epoch, snapshot_id.ref_sequence);
    }
    return CheckpointSnapshotBase{
        .snapshot = decodeRefTableSnapshot(openObject(FormatId::RefSnapshot, snapshot->bytes), ns.string(), snapshot_id),
        .bytes = snapshot->bytes.size(),
        .predecessor_seal_id = predecessor_seal_id};
}

std::optional<RecoveredRefTable> recoverRefTableDetailedFromAuthority(
    CasOperation & op, const Layout & layout, const std::optional<CatalogEntry> & catalog_entry,
    const std::optional<CkptSample> & ckpt, KeyReader * reader)
{
    BoundedRefRecovery recovery
        = recoverRefTableDetailedFromAuthorityBounded(op, layout, catalog_entry, ckpt, reader, /*may_read_next=*/{});
    chassert(recovery.end != BoundedRefRecovery::End::Stopped);
    if (recovery.end != BoundedRefRecovery::End::Recovered)
        return std::nullopt;
    return std::move(recovery.table);
}

BoundedRefRecovery recoverRefTableDetailedFromAuthorityBounded(
    CasOperation & op, const Layout & layout, const std::optional<CatalogEntry> & catalog_entry,
    const std::optional<CkptSample> & ckpt, KeyReader * reader, const std::function<bool()> & may_read_next)
{
    /// The frozen catalog row and `_ckpt` sample supplied by the caller ground the walk; this function
    /// must not re-read the catalog, or enumerate the stream, because that would splice unrelated
    /// physical observations into the caller's one authority cut.
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog_entry, ckpt ? std::optional<RefCkpt>{ckpt->ckpt} : std::nullopt);
    /// `chooseRecoveryGrounding` has just established that this is a Live/Removing row. Constructing
    /// the life from that SAME value, rather than resolving the name again, preserves the caller's
    /// catalog-cut join.
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(catalog_entry->ns, catalog_entry->incarnation);

    std::optional<RefTableSnapshot> base_snapshot;
    uint64_t base_snapshot_bytes = 0;
    if (grounding.base)
    {
        try
        {
            CheckpointSnapshotBase base = readCheckpointSnapshotBase(op, layout, life, ckpt->ckpt);
            base_snapshot = std::move(base.snapshot);
            base_snapshot_bytes = base.bytes;
        }
        catch (const Exception & e)
        {
            if (restartsOnMovedBase(e.code(), op, layout, life, ckpt->etag))
                return BoundedRefRecovery{.end = BoundedRefRecovery::End::BaseMoved, .table = std::nullopt};
            throw;
        }
    }

    RefReplayBuilder builder(std::move(base_snapshot), base_snapshot_bytes);
    bool replay_failed = false;
    const RefChainVisitor apply = [&](RefLogTxn && txn, uint64_t stored_bytes)
    {
        const int64_t footprint = static_cast<int64_t>(decodedRefLogTxnFootprint(txn));
        reportReplayMemoryDelta(footprint);
        SCOPE_EXIT({ reportReplayMemoryDelta(-footprint); });
        try
        {
            builder.applyOne(std::move(txn), stored_bytes);
        }
        catch (const Exception &)
        {
            replay_failed = true;
            throw;
        }
        return !may_read_next || may_read_next();
    };

    RefChainWalk walk;
    try
    {
        walk = walkRefChain(op, layout, life, RefChainOrigin{
            .first_key = *grounding.walk_from,
            .open_transition = std::nullopt,
            .ckpt_token = ckpt->etag,
            .confirm_base_after_end = true,
            .old_frontier = grounding.committed_through}, apply, reader);
    }
    catch (const Exception & e)
    {
        if (replay_failed && restartsOnMovedBase(e.code(), op, layout, life, ckpt->etag))
            return BoundedRefRecovery{.end = BoundedRefRecovery::End::BaseMoved, .table = std::nullopt};
        throw;
    }
    if (walk.end == RefChainEnd::BaseMoved)
        return BoundedRefRecovery{.end = BoundedRefRecovery::End::BaseMoved, .table = std::nullopt};
    if (walk.end == RefChainEnd::Stopped)
        return BoundedRefRecovery{.end = BoundedRefRecovery::End::Stopped, .table = std::nullopt};

    RecoveryResult result = std::move(builder).finish();
    return BoundedRefRecovery{
        .end = BoundedRefRecovery::End::Recovered,
        .table = RecoveredRefTable{
            .state = std::move(result.state),
            .newest_snapshot_id = result.newest_snapshot_id,
            .open_transition = walk.open_transition,
            .end = walk.end,
            .next_key = walk.next_key}};
}

}
