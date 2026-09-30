#include <Storages/MergeTree/ExportTTLScheduler.h>

#include <Core/UUID.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Storages/MergeTree/MergeTreeData.h>
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
    extern const MergeTreeSettingsUInt64 ttl_export_batch_window_seconds;
    extern const MergeTreeSettingsUInt64 ttl_export_batch_max_delay_seconds;
    extern const MergeTreeSettingsUInt64 ttl_export_batch_min_bytes;
    extern const MergeTreeSettingsUInt64 ttl_export_max_parts_per_group;
    extern const MergeTreeSettingsUInt64 ttl_export_max_bytes_per_group;
    extern const MergeTreeSettingsUInt64 ttl_export_max_concurrent_groups;
    extern const MergeTreeSettingsString ttl_export_settings_profile;
}

namespace
{

/// How long after its task failed a commit may still land: a destination may apply a request after
/// its sender gave up waiting for it.
constexpr time_t late_commit_window_seconds = 600;

/// The rule of move TTL: a part is eligible once the maximum TTL value of its rows is due. A part
/// without the TTL info (written before the TTL was added and not materialized) is never eligible.
bool isEligible(const IMergeTreeDataPart & part, const TTLDescriptions & export_ttls, time_t now)
{
    return selectTTLDescriptionForTTLInfos(export_ttls, part.ttl_infos.export_ttl, now, /* use_max */ true).has_value();
}

}

ExportTTLScheduler::ExportTTLScheduler(MergeTreeData & storage_)
    : storage(storage_)
    , log(getLogger(fmt::format("{} (ExportTTLScheduler)", storage_.getLogName())))
    , parts_held_by_delete_gate(CurrentMetrics::ExportTTLPartsHeldByDeleteGate, 0)
{
}

String ExportTTLScheduler::getDestinationKey() const
{
    std::lock_guard lock(mutex);
    return current_destination_key;
}

