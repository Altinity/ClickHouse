#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/CacheBase.h>
#include <Common/CurrentMetrics.h>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB::Cas { class Build; }

namespace DB::ContentAddressed
{

/// The single facade for committed content-addressed part-folder access (spec
/// 2026-07-08-cas-part-folder-cache). Reads build immutable `PartFolderView`s; committed part-ref
/// mutations are facade methods so cache effects are write-through, never a caller responsibility.
/// Phase 4: a bounded retained-view map (`Common/CacheBase`) is consulted for `CachedForLoad` and
/// validated against every fresh resolve (§The Validate-On-Hit Protocol) — `cache_bytes == 0`
/// (`CacheParams{}`, the unit-test default) keeps the Phase-2/3 call graph byte-identical.
/// Thread-safe; shared by all readers and transactions of one disk.
class CachedPartFolderAccess
{
public:
    /// Retention knobs (spec §Cache State And Memory Bound). `cache_bytes == 0` (the unit-test
    /// default) disables retention entirely — the disk factory default is 64 MiB.
    struct CacheParams
    {
        uint64_t cache_bytes = 0;            /// 0 = retention disabled (unit-test default;
                                             /// the DISK default is 64 MiB, set in the factory)
        uint64_t max_entries = 10000;
        uint64_t max_entry_bytes = 16ULL << 20;
    };

    /// NOTE: `CacheParams params_ = {}` cannot be a default ARGUMENT here — Clang's complete-class-
    /// context rule requires the enclosing class (`CachedPartFolderAccess`) to be complete before a
    /// nested class's (`CacheParams`) default member initializers can be evaluated, and a default
    /// argument written inside the class body is evaluated too early. Two overloads sidestep it; the
    /// single-arg form default-constructs `CacheParams` (retention disabled) out-of-line.
    explicit CachedPartFolderAccess(Cas::StorePtr store_);
    CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_);

    /// Resolve + validated manifest read, joined into a view. nullptr = the ref is absent.
    /// EVERY mode re-proves the manifest body via `readManifestShared`'s mandatory HEAD in this
    /// phase; a fresh ref resolve alone proves ref currency, NOT body existence (review 2026-07-08).
    std::shared_ptr<const PartFolderView> getView(const PartRefKey & key, Freshness freshness) const;

    /// Ref-only resolution (mutable per-part reads, part-dir existence, publish stamps): no
    /// manifest is read. `CachedForLoad` = stale-tolerant; other modes force-fresh.
    std::optional<Cas::Resolved> resolve(const PartRefKey & key, Freshness freshness) const;
    bool existsRef(const PartRefKey & key, Freshness freshness) const;

    /// ==== committed part-ref writes (spec §Two-Level API, level 1) ====
    /// Each primitive performs the protocol operation and owns the cache side effect (Phase 4:
    /// erase the affected view on success; on exception cache state is untouched — except
    /// dropRefBestEffort, which erases even on a swallowed failure: in its destructor/rollback
    /// context the ref's durable state is unknown, so dropping the view is the conservative
    /// direction). Committed-ref mutations anywhere else in wiring are style-check failures.

    /// The transaction's terminal publish: pending mutable payload + the atomic owner move.
    void promoteBuild(Cas::Build & build, const PartRefKey & key, UInt128 build_id,
                      const Cas::ManifestId & manifest_id, std::map<String, String> mutable_files);
    /// The shared committed-publish sequence (spec §Two-Level API, level 2): adopt-evidence over
    /// `entries`, stage a FRESH manifest, precommit, promote. Used by republishRef and by
    /// adoptPartFromManifest (their bodies were near-duplicates).
    void publishEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries,
                        std::map<String, String> mutable_files, Cas::ProvenanceOp op);
    /// Move a COMMITTED ref by republish + drop-source. false = absent source (nothing written).
    bool republishRef(const PartRefKey & src, const PartRefKey & dst);
    /// Mutable-only committed update (autocommit one-shots on a COMMITTED part). NO journal event.
    void updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator);
    void dropRef(const PartRefKey & key);
    /// Idempotent removal: absent ref is success; a drop racing between resolve and the shard
    /// re-read (FILE_DOESNT_EXIST) is success too — the removal unit is replay-safe.
    void dropRefIfPresent(const PartRefKey & key);
    /// Destructor/rollback cleanup: best-effort, never throws; lingering debris is GC-reclaimed.
    void dropRefBestEffort(const PartRefKey & key) noexcept;
    void dropNamespace(const Cas::RootNamespace & ns);

    /// ==== diagnostics (spec §Observability) ====
    enum class LastDecision : uint8_t
    { Hit, MutableRefresh, Mismatch, Miss, OversizedBypass, StrictBypass, ForceFreshRead, Invalidated };
    struct ExplainResult
    {
        bool retained = false;             /// whether the last-served view is currently retained
        LastDecision last_decision = LastDecision::Miss;
        String manifest_ref;               /// manifestRefDebugString of the last-served view
        size_t estimated_bytes = 0;
    };
    /// Test/log-only decision journal; absent key => default ExplainResult.
    ExplainResult explain(const PartRefKey & key) const;
    void clearForTest();

private:
    Cas::StorePtr store;
    CacheParams params;

    struct ViewWeight
    {
        size_t operator()(const PartFolderView & v) const { return v.estimatedBytes(); }
    };
    using ViewCache = CacheBase<String, PartFolderView, std::hash<String>, ViewWeight>;

    /// nullptr <=> retention disabled (cache_bytes == 0): same call graph, no retained map.
    std::unique_ptr<ViewCache> view_cache;

    /// Single-flight per PartRefKey for the build path: concurrent cold builders of the same key
    /// share ONE readManifestShared. NEVER held across I/O — the map only hands out futures.
    mutable std::mutex inflight_mutex;
    mutable std::unordered_map<String, std::shared_future<std::shared_ptr<const PartFolderView>>> inflight;

    std::shared_ptr<const PartFolderView> buildView(
        const PartRefKey & key, const Cas::Resolved & resolved, Freshness freshness) const;
    void eraseView(const PartRefKey & key);

    /// Decision journal for explain (test/log-only; spec §Observability). Bounded by wholesale
    /// clear — debug state, never consulted by the read/write paths.
    static constexpr size_t EXPLAIN_MAX_ENTRIES = 10000;
    mutable std::mutex explain_mutex;
    mutable std::unordered_map<String, ExplainResult> explain_map;
    void recordDecision(const PartRefKey & key, LastDecision decision,
                        const PartFolderView * view, bool retained) const;
};

}
