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
#include <optional>
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
/// so that its blocks are recorded as exported and a part merged from it and exported parts is too.
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
            if (!part->info.isPatch() && gate.check(part->name, part->info, part->ttl_infos, snapshot->exported_blocks.get()))
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
    /// The current task, which failed without committing: its retry exports exactly its blocks, under its commit id.
    std::optional<MergeTreeData::ExportTaskState> failed_task;

    if (!entry.current_task.empty())
    {
        const auto task = storage.getExportTask(entry.current_task);
        if (!task)
        {
            /// A plain `MergeTree` stores the entry before it saves the task, and may crash in between.
            LOG_INFO(log, "Export task {} of partition {} does not exist, so it exported nothing", entry.current_task, partition_id);
            entry.releaseClaim();
            changed = true;
        }
        else if (task->status == MergeTreeData::ExportTaskStatus::PENDING)
        {
            return;
        }
        else if (task->status == MergeTreeData::ExportTaskStatus::COMPLETED
            || destination->isExportTransactionCommitted(task->commit_id, context))
        {
            /// A completed task recorded its blocks when it committed, so this only repeats it. A failed
            /// one may have committed nevertheless, e.g. when it timed out after its commit landed.
            if (task->status != MergeTreeData::ExportTaskStatus::COMPLETED)
                LOG_INFO(log, "Export task {} of partition {} did not complete, but it committed to the destination",
                    entry.current_task, partition_id);
            entry.commitClaim(entry.current_task, ExportTTLUtils::toPartInfos(partition_id, task->block_ranges));
            changed = true;
        }
        else
        {
            failed_task = task;
        }
    }

    std::vector<MergeTreeDataPartPtr> group;
    ExportBlockNumberRanges block_ranges;

    if (failed_task)
    {
        /// Exactly the blocks of the failed task, so that the retry commits its rows and no other: the
        /// blocks that became due meanwhile wait for the next group. The parts that held them may have
        /// been merged since, which keeps the rows of the blocks.
        const auto failed_ranges = ExportTTLUtils::toPartInfos(partition_id, failed_task->block_ranges);
        for (const auto & part : parts)
            if (ExportBlockRanges::intersectsAny(part->info, failed_ranges))
                group.push_back(part);

        if (group.empty())
        {
            /// The blocks of a failed task that no longer exist, e.g. were dropped, have nothing left to export.
            LOG_INFO(log, "Export task {} of partition {} failed and none of its blocks exists anymore, releasing it",
                entry.current_task, partition_id);
            entry.releaseClaim();
            changed = true;
            failed_task.reset();
        }
        else
        {
            block_ranges = failed_task->block_ranges;
        }
    }

    if (!failed_task)
    {
        /// Every due part with blocks that are not exported, e.g. parts merged with exported ones.
        std::vector<MergeTreePartInfo> ranges;
        for (const auto & part : parts)
        {
            if (!isDue(*part, export_ttls, now))
                continue;

            auto not_exported = entry.notExported(part->info);
            if (not_exported.empty())
                continue;

            group.push_back(part);
            ranges.insert(ranges.end(), not_exported.begin(), not_exported.end());
        }
        block_ranges = ExportTTLUtils::toBlockNumberRanges(ExportBlockRanges::compact(std::move(ranges)));
    }

    if (!group.empty())
    {
        MergeTreeData::ExportPartsRequest request;
        request.destination = destination;
        request.partition_id = partition_id;
        request.parts.assign(group.begin(), group.end());
        request.block_ranges = block_ranges;
        request.source = ExportTaskSource::ttl;
        request.transaction_id = toString(UUIDHelpers::generateV4());
        request.commit_id = failed_task ? failed_task->commit_id : request.transaction_id;

        MergeTreeData::ExportTTLIndexUpdate update{.destination_key = destination_key, .entry = versioned};
        auto & updated = update.entry.entry;
        if (failed_task)
            updated.retryClaim(request.transaction_id);
        else
            updated.startClaim(request.transaction_id);

        if (storage.exportParts(request, context, &update))
        {
            if (failed_task)
                LOG_INFO(log, "Started export task {} of {} part(s) of partition {} to {}, retrying commit {}",
                    request.transaction_id, group.size(), partition_id, destination->getStorageID().getNameForLogs(), request.commit_id);
            else
                LOG_INFO(log, "Started export task {} of {} part(s) of partition {} to {}",
                    request.transaction_id, group.size(), partition_id, destination->getStorageID().getNameForLogs());
            return;
        }

        LOG_DEBUG(log, "The export index of partition {} changed while starting an export task, will retry", partition_id);
    }

    if (changed && !ttl_index.updateEntry(destination_key, versioned))
        LOG_DEBUG(log, "The export index of partition {} changed concurrently, will retry", partition_id);
}

void ExportTTLScheduler::cleanupDestination(const String & destination_key, const std::map<String, ExportTTLVersionedEntry> & entries)
{
    bool claims_left = false;

    for (auto [_, versioned] : entries)
    {
        if (versioned.entry.current_task.empty())
            continue;

        const auto transaction_id = versioned.entry.current_task;
        const auto task = storage.getExportTask(transaction_id);
        if (task && task->status == MergeTreeData::ExportTaskStatus::PENDING)
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

        std::optional<MergeTreeData::ExportTaskState> current_task;
        std::vector<MergeTreePartInfo> current_task_ranges;
        if (!entry.current_task.empty())
        {
            current_task = storage.getKnownExportTask(entry.current_task);
            if (current_task)
                current_task_ranges = ExportTTLUtils::toPartInfos(partition_id, current_task->block_ranges);
        }

        if (const auto it = parts_by_partition.find(partition_id); it != parts_by_partition.end())
        {
            for (const auto & part : it->second)
            {
                if (gate.check(part->name, part->info, part->ttl_infos, snapshot->exported_blocks.get()))
                    ++info.parts_held_by_delete_gate;

                const auto state = entry.classify(part->info, current_task_ranges);
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

        if (current_task && current_task->status == MergeTreeData::ExportTaskStatus::PENDING)
            info.current_transaction_id = entry.current_task;

        const auto error_it = errors.find(partition_id);
        info.last_error = error_it == errors.end() ? error : error_it->second;
    }

    return result;
}

}
