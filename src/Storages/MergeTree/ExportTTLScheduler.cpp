#include <Storages/MergeTree/ExportTTLScheduler.h>

#include <Core/UUID.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Storages/MergeTree/IExportTTLIndex.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreePartsMover.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/TTLDescription.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <algorithm>
#include <set>

namespace CurrentMetrics
{
    extern const Metric ExportTTLPartsHeldByDeleteGate;
}

namespace DB
{

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsUInt64 ttl_export_check_period_seconds;
    extern const MergeTreeSettingsString ttl_export_settings_profile;
}

namespace
{

/// The rule of move TTL: a part is due once the maximum TTL value of its rows is due. A part without
/// the TTL info (written before the TTL was added and not materialized) is never due. A part without
/// rows, e.g. emptied by a mutation, waits for no row: it is exported with its group, writing nothing,
/// so that it merges with the exported parts around it.
bool isDue(const IMergeTreeDataPart & part, const TTLDescriptions & export_ttls, time_t now)
{
    return part.rows_count == 0
        || selectTTLDescriptionForTTLInfos(export_ttls, part.ttl_infos.export_ttl, now, /* use_max */ true).has_value();
}

std::map<String, std::vector<MergeTreeDataPartPtr>> getPartsByPartition(const MergeTreeData & storage)
{
    std::map<String, std::vector<MergeTreeDataPartPtr>> result;
    for (const auto & part : storage.getDataPartsVectorForInternalUsage())
        if (!part->info.isPatch())
            result[part->info.getPartitionId()].push_back(part);
    return result;
}

std::set<String> getPartitionIds(
    const std::map<String, ExportTTLVersionedEntry> & entries, const std::map<String, std::vector<MergeTreeDataPartPtr>> & parts_by_partition)
{
    std::set<String> result;
    for (const auto & [partition_id, _] : entries)
        result.insert(partition_id);
    for (const auto & [partition_id, _] : parts_by_partition)
        result.insert(partition_id);
    return result;
}

String getDestinationKey(const StorageID & destination_id)
{
    return ExportTTLUtils::destinationKey(destination_id.database_name, destination_id.table_name);
}

const std::map<String, ExportTTLVersionedEntry> & getEntries(const ExportTTLIndexSnapshot & snapshot, const String & destination_key)
{
    static const std::map<String, ExportTTLVersionedEntry> no_entries;
    const auto it = snapshot.entries.find(destination_key);
    return it == snapshot.entries.end() ? no_entries : it->second;
}

}

ExportTTLScheduler::ExportTTLScheduler(MergeTreeData & storage_, IExportTTLIndex & index_)
    : storage(storage_)
    , ttl_index(index_)
    , log(getLogger(fmt::format("{} (ExportTTLScheduler)", storage_.getLogName())))
    , parts_held_by_delete_gate(CurrentMetrics::ExportTTLPartsHeldByDeleteGate, 0)
{
}

ContextPtr ExportTTLScheduler::makeContext() const
{
    auto context = Context::createCopy(storage.getContext());

    const String profile = (*storage.getSettings())[MergeTreeSetting::ttl_export_settings_profile];
    if (!profile.empty())
        context->setCurrentProfile(profile);

    /// The destination was validated when the TTL was created, and may be an Iceberg table.
    context->setSetting("allow_insert_into_iceberg", true);

    /// A pending mutation must not keep a part from being exported forever.
    context->setSetting("export_merge_tree_part_throw_on_pending_mutations", false);
    context->setSetting("export_merge_tree_part_throw_on_pending_patch_parts", false);

    ExportTTLUtils::allowLossyCasts(*context);

    return context;
}

bool ExportTTLScheduler::isPaused() const
{
    return storage.parts_mover.moves_blocker.isCancelled();
}

