#include <Storages/MergeTree/TTLExportScheduler.h>

#include <Core/ServerSettings.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTPartition.h>
#include <Storages/ExportReplicatedMergeTreePartitionManifest.h>
#include <Storages/ExportReplicatedMergeTreePartitionTaskEntry.h>
#include <Storages/MergeTree/MergeTreeDataPartTTLInfo.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Storages/PartitionCommands.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/TTLDescription.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/logger_useful.h>

#include <magic_enum.hpp>

#include <random>

namespace DB
{

namespace ServerSetting
{
    extern const ServerSettingsBool allow_experimental_export_merge_tree_partition;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsUInt64 export_merge_tree_partition_ttl_poll_interval_seconds;
    extern const MergeTreeSettingsUInt64 export_merge_tree_partition_ttl_min_backoff_seconds;
    extern const MergeTreeSettingsUInt64 export_merge_tree_partition_ttl_max_backoff_seconds;
}

namespace ErrorCodes
{
    extern const int UNKNOWN_TABLE;
}

namespace
{

/// Uniform ±25% jitter multiplier.
double jitter25()
{
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.75, 1.25);
    return dist(rng);
}

time_t computeBackoffDelay(size_t tries, UInt64 min_seconds, UInt64 max_seconds)
{
    if (tries == 0)
        tries = 1;
    /// Cap the shift to avoid UB; anything past 63 saturates to max anyway.
    const size_t shift = std::min<size_t>(tries - 1, 63);
    UInt64 base = min_seconds << shift;
    if (base == 0 || base > max_seconds)
        base = max_seconds;
    return static_cast<time_t>(static_cast<double>(base) * jitter25());
}

}

TTLExportScheduler::TTLExportScheduler(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
    , log(getLogger(storage.getStorageID().getNameForLogs() + " (TTLExport)"))
{
}

void TTLExportScheduler::run()
{
    /// Early returns intentionally skip the reschedule at the bottom:
    ///   - shutdown_called: the task will be deactivated by `partialShutdown`.
    ///   - is_readonly: `ReplicatedMergeTreeRestartingThread` deactivates and reactivates these
    ///     tasks across readonly transitions.
    ///   - no export TTL: the `alter` path calls `schedule()` when a TTL is added.
    ///   - experimental gate off: not toggleable at runtime in practice.
    if (storage.shutdown_called || storage.is_readonly)
        return;

    auto metadata = storage.getInMemoryMetadataPtr();
    if (!metadata->hasAnyExportTTL())
        return;

    const auto global_context = Context::getGlobalContextInstance();
    if (!global_context->getServerSettings()[ServerSetting::allow_experimental_export_merge_tree_partition])
        return;

    for (const auto & export_ttl : metadata->getExportTTLs())
    {
        try
        {
            processExportTTL(export_ttl);
        }
        catch (const Coordination::Exception &)
        {
            tryLogCurrentException(log, "ZK race while processing TTL export; will retry on next tick");
        }
        catch (...)
        {
            tryLogCurrentException(log, "Unhandled exception while processing TTL export");
        }
    }

    const auto settings = storage.getSettings();
    const UInt64 poll_seconds = (*settings)[MergeTreeSetting::export_merge_tree_partition_ttl_poll_interval_seconds];
    const auto delay_ms = static_cast<size_t>(static_cast<double>(poll_seconds) * 1000.0 * jitter25());
    storage.ttl_export_task->scheduleAfter(delay_ms);
}

std::optional<TTLExportScheduler::TtlMarker> TTLExportScheduler::findTtlMarker(
    const String & dest_database, const String & dest_table)
{
    std::lock_guard lock(storage.export_merge_tree_partition_mutex);

    std::optional<TtlMarker> latest;
    for (const auto & entry : storage.export_merge_tree_partition_task_entries_by_key)
    {
        if (entry.manifest.export_origin != ExportOrigin::ttl)
            continue;
        if (entry.manifest.destination_database != dest_database
            || entry.manifest.destination_table != dest_table)
            continue;

        if (!latest || entry.manifest.create_time > latest->create_time)
        {
            latest = TtlMarker{
                entry.manifest.partition_id,
                String(magic_enum::enum_name(entry.status)),
                entry.manifest.create_time
            };
        }
    }
    return latest;
}

std::optional<String> TTLExportScheduler::pickPartition(
    const TTLDescription & export_ttl, const std::optional<String> & floor)
{
    const auto active_parts = storage.getDataPartsVectorForInternalUsage();
    if (active_parts.empty())
        return std::nullopt;

    std::map<String, DataPartsVector> by_partition;
    for (const auto & part : active_parts)
        by_partition[part->info.getPartitionId()].push_back(part);

    /// Forward-walk in expiration-max order, not partition_id order: partition IDs are
    /// arbitrary strings derived from the partition expression, so lex comparison can
    /// permanently strand newer partitions when widths differ (e.g. "10" < "9").
    /// The floor's expiration max is recomputed from the source's current parts.
    std::optional<time_t> floor_max;
    if (floor)
    {
        if (auto it = by_partition.find(*floor); it != by_partition.end())
            floor_max = getPartitionExportTTLMax(export_ttl, it->second);
    }

    const auto now = time(nullptr);
    std::optional<String> best;
    std::optional<time_t> best_max;

    for (const auto & [pid, parts] : by_partition)
    {
        if (floor && pid == *floor)
            continue;
        const auto max_ttl = getPartitionExportTTLMax(export_ttl, parts);
        if (!max_ttl || *max_ttl >= now)
            continue;
        if (floor_max && *max_ttl < *floor_max)
            continue;
        if (!best_max || *max_ttl < *best_max)
        {
            best = pid;
            best_max = max_ttl;
        }
    }
    return best;
}

TTLExportScheduler::SubmitResult TTLExportScheduler::submit(
    const String & dest_database, const String & dest_table, const String & partition_id, bool force)
{
    auto cmd_context = Context::createCopy(storage.getContext());
    cmd_context->setSetting("export_merge_tree_partition_mark_as_ttl", true);
    if (force)
        cmd_context->setSetting("export_merge_tree_partition_force_export", true);

    PartitionCommand cmd;
    cmd.type = PartitionCommand::EXPORT_PARTITION;
    cmd.to_database = dest_database;
    cmd.to_table = dest_table;
    auto partition_ast = make_intrusive<ASTPartition>();
    partition_ast->setPartitionID(make_intrusive<ASTLiteral>(partition_id));
    cmd.partition = partition_ast;

    try
    {
        storage.exportPartitionToTable(cmd, cmd_context);
        LOG_INFO(log, "Submitted TTL export of partition {} to {}.{} (force={})",
            partition_id, dest_database, dest_table, force);
        return SubmitResult::Submitted;
    }
    catch (const Coordination::Exception &)
    {
        tryLogCurrentException(log, "ZK race while submitting TTL export");
        return SubmitResult::Transient;
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::UNKNOWN_TABLE)
        {
            LOG_WARNING(log, "TTL EXPORT destination {}.{} disappeared at submit time: {}",
                dest_database, dest_table, e.message());
            return SubmitResult::Transient;
        }
        tryLogCurrentException(log, "TTL export submission failed");
        return SubmitResult::Failure;
    }
    catch (...)
    {
        tryLogCurrentException(log, "TTL export submission failed");
        return SubmitResult::Failure;
    }
}

