#pragma once
// ===== PartRefKey (merge #7: the routed part identity) =====
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
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

// ===== PartFolderView (merge #7: the value/view over the pool) =====
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// Immutable snapshot of one resolved committed part/projection folder (spec
/// 2026-07-08-cas-part-folder-cache §PartFolderView). Index-free: the decoder guarantees strictly
/// ascending canonical path order, so file lookup is a binary search and directory listing is a
/// contiguous range scan over the SHARED decode (`manifest` is the same object the Pool's
/// manifest cache holds). No I/O; never mutated after construction. All answers are pure functions
/// of the members. All-tree-part-files Task 9: the mutable-files serving this view used to provide
/// (a separate out-of-band payload, refreshed independently of the manifest) is gone — every
/// per-part file is an ordinary manifest tree entry now, served by `findFile` like anything else; a
/// content change is always a manifest change (`repointRef`), so the existing manifest-id staleness
/// check is the only freshness check this view needs.
class PartFolderView
{
public:
    PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                   uint64_t published_at_ms_,
                   std::shared_ptr<const Cas::PartManifest> manifest_, uint64_t validated_at_ms_);

    /// Convenience join of a fresh `Resolved` + its validated shared decode. `validated_at_ms` is the
    /// caller's "now" (spec §3 part_folder_validate): construction is only reached after
    /// `readManifestShared`'s mandatory HEAD, so it IS a body-proven moment. The caller supplies it
    /// (rather than this reading a clock itself) so `CachedPartFolderAccess`'s single injectable clock
    /// drives both this stamp and its own age-window comparison — the same seam tests use.
    static std::shared_ptr<const PartFolderView> make(
        PartRefKey key, const Cas::Resolved & resolved,
        std::shared_ptr<const Cas::PartManifest> manifest, uint64_t validated_at_ms);

    /// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the
    /// PoC recognizer (B64, also matches the nested detached-staging shape). `file` is the ROUTED
    /// in-tree file path. Returns the "<file>/" prefix, or nullopt when not a projection dir.
    static std::optional<std::string> projectionDirPrefix(const std::string & file);

    const PartRefKey & refKey() const { return key; }
    const Cas::ManifestId & manifestId() const { return manifest_id; }
    uint64_t manifestSize() const { return manifest_size; }
    uint64_t publishedAtMs() const { return published_at_ms; }
    const std::shared_ptr<const Cas::PartManifest> & manifest() const { return manifest_body; }
    /// The wall-clock ms at which this view's manifest body was last HEAD-proven live (spec §3
    /// part_folder_validate). A mutable-only refresh clone carries the ORIGINAL view's stamp forward
    /// (a mutable drift did not re-prove the body).
    uint64_t validatedAtMs() const { return validated_at_ms; }

    const Cas::ManifestEntry * findFile(const String & path) const;
    bool hasFile(const String & path) const;
    std::optional<uint64_t> fileSize(const String & path) const;  /// inline / blob
    std::optional<String> inlineBytes(const String & path) const; /// Inline entries only
    std::vector<String> listChildren(const String & dir_prefix) const;
    bool hasDirectory(const String & dir_prefix) const;
    size_t estimatedBytes() const;

private:
    PartRefKey key;
    Cas::ManifestId manifest_id;
    uint64_t manifest_size = 0;
    uint64_t published_at_ms = 0;
    std::shared_ptr<const Cas::PartManifest> manifest_body;
    uint64_t validated_at_ms = 0;
};

}

// ===== PartFolderValidate + CachedPartFolderAccess (merge #7: the cache of one mechanism) =====
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
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

namespace DB::Cas { class PartWriteTxn; }