void ExportTTLScheduler::updatePartsHeldByDeleteGate()
{
    const auto gate = storage.getExportTTLDeleteGate();
    const auto snapshot = ttl_index.getLatest();
    /// Until the index is read, it is not known which parts are exported.
    if (gate.enabled && !snapshot)
        return;

    size_t held = 0;
    if (gate.enabled)
    {
        for (const auto & part : storage.getDataPartsVectorForInternalUsage())
            if (!part->info.isPatch() && gate.check(part->name, part->info, part->ttl_infos, snapshot->fence.get()))
                ++held;
    }
    parts_held_by_delete_gate.changeTo(held);
}

UInt64 ExportTTLScheduler::run()
{
    const UInt64 period_ms = std::max<UInt64>(1, (*storage.getSettings())[MergeTreeSetting::ttl_export_check_period_seconds]) * 1000;
    const auto metadata = storage.getInMemoryMetadataPtr(nullptr, false);
    const auto export_ttls = metadata->getExportTTLs();

    updatePartsHeldByDeleteGate();

    const auto clear_errors = [this]
    {
        std::lock_guard lock(mutex);
        last_errors.clear();
        table_error.clear();
    };

    /// Without an `EXPORT` TTL there is only the index of an earlier one to clean up, so a table that
    /// never had one neither takes the scheduler lock nor reads Keeper.
    if (export_ttls.empty())
    {
        const auto latest = ttl_index.getLatest();
        if (!latest || latest->entries.empty())
        {
            ttl_index.releaseSchedulerLock();
            clear_errors();
            return period_ms;
        }
    }

    if (!ttl_index.tryAcquireSchedulerLock())
    {
        clear_errors();
        return period_ms;
    }

    if (isPaused())
        return period_ms;

    const auto snapshot = ttl_index.getSnapshot();
    if (export_ttls.empty() && snapshot->entries.empty())
    {
        ttl_index.releaseSchedulerLock();
        clear_errors();
        return period_ms;
    }

    const auto destination_id = export_ttls.empty() ? StorageID::createEmpty() : storage.getExportTTLDestination(export_ttls.front());
    const String destination_key = export_ttls.empty() ? "" : getDestinationKey(destination_id);
    const time_t now = time(nullptr);

    for (const auto & [key, entries] : snapshot->entries)
    {
        if (key == destination_key)
            continue;

        try
        {
            cleanupDestination(key, entries);
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("While removing the TTL export index of destination {}", key));
        }
    }

    if (export_ttls.empty())
        return period_ms;

    const auto context = makeContext();

    /// A destination that cannot be resolved may be created again or not loaded yet, so what was
    /// exported to it is kept, and its parts stay held from the delete TTL.
    const auto destination = DatabaseCatalog::instance().tryGetTable(destination_id, context);
    if (!destination)
    {
        auto error = fmt::format("The destination table {} of the EXPORT TTL does not exist", destination_id.getNameForLogs());
        LOG_WARNING(log, "{}, nothing is exported", error);
        std::lock_guard lock(mutex);
        table_error = std::move(error);
        return period_ms;
    }

    const auto & entries = getEntries(*snapshot, destination_key);
    const auto parts_by_partition = getPartsByPartition(storage);

    std::map<String, String> errors;
    for (const auto & partition_id : getPartitionIds(entries, parts_by_partition))
    {
        ExportTTLVersionedEntry versioned;
        if (const auto it = entries.find(partition_id); it != entries.end())
            versioned = it->second;
        else
            versioned.entry.partition_id = partition_id;

        const auto parts_it = parts_by_partition.find(partition_id);

        try
        {
            schedulePartition(
                destination_key, destination, std::move(versioned),
                parts_it == parts_by_partition.end() ? std::vector<MergeTreeDataPartPtr>{} : parts_it->second,
                export_ttls, now, context);
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("While exporting partition {} by TTL", partition_id));
            errors[partition_id] = getCurrentExceptionMessage(/* with_stacktrace */ false);
        }
    }

    std::lock_guard lock(mutex);
    last_errors = std::move(errors);
    table_error.clear();
    return period_ms;
}

