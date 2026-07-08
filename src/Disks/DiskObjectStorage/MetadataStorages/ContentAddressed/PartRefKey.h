#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>

namespace DB::ContentAddressed
{

/// The stable identity of a committed part or projection folder (spec
/// 2026-07-08-cas-part-folder-cache §PartRefKey): owning root namespace + ref name
/// ("<part>" or "detached/<part>", B181 fold).
struct PartRefKey
{
    Cas::RootNamespace ns{""};
    String ref;

    bool operator==(const PartRefKey & o) const { return ns.string() == o.ns.string() && ref == o.ref; }

    /// Canonical map key. '\0' cannot occur in namespace strings or ref names (both derive from
    /// disk paths), so the join is unambiguous even though refs may contain '/'.
    String cacheKey() const { return ns.string() + '\0' + ref; }
};

/// Read-freshness policy at the part-folder access boundary (spec §Freshness). The
/// mutable-read-vs-write-evidence distinction is carried by the METHOD, not a fourth value:
/// mutable per-part reads call `resolve` (no manifest involved); write-path source reads call
/// `getView`, which under ForceFresh always re-proves the manifest body (mandatory HEAD in
/// `readManifestShared` — a fresh ref resolve alone proves ref currency, NOT body existence).
enum class Freshness
{
    CachedForLoad,   /// repeated load-window reads; stale-tolerant resolve (allow_stale=true)
    ForceFresh,      /// mutable per-part reads and write-path source reads; resolve fresh
    StrictValidate,  /// fsck/debug: bypass retained views entirely; fresh resolve + validated read
};

}
