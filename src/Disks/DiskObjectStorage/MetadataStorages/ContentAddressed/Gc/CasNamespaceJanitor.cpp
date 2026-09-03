#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>

namespace DB::Cas
{

NamespaceJanitorResult NamespaceJanitor::runOnePage(bool suppress_deletes, Liveness liveness)
{
    NamespaceJanitorResult result;
    CasOperation op = requests.admit(std::move(liveness));
    const GcMaintenanceReadResult progress = readGcMaintenanceState(op, layout);
    if (progress.status == GcMaintenanceReadStatus::Corrupt)
    {
        result.anomalies.push_back(progress.diagnostic);
        (void)casGcMaintenanceState(op, layout, progress.incarnation, GcMaintenanceState{}, Retry::standard());
        return result;
    }

    const String cursor = progress.state ? progress.state->janitor_cursor : String{};
    KeyPage page;
    try
    {
        page = op.list(layout.namespaceRootPrefix(), cursor, page_budget, Retry::standard());
    }
    catch (...)
    {
        (void)casGcMaintenanceState(op, layout, progress.incarnation, GcMaintenanceState{}, Retry::once());
        throw;
    }
    result.pages = 1;
    result.keys = page.keys.size();

    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, layout);
    bool ambiguous = false;
    try
    {
        catalog_cut.life_index.throwIfAmbiguous("CAS namespace janitor");
    }
    catch (const DB::Exception & e)
    {
        result.anomalies.push_back(e.message());
        ambiguous = true;
    }
    /// A valid page is complete only when the round had deletion authority for every dead-life
    /// candidate on it. Advancing while the global gate is closed can phase-lock a dead page onto
    /// every suppressed round and a different page onto every bounded forced fold. Ambiguous cuts and
    /// observed fence loss have the same shape: retain the old cursor so an authoritative round
    /// retries the exact page. Malformed keys, absent objects and token mismatches are final per-key
    /// outcomes and therefore do not by themselves prevent progress.
    bool page_decided = !ambiguous && !suppress_deletes;

    for (const KeyEntry & listed : page.keys)
    {
        std::optional<NamespaceLifePhysicalId> life_id;
        try
        {
            if (listed.key.starts_with(layout.namespaceStreamRootPrefix()))
            {
                if (const auto parsed = layout.parseRefObjectKey(listed.key))
                    life_id = parsed->life_id;
            }
            else if (listed.key.starts_with(layout.namespaceStateRootPrefix()))
            {
                if (const auto parsed = layout.parseRefCkptKey(listed.key))
                    life_id = *parsed;
                else if (const auto file_parsed = layout.parseNamespaceFileKey(listed.key))
                    life_id = file_parsed->life_id;
            }
        }
        catch (const DB::Exception & e)
        {
            result.anomalies.push_back(listed.key + ": " + e.message());
            continue;
        }

        if (!life_id)
        {
            result.anomalies.push_back(listed.key + ": unrecognized namespace object key");
            continue;
        }
        if (ambiguous || suppress_deletes || catalog_cut.life_index.resolve(*life_id))
            continue;

        std::optional<Incarnation> incarnation = listed.incarnation;
        if (!incarnation)
        {
            try
            {
                const std::optional<Meta> current = op.head(listed.key, Retry::standard());
                if (!current)
                    continue;
                incarnation = current->incarnation;
            }
            catch (const std::exception & e)
            {
                ++result.leaked;
                result.anomalies.push_back(
                    "leaked dead-life object '" + listed.key + "': exact HEAD failed: " + e.what());
                continue;
            }
        }
        if (!op.admitted())
        {
            page_decided = false;
            break;
        }
        try
        {
            if (op.remove(listed.key, *incarnation, Retry::standard()) == Removal::Removed)
                ++result.deleted;
        }
        catch (const std::exception & e)
        {
            ++result.leaked;
            result.anomalies.push_back(
                "leaked dead-life object '" + listed.key + "': exact delete failed: " + e.what());
        }
    }

    /// Recheck even when the page had no dead candidate. A tenure that observes fence loss after LIST
    /// or after the last exact delete must not publish progress. Loss after this check may still race
    /// with the leak-only maintenance CAS; already completed exact deletes remain safe to repeat.
    if (page_decided && !op.admitted())
        page_decided = false;

    if (page_decided)
    {
        const GcMaintenanceState next{.janitor_cursor = page.next_cursor};
        try
        {
            (void)casGcMaintenanceState(op, layout, progress.incarnation, next, Retry::standard());
        }
        catch (const std::exception & e)
        {
            result.anomalies.push_back("cursor publication failed: " + String(e.what()));
        }
    }
    return result;
}

}
