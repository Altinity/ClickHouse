#pragma once

#include <Common/Logger.h>
#include <base/types.h>
#include <ctime>
#include <optional>

namespace DB
{
class StorageReplicatedMergeTree;
struct TTLDescription;

/// Drives automatic partition exports for `TTL ... EXPORT TO TABLE db.table` clauses.
/// One instance per `StorageReplicatedMergeTree`; the background task pool calls `run`
/// on each tick. The scheduler is stateless across restarts — the manifest in ZooKeeper
/// is the source of truth and per-tick state is rebuilt from it.
class TTLExportScheduler
{
public:
    explicit TTLExportScheduler(StorageReplicatedMergeTree & storage_);

    /// One tick: iterate every EXPORT TTL on the source table. For each destination, look up
    /// the most recent ttl-origin manifest and act on it:
    ///   - no manifest    → submit the oldest eligible partition (smallest expiration max);
    ///   - PENDING        → wait;
    ///   - COMPLETED      → walk forward, submit the next eligible partition whose expiration max
    ///                       exceeds the marker's stored watermark (or equals it but with a
    ///                       different partition_id);
    ///   - FAILED         → resubmit the same partition with `force_export=1` to overwrite the
    ///                       failed manifest;
    ///   - KILLED         → idle; log a recovery recipe. The operator either retries the killed
    ///                       partition with `force_export=1` or steps past it by exporting a newer
    ///                       partition with `mark_as_ttl=1`.
    /// All exceptions from `submit` are logged and swallowed — the next tick retries.
    /// `UNKNOWN_TABLE` at submit time is a soft failure — the destination disappeared post-DDL.
    void run();

private:
    struct TtlMarker
    {
        String partition_id;
        String status;
        time_t create_time = 0;
        /// Partition-wide max of the EXPORT TTL expression at the time the manifest was created.
        /// Acts as the scheduler's high-water mark — `pickPartition` uses it as `floor_max` so the
        /// ordering survives the marker's partition being dropped from the source (paired DELETE TTL).
        time_t expiration_max = 0;
    };

    StorageReplicatedMergeTree & storage;
    LoggerPtr log;

    void processExportTTL(const TTLDescription & export_ttl);

    /// Locate the current ttl-origin manifest for `(this src, dest)`. The submission-time
    /// invariant keeps this set bounded; on transient races where more than one ttl-origin
    /// manifest coexists, the most recent by `create_time` is returned.
    std::optional<TtlMarker> findTtlMarker(const String & dest_database, const String & dest_table);

    /// Return the eligible partition with the smallest expiration max. When `floor` is set
    /// (a COMPLETED marker), only partitions with a strictly greater expiration max — or the same
    /// expiration max but a different partition_id — are considered. The floor's `expiration_max`
    /// is the durable watermark; no source-parts lookup needed for the floor itself.
    std::optional<String> pickPartition(const TTLDescription & export_ttl, const std::optional<TtlMarker> & floor);

    /// Submit a TTL-driven export via `exportPartitionToTable`. Exceptions are caught and logged;
    /// the next tick retries.
    void submit(const String & dest_database, const String & dest_table, const String & partition_id, bool force);
};

}