std::vector<ExportTTLPartitionInfo> ExportTTLScheduler::getInfo() const
{
    std::lock_guard lock(mutex);
    std::vector<ExportTTLPartitionInfo> result;
    result.reserve(info_by_partition.size());
    for (const auto & [_, info] : info_by_partition)
    {
        result.push_back(info);
        if (result.back().last_error.empty())
            result.back().last_error = current_destination_error;
    }
    return result;
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

std::vector<MergeTreeDataPartPtr> ExportTTLScheduler::GroupToStart::partsWithRows() const
{
    std::vector<MergeTreeDataPartPtr> result;
    for (const auto & part : parts)
        if (part->rows_count != 0)
            result.push_back(part);
    return result;
}

const ExportTTLScheduler::TaskState & ExportTTLScheduler::getCachedTaskState(TaskStates & task_states, const String & transaction_id)
{
    auto it = task_states.find(transaction_id);
    if (it == task_states.end())
        it = task_states.emplace(transaction_id, getTaskState(transaction_id)).first;
    return it->second;
}

UInt64 ExportTTLScheduler::run()
{
    const auto settings = storage.getSettings();
    const time_t period = static_cast<time_t>(std::max<UInt64>(1, (*settings)[MergeTreeSetting::ttl_export_check_period_seconds]));

    const auto metadata = storage.getInMemoryMetadataPtr(nullptr, false);
    const auto export_ttls = metadata->getExportTTLs();

    {
        std::lock_guard lock(mutex);
        if (!export_ttls.empty())
            may_have_index = true;
        if (!may_have_index)
            return period * 1000;
    }

    const auto context = makeContext();

    /// Every replica resolves the destination, because the delete gate of its merges depends on it.
    StoragePtr destination;
    String destination_key;
    String destination_error;
    if (!export_ttls.empty())
    {
        const auto destination_id = storage.getExportTTLDestination(export_ttls.front());
        destination = DatabaseCatalog::instance().tryGetTable(destination_id, context);
        if (destination)
            destination_key = ExportTTLUtils::destinationKey(destination_id.database_name, destination_id.table_name);
        else
            destination_error = fmt::format("The destination table {} of the EXPORT TTL does not exist", destination_id.getNameForLogs());
    }

    /// A destination that cannot be resolved may be created again or not loaded yet, so what was
    /// exported to it is kept, and its parts stay held from the delete TTL.
    const bool destination_missing = !export_ttls.empty() && !destination;

    {
        std::lock_guard lock(mutex);
        if (!destination_missing && current_destination_key != destination_key)
        {
            batches.clear();
            last_errors.clear();
            info_by_partition.clear();
            current_destination_key = destination_key;
        }
        current_destination_error = destination_error;
    }

    const auto snapshot = getIndexSnapshot();
    if (export_ttls.empty() && snapshot->entries.empty())
    {
        std::lock_guard lock(mutex);
        may_have_index = false;
        batches.clear();
        last_errors.clear();
        info_by_partition.clear();
        parts_held_by_delete_gate.changeTo(0);
        return period * 1000;
    }

    const bool is_scheduler = acquireSchedulerLock();
    if (is_scheduler && !destination_missing)
    {
        for (const auto & [key, index] : snapshot->entries)
        {
            if (key == destination_key)
                continue;

            try
            {
                cleanupDestination(key, index);
            }
            catch (...)
            {
                tryLogCurrentException(log, fmt::format("While removing the TTL export index of destination {}", key));
            }
        }
    }

    if (!destination)
    {
        if (is_scheduler && !destination_error.empty())
            LOG_WARNING(log, "{}, nothing is exported", destination_error);
        return period * 1000;
    }

    const bool act = is_scheduler && !isPaused();
    const String scheduler_replica = is_scheduler ? getReplicaName() : getSchedulerReplica();

    const time_t now = time(nullptr);

    static const std::map<String, ExportTTLVersionedEntry> no_entries;
    const auto index_it = snapshot->entries.find(destination_key);
    const auto & index = index_it == snapshot->entries.end() ? no_entries : index_it->second;

    std::map<String, std::vector<MergeTreeDataPartPtr>> parts_by_partition;
    for (const auto & part : storage.getDataPartsVectorForInternalUsage())
        if (!part->info.isPatch())
            parts_by_partition[part->info.getPartitionId()].push_back(part);

    TaskStates task_states;
    size_t in_flight = 0;
    if (act)
    {
        for (const auto & [_, versioned] : index)
            if (const auto & claim = versioned.entry.claim;
                claim && getCachedTaskState(task_states, claim->transaction_id).status == TaskStatus::PENDING)
                ++in_flight;
    }

    std::set<String> partition_ids;
    for (const auto & [partition_id, _] : index)
        partition_ids.insert(partition_id);
    for (const auto & [partition_id, _] : parts_by_partition)
        partition_ids.insert(partition_id);

    time_t next_tick = now + period;
    const std::vector<MergeTreeDataPartPtr> no_parts;
    for (const auto & partition_id : partition_ids)
    {
        ExportTTLVersionedEntry versioned;
        if (const auto it = index.find(partition_id); it != index.end())
            versioned = it->second;
        else
            versioned.entry.partition_id = partition_id;

        const auto parts_it = parts_by_partition.find(partition_id);
        const auto & parts = parts_it == parts_by_partition.end() ? no_parts : parts_it->second;

        PartitionBatch batch;
        String last_error;
        {
            std::lock_guard lock(mutex);
            batch = batches[partition_id];
            if (const auto it = last_errors.find(partition_id); it != last_errors.end())
                last_error = it->second;
        }

        PartitionView view;
        try
        {
            view = observePartition(versioned.entry, parts, export_ttls, now, std::move(batch), task_states);

            /// The error of acting on the partition is shown until the scheduler acts on it again.
            if (is_scheduler && !act)
                view.info.last_error = last_error;

            if (act)
            {
                /// Shown as it is after acting, e.g. with the parts of a recorded commit as exported.
                if (const auto updated_entry = actOnPartition(destination_key, destination, std::move(versioned), view, now, in_flight, task_states, context))
                    view = observePartition(*updated_entry, parts, export_ttls, now, std::move(view.batch), task_states);
            }
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("While exporting partition {} by TTL", partition_id));
            view.info.partition_id = partition_id;
            view.info.last_error = getCurrentExceptionMessage(/* with_stacktrace */ false);
        }

        view.info.destination_database = destination->getStorageID().database_name;
        view.info.destination_table = destination->getStorageID().table_name;
        view.info.scheduler_replica = scheduler_replica;

        if (view.info.next_group_time > now)
            next_tick = std::min(next_tick, view.info.next_group_time);
        if (view.next_eligible_time > now)
            next_tick = std::min(next_tick, view.next_eligible_time);

        std::lock_guard lock(mutex);
        batches[partition_id] = std::move(view.batch);
        if (view.info.last_error.empty())
            last_errors.erase(partition_id);
        else
            last_errors[partition_id] = view.info.last_error;
        info_by_partition[partition_id] = std::move(view.info);
    }

    {
        std::lock_guard lock(mutex);
        std::erase_if(info_by_partition, [&](const auto & item) { return !partition_ids.contains(item.first); });
        std::erase_if(batches, [&](const auto & item) { return !partition_ids.contains(item.first); });
        std::erase_if(last_errors, [&](const auto & item) { return !partition_ids.contains(item.first); });

        size_t held = 0;
        for (const auto & [_, info] : info_by_partition)
            held += info.parts_held_by_delete_gate;
        parts_held_by_delete_gate.changeTo(held);
    }

    return static_cast<UInt64>(std::max<time_t>(1, next_tick - now)) * 1000;
}

