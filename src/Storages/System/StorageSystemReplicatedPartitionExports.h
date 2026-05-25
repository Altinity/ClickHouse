#pragma once

#include <Storages/ExportReplicatedMergeTreePartitionManifest.h>
#include <Storages/System/IStorageSystemOneBlock.h>

namespace DB
{

class Context;

struct ReplicatedPartitionExportInfo
{
    String destination_database;
    String destination_table;
    String partition_id;
    String transaction_id;
    String query_id;
    time_t create_time;
    String source_replica;
    size_t parts_count;
    size_t parts_to_do;
    std::vector<String> parts;
    String status;
    /// One entry per replica that has recorded at least one exception for this task.
    /// Sourced verbatim from the in-memory mirror; no ZooKeeper traffic.
    std::vector<LastExceptionEntry> last_exception_per_replica;
    /// Sum of per-replica counts. Each replica owns its own count, so cross-replica
    /// updates do not race; the sum is exact w.r.t. the in-memory snapshot. Within a
    /// single replica the count is best-effort (concurrent failing writers may under-
    /// count by one), matching the documented column semantics.
    size_t exception_count = 0;
    /// Per-part destination file paths, keyed by part name. Mirrors the
    /// <export-entry>/processed/<part>/paths_in_destination data from ZooKeeper.
    /// Empty until parts complete; partial during PENDING.
    std::map<String, std::vector<String>> destination_file_paths_per_part;
    /// Iceberg commit-time paths surfaced from <export-entry>/commit_info.
    /// All empty for non-Iceberg destinations or before commit lands.
    String committed_metadata_file;
    String committed_manifest_list;
    String committed_manifest_file;
    /// Plain object storage commit marker file surfaced from
    /// <export-entry>/commit_info. Empty for Iceberg destinations or before
    /// commit lands.
    String committed_marker_file;
};

class StorageSystemReplicatedPartitionExports final : public IStorageSystemOneBlock
{
public:

    std::string getName() const override { return "SystemReplicatedPartitionExports"; }

    static ColumnsDescription getColumnsDescription();

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const override;
};

}
