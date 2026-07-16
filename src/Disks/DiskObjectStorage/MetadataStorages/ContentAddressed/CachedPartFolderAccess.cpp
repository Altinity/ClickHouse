#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Common/DateLUT.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <base/scope_guard.h>
#include <chrono>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
}

namespace ProfileEvents
{
    extern const Event CasPartFolderViewHits;
    extern const Event CasPartFolderViewValidationMismatches;
    extern const Event CasPartFolderViewMisses;
    extern const Event CasPartFolderViewOversizedBypasses;
    extern const Event CasPartFolderViewInvalidations;
    extern const Event CasRefRollbackBestEffortDropFailed;
    extern const Event CasPartFolderValidateSkipped;
    extern const Event CasRefRepoint;
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

CachedPartFolderAccess::CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_, std::function<uint64_t()> now_ms_fn_)
    : store(std::move(store_)), params(params_), now_ms_fn(std::move(now_ms_fn_))
{
    if (!now_ms_fn)
        now_ms_fn = []() -> uint64_t { return timeInMilliseconds(std::chrono::system_clock::now()); };
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

    /// One cache-key materialization per getView (B2): reused by the retained get/set and the journal.
    const String cache_key = key.cacheKey();

    /// Step 2: retained views serve ONLY CachedForLoad. ForceFresh must re-prove the manifest BODY
    /// (a fresh ref resolve proves ref currency, not body existence — review 2026-07-08);
    /// StrictValidate bypasses retention entirely.
    if (freshness == Freshness::CachedForLoad && view_cache)
    {
        if (auto cached = view_cache->get(cache_key))
        {
            if (cached->manifestId() == resolved->manifest_id)
            {
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                recordDecision(cache_key, LastDecision::Hit, cached.get(), /*retained=*/true);
                return cached;
            }
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewValidationMismatches);
            /// fall through to rebuild — the stale entry is superseded by the insert below
        }
    }

    /// §3 (part_folder_validate): ForceFresh may serve a retained view WITHOUT the mandatory body
    /// HEAD when the mode permits -- the ref currency is proven by `resolve` above; only the
    /// INV-NO-DANGLE body re-proof is skipped. StrictValidate never enters here (it bypasses
    /// retention, spec §Validate-On-Hit). A manifest_id mismatch still falls through to rebuild below
    /// -- a genuine change under a retained view is caught exactly like CachedForLoad's mismatch path
    /// above. All-tree-part-files Task 9: the former `mutableFiles()` comparison is gone -- every
    /// content change is now a manifest change (`repointRef`), so `manifestId()` alone proves currency.
    if (freshness == Freshness::ForceFresh && view_cache && params.validate.mode != PartFolderValidate::Mode::Always)
    {
        if (auto cached = view_cache->get(cache_key);
            cached && cached->manifestId() == resolved->manifest_id)
        {
            const bool fresh_enough = params.validate.mode == PartFolderValidate::Mode::Never
                || (now_ms_fn() - cached->validatedAtMs()) < params.validate.age_seconds * 1000ULL;
            if (fresh_enough)
            {
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                ProfileEvents::increment(ProfileEvents::CasPartFolderValidateSkipped);
                recordDecision(cache_key, LastDecision::Hit, cached.get(), /*retained=*/true);
                return cached;
            }
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
            view_cache->set(cache_key, std::const_pointer_cast<PartFolderView>(view));
            retained = true;
        }
        else
        {
            oversized = true;
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewOversizedBypasses);
        }
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses);
    recordDecision(cache_key,
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
        return PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id), now_ms_fn());

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
        auto view = PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id), now_ms_fn());
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
    const String cache_key = key.cacheKey();
    if (view_cache)
        view_cache->remove(cache_key);
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(cache_key, LastDecision::Invalidated, nullptr, /*retained=*/false);
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
                                          const Cas::ManifestId & manifest_id, bool allow_repoint)
{
    build.promote(key.ns, key.ref, build_id, manifest_id, allow_repoint);
    eraseView(key);
}

void CachedPartFolderAccess::publishEntries(const PartRefKey & dst,
    const std::vector<Cas::ManifestEntry> & entries, Cas::ProvenanceOp op, bool allow_repoint)
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
    promoteBuild(*build, dst, build->buildId(), id, allow_repoint);
}

bool CachedPartFolderAccess::republishRef(const PartRefKey & src, const PartRefKey & dst)
{
    /// Move a COMMITTED ref (rev. 15 §republish): content addressing has no rename. Force-fresh
    /// source read; readManifestShared's mandatory HEAD re-proves the source body (write evidence is
    /// never a cached view).
    auto resolved = store->resolveRef(src.ns, src.ref);
    if (!resolved)
        return false;
    const auto src_manifest = store->readManifestShared(resolved->manifest_id);

    /// BUG 1c: idempotent re-drive. If dst is ALREADY committed, the prior attempt's promote landed
    /// and only dropRef(src) was interrupted. Compare CONTENT (path-sorted `entries`, not the whole
    /// manifest — ref/namespace/digest legitimately differ): same content => finish the rename by
    /// dropping src; a different-content dst is a genuine conflict => fail closed (never silently
    /// drop src's content). All-tree-part-files Task 9: the former mutable_files re-sync on this path
    /// is gone -- every per-part file is an ordinary entry now, so the `entries` compare above already
    /// covers it (a drifted `metadata_version.txt` etc. IS a content difference, caught by the throw).
    if (auto dst_resolved = store->resolveRef(dst.ns, dst.ref))
    {
        const auto dst_manifest = store->readManifestShared(dst_resolved->manifest_id);
        if (dst_manifest->entries != src_manifest->entries)
            throw Exception(ErrorCodes::ABORTED,
                "republishRef: destination '{}' is already committed with different content — refusing "
                "(rename/attach conflict)", dst.ns.string() + "/" + dst.ref);
        dropRef(src);
        return true;
    }

    publishEntries(dst, src_manifest->entries, Cas::ProvenanceOp::Other);
    dropRef(src);
    return true;
}

