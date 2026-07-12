#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <base/scope_guard.h>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
}

namespace ProfileEvents
{
    extern const Event CasPartFolderViewHits;
    extern const Event CasPartFolderViewMutableRefreshes;
    extern const Event CasPartFolderViewValidationMismatches;
    extern const Event CasPartFolderViewMisses;
    extern const Event CasPartFolderViewOversizedBypasses;
    extern const Event CasPartFolderViewInvalidations;
    extern const Event CasRefRollbackBestEffortDropFailed;
}

namespace CurrentMetrics
{
    extern const Metric CasPartFolderCacheBytes;
    extern const Metric CasPartFolderCacheEntries;
}

namespace DB::ContentAddressed
{

CachedPartFolderAccess::CachedPartFolderAccess(Cas::StorePtr store_)
    : CachedPartFolderAccess(std::move(store_), CacheParams{})
{
}

CachedPartFolderAccess::CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_)
    : store(std::move(store_)), params(params_)
{
    if (params.cache_bytes > 0)
        view_cache = std::make_unique<ViewCache>(
            "LRU", CurrentMetrics::CasPartFolderCacheBytes, CurrentMetrics::CasPartFolderCacheEntries,
            params.cache_bytes, params.max_entries, ViewCache::DEFAULT_SIZE_RATIO);
}

std::shared_ptr<const PartFolderView>
CachedPartFolderAccess::getView(const PartRefKey & key, Freshness freshness) const
{
    /// Step 1 (spec §Validate-On-Hit): the SAME resolve every read already pays today. Absence is
    /// never retained.
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;

    /// Step 2: retained views serve ONLY CachedForLoad. ForceFresh must re-prove the manifest BODY
    /// (a fresh ref resolve proves ref currency, not body existence — review 2026-07-08);
    /// StrictValidate bypasses retention entirely.
    if (freshness == Freshness::CachedForLoad && view_cache)
    {
        if (auto cached = view_cache->get(key.cacheKey()))
        {
            if (cached->manifestId() == resolved->manifest_id)
            {
                if (cached->mutableFiles() == resolved->mutable_files)
                {
                    ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                    recordDecision(key, LastDecision::Hit, cached.get(), /*retained=*/true);
                    return cached;
                }
                /// 2b: manifest unchanged, mutable-only drift (txn_version bumps) — clone around
                /// the SAME shared decode; no manifest operation at all.
                auto refreshed = std::make_shared<PartFolderView>(
                    key, resolved->manifest_id, resolved->manifest_size,
                    resolved->published_at_ms, resolved->mutable_files, cached->manifest());
                if (refreshed->estimatedBytes() <= params.max_entry_bytes)
                    view_cache->set(key.cacheKey(), refreshed);
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewMutableRefreshes);
                recordDecision(key, LastDecision::MutableRefresh, refreshed.get(), /*retained=*/true);
                return refreshed;
            }
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewValidationMismatches);
            /// fall through to rebuild — the stale entry is superseded by the insert below
        }
    }

    auto view = buildView(key, *resolved, freshness);

    /// Step 4: retain (StrictValidate never populates; oversized views are served, not retained).
    /// `oversized` is tracked SEPARATELY from `retained`: with retention disabled (view_cache ==
    /// nullptr), `retained` is also false, but that is not an oversized bypass — it is the ordinary
    /// disabled-mode miss the Phase 3 baseline pins (deviation from the plan's draft, which
    /// conflated `!retained` with oversized and mis-recorded every disabled-mode CachedForLoad
    /// miss as OversizedBypass).
    bool retained = false;
    bool oversized = false;
    if (freshness != Freshness::StrictValidate && view_cache)
    {
        if (view->estimatedBytes() <= params.max_entry_bytes)
        {
            /// CacheBase stores mutable pointers; views are logically const (never mutated).
            view_cache->set(key.cacheKey(), std::const_pointer_cast<PartFolderView>(view));
            retained = true;
        }
        else
        {
            oversized = true;
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewOversizedBypasses);
        }
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses);
    recordDecision(key,
        freshness == Freshness::CachedForLoad ? (oversized ? LastDecision::OversizedBypass : LastDecision::Miss)
        : freshness == Freshness::ForceFresh  ? LastDecision::ForceFreshRead
                                              : LastDecision::StrictBypass,
        view.get(), retained);
    return view;
}

std::shared_ptr<const PartFolderView> CachedPartFolderAccess::buildView(
    const PartRefKey & key, const Cas::Resolved & resolved, Freshness freshness) const
{
    /// Fresh modes must not coalesce onto another caller's read (each ForceFresh/StrictValidate
    /// call owns its mandatory HEAD); only cold CachedForLoad builds single-flight.
    if (freshness != Freshness::CachedForLoad)
        return PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id));

    std::promise<std::shared_ptr<const PartFolderView>> promise;
    std::shared_future<std::shared_ptr<const PartFolderView>> future;
    bool leader = false;
    {
        std::lock_guard lock(inflight_mutex);
        if (auto it = inflight.find(key.cacheKey()); it != inflight.end())
            future = it->second;                          /// follower: share the leader's build
        else
        {
            leader = true;
            future = promise.get_future().share();
            inflight.emplace(key.cacheKey(), future);
        }
    }
    if (!leader)
        return future.get();                              /// rethrows the leader's failure, if any

    SCOPE_EXIT({
        std::lock_guard lock(inflight_mutex);
        inflight.erase(key.cacheKey());
    });
    try
    {
        auto view = PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id));
        promise.set_value(view);
        return view;
    }
    catch (...)
    {
        promise.set_exception(std::current_exception());  /// followers see the leader's failure
        throw;
    }
}

