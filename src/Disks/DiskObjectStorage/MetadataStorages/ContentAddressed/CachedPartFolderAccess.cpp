#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>

namespace DB::ContentAddressed
{

std::shared_ptr<const PartFolderView>
CachedPartFolderAccess::getView(const PartRefKey & key, Freshness freshness) const
{
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;   /// absence is never cached
    /// Fail-closed exactly as before: a live ref naming a missing body throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE surfaced); corrupt bodies throw CORRUPTED_DATA and are never cached.
    return PartFolderView::make(key, *resolved, store->readManifestShared(resolved->manifest_id));
}

std::optional<Cas::Resolved>
CachedPartFolderAccess::resolve(const PartRefKey & key, Freshness freshness) const
{
    return store->resolveRef(key.ns, key.ref, /*allow_stale=*/freshness == Freshness::CachedForLoad);
}

bool CachedPartFolderAccess::existsRef(const PartRefKey & key, Freshness freshness) const
{
    return resolve(key, freshness).has_value();
}

}
