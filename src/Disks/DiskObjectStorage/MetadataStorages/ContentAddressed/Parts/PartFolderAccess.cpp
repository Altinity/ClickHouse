#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <base/defines.h>
#include <algorithm>
#include <unordered_set>

namespace DB::ContentAddressed
{

PartFolderView::PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                               uint64_t published_at_ms_,
                               std::shared_ptr<const Cas::PartManifest> manifest_, uint64_t validated_at_ms_)
    : key(std::move(key_))
    , manifest_id(std::move(manifest_id_))
    , manifest_size(manifest_size_)
    , published_at_ms(published_at_ms_)
    , manifest_body(std::move(manifest_))
    , validated_at_ms(validated_at_ms_)
{
    chassert(manifest_body);
    /// The binary-search contract: entries must be strictly ascending by `path` (sorted and unique) —
    /// `decodePartManifest` enforces exactly this for every decoded body, and `findEntry`'s binary
    /// search assumes uniqueness. `adjacent_find` with `!(a.path < b.path)` flags any adjacent pair
    /// that is out-of-order OR duplicate (stronger than `is_sorted`, which permits duplicates); a
    /// hand-constructed manifest (tests) must honor it too.
    chassert(std::adjacent_find(manifest_body->entries.begin(), manifest_body->entries.end(),
        [](const Cas::ManifestEntry & a, const Cas::ManifestEntry & b) { return !(a.path < b.path); })
        == manifest_body->entries.end());
}

std::shared_ptr<const PartFolderView> PartFolderView::make(
    PartRefKey key, const Cas::Resolved & resolved, std::shared_ptr<const Cas::PartManifest> manifest,
    uint64_t validated_at_ms)
{
    return std::make_shared<const PartFolderView>(
        std::move(key), resolved.manifest_id, resolved.manifest_size,
        resolved.published_at_ms, std::move(manifest), validated_at_ms);
}

std::optional<std::string> PartFolderView::projectionDirPrefix(const std::string & file)
{
    if (file.empty())
        return std::nullopt;
    const auto last_slash = file.find_last_of('/');
    const std::string_view last_component
        = last_slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(last_slash + 1);
    if (last_component.ends_with(".proj") || last_component.ends_with(".tmp_proj"))
        return file + "/";
    return std::nullopt;
}

const Cas::ManifestEntry * PartFolderView::findFile(const String & path) const
{
    return Cas::findEntry(manifest_body->entries, path);
}

bool PartFolderView::hasFile(const String & path) const
{
    return findFile(path) != nullptr;
}

std::optional<uint64_t> PartFolderView::fileSize(const String & path) const
{
    if (const auto * e = findFile(path))
        return e->size();
    return std::nullopt;
}

std::optional<String> PartFolderView::inlineBytes(const String & path) const
{
    const auto * e = findFile(path);
    if (e && e->placement == Cas::EntryPlacement::Inline)
        return e->inline_bytes;
    return std::nullopt;
}

std::vector<String> PartFolderView::listChildren(const String & dir_prefix) const
{
    /// Collapse each entry to its first child component. Projection folders are structurally flat in
    /// `MergeTree`, so this produces the same names as the old projection-specific path handling while
    /// keeping one directory-listing rule for all manifest folders.
    std::unordered_set<String> names;
    auto add = [&](const String & full)
    {
        if (!full.starts_with(dir_prefix) || full.size() <= dir_prefix.size())
            return;
        const std::string_view rest = std::string_view(full).substr(dir_prefix.size());
        const auto slash = rest.find('/');
        names.emplace(slash == std::string_view::npos ? rest : rest.substr(0, slash));
    };
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    for (const auto * e = first; e != last; ++e)
        add(e->path);
    return {std::make_move_iterator(names.begin()), std::make_move_iterator(names.end())};
}

bool PartFolderView::hasDirectory(const String & dir_prefix) const
{
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    return first != last;
}

size_t PartFolderView::estimatedBytes() const
{
    /// Conservative cache weight: fixed overhead plus `manifest_size`. This deliberately over-counts
    /// the shared decode, which is safe because eviction should happen before the budget is exceeded.
    return 256 + manifest_size;
}

}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
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

