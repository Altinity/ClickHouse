#pragma once

#include <map>
#include <optional>
#include <Storages/ExportReplicatedMergeTreeTaskManifest.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>

namespace DB
{
struct ExportReplicatedMergeTreeTaskEntry
{
    using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;
    ExportReplicatedMergeTreeTaskManifest manifest;

    enum class Status
    {
        PENDING,
        COMPLETED,
        FAILED,
        KILLED
    };

    /// Allows us to skip completed / failed entries during scheduling
    mutable Status status;

    /// References to the parts that should be exported
    /// This is used to prevent the parts from being deleted before finishing the export operation
    /// It does not mean this replica will export all the parts
    /// There is also a chance this replica does not contain a given part and it is totally ok.
    mutable std::vector<DataPartPtr> part_references;

    /// In-memory mirror of <export-entry>/last_exception/<replica> leaves in ZK,
    /// keyed by replica name (verbatim, not escaped).
    mutable std::map<String, LastExceptionEntry> last_exception_per_replica;

    /// In-memory mirror of <export-entry>/processed/<part> leaves in ZK, keyed by
    /// part name. An empty map means no part has finished exporting yet for this task.
    /// Incomplete Keeper refreshes (or unreadable processed leaves) publish
    /// "<failed to read from zk>" as a whole-map key, or as the sole path value
    /// for the affected part leaf.
    mutable std::map<String, std::vector<String>> destination_file_paths_per_part;

    /// In-memory mirror of the <export-entry>/commit_info znode (written atomically
    /// with the COMPLETED status transition; see ExportTaskUtils::commit).
    /// nullopt until commit_info is observed in ZK. Empty fields inside the struct
    /// for non-Iceberg destinations.
    mutable std::optional<ExportCommitInfoEntry> commit_info;

    std::string getTransactionId() const
    {
        return manifest.transaction_id;
    }

    /// Get create_time for sorted iteration
    time_t getCreateTime() const
    {
        return manifest.create_time;
    }
};

struct ExportTaskEntryTagByTransactionId {};
struct ExportTaskEntryTagByCreateTime {};

// Multi-index container for export task entries. A task is stored at `<zookeeper_path>/exports/<transaction_id>`.
// - Index 0 (TagByTransactionId): hashed_unique on the transaction id, the task's key
// - Index 1 (TagByCreateTime): ordered_non_unique on create_time for sorted iteration
using ExportTaskEntriesContainer = boost::multi_index_container<
    ExportReplicatedMergeTreeTaskEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<ExportTaskEntryTagByTransactionId>,
            boost::multi_index::const_mem_fun<ExportReplicatedMergeTreeTaskEntry, std::string, &ExportReplicatedMergeTreeTaskEntry::getTransactionId>
        >,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<ExportTaskEntryTagByCreateTime>,
            boost::multi_index::const_mem_fun<ExportReplicatedMergeTreeTaskEntry, time_t, &ExportReplicatedMergeTreeTaskEntry::getCreateTime>
        >
    >
>;

}
