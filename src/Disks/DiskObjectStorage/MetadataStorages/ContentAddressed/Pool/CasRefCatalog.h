#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <functional>
#include <optional>

namespace DB::Cas
{

/// The `cas/ref_catalog` object (spec INV-3) as seen from the pool side: reading the current
/// catalog, and the generic token-CAS retry primitive every lifecycle transition rides. This class
/// builds ONLY that primitive -- the actual lifecycle steps (the three-conditional-write creation
/// sequence, the removal terminal-record-then-entry-delete sequence) are later tasks' job, built ON
/// TOP of `casUpdate`/`casAdmitEntry`.
class CasRefCatalog
{
public:
    /// The catalog snapshot as read from the backend: the decoded object plus the token an update
    /// must present to `casPut`. `token == std::nullopt` means the object is ABSENT -- a pool that
    /// has never admitted a namespace has never had a durable `cas/ref_catalog`, mirroring the
    /// bootstrap contract of every other token-CAS singleton (`_pool_meta`, `gc/state`).
    struct Snapshot
    {
        RefCatalog catalog;
        std::optional<Token> token;
    };

    /// Reads and decodes the current catalog. Absent key -> an empty catalog with `token = nullopt`.
    static Snapshot read(Backend & backend, const Layout & layout);

    /// The generic token-CAS retry loop shared by every catalog mutation, mirroring
    /// `PoolMeta::admitOrValidate`'s loop: read the current snapshot, apply `mutate` to obtain the
    /// CANDIDATE next catalog, `casPut` it against the observed token (`std::nullopt` create-if-absent
    /// when the object does not exist yet), and on `Conflict` re-read and re-apply `mutate` to the
    /// FRESH snapshot -- never re-encoding the stale candidate. `mutate` must return a canonically
    /// ordered, grammar-valid candidate; `encodeRefCatalog` (called internally) enforces that.
    ///
    /// Bounded (the same live-lock brake `publishCkpt`/`allocateWriterEpoch` use on their own
    /// contended token-CAS singletons): after 100 conflicting attempts it gives up and raises the
    /// typed retryable error `throwCasWriteRetryLater`, naming the key and the attempt count, rather
    /// than spinning forever against a pathologically busy catalog.
    ///
    /// A re-read that finds the object genuinely ABSENT after it was previously observed present is
    /// NOT treated as a fresh bootstrap: `Backend::get` returns `nullopt` only for authoritative
    /// absence, so an existing-then-vanished catalog is a real concurrent delete (or a lying store),
    /// and silently falling back to an empty catalog would let the next attempt's create-if-absent
    /// `casPut` replace EVERY other namespace's entry with whatever this one mutation produced. This
    /// raises `LOGICAL_ERROR` instead, mirroring `PoolMeta::admitOrValidate`'s identical fail-closed
    /// reaction to the same observation.
    ///
    /// This primitive runs NO admission check: Constraint 13 (removal is never refused) means
    /// whether a candidate must clear the additive predicate is the CALLER's decision, not this
    /// loop's. A caller mutating an entry's state without growing the catalog (a removal transition)
    /// uses this directly.
    static RefCatalog casUpdate(
        Backend & backend, const Layout & layout,
        const std::function<RefCatalog(const RefCatalog &)> & mutate);

    /// Admits exactly ONE new namespace into the catalog under INV-3's two-predicate gate, inserting
    /// `entry` at its canonical (ns-sorted) position and running the SAME bounded `casUpdate` retry
    /// loop. Takes the entry to insert rather than an arbitrary mutation, by design: an admission
    /// entry point that accepted a free-form candidate could be handed a REMOVAL by a future caller
    /// that reads as correct, silently reopening Constraint 13 (removal is never refused) behind a
    /// name that says "admitting". A namespace `entry.ns` already carries an entry is a bug in the
    /// caller (Task 3's creation lifecycle owns checking that first) and surfaces as
    /// `encodeRefCatalog`'s own canonical-order/no-duplicate grammar check, inside
    /// `checkCatalogAdmission`.
    static RefCatalog casAdmitEntry(Backend & backend, const Layout & layout, const CatalogEntry & entry);
};

}
