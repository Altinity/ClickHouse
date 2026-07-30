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
/// TOP of `casUpdate`/`casUpdateAdmitting`.
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
    /// This primitive runs NO admission check: Constraint 13 (removal is never refused) means
    /// whether a candidate must clear the additive predicate is the CALLER's decision, not this
    /// loop's. A caller mutating an entry's state without growing the catalog (a removal transition)
    /// uses this directly.
    static RefCatalog casUpdate(
        Backend & backend, const Layout & layout,
        const std::function<RefCatalog(const RefCatalog &)> & mutate);

    /// `casUpdate`, plus INV-3's two admission predicates run against `mutate`'s candidate before
    /// every attempt, naming `admitting_ns` in whichever predicate refuses. The path any
    /// entry-ADMITTING mutation must use. Never used for a removal transition (Constraint 13).
    static RefCatalog casUpdateAdmitting(
        Backend & backend, const Layout & layout, const RootNamespace & admitting_ns,
        const std::function<RefCatalog(const RefCatalog &)> & mutate);
};

}
