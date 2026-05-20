#pragma once

#include <Common/Logger.h>
#include <base/types.h>
#include <ctime>
#include <map>
#include <optional>
#include <tuple>

namespace DB
{
class StorageReplicatedMergeTree;
struct TTLDescription;

/// Drives automatic partition exports for `TTL ... EXPORT TO TABLE db.table` clauses.
/// One instance per `StorageReplicatedMergeTree`; the background task pool calls `run`
/// on each tick. The scheduler is stateless across restarts — the manifest in ZooKeeper
/// is the source of truth and per-tick state is rebuilt from it. Only the per-partition
/// retry backoff is held in memory.
class TTLExportScheduler
{
public:
    explicit TTLExportScheduler(StorageReplicatedMergeTree & storage_);

    /// One tick: iterate every EXPORT TTL on the source table. For each destination, look up
    /// the most recent ttl-origin manifest and act on it:
    ///   - no manifest    → submit the oldest eligible partition (smallest expiration max);
    ///   - PENDING        → wait;
    ///   - COMPLETED      → walk forward, submit the next eligible partition with
    ///                       `partition_id > completed`;
    ///   - FAILED         → respect per-partition in-memory backoff; on elapse resubmit the
    ///                       same partition with `force_export=1` to overwrite the failed manifest;
    ///   - KILLED         → idle; log a recovery recipe. The operator either retries the killed
    ///                       partition with `force_export=1` or steps past it by exporting a newer
    ///                       partition with `mark_as_ttl=1`.
    /// Exceptions surface as either transient (ZK race → next tick retries without bumping
    /// backoff) or terminal (logged; backoff bumped if applicable). `UNKNOWN_TABLE` at submit
    /// time is a soft failure — the destination disappeared post-DDL.
    void run();

private:
    struct BackoffState
    {
        size_t tries = 0;
        time_t next_attempt_at = 0;
    };

    using BackoffKey = std::tuple<String, String, String>; /// (partition_id, dest_db, dest_table)

    struct TtlMarker
    {
        String partition_id;
        String status;
        time_t create_time = 0;
    };

    StorageReplicatedMergeTree & storage;
    LoggerPtr log;
    std::map<BackoffKey, BackoffState> backoff;

    void processExportTTL(const TTLDescription & export_ttl);

    /// Locate the current ttl-origin manifest for `(this src, dest)`. The submission-time
    /// invariant keeps this set bounded; on transient races where more than one ttl-origin
    /// manifest coexists, the most recent by `create_time` is returned.
    std::optional<TtlMarker> findTtlMarker(const String & dest_database, const String & dest_table);

    /// Return the eligible partition with the smallest expiration max. If `floor` is set,
    /// only partitions with `partition_id > *floor` are considered — that's how forward-walking
    /// is enforced from a COMPLETED marker.
    std::optional<String> pickPartition(const TTLDescription & export_ttl, const std::optional<String> & floor);

    enum class SubmitResult
    {
        Submitted,    /// Manifest was created; backoff (if any) should be reset.
        Transient,    /// Lost a ZK race or destination missing at submit time — neither failure
                      /// nor success; let the next tick retry without bumping backoff.
        Failure,      /// Any other exception. Caller should bump backoff if it tracks one.
    };

    /// Submit a TTL-driven export via `exportPartitionToTable`. Catches and classifies the
    /// outcome; the caller decides whether to bump backoff. Diagnostics are logged here so
    /// each call site stays focused on state transitions.
    SubmitResult submit(const String & dest_database, const String & dest_table, const String & partition_id, bool force);
};

}
