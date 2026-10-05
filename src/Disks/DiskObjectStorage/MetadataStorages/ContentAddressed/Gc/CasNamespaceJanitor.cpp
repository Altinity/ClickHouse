#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>
#include <fmt/format.h>
#include <algorithm>

namespace DB::Cas
{

namespace
{

/// The legacy `casPut` this write replaces reported a definite conflict as a value (never a failure to
/// this caller) and reported a store failure -- a refusal, an exhausted policy -- by throwing. Only
/// `Refused`/`GaveUp` are the alternatives a thrown exception used to carry, so only those propagate;
/// `Committed`/`Declined`/`Conflict` stay silent exactly as they did before.
void throwOnRefusedOrGaveUp(WriteResult && result, std::string_view what)
{
    if (std::holds_alternative<Refused>(result) || std::holds_alternative<GaveUp>(result))
        (void)orThrow(std::move(result), what);
}

uint64_t removeOnCallerThread(CasOperation & op, const std::vector<WriteOnceKey> & keys, std::vector<String> & anomalies)
{
    uint64_t leaked = 0;
    for (size_t begin = 0; begin < keys.size(); begin += kBulkDeleteMaxKeys)
    {
        const std::vector<WriteOnceKey> batch(
            keys.begin() + begin, keys.begin() + std::min(keys.size(), begin + kBulkDeleteMaxKeys));
        try
        {
            op.removeManyWriteOnce(batch, Retry::standard());
        }
        catch (const std::exception & e)
        {
            leaked += batch.size();
            anomalies.push_back(fmt::format(
                "leaked {} dead-life objects starting at '{}': batch delete failed: {}",
                batch.size(), batch.front().str(), e.what()));
        }
    }
    return leaked;
}

}

NamespaceJanitor::NamespaceJanitor(
    CasRequests & requests_, const Layout & layout_, size_t page_budget_, RemoveWriteOnce remove_write_once_)
    : requests(requests_)
    , layout(layout_)
    , page_budget(page_budget_)
    , remove_write_once(remove_write_once_ ? std::move(remove_write_once_) : removeOnCallerThread)
{
}

NamespaceJanitorResult NamespaceJanitor::runOnePage(bool suppress_deletes, Liveness liveness)
{
    NamespaceJanitorResult result;
    CasOperation op = requests.admit(std::move(liveness));
    const GcMaintenanceReadResult progress = readGcMaintenanceState(op, layout);
    if (progress.status == GcMaintenanceReadStatus::Corrupt)
    {
        result.anomalies.push_back(progress.diagnostic);
        throwOnRefusedOrGaveUp(
            casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::standard()),
            "CAS namespace janitor: corrupt maintenance-state reset");
        return result;
    }

    const String cursor = progress.state ? progress.state->janitor_cursor : String{};
    ListPage page;
    try
    {
        page = op.list(layout.namespaceRootPrefix(), cursor, page_budget, Retry::standard());
    }
    catch (...)
    {
        (void)casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::once());
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
    /// every suppressed round and a different page onto every bounded forced fold. An ambiguous cut
    /// retains the old cursor so an authoritative round retries the exact page; a lost liveness sample
    /// only reaches this retained-cursor path when it is caught between the two `op.admitted()` checks
    /// below -- a sample lost earlier throws out of a read verb (the maintenance read, the list, or a
    /// HEAD) before this line is ever reached, ending the page by exception instead. Malformed keys,
    /// absent objects and token mismatches are final per-key outcomes and therefore do not by
    /// themselves prevent progress.
    bool page_decided = !ambiguous && !suppress_deletes;
    std::vector<WriteOnceKey> dead_stream_keys;

    for (const ListedKey & listed : page.keys)
    {
        std::optional<NamespaceLifePhysicalId> life_id;
        std::optional<ParsedRefObjectKey> stream_key;
        try
        {
            if (listed.key.starts_with(layout.namespaceStreamRootPrefix()))
            {
                stream_key = layout.parseRefObjectKey(listed.key);
                if (stream_key)
                    life_id = stream_key->life_id;
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

        if (stream_key)
        {
            if (std::optional<WriteOnceKey> write_once = layout.writeOnceStreamKey(*stream_key, listed.key))
            {
                dead_stream_keys.push_back(std::move(*write_once));
                continue;
            }
        }

        std::optional<Etag> etag = listed.etag;
        if (!etag)
        {
            try
            {
                const std::optional<Meta> current = op.head(listed.key, Retry::standard());
                if (!current)
                    continue;
                etag = current->etag;
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
            if (op.remove(listed.key, *etag, Retry::standard()) == Removal::Removed)
                ++result.deleted;
        }
        catch (const std::exception & e)
        {
            ++result.leaked;
            result.anomalies.push_back(
                "leaked dead-life object '" + listed.key + "': exact delete failed: " + e.what());
        }
    }

    if (page_decided && !dead_stream_keys.empty() && op.admitted())
    {
        const uint64_t leaked = remove_write_once(op, dead_stream_keys, result.anomalies);
        result.leaked += leaked;
        result.deleted += dead_stream_keys.size() - leaked;
    }

    /// Recheck even when the page had no dead candidate. A tenure that observes fence loss after LIST
    /// or after the last delete must not publish progress. Loss after this check may still race
    /// with the leak-only maintenance CAS; already completed exact deletes remain safe to repeat.
    if (page_decided && !op.admitted())
        page_decided = false;

    if (page_decided)
    {
        const GcMaintenanceState next{.janitor_cursor = page.next_cursor};
        try
        {
            const WriteResult published = casGcMaintenanceState(op, layout, progress.etag, next, Retry::standard());
            if (std::holds_alternative<Refused>(published) || std::holds_alternative<GaveUp>(published))
                result.anomalies.push_back("cursor publication did not commit");
            else if (std::holds_alternative<Committed>(published))
                result.more = result.deleted > 0 && !(cursor.empty() && page.next_cursor.empty());
        }
        catch (const std::exception & e)
        {
            result.anomalies.push_back("cursor publication failed: " + String(e.what()));
        }
    }
    return result;
}

}
