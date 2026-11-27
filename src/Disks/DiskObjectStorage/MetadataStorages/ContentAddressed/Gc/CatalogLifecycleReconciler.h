#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
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

/// Settles catalog rows that are safe to remove: a `Removing` row an already-adopted parent fold seal
/// proved complete, and a `Creating` row whose creator fence is terminal.
///
/// This component owns eligible-row selection and the catalog `N + 1` drain. Selecting a `Creating` row
/// reads its creator's mount slot, once per creator and epoch per drain. It neither discovers the parent
/// seal nor performs a hot ref LIST, ref walk, fold, publication, or physical deletion. Runtime
/// invalidation is the caller's `before_erase` and `retiredLives`.
class CatalogLifecycleReconciler
{
public:
    /// `probe_creator_slot` is `probeCreatorSlot` in production. A `Creating` row owns no edges, because a
    /// ref append needs a `Live` life, so a certificate that its creator can no longer complete the flip is
    /// enough to remove it. A creator whose slot read throws, or does not decode, keeps its rows for this
    /// drain; the drain goes on with the other rows.
    CatalogLifecycleReconciler(
        CasOperation & op_, const Layout & layout_, const CasFoldSeal & adopted_parent_,
        std::function<CreatorSlotStatus(const CreatorFence &)> probe_creator_slot_);

    /// `refresh_authority` is forwarded to each erase, which runs it at the top of every attempt, so
    /// every erase is authorised by a reading taken in its own attempt, and it is run once more before
    /// the drain-complete verdict, which would otherwise report from the reading its last erase left.
    /// It is a refresh, not a verdict: the verdict stays `op.admitted()`. Each call receives the policy that
    /// bounds its reads: the cancel's policy inside the catalog lane, `Retry::standard` elsewhere.
    ///
    /// `before_erase` is called with each `Removing` life before its first erase attempt, so the caller
    /// can record the retirement while the row still exists; the erase may not follow. A `Creating` life
    /// is never announced: an opener on this server may take the same life over and win the swap.
    CatalogLifecycleReconcileResult reconcile(
        const std::function<void(const Retry &)> & refresh_authority,
        const std::function<void(const NamespaceLifeId &)> & before_erase);
    /// For a refresh that sends no request, so has nothing to bound.
    CatalogLifecycleReconcileResult reconcile(
        const std::function<void()> & refresh_authority,
        const std::function<void(const NamespaceLifeId &)> & before_erase)
    {
        return reconcile([&](const Retry &) { refresh_authority(); }, before_erase);
    }

    /// The records below grow as `reconcile` runs and stay readable after it throws.

    /// `Removing` rows erased: erases the store acknowledged, plus erases whose resolution read found the row
    /// absent after an unacknowledged attempt; each erase counts once.
    uint64_t completedErases() const { return completed_erases; }

    /// `Creating` rows whose cancel returned `Cancelled`.
    uint64_t cancelledCreating() const { return cancelled_creating; }

    /// Lives whose erase was acknowledged or whose row a resolution read found gone, in order. Each
    /// `Removing` life was announced to `before_erase` first, and an admission that began after that
    /// announcement saw no retirement, so the caller announces these lives again. A `Creating` life is
    /// here only once its cancellation returned `Cancelled`.
    const std::vector<NamespaceLifeId> & retiredLives() const { return retired_lives; }

private:
    std::optional<CatalogEntry> selectEligible(const CasRefCatalog::Snapshot & catalog);
    static CatalogResolution resolveExactRow(
        const CasRefCatalog::Snapshot & catalog, const CatalogEntry & observed);

    /// The resolution of `observed` when the drain must stop because authority was lost, else nothing.
    std::optional<CatalogResolution> cancelStalledCreating(
        const CatalogEntry & observed, const std::function<void(const Retry &)> & refresh_authority,
        CasRefCatalog::Snapshot & catalog);

    /// The memoized verdict on `creator`. A terminal certificate never reverts within a drain: the slot is
    /// never deleted, and every later claim of the root takes a higher epoch.
    bool isCreatorTerminal(const CreatorFence & creator);

    CasOperation & op;
    const Layout & layout;
    const CasFoldSeal & adopted_parent;
    const std::function<CreatorSlotStatus(const CreatorFence &)> probe_creator_slot;
    /// Per creator root and epoch; no value when the slot read threw.
    std::map<std::pair<String, uint64_t>, std::optional<CreatorSlotStatus>> creator_slots;
    /// Namespaces whose `Creating` row this drain already tried. Each is tried once, so a row that
    /// keeps changing under the attempt cannot hold the drain; it waits for the next round.
    std::set<String> creating_tried;
    uint64_t completed_erases = 0;
    uint64_t cancelled_creating = 0;
    std::vector<NamespaceLifeId> retired_lives;
};

}