void TTLExportScheduler::processExportTTL(const TTLDescription & export_ttl)
{
    const auto context = storage.getContext();
    /// Unqualified destination names resolve against the source's database (where the TTL was
    /// declared), not the scheduler context's `current_database`, which is the global server
    /// context's default.
    const auto dest_database = export_ttl.destination_database.empty()
        ? storage.getStorageID().getDatabaseName()
        : export_ttl.destination_database;
    const auto & dest_table = export_ttl.destination_name;
    const String dest_full = dest_database + "." + dest_table;

    if (!DatabaseCatalog::instance().tryGetTable({dest_database, dest_table}, context))
    {
        LOG_WARNING(log, "TTL EXPORT destination {} does not exist; will retry on next tick", dest_full);
        return;
    }

    const auto marker = findTtlMarker(dest_database, dest_table);

    if (!marker)
    {
        if (auto pid = pickPartition(export_ttl, std::nullopt))
            (void)submit(dest_database, dest_table, *pid, /* force = */ false);
        return;
    }

    if (marker->status == "PENDING")
        return;

    if (marker->status == "COMPLETED")
    {
        if (auto pid = pickPartition(export_ttl, marker->partition_id))
            (void)submit(dest_database, dest_table, *pid, /* force = */ false);
        return;
    }

    if (marker->status == "FAILED")
    {
        const BackoffKey key{marker->partition_id, dest_database, dest_table};
        const auto now = time(nullptr);
        auto & state = backoff[key];
        if (now < state.next_attempt_at)
            return;

        const auto result = submit(dest_database, dest_table, marker->partition_id, /* force = */ true);
        if (result == SubmitResult::Submitted)
        {
            backoff.erase(key);
        }
        else if (result == SubmitResult::Failure)
        {
            const auto settings = storage.getSettings();
            const auto min_s = (*settings)[MergeTreeSetting::export_merge_tree_partition_ttl_min_backoff_seconds];
            const auto max_s = (*settings)[MergeTreeSetting::export_merge_tree_partition_ttl_max_backoff_seconds];
            state.tries += 1;
            const auto delay = computeBackoffDelay(state.tries, min_s, max_s);
            state.next_attempt_at = now + delay;
            LOG_INFO(log, "TTL export of partition {} to {} failed (try {}); next attempt in {}s",
                marker->partition_id, dest_full, state.tries, delay);
        }
        return;
    }

    if (marker->status == "KILLED")
    {
        LOG_WARNING(log,
            "TTL export scheduler is idle for {}: most recent ttl-origin manifest at partition {} is KILLED. "
            "To retry: `ALTER TABLE {} EXPORT PARTITION '{}' TO TABLE {} SETTINGS "
            "export_merge_tree_partition_mark_as_ttl=1, export_merge_tree_partition_force_export=1`. "
            "Or advance past it by exporting a newer partition with mark_as_ttl=1.",
            dest_full, marker->partition_id,
            storage.getStorageID().getFullTableName(), marker->partition_id, dest_full);
        return;
    }

    LOG_WARNING(log, "Unrecognised TTL export status `{}` for partition {} of {}; ignoring",
        marker->status, marker->partition_id, dest_full);
}

}