bool CachedPartFolderAccess::repointRef(const PartRefKey & key, std::vector<Cas::ManifestEntry> entries, Cas::ProvenanceOp op)
{
    /// Byte-equal no-op: compare the candidate `entries` against the CURRENTLY committed manifest's
    /// decoded entries. This must NOT stage a candidate manifest first: `stageManifest` mints a
    /// non-content-derived `ManifestRef` (epoch/build_seq/ordinal) AND durably PUTs the encoded body
    /// on every call (CasBuild.cpp), so staging-then-comparing IDs would itself be a pool mutation on
    /// the byte-equal path — violating the "ZERO pool mutations" contract this primitive exists to
    /// provide.
    ///
    /// codecs-v3 phase 6: the comparison must be SYMMETRIC. `committed_manifest->entries` already went
    /// through one `decodePartManifest` round-trip, which does not carry `blob_size` for Inline
    /// entries on the wire (it is redundant with `inline_bytes.size()`, use `ManifestEntry::size()` for
    /// the logical size instead, and it is excluded from both the canonical encoding and the payload
    /// digest — see `CasPartManifestFormat.cpp`'s `writeEntryRecord`/`decodePartManifest`). The
    /// freshly-constructed `entries` are now built the same way (the inline write path no longer sets
    /// `blob_size`), but a straight struct compare against them is still not guaranteed byte-identical
    /// (canonical path ordering, etc.), so route the candidate through the identical encode/decode
    /// round-trip before comparing regardless — mirrors `republishRef`'s BUG 1c compare just above,
    /// which is symmetric for the same reason (both sides there are already decoded).
    auto resolved = resolve(key, Freshness::ForceFresh);
    if (resolved)
    {
        const auto committed_manifest = store->readManifestShared(resolved->manifest_id);
        Cas::PartManifest probe;
        probe.ref = committed_manifest->ref;
        probe.root_namespace_id = committed_manifest->root_namespace_id;
        probe.entries = entries;
        probe.payload_digest = Cas::computePayloadDigest(probe);
        const Cas::PartManifest canonical_candidate = Cas::decodePartManifest(Cas::encodePartManifest(probe));
        if (committed_manifest->entries == canonical_candidate.entries)
            return false;
    }
    /// `publishEntries` takes `entries` by const&, so it is read here after the call without issue.
    publishEntries(key, entries, op, /*allow_repoint=*/true);
    ProfileEvents::increment(ProfileEvents::CasRefRepoint);
    if (resolved)
    {
        /// Post-all-tree, a standalone write/remove on a committed part (Task 4's committed-file
        /// write, Task 8's removal-mark resolution, Task 9's uuid.txt/metadata_version.txt/
        /// txn_version.txt fill-ins) resolves through repoint routinely — it is the designed
        /// mechanism, not an anomaly. WARNING here trained operators to ignore it (it fires on
        /// every ordinary DROP/REPLACE/ATTACH/freeze); DEBUG keeps the trail without the noise. The
        /// counter stays unconditional — it is the operator-facing signal now.
        LOG_DEBUG(getLogger("CachedPartFolderAccess"),
            "Repointed committed ref {}/{} ({} entries) — standalone write/remove on a committed part",
            key.ns.string(), key.ref, entries.size());
    }
    else
    {
        /// `!resolved` means this call repointed a key with no prior committed ref to repoint —
        /// unreachable today (every caller only invokes repointRef on an already-resolving key; see
        /// BACKLOG "repointRef non-resolving-key audit gap"), so if it ever fires it is a genuine
        /// anomaly, not the routine case above. Stays at WARNING.
        LOG_WARNING(getLogger("CachedPartFolderAccess"),
            "repointRef published {}/{} ({} entries) with no prior committed ref to repoint — "
            "unexpected call shape (see BACKLOG repointRef non-resolving-key audit gap)",
            key.ns.string(), key.ref, entries.size());
    }
    return true;
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

void CachedPartFolderAccess::recordDecision(const String & cache_key, LastDecision decision,
                                            const PartFolderView * view, bool retained) const
{
    if (!params.explain_enabled)
        return;   /// B2: the hit path pays neither the per-disk mutex nor a journal allocation.
    std::lock_guard lock(explain_mutex);
    if (explain_map.size() >= EXPLAIN_MAX_ENTRIES)
        explain_map.clear();
    auto & e = explain_map[cache_key];
    e.last_decision = decision;
    e.retained = retained;
    if (view)
    {
        e.manifest_ref = Cas::manifestRefDebugString(view->manifestId().ref);
        e.estimated_bytes = view->estimatedBytes();
    }
}

size_t CachedPartFolderAccess::explainJournalSizeForTest() const
{
    std::lock_guard lock(explain_mutex);
    return explain_map.size();
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