CachedPartFolderAccess::CachedPartFolderAccess(Cas::PoolPtr store_)
    : CachedPartFolderAccess(std::move(store_), CacheParams{})
{
}

CachedPartFolderAccess::CachedPartFolderAccess(Cas::PoolPtr store_, CacheParams params_, std::function<uint64_t()> now_ms_fn_)
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
    /// Resolve first on every access. Absence is never retained, and the same ref-resolution result
    /// supplies the manifest ID used to validate a retained view.
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;

    /// Reuse one canonical key for the retained view and the optional diagnostic journal.
    const String cache_key = key.cacheKey();

    /// Retained views serve `CachedForLoad` directly only after their manifest ID matches the fresh
    /// resolve. `ForceFresh` must re-prove the manifest body unless the configured validation policy
    /// explicitly permits a recent retained view; a fresh ref resolve proves ref currency, not body existence.
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
            /// Rebuild below; the stale entry is superseded by the new view when retention is enabled.
        }
    }

    /// With a non-`Always` validation policy, `ForceFresh` may serve a retained view without another
    /// body HEAD when its manifest ID still matches and its validation timestamp is within the age
    /// policy. `StrictValidate` bypasses retention. A manifest-ID mismatch always rebuilds, because all
    /// part content is represented by the manifest.
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

    /// Retain eligible views. `StrictValidate` never populates the cache, and oversized views are
    /// served but not retained.
    /// `oversized` is tracked separately from `retained`: with retention disabled (`view_cache ==
    /// nullptr`), `retained` is also false, but that is an ordinary disabled-mode miss rather than
    /// an oversized bypass.
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
    /// Fresh modes do not coalesce: each `ForceFresh`/`StrictValidate` call owns its mandatory HEAD.
    /// Only cold `CachedForLoad` builds use single-flight.
    if (freshness != Freshness::CachedForLoad)
        return PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id), now_ms_fn());

    std::promise<std::shared_ptr<const PartFolderView>> promise;
    std::shared_future<std::shared_ptr<const PartFolderView>> future;
    bool leader = false;
    {
        std::lock_guard lock(inflight_mutex);
        if (auto it = inflight.find(key.cacheKey()); it != inflight.end())
            future = it->second;                          /// Follower: share the leader's build.
        else
        {
            leader = true;
            future = promise.get_future().share();
            inflight.emplace(key.cacheKey(), future);
        }
    }
    if (!leader)
        return future.get();                              /// Rethrows the leader's exception, if any.

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
        promise.set_exception(std::current_exception());  /// Followers see the leader's exception.
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

void CachedPartFolderAccess::promoteBuild(Cas::PartWriteTxn & build, const PartRefKey & key, UInt128 build_id,
                                          const Cas::ManifestId & manifest_id, bool allow_repoint)
{
    build.promote(key.ns, key.ref, build_id, manifest_id, allow_repoint);
    eraseView(key);
}

void CachedPartFolderAccess::publishEntries(const PartRefKey & dst,
    const std::vector<Cas::ManifestEntry> & entries, Cas::ProvenanceOp op, bool allow_repoint)
{
    auto build = store->beginPartWrite(Cas::PartWriteInfo{.intended_ref = dst.ns.string() + "/" + dst.ref,
                                                  .intended_namespace = dst.ns, .op = op});
    /// Record write evidence for each non-inline entry. No pool HEAD/GET is performed before
    /// precommit; the promote path re-proves each dependency fail-closed. Inline entries need no evidence.
    for (const auto & entry : entries)
        build->adoptEvidence(entry);
    /// Stage a fresh manifest over the same entries. Blobs are content-addressed, but each part owns
    /// its manifest ID, so `dst` receives a distinct manifest before ownership moves to it.
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(dst.ns, dst.ref, id);
    promoteBuild(*build, dst, build->buildId(), id, allow_repoint);
}