ExportTTLScheduler::ResolvedClaim ExportTTLScheduler::resolveClaim(
    ExportTTLIndexEntry & entry, const StoragePtr & destination, TaskStates & task_states, const ContextPtr & context)
{
    ResolvedClaim result;
    if (!entry.claim)
        return result;

    const auto transaction_id = entry.claim->transaction_id;
    const auto & state = getCachedTaskState(task_states, transaction_id);
    switch (state.status)
    {
        case TaskStatus::PENDING:
            result.in_flight = transaction_id;
            break;

        case TaskStatus::COMPLETED:
            /// The commit of a plain `MergeTree` records it here, after the task is marked completed.
            entry.commitClaim(transaction_id, {});
            result.changed = true;
            break;

        case TaskStatus::FAILED:
        case TaskStatus::KILLED:
        case TaskStatus::MISSING:
            /// E.g. a commit that landed and then the task timed out before it was marked completed.
            if (state.reached_commit && destination->isExportTransactionCommitted(transaction_id, context))
            {
                LOG_INFO(log, "Export task {} of partition {} did not complete, but it committed to the destination",
                    transaction_id, entry.partition_id);
                entry.commitClaim(transaction_id, {});
                result.changed = true;
                break;
            }

            result.failed = transaction_id;
            break;
    }

    return result;
}

ExportRetriedTasks ExportTTLScheduler::collectRetriedTasks(
    const ExportTTLIndexEntry & entry,
    const String & failed,
    const StoragePtr & destination,
    time_t now,
    TaskStates & task_states,
    const ContextPtr & context)
{
    ExportRetriedTasks result;
    const auto & state = getCachedTaskState(task_states, failed);

    /// A task that did not export all its parts never committed, and a missing one was either
    /// never created or checked when it went missing.
    if (state.status != TaskStatus::MISSING && state.reached_commit)
        result.push_back(ExportRetriedTask{
            .transaction_id = failed,
            .block_ranges = ExportTTLUtils::toBlockRanges(entry.claim->ranges),
            .failed_time = now,
        });

    for (const auto & retried : state.retry_of)
    {
        /// Past the window, and with no commit of it in progress, the task is checked once more,
        /// and not retried by the next groups if it did not land.
        const bool may_land = now < retried.failed_time + late_commit_window_seconds
            || isCommitInProgress(retried.transaction_id)
            || destination->isExportTransactionCommitted(retried.transaction_id, context);

        if (!may_land)
        {
            LOG_DEBUG(log, "Export task {} of partition {} did not commit, it is no longer checked", retried.transaction_id, entry.partition_id);
            continue;
        }

        result.push_back(retried);
    }

    return result;
}

