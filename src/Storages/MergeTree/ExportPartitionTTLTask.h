#pragma once

#include <Core/QualifiedTableName.h>
#include <chrono>
#include <unordered_map>


namespace DB
{

class StorageReplicatedMergeTree;

/// Background task that periodically scans `metadata.table_ttl.export_ttl` rules and schedules
/// partition exports for partitions whose top-boundary + TTL interval has elapsed.
///
/// Invariants:
///  - Only one TTL-originated export is in flight per `(source, destination)` pair. The marker map
///    on the storage tells the task which partition_ids have already been TTL-scheduled.
///  - The task happily runs even when `allow_experimental_export_merge_tree_partition` is OFF:
///    `StorageReplicatedMergeTree::exportPartitionToTableWithOrigin` will throw early in that case
///    and we just log + reschedule. This keeps the TTL configuration coherent on every replica
///    regardless of the experimental gate.
///  - The task is replica-local; ZK uniqueness on `export_key` makes concurrent attempts harmless.
class ExportPartitionTTLTask
{
public:
    explicit ExportPartitionTTLTask(StorageReplicatedMergeTree & storage);

    /// One iteration. Returns the suggested delay (in milliseconds) for the next scheduling.
    std::chrono::milliseconds run();

private:
    StorageReplicatedMergeTree & storage;

    /// Throttle log spam: per (table, destination), keep timestamp of last "experimental flag off"
    /// log so we report it at most once a minute.
    std::unordered_map<QualifiedTableName, time_t> last_experimental_off_log_at;
};

}
