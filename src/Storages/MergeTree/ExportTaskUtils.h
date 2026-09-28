#pragma once

#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include <string>
#include <Core/Field.h>
#include <Core/ColumnsWithTypeAndName.h>
#include <Core/SettingsEnums.h>
#include <Common/Logger.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include "Storages/IStorage.h"
#include <Storages/ExportRetriedTask.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <config.h>

#if USE_AVRO
#include <Parsers/IAST.h>
#include <Poco/JSON/Object.h>
#endif

namespace DB
{

class MergeTreeData;
class ReplicatedExportTTLIndex;
struct ExportReplicatedMergeTreeTaskManifest;

namespace ExportTaskUtils
{
    bool isNonRetryableExportError(int code);

    bool isNonRetryablePlainExportError(int code);

    size_t computeRetryBackoffSeconds(size_t retry_count, size_t initial_backoff_seconds, size_t max_backoff_seconds);

    bool isExportTaskTimedOut(time_t create_time, size_t timeout_seconds, time_t now);

    /// The partition of an export task, derived from the names of its parts, which all belong to one partition.
    String getPartitionIdOfParts(const std::vector<String> & part_names, MergeTreeDataFormatVersion format_version);

    struct ExportedPaths
    {
        /// Number of `<export_path>/processed` leaves, that is, parts this export has finished.
        size_t processed_parts_count = 0;

        /// Destination paths recorded by those leaves, flattened. A leaf may carry none, so this
        /// can legitimately be shorter than `processed_parts_count`.
        std::vector<std::string> paths;

        /// The same paths by the name of the part they were exported from.
        std::map<std::string, std::vector<std::string>> paths_by_part;
    };

    /// Appends the ops that create the nodes of a new export task at `task_path` (`<zookeeper_path>/exports/<transaction_id>`).
    void appendCreateExportTaskOps(Coordination::Requests & ops, const std::string & task_path, const ExportReplicatedMergeTreeTaskManifest & manifest);

    /// Block ranges of `partition_id` committed to `destination_storage` by the tasks in `retry_of`
    /// that landed, e.g. a commit that the destination applied after the task was considered failed.
    std::vector<MergeTreePartInfo> getRangesCommittedByRetriedTasks(
        const ExportRetriedTasks & retry_of,
        const StoragePtr & destination_storage,
        const String & partition_id,
        const ContextPtr & context);

    /// Parts of `part_names` whose rows are not in `committed_ranges`.
    std::vector<String> getPartsNotCommitted(
        const std::vector<String> & part_names, const std::vector<MergeTreePartInfo> & committed_ranges, MergeTreeDataFormatVersion format_version);

    /// Reads the destination paths recorded under `<export_path>/processed`.
    ExportedPaths getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path);

    /// Build a query context carrying the export task's persisted settings. Templated on the
    /// descriptor type so it serves both the replicated manifest (backed by ZooKeeper) and the
    /// plain `MergeTreeExportTask` (backed by disk); both expose the same setting fields.
    template <typename ManifestT>
    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ManifestT & manifest);

#if USE_AVRO
    std::string verifyAndExtractDestinationIcebergMetadataJson(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StoragePtr & dest_storage,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context);