ExportTTLScheduler::PartitionView ExportTTLScheduler::observePartition(
    const ExportTTLIndexEntry & entry,
    const std::vector<MergeTreeDataPartPtr> & parts,
    const TTLDescriptions & export_ttls,
    time_t now,
    PartitionBatch batch,
    TaskStates & task_states)
{
    const auto settings = storage.getSettings();

    PartitionView view;
    auto & info = view.info;
    info.partition_id = entry.partition_id;

    std::vector<MergeTreeDataPartPtr> eligible;
    for (const auto & part : parts)
    {
        const auto state = entry.classify(part->info);
        if (state == PartExportState::EXPORTED)
        {
            ++info.exported_parts;
            continue;
        }

        if (part->ttl_infos.part_min_ttl && part->ttl_infos.part_min_ttl <= now)
            ++info.parts_held_by_delete_gate;

        if (state == PartExportState::CLAIMED)
        {
            ++info.claimed_parts;
            view.claimed_parts.push_back(part);
            continue;
        }

        /// A part without rows, e.g. emptied by a mutation, waits for no row. Nothing of it is exported,
        /// but its group records it as exported, so that it merges with the exported parts around it.
        if (part->rows_count != 0 && !isEligible(*part, export_ttls, now))
        {
            for (const auto & [_, ttl_info] : part->ttl_infos.export_ttl)
                if (ttl_info.max > now && (!view.next_eligible_time || ttl_info.max < view.next_eligible_time))
                    view.next_eligible_time = ttl_info.max;
            continue;
        }

        eligible.push_back(part);
        if (!isPartBeingMerged(part))
            view.shippable.push_back(part);
    }

    std::vector<MergeTreePartInfo> eligible_ranges;
    for (const auto & part : eligible)
    {
        eligible_ranges.push_back(part->info);
        info.eligible_bytes += part->getBytesOnDisk();
    }
    eligible_ranges = ExportFenceUtils::compactRanges(std::move(eligible_ranges));

    for (const auto & part : eligible)
    {
        if (!ExportFenceUtils::isCoveredByUnion(part->info, batch.seen_ranges))
        {
            batch.last_new_part_time = now;
            if (!batch.first_eligible_time)
                batch.first_eligible_time = now;
        }
    }

    batch.seen_ranges = std::move(eligible_ranges);
    if (eligible.empty())
        batch = PartitionBatch{};

    info.eligible_parts = eligible.size();
    info.first_eligible_time = eligible.empty() ? 0 : batch.first_eligible_time;

    std::sort(view.shippable.begin(), view.shippable.end(), [](const auto & lhs, const auto & rhs) { return lhs->info.min_block < rhs->info.min_block; });

    size_t shippable_bytes = 0;
    for (const auto & part : view.shippable)
        shippable_bytes += part->getBytesOnDisk();

    if (!view.shippable.empty() && batch.first_eligible_time)
    {
        const auto window = static_cast<time_t>((*settings)[MergeTreeSetting::ttl_export_batch_window_seconds]);
        const auto max_delay = static_cast<time_t>((*settings)[MergeTreeSetting::ttl_export_batch_max_delay_seconds]);
        const UInt64 min_bytes = (*settings)[MergeTreeSetting::ttl_export_batch_min_bytes];

        info.next_group_time = std::min(batch.last_new_part_time + window, batch.first_eligible_time + max_delay);
        view.batch_ready = now >= info.next_group_time || (min_bytes && shippable_bytes >= min_bytes);
    }

    bool retry_pending = false;
    if (entry.claim)
    {
        const auto status = getCachedTaskState(task_states, entry.claim->transaction_id).status;
        if (status == TaskStatus::PENDING)
            info.current_transaction_id = entry.claim->transaction_id;
        else if (status != TaskStatus::COMPLETED)
            retry_pending = true;
    }

    if (info.current_transaction_id.empty() && (view.batch_ready || retry_pending))
        info.next_group_time = now;

    view.batch = std::move(batch);
    return view;
}

