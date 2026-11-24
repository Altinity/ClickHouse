#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CatalogLifecycleReconciler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>

#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <fmt/format.h>
#include <algorithm>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace ProfileEvents
{
    extern const Event CASGCStalledCreatingCancelled;
}

namespace DB::Cas
{

CatalogLifecycleReconciler::CatalogLifecycleReconciler(
    CasOperation & op_, const Layout & layout_, const CasFoldSeal & adopted_parent_,
    std::function<CreatorSlotStatus(const CreatorFence &)> probe_creator_slot_)
    : op(op_)
    , layout(layout_)
    , adopted_parent(adopted_parent_)
    , probe_creator_slot(std::move(probe_creator_slot_))
{
}

bool CatalogLifecycleReconciler::isCreatorTerminal(const CreatorFence & creator)
{
    const auto [it, inserted] = creator_slots.try_emplace({creator.server_root_id, creator.writer_epoch});
    if (!inserted)
        return it->second == CreatorSlotStatus::Terminal;
    try
    {
        it->second = probe_creator_slot(creator);
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::LOGICAL_ERROR)
            throw;
        /// A deposed leader's read fails too; the drain's own verdict reports that, not this creator.
        if (op.admitted())
            LOG_WARNING(getLogger("CatalogLifecycleReconciler"),
                "CAS GC catalog drain: the mount slot of creator '{}' could not be read ({}); its `Creating` rows "
                "for writer epoch {} stay for a later round", creator.server_root_id, e.message(), creator.writer_epoch);
        return false;
    }
    if (it->second == CreatorSlotStatus::Undecodable)
        LOG_WARNING(getLogger("CatalogLifecycleReconciler"),
            "CAS GC catalog drain: the mount slot of creator '{}' does not decode; its `Creating` rows for writer "
            "epoch {} stay", creator.server_root_id, creator.writer_epoch);
    return it->second == CreatorSlotStatus::Terminal;
}

std::optional<CatalogResolution> CatalogLifecycleReconciler::cancelStalledCreating(
    const CatalogEntry & observed, const std::function<void(const Retry &)> & refresh_authority,
    CasRefCatalog::Snapshot & catalog)
{
    const CasRefCatalog::StalledCreatingCancelOutcome outcome = CasRefCatalog::cancelStalledCreating(
        op, layout, observed, [this](const CreatorFence & creator) { return isCreatorTerminal(creator); },
        refresh_authority);
    const bool cancelled = outcome == CasRefCatalog::StalledCreatingCancelOutcome::Cancelled;
    if (cancelled)
    {
        ++cancelled_creating;
        ProfileEvents::increment(ProfileEvents::CASGCStalledCreatingCancelled);
        retired_lives.push_back(NamespaceLifeId::fromCatalogEntry(observed.ns, observed.incarnation));
        LOG_INFO(getLogger("CatalogLifecycleReconciler"),
            "CAS GC catalog drain: cancelled the `Creating` row of namespace '{}': its creator '{}' at writer epoch {} "
            "cannot finish it", observed.ns.string(), observed.creator->server_root_id, observed.creator->writer_epoch);
    }
    /// A cancel whose response was lost and that a later catalog write followed ends `EntryChanged`; this
    /// life is then neither counted nor retired. For every outcome the fresh cut below decides what is next.
    if (outcome == CasRefCatalog::StalledCreatingCancelOutcome::FencedOut || !op.admitted())
        return cancelled ? CatalogResolution::ExactRowAbsent : resolveExactRow(catalog, observed);
    catalog = CasRefCatalog::read(op, layout);
    return std::nullopt;
}

std::optional<CatalogEntry> CatalogLifecycleReconciler::selectEligible(
    const CasRefCatalog::Snapshot & catalog)
{
    for (const CatalogEntry & entry : catalog.catalog.entries)
    {
        if (entry.state == NsState::Creating)
        {
            /// The probe reads the creator's mount slot, which a deposed leader may not do; the verdict
            /// that follows an empty selection reports the deposition.
            if (!op.admitted() || !creating_tried.insert(entry.ns.string()).second)
                continue;
            if (isCreatorTerminal(*entry.creator))
                return entry;
            continue;
        }
        if (entry.state != NsState::Removing)
            continue;

        const auto parent_row = adopted_parent.ref_lives.find(entry.incarnation);
        if (parent_row == adopted_parent.ref_lives.end()
            || !parent_row->second.cleanup_evidence
            || parent_row->second.coverage.hold)
            continue;

        return entry;
    }
    return std::nullopt;
}