namespace DB::ContentAddressed
{

/// §3 (spec 2026-07-13-cas-memory-s3-budget-optimizations-design.md): the ForceFresh re-proof HEAD
/// (`readManifestShared`'s mandatory body HEAD -- an INV-NO-DANGLE fail-closed net, not protocol
/// correctness) is configurable via the `part_folder_validate` disk setting. `Always` (the default)
/// is byte-for-byte pre-§3 behavior: EVERY `ForceFresh` re-proves the manifest body. `Age`/`Never`
/// may instead serve a retained view whose manifest id + mutable files still match a fresh `resolve`
/// -- ref currency is proven either way; only the body-existence re-proof is skipped, so a real
/// manifest change under a retained view is still caught by the resolve-vs-cache mismatch and rebuilt.
struct PartFolderValidate
{
    enum class Mode : uint8_t { Always, Age, Never };
    Mode mode = Mode::Always;
    uint64_t age_seconds = 0;    /// only meaningful for Mode::Age
};

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
        /// The explain decision journal (spec §Observability) is test/log-only and its recordDecision
        /// path takes a per-disk global mutex and allocates on EVERY read. Off by default so the read
        /// hit path never pays for it; the disk factory / tests turn it on when they consult explain().
        bool explain_enabled = false;
        /// §3: the ForceFresh body re-proof policy. Default `Always` keeps `CacheParams{}` (the
        /// unit-test default, and every pre-§3 caller) byte-for-byte unchanged.
        PartFolderValidate validate;
    };

    /// NOTE: `CacheParams params_ = {}` cannot be a default ARGUMENT here — Clang's complete-class-
    /// context rule requires the enclosing class (`CachedPartFolderAccess`) to be complete before a
    /// nested class's (`CacheParams`) default member initializers can be evaluated, and a default
    /// argument written inside the class body is evaluated too early. Two overloads sidestep it; the
    /// single-arg form default-constructs `CacheParams` (retention disabled) out-of-line.
    explicit CachedPartFolderAccess(Cas::PoolPtr store_);
    /// `now_ms_fn_`: wall-clock ms, injected (tests) for the §3 age-window comparison AND the
    /// retained view's `validated_at_ms` stamp -- the SAME function drives both, so a test controls
    /// each side of the comparison exactly. Defaults to `std::chrono::system_clock` (mirrors
    /// `Cas::Gc`'s `now_ms_fn` convention) when empty.
    CachedPartFolderAccess(Cas::PoolPtr store_, CacheParams params_, std::function<uint64_t()> now_ms_fn_ = {});

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

    /// The transaction's terminal publish: the atomic owner move.
    /// `allow_repoint` (all-tree-part-files Task 2/3): threads through to `PartWriteTxn::promote`, opting
    /// into retargeting a committed ref that already names a DIFFERENT manifest.
    void promoteBuild(Cas::PartWriteTxn & build, const PartRefKey & key, UInt128 build_id,
                      const Cas::ManifestId & manifest_id, bool allow_repoint = false);
    /// The shared committed-publish sequence (spec §Two-Level API, level 2): adopt-evidence over
    /// `entries`, stage a FRESH manifest, precommit, promote. Used by republishRef and by
    /// adoptPartFromManifest (their bodies were near-duplicates).
    void publishEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries,
                        Cas::ProvenanceOp op, bool allow_repoint = false);
    /// Move a COMMITTED ref by republish + drop-source. false = absent source (nothing written).
    bool republishRef(const PartRefKey & src, const PartRefKey & dst);
    /// Standalone write/remove on an already-COMMITTED part (all-tree-part-files Task 3, spec §4):
    /// republishes `key`'s manifest with `entries`. Byte-equal candidate (same decoded entries as the
    /// currently committed manifest) is a ZERO-pool-mutation no-op, returns false. Otherwise republishes
    /// via `publishEntries(allow_repoint=true)`, audits loudly (`ProfileEvents::CasRefRepoint` +
    /// `LOG_WARNING`; `PartWriteTxn::promote` itself emits the `CasEventType::RefRepoint` event), erases the
    /// cached view, and returns true. `key` must already resolve (a repoint targets a committed ref).
    bool repointRef(const PartRefKey & key, std::vector<Cas::ManifestEntry> entries, Cas::ProvenanceOp op);
    void dropRef(const PartRefKey & key);
    /// Idempotent removal: absent ref is success; a drop racing between resolve and the shard
    /// re-read (FILE_DOESNT_EXIST) is success too — the removal unit is replay-safe.
    void dropRefIfPresent(const PartRefKey & key);
    /// Destructor/rollback cleanup: best-effort, never throws; lingering debris is GC-reclaimed.
    void dropRefBestEffort(const PartRefKey & key) noexcept;
    void dropNamespace(const Cas::RootNamespace & ns);

    /// ==== diagnostics (spec §Observability) ====
    enum class LastDecision : uint8_t
    { Hit, Mismatch, Miss, OversizedBypass, StrictBypass, ForceFreshRead, Invalidated };
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
    /// Test-only: number of entries in the decision journal (0 whenever explain is disabled).
    size_t explainJournalSizeForTest() const;

private:
    Cas::PoolPtr store;
    CacheParams params;
    /// §3: wall-clock ms; see the ctor doc comment. `std::function::operator()` is const, so this is
    /// callable from const methods (`getView`, `buildView`) without a `mutable` qualifier.
    std::function<uint64_t()> now_ms_fn;

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
    void recordDecision(const String & cache_key, LastDecision decision,
                        const PartFolderView * view, bool retained) const;
};

}