void CachedPartFolderAccess::eraseView(const PartRefKey & key)
{
    if (view_cache)
        view_cache->remove(key.cacheKey());
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr, /*retained=*/false);
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
    eraseView(key);
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
            updateMutableFiles(dst, [&](Cas::RefMutableFilesUpdate & payload)
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

void CachedPartFolderAccess::updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RefMutableFilesUpdate &)> mutator)
{
    store->updateRefPayload(key.ns, key.ref, std::move(mutator));
    eraseView(key);
}

void CachedPartFolderAccess::dropRef(const PartRefKey & key)
{
    store->dropRef(key.ns, key.ref);
    eraseView(key);
}

void CachedPartFolderAccess::dropRefIfPresent(const PartRefKey & key)
{
    /// resolveRef gates the common case (a tmp ref that was never committed is a no-op, not an
    /// error); dropRef re-reads the shard inside its own CAS loop, so a concurrent drop can land in
    /// the window between our resolve and that re-read — surfacing as FILE_DOESNT_EXIST. Removal is
    /// replay-safe, so an already-gone ref is success; any other error still propagates. (Moved
    /// verbatim from ContentAddressedTransaction.) Cheap and harmless to also erase the view on the
    /// early-return absent path (Phase 4).
    if (!store->resolveRef(key.ns, key.ref, /*allow_stale=*/true))
    {
        eraseView(key);
        return;
    }
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
            throw;
        eraseView(key);
        return;   /// raced away between the gate and dropRef — nothing was actually dropped here
    }
    eraseView(key);
}

void CachedPartFolderAccess::dropRefBestEffort(const PartRefKey & key) noexcept
{
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (...)
    {
        /// Best-effort destructor/rollback cleanup: debris is GC-reclaimed, never a masked throw. But
        /// NEVER silent — unlike every other swallow in the feature this had no log trail, so a
        /// correlated backend outage during rollback could leave a permanently-live phantom ref with
        /// no diagnostic (A9). Log + count it as a countable anomaly.
        ProfileEvents::increment(ProfileEvents::CasRefRollbackBestEffortDropFailed);
        tryLogCurrentException(getLogger("CachedPartFolderAccess"),
            fmt::format("CA best-effort rollback dropRef failed (ns={} ref={}); the ref may remain live",
                        key.ns.string(), key.ref));
    }
    /// eraseView(key) deliberately ALSO on the swallowed-failure path (spec §Two-Level API): in this
    /// destructor/rollback context the ref's durable state is unknown, so dropping the view is the
    /// conservative direction.
    eraseView(key);
}

void CachedPartFolderAccess::dropNamespace(const Cas::RootNamespace & ns)
{
    store->dropNamespace(ns);
    if (view_cache)
    {
        const String prefix = ns.string() + '\0';
        view_cache->remove([&](const String & k, const auto &) { return k.starts_with(prefix); });
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
}

void CachedPartFolderAccess::recordDecision(const PartRefKey & key, LastDecision decision,
                                            const PartFolderView * view, bool retained) const
{
    std::lock_guard lock(explain_mutex);
    if (explain_map.size() >= EXPLAIN_MAX_ENTRIES)
        explain_map.clear();
    auto & e = explain_map[key.cacheKey()];
    e.last_decision = decision;
    e.retained = retained;
    if (view)
    {
        e.manifest_ref = Cas::manifestRefDebugString(view->manifestId().ref);
        e.estimated_bytes = view->estimatedBytes();
    }
}

CachedPartFolderAccess::ExplainResult CachedPartFolderAccess::explain(const PartRefKey & key) const
{
    ExplainResult result;
    {
        std::lock_guard lock(explain_mutex);
        const auto it = explain_map.find(key.cacheKey());
        if (it != explain_map.end())
            result = it->second;
    }
    /// `retained` is reported LIVE against the cache, not from the decision snapshot: `dropNamespace`
    /// erases every key of a namespace via one CacheBase::remove(predicate) sweep without a per-key
    /// recordDecision call (nothing enumerates the removed keys cheaply — spec §Observability), so a
    /// snapshot value would go stale for every key it touches except the one last read. A live
    /// membership check is authoritative for every eraser (write-through or namespace-wide) and
    /// costs one more CacheBase lookup on this test/log-only path.
    result.retained = view_cache && view_cache->get(key.cacheKey()) != nullptr;
    return result;
}

void CachedPartFolderAccess::clearForTest()
{
    std::lock_guard lock(explain_mutex);
    explain_map.clear();
    if (view_cache)
        view_cache->clear();
}

}
