#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace DB::Cas
{

enum class AuthorityStatus : uint8_t
{
    Authoritative,
    FencedOut,
};

enum class CatalogResolution : uint8_t
{
    DrainComplete,
    ExactRowAbsent,
    ExactRowReplaced,
    ExactRowStillPresent,
};

/// The catalog-only result produced before a GC round may enumerate or publish successor state.
struct CatalogLifecycleReconcileResult
{
    AuthorityStatus authority_status;
    CatalogResolution catalog_resolution;
    std::optional<CasRefCatalog::Snapshot> final_catalog_cut;
};

/// Settles catalog rows that an already-adopted parent fold seal proved safe to remove.
///
/// This component owns only deterministic eligible-row selection and the catalog `N + 1` drain.
/// It neither discovers the parent seal nor performs a hot ref LIST, ref walk, fold, publication,
/// or physical deletion. Runtime invalidation is the caller's `before_erase`.
class CatalogLifecycleReconciler
{
public:
    CatalogLifecycleReconciler(
        CasOperation & op_, const Layout & layout_, const CasFoldSeal & adopted_parent_);

    /// `refresh_authority` is forwarded to each erase, which runs it at the top of every attempt, so
    /// every erase is authorised by a reading taken in its own attempt, and it is run once more before
    /// the drain-complete verdict, which would otherwise report from the reading its last erase left.
    /// It is a refresh, not a verdict: the verdict stays `op.admitted()`.
    ///
    /// `before_erase` is called with each life before its first erase attempt, so the caller can
    /// record the retirement while the row still exists; the erase may not follow.
    CatalogLifecycleReconcileResult reconcile(
        const std::function<void()> & refresh_authority,
        const std::function<void(const NamespaceLifeId &)> & before_erase);

    /// The two records below grow as `reconcile` runs and stay readable after it throws.

    /// Erases the store acknowledged, plus erases whose resolution read found the row absent after an
    /// unacknowledged attempt; each erase counts once.
    uint64_t completedErases() const { return completed_erases; }

    /// Lives whose erase was acknowledged or whose row a resolution read found gone, in order: each was
    /// announced to `before_erase` first. An admission that began after that announcement saw no
    /// retirement, so the caller announces these lives again.
    const std::vector<NamespaceLifeId> & retiredLives() const { return retired_lives; }

private:
    std::optional<CatalogEntry> selectEligible(const CasRefCatalog::Snapshot & catalog) const;
    static CatalogResolution resolveExactRow(
        const CasRefCatalog::Snapshot & catalog, const CatalogEntry & observed);

    CasOperation & op;
    const Layout & layout;
    const CasFoldSeal & adopted_parent;
    uint64_t completed_erases = 0;
    std::vector<NamespaceLifeId> retired_lives;
};

}
