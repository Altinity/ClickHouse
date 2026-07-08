#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
}

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

void CachedPartFolderAccess::promoteBuild(Cas::Build & build, const PartRefKey & key, UInt128 build_id,
                                          const Cas::ManifestId & manifest_id, std::map<String, String> mutable_files)
{
    build.setPendingMutableFiles(std::move(mutable_files));
    build.promote(key.ns, key.ref, build_id, manifest_id);
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator)
{
    store->updateRefPayload(key.ns, key.ref, std::move(mutator));
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRef(const PartRefKey & key)
{
    store->dropRef(key.ns, key.ref);
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRefIfPresent(const PartRefKey & key)
{
    /// resolveRef gates the common case (a tmp ref that was never committed is a no-op, not an
    /// error); dropRef re-reads the shard inside its own CAS loop, so a concurrent drop can land in
    /// the window between our resolve and that re-read — surfacing as FILE_DOESNT_EXIST. Removal is
    /// replay-safe, so an already-gone ref is success; any other error still propagates. (Moved
    /// verbatim from ContentAddressedTransaction.)
    if (!store->resolveRef(key.ns, key.ref, /*allow_stale=*/true))
        return;
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
            throw;
    }
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRefBestEffort(const PartRefKey & key) noexcept
{
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Best-effort destructor/rollback cleanup: debris is GC-reclaimed, never a masked throw.
    }
    /// Phase 4: eraseView(key) — deliberately ALSO on the swallowed-failure path (spec §Two-Level API).
}

void CachedPartFolderAccess::dropNamespace(const Cas::RootNamespace & ns)
{
    store->dropNamespace(ns);
    /// Phase 4: erase every view whose key is in `ns`
}

}