CatalogResolution CatalogLifecycleReconciler::resolveExactRow(
    const CasRefCatalog::Snapshot & catalog, const CatalogEntry & observed)
{
    const auto current = std::find_if(
        catalog.catalog.entries.begin(),
        catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns.string() == observed.ns.string(); });
    if (current == catalog.catalog.entries.end())
        return CatalogResolution::ExactRowAbsent;
    if (current->incarnation != observed.incarnation)
        return CatalogResolution::ExactRowReplaced;
    return CatalogResolution::ExactRowStillPresent;
}

CatalogLifecycleReconcileResult CatalogLifecycleReconciler::reconcile(
    const std::function<void(const Retry &)> & refresh_authority,
    const std::function<void(const NamespaceLifeId &)> & before_erase)
{
    CatalogLifecycleReconcileResult result{
        .authority_status = AuthorityStatus::Authoritative,
        .catalog_resolution = CatalogResolution::DrainComplete,
        .final_catalog_cut = std::nullopt};
    CasRefCatalog::Snapshot catalog = CasRefCatalog::read(op, layout);

    for (;;)
    {
        const std::optional<CatalogEntry> eligible = selectEligible(catalog);
        if (!eligible)
        {
            /// The verdict gets its own refresh, not just each erase: a deposition landing after the
            /// last erase is invisible to the reading that erase took, so without this the drain hands
            /// a deposed leader `Authoritative` and the round only learns better at its `gc/state`
            /// commit -- after a ref walk and a fold seal it never had the authority to build.
            refresh_authority(Retry::standard());
            if (!op.admitted())
            {
                result.authority_status = AuthorityStatus::FencedOut;
                return result;
            }
            result.catalog_resolution = CatalogResolution::DrainComplete;
            result.final_catalog_cut = std::move(catalog);
            return result;
        }

        if (eligible->state == NsState::Creating)
        {
            if (const auto stopped = cancelStalledCreating(*eligible, refresh_authority, catalog))
            {
                result.authority_status = AuthorityStatus::FencedOut;
                result.catalog_resolution = *stopped;
                return result;
            }
            continue;
        }

        bool acknowledged = false;
        CasRefCatalog::CompletedRemovingDeleteResult delete_result
            = CasRefCatalog::deleteCompletedRemovingAtSnapshot(
                op, layout, std::move(catalog), *eligible, adopted_parent,
                [&] { refresh_authority(Retry::standard()); }, before_erase,
                [&](const NamespaceLifeId & life)
                {
                    acknowledged = true;
                    ++completed_erases;
                    retired_lives.push_back(life);
                });
        if (!delete_result.catalog_snapshot)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS catalog lifecycle reconciliation returned no catalog resolution snapshot");

        if (!acknowledged && delete_result.invalidated_life)
            retired_lives.push_back(*delete_result.invalidated_life);
        if (!acknowledged && delete_result.outcome == CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted)
            ++completed_erases;
        catalog = std::move(*delete_result.catalog_snapshot);
        result.catalog_resolution = resolveExactRow(catalog, *eligible);

        if (delete_result.outcome == CasRefCatalog::CompletedRemovingDeleteOutcome::FencedOut)
        {
            result.authority_status = AuthorityStatus::FencedOut;
            return result;
        }
        if (delete_result.outcome == CasRefCatalog::CompletedRemovingDeleteOutcome::ProofRefused)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS catalog lifecycle reconciliation selected namespace '{}' without matching no-hold cleanup evidence",
                eligible->ns.string());
        if (result.catalog_resolution == CatalogResolution::ExactRowStillPresent)
            throwCasRequestRetryLater(fmt::format(
                "CAS catalog lifecycle reconciliation left completed-removal namespace '{}' present",
                eligible->ns.string()));
    }
}

}
