#include <Storages/MergeTree/ExportPartitionTTLTask.h>

#include <Common/logger_useful.h>
#include <Core/QualifiedTableName.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTPartition.h>
#include <Storages/MergeTree/MergeTreePartitionTopBoundary.h>
#include <Storages/PartitionCommands.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/TTLDescription.h>

#include <algorithm>
#include <ctime>


namespace DB
{

ExportPartitionTTLTask::ExportPartitionTTLTask(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
{
}

namespace
{

/// Default cadence between iterations when there is nothing immediately ready to export.
constexpr std::chrono::milliseconds kDefaultDelay{10'000};

/// Soft cap on the per-rule wait so we don't sleep for an hour if a partition becomes
/// eligible right after we exit run().
constexpr std::chrono::milliseconds kMaxDelay{60 * 60 * 1000};

bool entryCoversPartition(
    const ExportReplicatedMergeTreePartitionTaskEntry & entry,
    const String & destination_database,
    const String & destination_table,
    const String & partition_id)
{
    return entry.manifest.destination_database == destination_database
        && entry.manifest.destination_table == destination_table
        && entry.manifest.partition_id == partition_id;
}

}

std::chrono::milliseconds ExportPartitionTTLTask::run()
{
    auto log = storage.log.load();

    const auto metadata = storage.getInMemoryMetadataPtr();
    if (!metadata || !metadata->hasAnyExportTTL())
        return kDefaultDelay;

    /// Validate the partition expression once per tick. If it's unsupported (e.g. user manually
    /// modified the metadata to add a TTL EXPORT with a non-curated partition expression), give up.
    if (!MergeTreePartitionTopBoundary::isPartitionExpressionSupported(metadata->getPartitionKey()))
    {
        LOG_WARNING(log,
            "ExportPartitionTTLTask: partition expression of table {} is not in the curated EXPORT TTL whitelist; "
            "skipping. Drop the TTL EXPORT rule or change PARTITION BY to a supported expression.",
            storage.getStorageID().getNameForLogs());
        return kDefaultDelay;
    }

    const auto all_partition_ids_set = storage.getAllPartitionIds();
    if (all_partition_ids_set.empty())
        return kDefaultDelay;

    /// Sort partitions numerically (per the curated whitelist they're all integer-valued).
    /// Lex compare is not always equivalent (DateTime unix-ts strings differ in length).
    std::vector<String> all_partition_ids(all_partition_ids_set.begin(), all_partition_ids_set.end());
    std::sort(all_partition_ids.begin(), all_partition_ids.end(),
        [&](const String & a, const String & b)
        {
            return MergeTreePartitionTopBoundary::comparePartitionIds(metadata->getPartitionKey(), a, b) < 0;
        });

    const auto now = time(nullptr);
    auto min_delay = kDefaultDelay;

    for (const auto & rule : metadata->table_ttl.export_ttl)
    {
        /// Resolve the destination once, through the storage helper. This yields a non-empty
        /// database (an omitted `db.` qualifier resolves to the source table's database) that is
        /// used both for cache lookups below and as `command.to_database` when scheduling the
        /// export, so the database written into the manifest always matches the lookup keys.
        const QualifiedTableName destination = storage.resolveExportTTLDestination(rule);

        /// Quickly skip rules that already have an in-flight TTL entry for this destination
        /// (one-export-at-a-time per destination), and grab the marker.
        String marker;
        bool has_pending_for_destination = false;
        {
            std::lock_guard lock(storage.export_merge_tree_partition_mutex);
            for (const auto & entry : storage.export_merge_tree_partition_task_entries_by_key)
            {
                if (entry.manifest.origin != ExportReplicatedMergeTreePartitionOrigin::TTL)
                    continue;
                if (entry.manifest.destination_database != destination.database
                    || entry.manifest.destination_table != destination.table)
                    continue;
                if (entry.status == ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING)
                {
                    has_pending_for_destination = true;
                    break;
                }
            }

            /// Marker is derived on demand from the manifest cache (the only source of truth for
            /// TTL-origin entries). This keeps `StorageReplicatedMergeTree` free of a redundant
            /// derived data structure that has to be maintained from multiple write sites.
            marker = storage.computeTTLExportMarkerLocked(destination);
        }

        if (has_pending_for_destination)
            continue;

        /// Walk candidates: partition_ids strictly greater than the marker, in ascending order.
        for (const auto & partition_id : all_partition_ids)
        {
            if (!marker.empty()
                && MergeTreePartitionTopBoundary::comparePartitionIds(metadata->getPartitionKey(), partition_id, marker) <= 0)
            {
                continue;
            }

            /// Anti-duplication: if any entry (any origin, any status) covers this partition for
            /// this destination, skip and advance the marker past it on the next add/recompute.
            bool already_covered = false;
            {
                std::lock_guard lock(storage.export_merge_tree_partition_mutex);
                for (const auto & entry : storage.export_merge_tree_partition_task_entries_by_key)
                {
                    if (entryCoversPartition(entry, destination.database, destination.table, partition_id))
                    {
                        already_covered = true;
                        break;
                    }
                }
            }
            if (already_covered)
            {
                /// The entry is already in the cache (e.g. user manually exported it with ALTER).
                /// We don't update the TTL marker here because the entry is not origin=TTL; we just
                /// skip past it on this pass and the next eligible TTL export will move the marker.
                continue;
            }

            /// Has the partition's TTL window elapsed?
            const time_t top_boundary = MergeTreePartitionTopBoundary::computeTopBoundary(
                metadata->getPartitionKey(), partition_id);
            const time_t fires_at = MergeTreePartitionTopBoundary::addInterval(
                top_boundary, rule.export_interval_kind, rule.export_interval_count);

            if (fires_at > now)
            {
                /// Not yet eligible. Sleep just long enough to wake up when it is.
                const auto wait_ms = std::min<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(kMaxDelay).count(),
                    static_cast<int64_t>(fires_at - now) * 1000);
                if (wait_ms > 0 && std::chrono::milliseconds(wait_ms) < min_delay)
                    min_delay = std::chrono::milliseconds(wait_ms);
                break;
            }

            /// Schedule the export. Catch any exception (including the experimental-flag gate)
            /// and keep looping over the next rule.
            try
            {
                PartitionCommand command;
                command.type = PartitionCommand::EXPORT_PARTITION;
                auto partition_ast = make_intrusive<ASTPartition>();
                partition_ast->setPartitionID(make_intrusive<ASTLiteral>(partition_id));
                command.partition = partition_ast;
                command.to_database = destination.database;
                command.to_table = destination.table;

                /// A new context — using the global query context — keeps server-level settings.
                /// We don't want user-session overrides for the background scheduler.
                auto background_context = Context::createCopy(storage.getContext());

                background_context->setSetting("allow_insert_into_iceberg", 1);

                storage.exportPartitionToTableWithOrigin(
                    command, background_context, ExportReplicatedMergeTreePartitionOrigin::TTL);

                LOG_INFO(log,
                    "ExportPartitionTTLTask: scheduled TTL export of partition {} from {} to {}.{}",
                    partition_id,
                    storage.getStorageID().getNameForLogs(),
                    destination.database, destination.table);
            }
            catch (const Exception & e)
            {
                /// SUPPORT_IS_DISABLED is the typical "experimental flag is off" exception. Log
                /// once per minute per (table, destination) to avoid log spam.
                auto & last_at = last_experimental_off_log_at[destination];
                if (last_at + 60 < now)
                {
                    LOG_INFO(log,
                        "ExportPartitionTTLTask: could not schedule TTL export of partition {} from {} to {}.{}: {}",
                        partition_id,
                        storage.getStorageID().getNameForLogs(),
                        destination.database, destination.table,
                        e.message());
                    last_at = now;
                }
            }
            catch (...)
            {
                tryLogCurrentException(log, __PRETTY_FUNCTION__);
            }

            /// At most one TTL-scheduled export per (src, dst) per tick — break out to the next rule.
            break;
        }
    }

    return min_delay;
}

}