bool CachedPartFolderAccess::republishRef(const PartRefKey & src, const PartRefKey & dst)
{
    /// Content addressing has no rename, so move a committed ref by reading the source body freshly,
    /// publishing equivalent entries at the destination, and then dropping the source. The source
    /// body is re-proved and is never taken from a retained view.
    auto resolved = store->resolveRef(src.ns, src.ref);
    if (!resolved)
        return false;
    const auto src_manifest = store->readManifestShared(resolved->manifest_id);

    /// If `dst` is already committed, a previous attempt may have completed its promote before the
    /// source drop. Compare content rather than the whole manifest, whose ref/namespace/digest
    /// legitimately differ: equal content completes the move by dropping `src`; different content
    /// is a conflict and leaves the source intact.
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
    /// Compare the candidate `entries` against the currently committed manifest's
    /// decoded entries. This must NOT stage a candidate manifest first: `stageManifest` mints a
    /// non-content-derived `ManifestRef` (epoch/build_seq/ordinal) AND durably PUTs the encoded body
    /// on every call (CasPartWriteTxn.cpp), so staging-then-comparing IDs would itself be a pool mutation on
    /// the byte-equal path — violating the "ZERO pool mutations" contract this primitive exists to
    /// provide.
    ///
    /// The comparison must be symmetric. `committed_manifest->entries` already went
    /// through one `decodePartManifest` round-trip, which does not carry `blob_size` for Inline
    /// entries on the wire (it is redundant with `inline_bytes.size()`, use `ManifestEntry::size()` for
    /// the logical size instead, and it is excluded from both the canonical encoding and the payload
    /// digest — see `CasPartManifestFormat.cpp`'s `writeEntryRecord`/`decodePartManifest`). The
    /// freshly constructed `entries` may not have the same incidental fields (the inline write path no longer sets
    /// `blob_size`), but a straight struct compare against them is still not guaranteed byte-identical
    /// (canonical path ordering, etc.), so route the candidate through the identical encode/decode
    /// round-trip before comparing regardless — this is the same content comparison used by `republishRef`,
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
    /// `publishEntries` takes `entries` by const reference, so the caller-owned vector remains valid.
    publishEntries(key, entries, op, /*allow_repoint=*/true);
    ProfileEvents::increment(ProfileEvents::CasRefRepoint);
    if (resolved)
    {
        /// Repoint is the normal mechanism for effective standalone writes/removes on committed parts,
        /// so this routine event is logged at debug level while the counter remains an operator-facing signal.
        LOG_DEBUG(getLogger("CachedPartFolderAccess"),
            "Repointed committed ref {}/{} ({} entries) — standalone write/remove on a committed part",
            key.ns.string(), key.ref, entries.size());
    }
    else
    {
        /// A repoint normally targets an existing ref. Keep an unexpected create-shaped call visible
        /// at warning level rather than silently treating it as ordinary publication.
        LOG_WARNING(getLogger("CachedPartFolderAccess"),
            "repointRef published {}/{} ({} entries) with no prior committed ref to repoint — "
            "unexpected call shape (repointRef requires an existing committed ref)",
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
    /// resolveRef gates the common case (a temporary ref that was never committed is a no-op, not an
    /// error); dropRef re-reads the shard inside its own CAS loop, so a concurrent drop can land in
    /// the window between our resolve and that re-read — surfacing as FILE_DOESNT_EXIST. Removal is
    /// replay-safe, so an already-gone ref is success; any other exception still propagates. The view
    /// is also erased on the early-return absent path.
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
        return;   /// Raced away between the gate and dropRef; nothing was actually dropped here.
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
        /// Best-effort destructor/rollback cleanup: debris is GC-reclaimed, but swallowing the
        /// exception without a diagnostic could leave a live phantom ref after a backend outage.
        ProfileEvents::increment(ProfileEvents::CasRefRollbackBestEffortDropFailed);
        tryLogCurrentException(getLogger("CachedPartFolderAccess"),
            fmt::format("CA best-effort rollback dropRef failed (ns={} ref={}); the ref may remain live",
                        key.ns.string(), key.ref));
    }
    /// In destructor/rollback context the ref's durable state is unknown, so invalidate the view even
    /// after a swallowed cleanup exception.
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
        return;   /// Disabled diagnostics keep the read path free of journal locking and allocation.
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
    /// `retained` is reported live against the cache, not from the decision snapshot: `dropNamespace`
    /// erases every key of a namespace via one `CacheBase::remove` predicate sweep without a per-key
    /// `recordDecision` call, so a snapshot value would go stale for every key it touches except the
    /// one last read. A live membership check is authoritative for every eraser (write-through or
    /// namespace-wide) and costs one more `CacheBase` lookup on this test/log-only path.
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