std::optional<ExportTTLIndexEntry> ExportTTLScheduler::actOnPartition(
    const String & destination_key,
    const StoragePtr & destination,
    ExportTTLVersionedEntry versioned,
    PartitionView & view,
    time_t now,
    size_t & in_flight,
    TaskStates & task_states,
    const ContextPtr & context)
{
    auto & entry = versioned.entry;
    auto & info = view.info;
    const auto & partition_id = entry.partition_id;
    const auto settings = storage.getSettings();

    auto resolved = resolveClaim(entry, destination, task_states, context);
    info.current_transaction_id = resolved.in_flight;

    /// The claimed parts are the parts of the failed task that still exist.
    std::vector<MergeTreeDataPartPtr> retry_parts;
    if (!resolved.failed.empty())
        retry_parts = view.claimed_parts;

    /// The parts of a failed task that no longer exist, e.g. were dropped, have nothing left to export.
    if (!resolved.failed.empty() && retry_parts.empty())
    {
        LOG_INFO(log, "Export task {} of partition {} failed and none of its parts exists anymore, releasing its claim",
            resolved.failed, partition_id);
        entry.releaseClaim();
        resolved.failed.clear();
        resolved.changed = true;
    }

    const bool ready = !retry_parts.empty() || view.batch_ready;
    const UInt64 max_concurrent = (*settings)[MergeTreeSetting::ttl_export_max_concurrent_groups];

    if (ready && resolved.in_flight.empty() && (!max_concurrent || in_flight < max_concurrent))
    {
        info.next_group_time = now;

        const UInt64 max_parts = (*settings)[MergeTreeSetting::ttl_export_max_parts_per_group];
        const UInt64 max_bytes = (*settings)[MergeTreeSetting::ttl_export_max_bytes_per_group];

        GroupToStart group;
        group.transaction_id = toString(UUIDHelpers::generateV4());
        group.destination_key = destination_key;
        group.destination = destination;
        group.partition_id = partition_id;

        /// Every claimed part of the failed task is retried: a part left out would lose its claim and
        /// could be exported again later although the failed task may still land.
        group.parts = retry_parts;
        size_t group_bytes = 0;
        for (const auto & part : group.parts)
            group_bytes += part->getBytesOnDisk();

        for (const auto & part : view.shippable)
        {
            if (max_parts && group.parts.size() >= max_parts)
                break;
            if (max_bytes && !group.parts.empty() && group_bytes + part->getBytesOnDisk() > max_bytes)
                break;
            group.parts.push_back(part);
            group_bytes += part->getBytesOnDisk();
        }

        if (!group.parts.empty())
        {
            /// A group of parts without rows only exports nothing, so it has no task: it records them
            /// as exported when it starts.
            const size_t parts_with_rows = group.partsWithRows().size();
            if (parts_with_rows && !resolved.failed.empty())
                group.retry_of = collectRetriedTasks(entry, resolved.failed, destination, now, task_states, context);

            /// The group takes over the claim of the failed task it retries, if any.
            group.entry = versioned;
            group.entry.entry.releaseClaim();

            std::vector<MergeTreePartInfo> infos;
            infos.reserve(group.parts.size());
            for (const auto & part : group.parts)
                infos.push_back(part->info);

            if (parts_with_rows)
                group.entry.entry.startClaim(group.transaction_id, infos);
            else
                group.entry.entry.addExported(infos);

            if (startGroup(group, context))
            {
                if (parts_with_rows)
                {
                    ++in_flight;
                    task_states.insert_or_assign(group.transaction_id, TaskState{.status = TaskStatus::PENDING, .reached_commit = false, .retry_of = group.retry_of});
                    LOG_INFO(log, "Started export task {} of {} part(s) of partition {} to {}{}",
                        group.transaction_id, parts_with_rows, partition_id, destination->getStorageID().getNameForLogs(),
                        resolved.failed.empty() ? "" : fmt::format(", retrying {}", resolved.failed));
                }
                else
                {
                    LOG_INFO(log, "Recorded {} part(s) of partition {} without rows as exported to {}",
                        group.parts.size(), partition_id, destination->getStorageID().getNameForLogs());
                }
                return std::move(group.entry.entry);
            }

            LOG_DEBUG(log, "Merges or the export index of partition {} changed while starting an export task, will retry", partition_id);
        }
    }

    if (!resolved.changed)
        return std::nullopt;

    if (!updateIndexEntry(destination_key, versioned))
    {
        LOG_DEBUG(log, "The export index of partition {} changed concurrently, will retry", partition_id);
        return std::nullopt;
    }

    return std::move(entry);
}

void ExportTTLScheduler::cleanupDestination(const String & destination_key, const std::map<String, ExportTTLVersionedEntry> & index)
{
    TaskStates task_states;
    bool claims_left = false;

    for (auto [_, versioned] : index)
    {
        if (!versioned.entry.claim)
            continue;

        const auto transaction_id = versioned.entry.claim->transaction_id;
        if (getCachedTaskState(task_states, transaction_id).status == TaskStatus::PENDING)
        {
            LOG_INFO(log, "Killing export task {}: the EXPORT TTL no longer exports to destination {}", transaction_id, destination_key);
            killTask(transaction_id);
            claims_left = true;
            continue;
        }

        versioned.entry.releaseClaim();
        if (!updateIndexEntry(destination_key, versioned))
            claims_left = true;
    }

    if (!claims_left)
        removeDestination(destination_key);
}

}
