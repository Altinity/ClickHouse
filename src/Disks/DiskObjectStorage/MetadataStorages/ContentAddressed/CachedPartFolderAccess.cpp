#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Common/ProfileEvents.h>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
}

namespace ProfileEvents
{
    extern const Event CasPartFolderViewMisses;
    extern const Event CasPartFolderViewInvalidations;
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
    auto view = PartFolderView::make(key, *resolved, store->readManifestShared(resolved->manifest_id));
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses);
    recordDecision(key,
        freshness == Freshness::CachedForLoad ? LastDecision::Miss
        : freshness == Freshness::ForceFresh ? LastDecision::ForceFreshRead
        : LastDecision::StrictBypass,
        view.get());
    return view;
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
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr);
}

void CachedPartFolderAccess::publishEntries(const PartRefKey & dst,
    const std::vector<Cas::ManifestEntry> & entries, std::map<String, String> mutable_files, Cas::ProvenanceOp op)
{
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = dst.ns.string() + "/" + dst.ref,
                                                  .intended_namespace = dst.ns, .op = op});
    /// Tokenless W-EVIDENCE dep per entry — NO pool HEAD/GET before precommit; promote re-proves
    /// each fail-closed. Inline entries record nothing (adoptEvidence skips them).
    for (const auto & entry : entries)
        build->adoptEvidence(entry);
    /// A FRESH dst manifest over the SAME entries (only blobs are content-addressed; a part is a
    /// single-owner ManifestId, so dst gets its own id), then move ownership in.
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(dst.ns, dst.ref, id);
    promoteBuild(*build, dst, build->buildId(), id, std::move(mutable_files));
}

bool CachedPartFolderAccess::republishRef(const PartRefKey & src, const PartRefKey & dst)
{
    /// Move a COMMITTED ref (rev. 15 §republish): content addressing has no rename. Force-fresh
    /// source read (RENAME/move: stale mutable_files must not carry to dst); readManifestShared's
    /// mandatory HEAD re-proves the source body (write evidence is never a cached view).
    auto resolved = store->resolveRef(src.ns, src.ref);
    if (!resolved)
        return false;
    const auto src_manifest = store->readManifestShared(resolved->manifest_id);

    /// BUG 1c: idempotent re-drive. If dst is ALREADY committed, the prior attempt's promote landed
    /// and only dropRef(src) was interrupted. Compare CONTENT (path-sorted `entries`, not the whole
    /// manifest — ref/namespace/digest legitimately differ): same content => finish the rename by
    /// dropping src; a different-content dst is a genuine conflict => fail closed (never silently
    /// drop src's content). `mutable_files` is NOT part of the idempotency key and can have drifted
    /// on src between the crashed promote(dst) and this re-drive — re-sync it onto dst.
    if (auto dst_resolved = store->resolveRef(dst.ns, dst.ref))
    {
        const auto dst_manifest = store->readManifestShared(dst_resolved->manifest_id);
        if (dst_manifest->entries != src_manifest->entries)
            throw Exception(ErrorCodes::ABORTED,
                "republishRef: destination '{}' is already committed with different content — refusing "
                "(rename/attach conflict)", dst.ns.string() + "/" + dst.ref);
        if (dst_resolved->mutable_files != resolved->mutable_files)
        {
            const std::map<String, String> current_mutable_files = resolved->mutable_files;
            updateMutableFiles(dst, [&](Cas::RootRef & payload)
            {
                payload.mutable_files = current_mutable_files;
            });
        }
        dropRef(src);
        return true;
    }

    /// Mutable files carry over (a rename is not a new part). promote stamps the dst publish clock.
    publishEntries(dst, src_manifest->entries, resolved->mutable_files, Cas::ProvenanceOp::Other);
    dropRef(src);
    return true;
}

void CachedPartFolderAccess::updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator)
{
    store->updateRefPayload(key.ns, key.ref, std::move(mutator));
    /// Phase 4: eraseView(key)
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr);
}

void CachedPartFolderAccess::dropRef(const PartRefKey & key)
{
    store->dropRef(key.ns, key.ref);
    /// Phase 4: eraseView(key)
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr);
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
        return;   /// raced away between the gate and dropRef — nothing was actually dropped here
    }
    /// Phase 4: eraseView(key)
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr);
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
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr);
}

void CachedPartFolderAccess::dropNamespace(const Cas::RootNamespace & ns)
{
    store->dropNamespace(ns);
    /// Phase 4: erase every view whose key is in `ns`. Nothing enumerates the namespace's keys
    /// cheaply until then, so per-key `recordDecision` is skipped here (spec §Observability).
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
}

void CachedPartFolderAccess::recordDecision(const PartRefKey & key, LastDecision decision,
                                            const PartFolderView * view) const
{
    std::lock_guard lock(explain_mutex);
    if (explain_map.size() >= EXPLAIN_MAX_ENTRIES)
        explain_map.clear();
    auto & e = explain_map[key.cacheKey()];
    e.last_decision = decision;
    if (view)
    {
        e.manifest_ref = Cas::manifestRefDebugString(view->manifestId().ref);
        e.estimated_bytes = view->estimatedBytes();
    }
    /// `retained` stays false until Phase 4 sets it on insert/erase.
}

CachedPartFolderAccess::ExplainResult CachedPartFolderAccess::explain(const PartRefKey & key) const
{
    std::lock_guard lock(explain_mutex);
    const auto it = explain_map.find(key.cacheKey());
    return it == explain_map.end() ? ExplainResult{} : it->second;
}

void CachedPartFolderAccess::clearForTest()
{
    std::lock_guard lock(explain_mutex);
    explain_map.clear();
}

}