void ExportTTLScheduler::schedulePartition(
    const String & destination_key,
    const StoragePtr & destination,
    ExportTTLVersionedEntry versioned,
    const std::vector<MergeTreeDataPartPtr> & parts,
    const TTLDescriptions & export_ttls,
    time_t now,
    const ContextPtr & context)
{
    auto & entry = versioned.entry;
    const auto & partition_id = entry.partition_id;

    /// Whether `entry` differs from the stored one.
    bool changed = false;
    /// The commit id of the failed task that holds the claim, which the group retries.
    String retried_commit_id;

    if (entry.claim)
    {
        const auto claim = *entry.claim;
        const auto status = storage.getExportTaskStatus(claim.transaction_id);
        if (status == MergeTreeData::ExportTaskStatus::PENDING)
            return;

        if (status == MergeTreeData::ExportTaskStatus::COMPLETED)
        {
            /// The commit of a plain `MergeTree` records it here, after the task is marked completed.
            entry.commitClaim(claim.commit_id, {});
            changed = true;
        }
        else if (destination->isExportTransactionCommitted(claim.commit_id, context))
        {
            /// E.g. a commit that landed and then the task timed out before it was marked completed.
            LOG_INFO(log, "Export task {} of partition {} did not complete, but it committed to the destination",
                claim.transaction_id, partition_id);
            entry.commitClaim(claim.commit_id, {});
            changed = true;
        }
        else
        {
            retried_commit_id = claim.commit_id;
        }
    }

    std::vector<MergeTreeDataPartPtr> group;
    for (const auto & part : parts)
    {
        const auto state = entry.classify(part->info);
        if (!retried_commit_id.empty())
        {
            /// Exactly the claimed parts, so that the retry commits the rows of the failed task and no
            /// other: the parts that became due meanwhile wait for the next group.
            if (state == PartExportState::CLAIMED)
                group.push_back(part);
        }
        else if (state == PartExportState::NONE && isDue(*part, export_ttls, now) && !storage.isPartBeingMerged(part->info))
        {
            group.push_back(part);
        }
    }

    /// The parts of a failed task that no longer exist, e.g. were dropped, have nothing left to export.
    if (!retried_commit_id.empty() && group.empty())
    {
        LOG_INFO(log, "Export task {} of partition {} failed and none of its parts exists anymore, releasing its claim",
            entry.claim->transaction_id, partition_id);
        entry.releaseClaim();
        changed = true;
        retried_commit_id.clear();

        for (const auto & part : parts)
            if (entry.classify(part->info) == PartExportState::NONE && isDue(*part, export_ttls, now) && !storage.isPartBeingMerged(part->info))
                group.push_back(part);
    }

    if (!group.empty())
    {
        MergeTreeData::ExportPartsRequest request;
        request.destination = destination;
        request.partition_id = partition_id;
        request.parts.assign(group.begin(), group.end());
        request.source = ExportTaskSource::ttl;
        request.transaction_id = toString(UUIDHelpers::generateV4());
        request.commit_id = retried_commit_id.empty() ? request.transaction_id : retried_commit_id;

        std::vector<MergeTreePartInfo> infos;
        infos.reserve(group.size());
        for (const auto & part : group)
            infos.push_back(part->info);

        MergeTreeData::ExportTTLIndexUpdate update{.destination_key = destination_key, .entry = versioned};
        auto & updated = update.entry.entry;
        if (retried_commit_id.empty())
            updated.startClaim(request.commit_id, request.transaction_id, infos);
        else
            updated.retryClaim(request.transaction_id);

        if (storage.exportParts(request, context, &update))
        {
            if (retried_commit_id.empty())
                LOG_INFO(log, "Started export task {} of {} part(s) of partition {} to {}",
                    request.transaction_id, group.size(), partition_id, destination->getStorageID().getNameForLogs());
            else
                LOG_INFO(log, "Started export task {} of {} part(s) of partition {} to {}, retrying commit {}",
                    request.transaction_id, group.size(), partition_id, destination->getStorageID().getNameForLogs(), retried_commit_id);
            return;
        }

        LOG_DEBUG(log, "Merges or the export index of partition {} changed while starting an export task, will retry", partition_id);
    }

    if (changed && !ttl_index.updateEntry(destination_key, versioned))
        LOG_DEBUG(log, "The export index of partition {} changed concurrently, will retry", partition_id);
}