#endif

    /// Invokes `commitExportTransaction` on the destination storage. Does not mark the export
    /// task complete; the caller persists the returned commit info.
    IStorage::ExportCommitInfo commitExportOnDestination(
        const String & transaction_id,
        const String & partition_id,
        const String & iceberg_metadata_json,
        bool write_full_path_in_iceberg_metadata,
        const std::optional<String> & iceberg_partition_timezone,
        const std::vector<std::string> & exported_paths,
        const std::vector<String> & exported_part_names,
        const StoragePtr & destination_storage,
        MergeTreeData & source_storage,
        const ContextPtr & context);

    /// Get the min/max values from the partition expression columns
    Block getPartitionSourceBlockForIcebergCommit(
        MergeTreeData & storage, const String & partition_id, const std::vector<String> & exported_part_names);

    /// Commits the files of a replicated export task and marks it completed. For a task of the
    /// `EXPORT` TTL, the files of parts that a task it retries committed are left out, and the
    /// export index records the parts as exported in the same transaction.
    void commit(
        const ExportReplicatedMergeTreeTaskManifest & manifest,
        const StoragePtr & destination_storage,
        const zkutil::ZooKeeperPtr & zk,
        const LoggerPtr & log,
        const std::string & entry_path,
        const ContextPtr & context,
        MergeTreeData & source_storage,
        const String & replica_name,
        const ReplicatedExportTTLIndex & export_fence
    );

    /// Handles a commit-phase failure for a replicated partition export:
    ///  - records the exception via appendExceptionOps in the same multi
    ///  - if `exception_code` is non-retryable (see isNonRetryableExportError), sets
    ///    <entry_path>/status to FAILED (version-checked against the PENDING read)
    ///  - otherwise leaves the task PENDING so the commit is retried (by the next
    ///    last-part success or deferred-commit recovery) until the absolute task timeout
    ///
    /// There is no per-task commit-attempt budget: retryable commit failures retry until
    /// success or timeout, matching the per-part retry semantics.
    ///
    /// Returns true if this call transitioned the task to FAILED.
    bool handleCommitFailure(
        const zkutil::ZooKeeperPtr & zk,
        const std::string & entry_path,
        int exception_code,
        const std::string & replica_name,
        const std::string & exception_message,
        const LoggerPtr & log);

    /// Appends a single ZK op to `ops` that writes the per-replica leaf
    ///   <entry_path>/last_exception/<escaped replica_name>
    /// with a JSON-encoded LastExceptionEntry containing the message, part,
    /// replica, time, and an incremented count. If the leaf does not yet exist
    /// the op is a Create; otherwise it is a Set with version -1.
    ///
    /// Cross-replica updates do not race: each replica only writes its own
    /// leaf. Within a single replica the count increment is best-effort and
    /// non-atomic (synchronous tryGet + Set with version -1); concurrent
    /// failing writers may under-count by one, which is accepted.
    void appendExceptionOps(
        Coordination::Requests & ops,
        const zkutil::ZooKeeperPtr & zk,
        const std::filesystem::path & entry_path,
        const std::string & replica_name,
        const std::string & part_name,
        const std::string & exception_message,
        const LoggerPtr & log);

    void assertPartitionKeyASTAreEqual(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata);

    void checkExportSchemaColumnsCount(
        size_t source_columns_count,
        size_t destination_columns_count,
        bool ignore_extra_source_columns);

    /// Validates that source columns can be exported into the destination with the configured
    /// positional or name-based CAST matching (`export_merge_tree_part_schema_match_mode`) and
    /// unmatched-column policy (`export_merge_tree_part_ignore_extra_source_columns`). Lossy casts are rejected unless
    /// `export_merge_tree_part_allow_lossy_cast` is set. Throws BAD_ARGUMENTS on any violation.
    void verifyExportSchemaCastable(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StorageID & destination_storage_id,
        const ContextPtr & context);

    void verifyPlainPartitionCompatibility(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context);

    /// Throws if no group of parts of the source could be exported to the destination without being split
    /// across its partitions, checking what does not need the parts: every destination partition expression
    /// must match the source partition key structurally, or be monotonic in a single non-Nullable column of
    /// it, so that the min/max values of the parts can prove it when they are exported.
    void verifyPartitionKeyCanBeCompatible(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StoragePtr & destination_storage,
        const MergeTreeSettingsPtr & source_settings,
        const ContextPtr & context);

#if USE_AVRO
    /// Verifies the source MergeTree partition key is compatible with the destination Iceberg
    /// partition spec: every destination partition field must be single-valued across the exported
    /// source partition (which the commit path requires - it writes one partition tuple per export).
    /// A field is proven either structurally (the source key already applies the matching transform
    /// on that column) or dynamically, by checking the destination transform is constant over the
    /// partition's actual [min, max] folded across `parts`. `bucket` is non-monotonic and can only be
    /// matched structurally. Throws BAD_ARGUMENTS when a field cannot be proven.
    void verifyIcebergPartitionCompatibility(
        const Poco::JSON::Object::Ptr & metadata_object,
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context);
#endif
}

}
