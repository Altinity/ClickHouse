#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace DB::Cas { class Build; }

namespace DB::ContentAddressed
{

/// The single facade for committed content-addressed part-folder access (spec
/// 2026-07-08-cas-part-folder-cache). Reads build immutable `PartFolderView`s; committed part-ref
/// mutations are facade methods so cache effects (Phase 4) are write-through, never a caller
/// responsibility. Phase-2 shape: NO retained state — `getView` builds a fresh view per call; the
/// call graph is already final, retention (Phase 4) only adds the retained-map consultation.
/// Thread-safe; shared by all readers and transactions of one disk.
class CachedPartFolderAccess
{
public:
    explicit CachedPartFolderAccess(Cas::StorePtr store_) : store(std::move(store_)) {}

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
    /// Mutable-only committed update (autocommit one-shots on a COMMITTED part). NO journal event.
    void updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator);
    void dropRef(const PartRefKey & key);
    /// Idempotent removal: absent ref is success; a drop racing between resolve and the shard
    /// re-read (FILE_DOESNT_EXIST) is success too — the removal unit is replay-safe.
    void dropRefIfPresent(const PartRefKey & key);
    /// Destructor/rollback cleanup: best-effort, never throws; lingering debris is GC-reclaimed.
    void dropRefBestEffort(const PartRefKey & key) noexcept;
    void dropNamespace(const Cas::RootNamespace & ns);

private:
    Cas::StorePtr store;
};

}