void ExportTTLScheduler::cleanupDestination(const String & destination_key, const std::map<String, ExportTTLVersionedEntry> & entries)
{
    bool claims_left = false;

    for (auto [_, versioned] : entries)
    {
        if (!versioned.entry.claim)
            continue;

        const auto transaction_id = versioned.entry.claim->transaction_id;
        if (storage.getExportTaskStatus(transaction_id) == MergeTreeData::ExportTaskStatus::PENDING)
        {
            LOG_INFO(log, "Killing export task {}: the EXPORT TTL no longer exports to destination {}", transaction_id, destination_key);
            storage.killExportTask(transaction_id);
            claims_left = true;
            continue;
        }

        versioned.entry.releaseClaim();
        if (!ttl_index.updateEntry(destination_key, versioned))
            claims_left = true;
    }

    if (!claims_left)
        ttl_index.removeDestination(destination_key);
}

std::vector<ExportTTLPartitionInfo> ExportTTLScheduler::getInfo() const
{
    const auto metadata = storage.getInMemoryMetadataPtr(nullptr, false);
    const auto export_ttls = metadata->getExportTTLs();
    if (export_ttls.empty())
        return {};

    /// Until the index is read, it is not known what was exported, so the table has no rows.
    const auto snapshot = ttl_index.getLatest();
    if (!snapshot)
        return {};

    const auto destination_id = storage.getExportTTLDestination(export_ttls.front());
    const auto & entries = getEntries(*snapshot, getDestinationKey(destination_id));
    const auto parts_by_partition = getPartsByPartition(storage);
    const auto scheduler_replica = ttl_index.getSchedulerReplica();
    const auto gate = storage.getExportTTLDeleteGate();
    const time_t now = time(nullptr);

    std::map<String, String> errors;
    String error;
    {
        std::lock_guard lock(mutex);
        errors = last_errors;
        error = table_error;
    }

    std::vector<ExportTTLPartitionInfo> result;
    for (const auto & partition_id : getPartitionIds(entries, parts_by_partition))
    {
        auto & info = result.emplace_back();
        info.partition_id = partition_id;
        info.destination_database = destination_id.database_name;
        info.destination_table = destination_id.table_name;
        info.scheduler_replica = scheduler_replica;

        ExportTTLIndexEntry entry;
        entry.partition_id = partition_id;
        if (const auto it = entries.find(partition_id); it != entries.end())
            entry = it->second.entry;

        if (const auto it = parts_by_partition.find(partition_id); it != parts_by_partition.end())
        {
            for (const auto & part : it->second)
            {
                if (gate.check(part->name, part->info, part->ttl_infos, snapshot->fence.get()))
                    ++info.parts_held_by_delete_gate;

                const auto state = entry.classify(part->info);
                if (state == PartExportState::EXPORTED)
                {
                    ++info.exported_parts;
                }
                else if (state == PartExportState::CLAIMED)
                {
                    ++info.claimed_parts;
                }
                else if (isDue(*part, export_ttls, now))
                {
                    ++info.eligible_parts;
                    info.eligible_bytes += part->getBytesOnDisk();
                }
            }
        }

        if (entry.claim && storage.getKnownExportTaskStatus(entry.claim->transaction_id) == MergeTreeData::ExportTaskStatus::PENDING)
            info.current_transaction_id = entry.claim->transaction_id;

        const auto error_it = errors.find(partition_id);
        info.last_error = error_it == errors.end() ? error : error_it->second;
    }

    return result;
}

}
