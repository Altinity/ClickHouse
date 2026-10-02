#pragma once

#include <Storages/MergeTree/ExportFence.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <base/types.h>

namespace DB
{

/// Where a table keeps the export index of its `EXPORT` TTL (see `ExportTTLIndexEntry`): in Keeper
/// for a `ReplicatedMergeTree`, in the data directory for a plain `MergeTree`.
class IExportTTLIndex
{
public:
    virtual ~IExportTTLIndex() = default;

    /// The whole index, read again only if it changed since it was last read.
    virtual ExportTTLIndexSnapshotPtr getSnapshot() = 0;

    /// The index as of its last read, without reading it. nullptr if it was not read yet.
    virtual ExportTTLIndexSnapshotPtr getLatest() const = 0;

    /// Stores `versioned` if the stored version is still `versioned.version`. Returns false otherwise.
    virtual bool updateEntry(const String & destination_key, const ExportTTLVersionedEntry & versioned) = 0;

    virtual void removeDestination(const String & destination_key) = 0;

    /// Only one replica schedules the exports of the `EXPORT` TTL, which keeps the replicas from
    /// conflicting on the index; correctness does not depend on it. The lock is kept until it is lost
    /// or released.
    virtual bool tryAcquireSchedulerLock() = 0;

    /// Released when the table has nothing left to schedule, so that it does not hold Keeper nodes.
    virtual void releaseSchedulerLock() = 0;

    /// The replica holding the scheduler lock, empty if there is none or the table is not replicated.
    /// Does not read it.
    virtual String getSchedulerReplica() const = 0;
};

}
