#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>

namespace DB::Cas
{

NamespaceJanitorResult NamespaceJanitor::runOnePage(
    bool suppress_deletes, const std::function<bool()> & fence_held)
{
    NamespaceJanitorResult result;
    const GcMaintenanceReadResult progress = readGcMaintenanceState(backend, layout);
    if (progress.status == GcMaintenanceReadStatus::Corrupt)
    {
        result.anomalies.push_back(progress.diagnostic);
        (void)casGcMaintenanceState(backend, layout, progress.token, GcMaintenanceState{});
        return result;
    }

    const String cursor = progress.state ? progress.state->janitor_cursor : String{};
    ListPage page;
    try
    {
        page = backend.list(layout.namespaceRootPrefix(), cursor, page_budget);
    }
    catch (...)
    {
        (void)casGcMaintenanceState(backend, layout, progress.token, GcMaintenanceState{});
        throw;
    }
    result.pages = 1;
    result.keys = page.keys.size();

    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(backend, layout);
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

    for (const ListedKey & listed : page.keys)
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
        if (!listed.token)
        {
            result.anomalies.push_back(listed.key + ": LIST returned no exact token");
            continue;
        }
        if (!fence_held())
            break;
        if (backend.deleteExact(listed.key, *listed.token).kind == DeleteOutcome::Kind::Deleted)
            ++result.deleted;
    }

    const GcMaintenanceState next{.janitor_cursor = page.next_cursor};
    try
    {
        (void)casGcMaintenanceState(backend, layout, progress.token, next);
    }
    catch (const std::exception & e)
    {
        result.anomalies.push_back("cursor publication failed: " + String(e.what()));
    }
    return result;
}

}
