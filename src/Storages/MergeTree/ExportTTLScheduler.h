#pragma once

#include <Interpreters/Context_fwd.h>
#include <Interpreters/StorageID.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/TTLDescription.h>
#include <Common/CurrentMetrics.h>
#include <Common/Logger.h>
#include <base/types.h>

#include <map>
#include <mutex>
#include <vector>

namespace DB
{

class IExportTTLIndex;
class MergeTreeData;

/// What `system.ttl_exports` shows about one partition of a table with a `TTL ... EXPORT` expression.
struct ExportTTLPartitionInfo
{
    String partition_id;
    String destination_database;
    String destination_table;
    size_t exported_parts = 0;
    size_t claimed_parts = 0;
    size_t eligible_parts = 0;
    size_t eligible_bytes = 0;
    /// Parts held back from the delete TTL because they are not exported yet.
    size_t parts_held_by_delete_gate = 0;
    /// Transaction id of the task exporting the partition now, empty if there is none.
    String current_transaction_id;
    String last_error;
    /// The replica that schedules the TTL exports of the table, empty for a plain `MergeTree`.
    String scheduler_replica;
};

/// The background task of a table with a `TTL <expr> EXPORT TO TABLE <destination>` expression.
///
/// Every check ships the eligible parts (the maximum TTL value of their rows is due) of each partition
/// to the destination as one export task of the table, and never exports a part twice: what was
/// exported is recorded in the export index (see `ExportTTLIndexEntry`), and the merge fence keeps
/// parts in different export states from merging. A partition has at most one task in flight, so the
/// check period is the batch interval. A task that failed without committing keeps its parts claimed,
/// and a new task retries exactly them under the same commit id, so the destination commits them once
/// even if the failed task landed after all.
///
/// Only the replica holding the scheduler lock checks. `system.ttl_exports` is computed when it is
/// queried, from the copy of the index that the replica keeps (see `IExportTTLIndex::getLatest`) and
/// its parts, without reading Keeper.
class ExportTTLScheduler final
{
public:
    ExportTTLScheduler(MergeTreeData & storage_, IExportTTLIndex & index_);

    /// One check. Returns the number of milliseconds until the next one.
    UInt64 run();

    std::vector<ExportTTLPartitionInfo> getInfo() const;

private:
    MergeTreeData & storage;
    IExportTTLIndex & ttl_index;
    const LoggerPtr log;

    mutable std::mutex mutex;
    /// Kept by the replica that schedules: the error of the last check of each partition, and why the
    /// table exports nothing.
    std::map<String, String> last_errors;
    String table_error;

    CurrentMetrics::Increment parts_held_by_delete_gate;

    ContextPtr makeContext() const;

    /// E.g. `SYSTEM STOP MOVES`.
    bool isPaused() const;

    void updatePartsHeldByDeleteGate();

    /// Resolves the claim of the partition unless its task is in flight. A failed task is retried with
    /// exactly its claimed parts; otherwise every due part of the partition that is not being merged
    /// is exported.
    void schedulePartition(
        const String & destination_key,
        const StoragePtr & destination,
        ExportTTLVersionedEntry versioned,
        const std::vector<MergeTreeDataPartPtr> & parts,
        const TTLDescriptions & export_ttls,
        time_t now,
        const ContextPtr & context);

    /// Kills the TTL tasks of a destination that is no longer the destination of the TTL, and
    /// removes its index once none of them holds a claim.
    void cleanupDestination(const String & destination_key, const std::map<String, ExportTTLVersionedEntry> & entries);
};

}
