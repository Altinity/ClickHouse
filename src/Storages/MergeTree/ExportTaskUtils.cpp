#include <Storages/MergeTree/ExportTaskUtils.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ProfileEvents.h>
#include <Common/FailPoint.h>
#include <Common/escapeForFileName.h>
#include <Common/logger_useful.h>
#include "Storages/ExportReplicatedMergeTreeTaskManifest.h"
#include "Storages/ExportReplicatedMergeTreeTaskEntry.h"
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeExportTask.h>
#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/ReplicatedExportTTLIndex.h>
#if USE_AVRO
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageCluster.h>
#endif
#include <Parsers/IAST.h>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <Core/Block.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/Utils.h>
#include <Functions/FunctionHelpers.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Processors/QueryPlan/Optimizations/actionsDAGUtils.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/KeyDescription.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTExpressionList.h>
#include <Functions/CastOverloadResolver.h>
#include <Functions/IFunction.h>
#include <Interpreters/castColumn.h>
#include <Storages/ColumnsDescription.h>

#if USE_AVRO
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#endif

namespace ProfileEvents
{
    extern const Event ExportTaskZooKeeperRequests;
    extern const Event ExportTaskZooKeeperGet;
    extern const Event ExportTaskZooKeeperGetChildren;
    extern const Event ExportTaskZooKeeperSet;
    extern const Event ExportTaskZooKeeperCreate;
    extern const Event ExportTaskZooKeeperMulti;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int FAULT_INJECTED;
    extern const int BAD_ARGUMENTS;
    extern const int NO_SUCH_DATA_PART;
    extern const int UNKNOWN_TABLE;
    extern const int CORRUPTED_DATA;
    extern const int NETWORK_ERROR;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int SUPPORT_IS_DISABLED;
    extern const int TYPE_MISMATCH;
    extern const int CANNOT_CONVERT_TYPE;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ILLEGAL_COLUMN;
    extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
    extern const int THERE_IS_NO_COLUMN;
    extern const int INCOMPATIBLE_COLUMNS;
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int FILE_ALREADY_EXISTS;
    extern const int METADATA_MISMATCH;
    extern const int CANNOT_PARSE_TEXT;
    extern const int CANNOT_PARSE_NUMBER;
    extern const int CANNOT_PARSE_DATE;
    extern const int CANNOT_PARSE_DATETIME;
    extern const int CANNOT_PARSE_BOOL;
    extern const int CANNOT_PARSE_UUID;
    extern const int CANNOT_PARSE_IPV4;
    extern const int CANNOT_PARSE_IPV6;
    extern const int CANNOT_PARSE_QUOTED_STRING;
    extern const int CANNOT_PARSE_ESCAPE_SEQUENCE;
    extern const int CANNOT_PARSE_INPUT_ASSERTION_FAILED;
    extern const int CANNOT_PARSE_DOMAIN_VALUE_FROM_STRING;
    extern const int VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE;
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int CANNOT_READ_ARRAY_FROM_TEXT;
    extern const int DECIMAL_OVERFLOW;
}

namespace Setting
{
    extern const SettingsBool export_merge_tree_part_allow_lossy_cast;
#if USE_AVRO
    extern const SettingsBool allow_insert_into_iceberg;
    extern const SettingsTimezone iceberg_partition_timezone;
#endif
    extern const SettingsMergeTreePartExportSchemaMatchMode export_merge_tree_part_schema_match_mode;
    extern const SettingsBool export_merge_tree_part_ignore_extra_source_columns;
}

namespace FailPoints
{
    extern const char iceberg_export_after_commit_before_zk_completed[];
    extern const char export_partition_commit_always_throw[];
}

namespace fs = std::filesystem;

namespace ExportTaskUtils
{
    bool isNonRetryableExportError(int code)
    {
        /// Deterministic failures where retrying cannot possibly succeed (schema/type
        /// incompatibilities, unsupported features, programming errors). Everything else
        /// (memory limits, network/object-storage/Keeper transient errors, ...) is retryable.
        /// `QUERY_WAS_CANCELLED` is handled separately by the caller and never reaches here.
        ///
        /// ErrorCodes values are runtime `extern const int`, not constant expressions, so they
        /// cannot be used as `switch` labels; compare against a static set instead.
        static const std::unordered_set<int> non_retryable_codes = {
            ErrorCodes::BAD_ARGUMENTS,
            ErrorCodes::TYPE_MISMATCH,
            ErrorCodes::CANNOT_CONVERT_TYPE,
            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
            ErrorCodes::ILLEGAL_COLUMN,
            ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH,
            ErrorCodes::THERE_IS_NO_COLUMN,
            ErrorCodes::INCOMPATIBLE_COLUMNS,
            ErrorCodes::NO_SUCH_COLUMN_IN_TABLE,
            ErrorCodes::NOT_IMPLEMENTED,
            ErrorCodes::SUPPORT_IS_DISABLED,
            ErrorCodes::LOGICAL_ERROR,
            ErrorCodes::FILE_ALREADY_EXISTS,
            ErrorCodes::METADATA_MISMATCH,
            ErrorCodes::CANNOT_PARSE_TEXT,
            ErrorCodes::CANNOT_PARSE_NUMBER,
            ErrorCodes::CANNOT_PARSE_DATE,
            ErrorCodes::CANNOT_PARSE_DATETIME,
            ErrorCodes::CANNOT_PARSE_BOOL,
            ErrorCodes::CANNOT_PARSE_UUID,
            ErrorCodes::CANNOT_PARSE_IPV4,
            ErrorCodes::CANNOT_PARSE_IPV6,
            ErrorCodes::CANNOT_PARSE_QUOTED_STRING,
            ErrorCodes::CANNOT_PARSE_ESCAPE_SEQUENCE,
            ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED,
            ErrorCodes::CANNOT_PARSE_DOMAIN_VALUE_FROM_STRING,
            ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE,
            ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF,
            ErrorCodes::CANNOT_READ_ARRAY_FROM_TEXT,
            ErrorCodes::DECIMAL_OVERFLOW,
        };
        return non_retryable_codes.contains(code);
    }

    bool isNonRetryablePlainExportError(int code)
    {
        return isNonRetryableExportError(code)
            || code == ErrorCodes::UNKNOWN_TABLE
            || code == ErrorCodes::NO_SUCH_DATA_PART;
    }

    size_t computeRetryBackoffSeconds(size_t retry_count, size_t initial_backoff_seconds, size_t max_backoff_seconds)
    {
        const size_t initial = std::min(initial_backoff_seconds, max_backoff_seconds);

        if (retry_count <= 1 || initial == 0)
            return initial;

        const size_t shift = retry_count - 1;

        /// If shifting would overflow size_t, the result is certainly clamped to the cap.
        static constexpr size_t bits = sizeof(size_t) * 8;
        if (shift >= bits)
            return max_backoff_seconds;

        const size_t headroom = std::numeric_limits<size_t>::max() >> shift;
        if (initial > headroom)
            return max_backoff_seconds;

        return std::min(initial << shift, max_backoff_seconds);
    }

    bool isExportTaskTimedOut(time_t create_time, size_t timeout_seconds, time_t now)
    {
        if (timeout_seconds == 0)
            return false;
        if (now <= create_time)
            return false;

        /// Compare elapsed seconds instead of `create_time + timeout`. A UInt64 timeout that does
        /// not fit in time_t would wrap to a negative deadline and look expired on the first tick.
        return static_cast<UInt64>(now) - static_cast<UInt64>(create_time) > timeout_seconds;
    }

    String getPartitionIdOfParts(const std::vector<String> & part_names, MergeTreeDataFormatVersion format_version)
    {
        if (part_names.empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "An export task has no parts, cannot determine its partition");
        return MergeTreePartInfo::fromPartName(part_names.front(), format_version).getPartitionId();
    }

    Block getPartitionSourceBlockForIcebergCommit(
        MergeTreeData & storage, const String & partition_id, const std::vector<String> & exported_part_names)
    {
        auto lock = storage.readLockParts();
        const auto parts = storage.getDataPartsVectorInPartitionForInternalUsage(
            {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated}, partition_id, lock);

        /// Only look at the parts being exported. These parts are guaranteed to map to a single partition.
        /// Parts that were later inserted shall be ignored
        const std::unordered_set<String> exported(exported_part_names.begin(), exported_part_names.end());
        IMergeTreeDataPart::MinMaxIndex minmax;
        for (const auto & part : parts)
            if (exported.contains(part->name))
                minmax.merge(*part->getMinMaxIndex());

        if (!minmax.initialized)
            throw Exception(ErrorCodes::NO_SUCH_DATA_PART,
                "Cannot find any of the exported parts for partition_id '{}' to derive Iceberg partition "
                "values. They may have been merged and cleaned up before this commit, or are not present "
                "on this replica.",
                partition_id);

        const auto metadata_snapshot = storage.getInMemoryMetadataPtr(storage.getContext(), false);
        const auto & partition_key = metadata_snapshot->getPartitionKey();
        const auto minmax_columns = MergeTreeData::getMinMaxColumns(
            partition_key, storage.getSettings(), MergeTreePartMinMaxIndexColumns::PARTITION_KEY_ONLY);

        if (minmax.hyperrectangle.size() < minmax_columns.size())
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Cannot derive Iceberg partition values: the exported parts of partition '{}' hold min/max "
                "statistics for {} columns, but the partition key has {}.",
                partition_id, minmax.hyperrectangle.size(), minmax_columns.size());

        /// When the query was scheduled, we validated that dst_expression(min) == dst_expression(max).
        /// Therefore, we can use only the min value, no need for the max.
        Block block;
        size_t i = 0;
        for (const auto & [column_name, column_type] : minmax_columns)
        {
            auto column = column_type->createColumn();
            column->insert(minmax.hyperrectangle[i].left);
            block.insert(ColumnWithTypeAndName(column->getPtr(), column_type, column_name));
            ++i;
        }

        return block;
    }

    template <typename ManifestT>
    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ManifestT & manifest)
    {
        auto context_copy = Context::createCopy(context);
        context_copy->makeQueryContextForExportPart();
        context_copy->setCurrentQueryId(manifest.query_id);
        context_copy->setSetting("output_format_parallel_formatting", manifest.parallel_formatting);
        context_copy->setSetting("output_format_parquet_parallel_encoding", manifest.parquet_parallel_encoding);

        /// Backwards compatibility
        if (manifest.parquet_compression_method)
            context_copy->setSetting("output_format_parquet_compression_method", *manifest.parquet_compression_method);
        if (manifest.output_format_compression_level)
            context_copy->setSetting("output_format_compression_level", *manifest.output_format_compression_level);
        if (manifest.parquet_row_group_size)
            context_copy->setSetting("output_format_parquet_row_group_size", *manifest.parquet_row_group_size);
        if (manifest.parquet_row_group_size_bytes)
            context_copy->setSetting("output_format_parquet_row_group_size_bytes", *manifest.parquet_row_group_size_bytes);
        /// Manifests written before these settings existed have no value here; such tasks were always
        /// scheduled under the old, strict column-matching check, so an absent value must resolve to
        /// `POSITION` / `false` regardless of the ambient context's settings (which may have
        /// since been changed).
        context_copy->setSetting(
            "export_merge_tree_part_schema_match_mode",
            String(magic_enum::enum_name(manifest.schema_match_mode.value_or(MergeTreePartExportSchemaMatchMode::POSITION))));
        context_copy->setSetting(
            "export_merge_tree_part_ignore_extra_source_columns",
            manifest.ignore_extra_source_columns.value_or(false));

        context_copy->setSetting("max_threads", manifest.max_threads);
        context_copy->setSetting("export_merge_tree_part_file_already_exists_policy", String(magic_enum::enum_name(manifest.file_already_exists_policy)));
        context_copy->setSetting("export_merge_tree_part_max_bytes_per_file", manifest.max_bytes_per_file);
        context_copy->setSetting("export_merge_tree_part_max_rows_per_file", manifest.max_rows_per_file);
        context_copy->setSetting("iceberg_insert_max_bytes_in_data_file", manifest.max_bytes_per_file);
        context_copy->setSetting("iceberg_insert_max_rows_in_data_file", manifest.max_rows_per_file);

        /// always skip pending mutations and patch parts because we already validated the parts during query processing
        context_copy->setSetting("export_merge_tree_part_throw_on_pending_mutations", false);
        context_copy->setSetting("export_merge_tree_part_throw_on_pending_patch_parts", false);

        context_copy->setSetting("export_merge_tree_part_filename_pattern", manifest.filename_pattern);
        context_copy->setSetting("write_full_path_in_iceberg_metadata", manifest.write_full_path_in_iceberg_metadata);

        /// The request-time call to exportPartitionToTable has already validated allow_insert_into_iceberg
        /// against the initiator's settings. Once the manifest is in ZooKeeper, every replica must be
        /// able to execute the task regardless of its own profile - otherwise an export silently
        /// stalls when the setting is only set at the query level.
        context_copy->setSetting("allow_insert_into_iceberg", true);

        /// Reapply the initiator's lossy-cast decision (persisted in the manifest) so the
        /// worker's schema revalidation honors the user's choice. Without this, a task
        /// scheduled without the opt-in could still apply a lossy cast if the destination
        /// schema drifts to a lossy target between scheduling and execution.
        context_copy->setSetting("export_merge_tree_part_allow_lossy_cast", manifest.allow_lossy_cast);

        if (manifest.iceberg_partition_timezone)
        {
            context_copy->setSetting("iceberg_partition_timezone", *manifest.iceberg_partition_timezone);
        }

        return context_copy;
    }

    template ContextPtr getContextCopyWithTaskSettings<ExportReplicatedMergeTreeTaskManifest>(
        const ContextPtr &, const ExportReplicatedMergeTreeTaskManifest &);
    template ContextPtr getContextCopyWithTaskSettings<MergeTreeExportTask>(
        const ContextPtr &, const MergeTreeExportTask &);

#if USE_AVRO
namespace
{
    Poco::JSON::Object::Ptr getIcebergMetadataObject(const StoragePtr & dest_storage, const ContextPtr & context)
    {
        auto * object_storage = dynamic_cast<StorageObjectStorage *>(dest_storage.get());
        auto * object_storage_cluster = dynamic_cast<StorageObjectStorageCluster *>(dest_storage.get());

        /// in theory this should never happen, but just in case
        if (!object_storage && !object_storage_cluster)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Destination storage {} is not a StorageObjectStorage", dest_storage->getName());

        IcebergMetadata * iceberg_metadata = nullptr;
        if (object_storage)
            iceberg_metadata = dynamic_cast<IcebergMetadata *>(object_storage->getExternalMetadata(context));
        else if (object_storage_cluster)
            iceberg_metadata = dynamic_cast<IcebergMetadata *>(object_storage_cluster->getExternalMetadata(context));
        if (!iceberg_metadata)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Destination storage {} is a data lake but not an iceberg table", dest_storage->getName());

        return iceberg_metadata->getMetadataJSON(context);
    }
}

    std::string verifyAndExtractDestinationIcebergMetadataJson(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StoragePtr & dest_storage,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        if (!context->getSettingsRef()[Setting::allow_insert_into_iceberg])
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                "Iceberg writes are experimental. "
                "To allow its usage, enable the setting `allow_insert_into_iceberg` on the initiator (query, session or profile) - replicas inherit it from the scheduled task.");

        const auto metadata_object = getIcebergMetadataObject(dest_storage, context);

        verifyIcebergPartitionCompatibility(
            metadata_object, source_metadata, destination_metadata, parts, partition_id, context);

        std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        oss.exceptions(std::ios::failbit);
        metadata_object->stringify(oss);
        return oss.str();
    }
#endif

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
        const ContextPtr & context_in)
    {
        auto context = Context::createCopy(context_in);
        context->setSetting("write_full_path_in_iceberg_metadata", write_full_path_in_iceberg_metadata);

        if (iceberg_partition_timezone)
            context->setSetting("iceberg_partition_timezone", *iceberg_partition_timezone);

        IStorage::IcebergCommitExportArguments iceberg_args;

        if (!iceberg_metadata_json.empty())
        {
            iceberg_args.metadata_json_string = iceberg_metadata_json;
            const auto source_metadata = source_storage.getInMemoryMetadataPtr(context, false);
            if (source_metadata->hasPartitionKey())
                iceberg_args.partition_source_block =
                    getPartitionSourceBlockForIcebergCommit(source_storage, partition_id, exported_part_names);
        }

        return destination_storage->commitExportTransaction(
            transaction_id, partition_id, exported_paths, iceberg_args, context);
    }

    /// Collect all the exported paths from the processed parts
    /// If multiRead is supported by the keeper implementation, it is done in a single request
    /// Otherwise, multiple async requests are sent
    ExportedPaths getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path)
    {
        LOG_DEBUG(log, "Export task: Getting exported paths for {}", export_path);

        const auto processed_parts_path = fs::path(export_path) / "processed";

        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperGetChildren);
        std::vector<std::string> processed_parts;
        if (const auto code = zk->tryGetChildren(processed_parts_path, processed_parts); code != Coordination::Error::ZOK)
            throw Coordination::Exception::fromPath(code, processed_parts_path);

        std::vector<std::string> get_paths;
        get_paths.reserve(processed_parts.size());

        for (const auto & processed_part : processed_parts)
        {
            get_paths.emplace_back(processed_parts_path / processed_part);
        }

        auto responses = zk->tryGet(get_paths);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperGet, get_paths.size());

        responses.waitForResponses();

        ExportedPaths result;
        result.processed_parts_count = processed_parts.size();

        for (size_t i = 0; i < responses.size(); ++i)
        {
            if (responses[i].error != Coordination::Error::ZOK)
                throw Coordination::Exception::fromPath(responses[i].error, get_paths[i]);

            const auto processed_part_entry = ExportReplicatedMergeTreeProcessedPartEntry::fromJsonString(responses[i].data);

            for (const auto & path_in_destination : processed_part_entry.paths_in_destination)
            {
                result.paths.emplace_back(path_in_destination);
            }
            result.paths_by_part[processed_parts[i]] = processed_part_entry.paths_in_destination;
        }

        return result;
    }

    void appendCreateExportTaskOps(Coordination::Requests & ops, const std::string & task_path, const ExportReplicatedMergeTreeTaskManifest & manifest)
    {
        const fs::path path = task_path;

        ops.emplace_back(zkutil::makeCreateRequest(path, "", zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(path / "metadata.json", manifest.toJsonString(), zkutil::CreateMode::Persistent));

        /// Container for per-replica last_exception leaves; children are created lazily by the
        /// first writer per replica (see appendExceptionOps).
        ops.emplace_back(zkutil::makeCreateRequest(path / "last_exception", "", zkutil::CreateMode::Persistent));

        ops.emplace_back(zkutil::makeCreateRequest(path / "processing", "", zkutil::CreateMode::Persistent));
        for (const auto & part : manifest.parts)
        {
            ExportReplicatedMergeTreeProcessingPartEntry entry;
            entry.status = ExportReplicatedMergeTreeProcessingPartEntry::Status::PENDING;
            entry.part_name = part;
            ops.emplace_back(zkutil::makeCreateRequest(path / "processing" / part, entry.toJsonString(), zkutil::CreateMode::Persistent));
        }

        ops.emplace_back(zkutil::makeCreateRequest(path / "processed", "", zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(path / "locks", "", zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(path / "status", "PENDING", zkutil::CreateMode::Persistent));
    }

    std::vector<MergeTreePartInfo> getRangesCommittedByRetriedTasks(
        const ExportRetriedTasks & retry_of,
        const StoragePtr & destination_storage,
        const String & partition_id,
        const ContextPtr & context)
    {
        std::vector<MergeTreePartInfo> ranges;
        for (const auto & retried : retry_of)
        {
            if (!destination_storage->isExportTransactionCommitted(retried.transaction_id, context))
                continue;

            const auto task_ranges = ExportTTLUtils::fromBlockRanges(partition_id, retried.block_ranges);
            ranges.insert(ranges.end(), task_ranges.begin(), task_ranges.end());
        }
        return ExportFenceUtils::compactRanges(std::move(ranges));
    }

    std::vector<String> getPartsNotCommitted(
        const std::vector<String> & part_names, const std::vector<MergeTreePartInfo> & committed_ranges, MergeTreeDataFormatVersion format_version)
    {
        std::vector<String> result;
        for (const auto & part_name : part_names)
            if (!ExportFenceUtils::isCoveredByUnion(MergeTreePartInfo::fromPartName(part_name, format_version), committed_ranges))
                result.push_back(part_name);
        return result;
    }

    void commit(
        const ExportReplicatedMergeTreeTaskManifest & manifest,
        const StoragePtr & destination_storage,
        const zkutil::ZooKeeperPtr & zk,
        const LoggerPtr & log,
        const std::string & entry_path,
        const ContextPtr & context_in,
        MergeTreeData & source_storage,
        const String & replica_name,
        const ReplicatedExportTTLIndex & export_ttl_index)
    {
        /// Failpoint used by integration tests to force persistent commit failure and exercise
        /// the commit-attempts budget / FAILED state transition.
        fiu_do_on(FailPoints::export_partition_commit_always_throw,
        {
            throw Exception(ErrorCodes::FAULT_INJECTED,
                "Failpoint: export_partition_commit_always_throw");
        });

        /// Per-task ephemeral lock that serializes the commit phase across replicas.
        /// Without it, `handlePartExportSuccess` (post-last-part path) and `tryCleanup`
        /// (poll/recovery path) can drive `commitExportTransaction` concurrently
        /// for the same task.
        const auto commit_lock_path = fs::path(entry_path) / "commit_lock";
        auto commit_lock = zkutil::EphemeralNodeHolder::tryCreate(commit_lock_path, *zk, replica_name);
        if (!commit_lock)
        {
            LOG_DEBUG(log, "Export task: commit_lock for {} is held by another replica, skipping commit on this replica", entry_path);
            return;
        }
        LOG_INFO(log, "Export task: commit_lock for {} acquired by replica {}", entry_path, replica_name);

        /// Honor a concurrent KILL: commit_lock serializes us against killExportTask,
        /// so a non-PENDING status here means cancel won the race.
        std::string status_str;
        if (!zk->tryGet(fs::path(entry_path) / "status", status_str))
            return;
        const auto status = magic_enum::enum_cast<ExportReplicatedMergeTreeTaskEntry::Status>(status_str);
        if (!status || *status != ExportReplicatedMergeTreeTaskEntry::Status::PENDING)
        {
            LOG_DEBUG(log, "Export task: {} not PENDING, skipping commit", entry_path);
            return;
        }

        const auto exported = ExportTaskUtils::getExportedPaths(log, zk, entry_path);

        if (exported.processed_parts_count < manifest.parts.size())
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Export task: Reached the commit phase, but only {} of {} parts are marked as processed, "
                "will not commit export. This might be a bug",
                exported.processed_parts_count, manifest.parts.size());
        }

        const auto partition_id = getPartitionIdOfParts(manifest.parts, source_storage.format_version);

        /// A task this one retries may have landed after it was considered failed. Its parts are
        /// then in the destination already, so only the files of the other parts are committed.
        std::vector<std::string> paths_to_commit = exported.paths;
        if (!manifest.retry_of.empty())
        {
            const auto committed_ranges = getRangesCommittedByRetriedTasks(
                manifest.retry_of, destination_storage, partition_id, context_in);

            if (!committed_ranges.empty())
            {
                paths_to_commit.clear();
                for (const auto & part_name : getPartsNotCommitted(manifest.parts, committed_ranges, source_storage.format_version))
                {
                    const auto it = exported.paths_by_part.find(part_name);
                    if (it != exported.paths_by_part.end())
                        paths_to_commit.insert(paths_to_commit.end(), it->second.begin(), it->second.end());
                }

                LOG_INFO(log, "Export task: a task retried by {} committed some of its parts, committing {} of {} files",
                    entry_path, paths_to_commit.size(), exported.paths.size());
            }
        }

        IStorage::ExportCommitInfo destination_commit_info;

        if (paths_to_commit.empty())
        {
            LOG_INFO(log, "Export task: {} has no destination files to commit", entry_path);
        }
        else
        {
            destination_commit_info = commitExportOnDestination(
                manifest.transaction_id,
                partition_id,
                manifest.iceberg_metadata_json,
                manifest.write_full_path_in_iceberg_metadata,
                manifest.iceberg_partition_timezone,
                paths_to_commit,
                manifest.parts,
                destination_storage,
                source_storage,
                context_in);
        }

        /// Failpoint to simulate a crash after the Iceberg commit succeeds but before
        /// ZooKeeper is updated to COMPLETED. Used by idempotency integration tests.
        fiu_do_on(FailPoints::iceberg_export_after_commit_before_zk_completed,
        {
            LOG_INFO(log, "Failpoint: simulating crash after Iceberg commit, before ZK COMPLETED");
            std::this_thread::sleep_for(std::chrono::seconds(10));
            throw Exception(ErrorCodes::FAULT_INJECTED,
                "Failpoint: simulating crash after Iceberg commit, before ZK COMPLETED");
        });

        LOG_INFO(log, "Export task: Committed export, mark as completed");

        const std::string status_path = fs::path(entry_path) / "status";
        const std::string completed_name = String(magic_enum::enum_name(ExportReplicatedMergeTreeTaskEntry::Status::COMPLETED)).data();

        ExportCommitInfoEntry commit_info_entry {
            destination_commit_info.iceberg_metadata_file,
            destination_commit_info.iceberg_manifest_list,
            destination_commit_info.iceberg_manifest_file,
            destination_commit_info.commit_marker_file};

        const std::string commit_info_path = fs::path(entry_path) / "commit_info";

        const bool is_ttl_task = manifest.source == ExportTaskSource::ttl;
        const auto destination_key = ExportTTLUtils::destinationKey(manifest.destination_database, manifest.destination_table);

        /// The index entry of a TTL task is stored with a check of its version, because the TTL
        /// scheduler may change it meanwhile, e.g. when it resolves another claim of the partition.
        static constexpr size_t max_attempts = 10;
        for (size_t attempt = 0; attempt < max_attempts; ++attempt)
        {
            Coordination::Requests ops;
            ops.emplace_back(zkutil::makeSetRequest(status_path, completed_name, -1));
            ops.emplace_back(zkutil::makeCreateRequest(commit_info_path, commit_info_entry.toJsonString(), zkutil::CreateMode::Persistent));

            if (is_ttl_task)
            {
                auto versioned = export_ttl_index.readIndexEntry(zk, destination_key, partition_id);
                if (versioned.version < 0)
                    export_ttl_index.ensureDestination(zk, destination_key, fmt::format("{}.{}", manifest.destination_database, manifest.destination_table));

                versioned.entry.commitClaim(manifest.transaction_id, ExportTTLUtils::rangesOfParts(manifest.parts, source_storage.format_version));
                export_ttl_index.appendUpdateEntryOps(ops, destination_key, versioned.entry, versioned.version);
            }

            ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
            ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperMulti);

            Coordination::Responses responses;
            const auto rc = zk->tryMulti(ops, responses);

            if (rc == Coordination::Error::ZOK)
            {
                LOG_INFO(log, "Export task: Marked export as completed and persisted commit_info");
                return;
            }

            const auto failed_op = zkutil::getFailedOpIndex(rc, responses);

            if (rc == Coordination::Error::ZNODEEXISTS && failed_op == 1)
            {
                LOG_INFO(log, "Export task: commit_info already present (peer wrote it first); task already COMPLETED");
                return;
            }

            if (is_ttl_task && failed_op >= 2 && (rc == Coordination::Error::ZBADVERSION || rc == Coordination::Error::ZNODEEXISTS))
            {
                LOG_DEBUG(log, "Export task: the export index changed while completing {}, retrying", entry_path);
                continue;
            }

            throw Exception(ErrorCodes::NETWORK_ERROR, "Export task: Failed to mark export as completed (rc={}), will not try to fix it", rc);
        }

        throw Exception(ErrorCodes::NETWORK_ERROR,
            "Export task: the export index kept changing while completing {}, will retry the commit", entry_path);
    }

    bool handleCommitFailure(
        const zkutil::ZooKeeperPtr & zk,
        const std::string & entry_path,
        int exception_code,
        const std::string & replica_name,
        const std::string & exception_message,
        const LoggerPtr & log)
    {
        const std::string status_path = fs::path(entry_path) / "status";

        /// Read /status together with its stat so we can (a) bail early if another
        /// replica has already moved the task out of PENDING and (b) use a
        /// version-checked Set later to avoid clobbering a concurrent write
        /// (e.g. a racing successful commit that marked the task COMPLETED between
        /// our read and our tryMulti).
        Coordination::Stat status_stat;
        std::string current_status;

        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperGet);
        if (!zk->tryGet(status_path, current_status, &status_stat))
        {
            /// Task was removed (TTL cleanup or force-overwrite). Nothing to do.
            LOG_DEBUG(log, "Export task: /status missing for {}, skipping commit-failure bookkeeping", entry_path);
            return false;
        }

        const auto status = magic_enum::enum_cast<ExportReplicatedMergeTreeTaskEntry::Status>(current_status);
        if (!status)
        {
            LOG_WARNING(log, "Export task: Invalid status {} for task {}, skipping commit-failure bookkeeping", current_status, entry_path);
            return false;
        }

        if (status != ExportReplicatedMergeTreeTaskEntry::Status::PENDING)
        {
            /// Another replica already reached a terminal state (COMPLETED or FAILED).
            /// Do NOT overwrite — a successful commit by a peer must win.
            LOG_DEBUG(log,
                "Export task: /status for {} is {} (not PENDING), skipping commit-failure bookkeeping",
                entry_path, current_status);
            return false;
        }

        Coordination::Requests ops;

        /// Record the exception in the same multi as the (possible) FAILED transition, so the
        /// user-visible last_exception znode is updated atomically with the state change that
        /// exposes it.
        appendExceptionOps(ops, zk, fs::path(entry_path), replica_name, /*part_name=*/"", exception_message, log);

        /// A non-retryable error (schema/spec mismatch, ...) can never succeed,
        /// so fail the task immediately
        const bool non_retryable = isNonRetryableExportError(exception_code);
        if (non_retryable)
        {
            /// Version-checked Set: if /status has changed since we read it (e.g. a peer's
            /// commit() succeeded and wrote COMPLETED), the whole multi aborts with
            /// ZBADVERSION and we safely do nothing — the winning terminal state stands.
            ops.emplace_back(zkutil::makeSetRequest(
                status_path,
                String(magic_enum::enum_name(ExportReplicatedMergeTreeTaskEntry::Status::FAILED)).data(),
                status_stat.version));
        }

        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperMulti);
        Coordination::Responses responses;
        const auto rc = zk->tryMulti(ops, responses);
        if (rc != Coordination::Error::ZOK)
        {
            LOG_WARNING(log, "Export task: Failed to persist commit failure bookkeeping for {}: {}", entry_path, rc);
            return false;
        }

        LOG_INFO(log,
            "Export task: Commit failure recorded for {} (code {}){}",
            entry_path, exception_code,
            non_retryable ? ", task transitioned to FAILED (non-retryable)" : ", will retry until task timeout");

        return non_retryable;
    }

    void appendExceptionOps(
        Coordination::Requests & ops,
        const zkutil::ZooKeeperPtr & zk,
        const std::filesystem::path & entry_path,
        const std::string & replica_name,
        const std::string & part_name,
        const std::string & exception_message,
        const LoggerPtr & log)
    {
        /// Per-replica leaf under the `last_exception/` container created at task setup.
        /// Each replica only ever writes its own leaf, so cross-replica updates never
        /// race on the count. Concurrent writers within the same replica still race
        /// on read+1+write (best-effort), matching the documented column semantics.
        const auto last_exception_path
            = entry_path / "last_exception" / escapeForFileName(replica_name);

        LastExceptionEntry entry;
        std::string current_data;

        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperGet);
        const bool leaf_exists = zk->tryGet(last_exception_path, current_data);
        if (leaf_exists)
        {
            try
            {
                entry = LastExceptionEntry::fromJsonString(current_data);
            }
            catch (...)
            {
                LOG_WARNING(log, "Export task: last_exception JSON at {} is malformed, resetting", last_exception_path.string());
                entry = LastExceptionEntry{};
            }
        }

        entry.message = exception_message;
        entry.part = part_name;
        entry.replica = replica_name;
        entry.time = ::time(nullptr);
        entry.count += 1;

        if (!leaf_exists)
        {
            /// Materialize the leaf out-of-band (idempotently) so the op we hand back to the
            /// caller's atomic multi is always a conflict-free Set. Two failing parts on the
            /// same replica whose first failures race would both pick Create here; one of the
            /// enclosing multis would then abort with ZNODEEXISTS and roll back its own part
            /// lock removal, stranding that part behind its ephemeral lock until session loss
            /// or task timeout. A peer thread winning this create (ZNODEEXISTS) is benign.
            ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperRequests);
            ProfileEvents::increment(ProfileEvents::ExportTaskZooKeeperCreate);
            const auto create_code = zk->tryCreate(last_exception_path, entry.toJsonString(), zkutil::CreateMode::Persistent);
            if (create_code != Coordination::Error::ZOK && create_code != Coordination::Error::ZNODEEXISTS)
                LOG_INFO(log, "Export task: could not pre-create last_exception leaf {}: {}", last_exception_path.string(), create_code);
        }

        /// Always a version -1 Set: it can neither conflict with a peer's create nor abort the
        /// enclosing multi, so the lock-release / FAILED-set ops it accompanies always commit.
        ops.emplace_back(zkutil::makeSetRequest(last_exception_path, entry.toJsonString(), -1));
    }

namespace
{
    /// Two types are interchangeable for partitioning only if their canonical names match. IDataType::equals
    /// is too weak here: it deliberately treats DateTime and DateTime64 with different time zones as equal,
    /// since they are interchangeable for INSERT, but a time zone changes what a temporal transform returns,
    /// so the same expression over the two types can produce different partitions.
    bool isSameTypeForPartitioning(const DataTypePtr & lhs, const DataTypePtr & rhs)
    {
        return lhs->getName() == rhs->getName();
    }

    /// The structural match is kind of permissive and is matching terms by name, not by type.
    /// We also need to ensure types are the same if they are wrapped by functions.
    bool castCannotBreakStructuralMatch(
        const ActionsDAG::Node * destination_output,
        const Names & minmax_column_names,
        const DataTypes & minmax_column_types)
    {
        if (destination_output->type == ActionsDAG::ActionType::INPUT)
            return true;

        for (const auto & required : ActionsDAG::cloneSubDAG({destination_output}, /*remove_aliases=*/ true).getRequiredColumns())
        {
            const auto it = std::find(minmax_column_names.begin(), minmax_column_names.end(), required.name);
            if (it == minmax_column_names.end())
                return false;

            if (!isSameTypeForPartitioning(minmax_column_types[static_cast<size_t>(it - minmax_column_names.begin())], required.type))
                return false;
        }

        return true;
    }

    /// Dynamically verifies the destination expression maps to a single partition by checking its monotonicity over the source range.
    /// Without `minmax`, only checks that it could: the expression is monotonic in a single column of the source partition key.
    void verifyOutputMapsToSinglePartition(
        const ActionsDAG::Node * destination_output,
        const Names & minmax_column_names,
        const DataTypes & minmax_column_types,
        const IMergeTreeDataPart::MinMaxIndex * minmax,
        const String & partition_id,
        const ContextPtr & context)
    {
        auto chain = buildPossiblyMonotonicChain(destination_output);
        if (!chain.input_node)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: the destination partition expression '{}' is not a chain of functions "
                "with known monotonicity over a single column, so it cannot be proven that the source partition "
                "maps to a single destination partition.", destination_output->result_name);

        const auto & column = chain.input_node->result_name;
        const auto slot_it = std::find(minmax_column_names.begin(), minmax_column_names.end(), column);
        if (slot_it == minmax_column_names.end())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: the destination partition expression uses column '{}', which is "
                "not part of the source MergeTree partition key.", column);
        const size_t slot = static_cast<size_t>(slot_it - minmax_column_names.begin());
        const auto & source_type = minmax_column_types[slot];

        /// A NULL value forms its own destination partition, so a Nullable column may split the source
        /// partition; min/max cannot rule that out. Require a structural match for such columns.
        if (isNullableOrLowCardinalityNullable(source_type))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: column '{}' is Nullable, so a NULL forms a separate destination "
                "partition; partition the source by the matching destination partition expression.", column);

        const auto & destination_type = chain.input_node->result_type;
        const bool needs_cast = !isSameTypeForPartitioning(source_type, destination_type);
        FunctionBasePtr cast_function;
        if (needs_cast)
        {
            cast_function = createInternalCast({source_type, column}, destination_type, CastType::nonAccurate, {}, context);
            if (!cast_function->hasInformationAboutMonotonicity())
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export partition: the cast of column '{}' to the destination type {} has no known "
                    "monotonicity, so it cannot be proven that a source partition maps to a single destination partition.",
                    column, destination_type->getName());
        }

        if (!isMonotonicChain(destination_output, chain))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: the destination partition expression '{}' is not monotonic in "
                "column '{}' (a hash such as icebergBucket never is), so its values at the endpoints of the "
                "partition do not bound the rows in between.",
                destination_output->result_name, column);

        if (!minmax)
            return;

        if (!minmax->initialized || slot >= minmax->hyperrectangle.size())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: no min/max statistics available for column '{}' in partition "
                "'{}'; cannot validate partitioning.", column, partition_id);
        const auto & min_value = minmax->hyperrectangle[slot].left;
        const auto & max_value = minmax->hyperrectangle[slot].right;

        /// If the types are not the same, we need to check if the cast is monotonic
        if (needs_cast && !cast_function->getMonotonicityForRange(*source_type, min_value, max_value).is_monotonic)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition '{}': values of column '{}' cross a non-monotonic cast boundary to "
                "the destination type {}, so it spans multiple destination partitions.",
                partition_id, column, destination_type->getName());

        auto endpoints = source_type->createColumn();
        endpoints->insert(min_value);
        endpoints->insert(max_value);

        Block block{{castColumn({std::move(endpoints), source_type, column}, destination_type), destination_type, column}};
        ExpressionActions(ActionsDAG::cloneSubDAG({destination_output}, /*remove_aliases=*/ true)).execute(block);

        const auto & result = *block.getByName(destination_output->result_name).column;
        Field at_min;
        Field at_max;
        result.get(0, at_min);
        result.get(1, at_max);

        if (at_min != at_max)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition '{}': the source partition might span multiple destination partitions "
                "for expression '{}'. A source MergeTree partition must map to a single destination partition.",
                partition_id, destination_output->result_name);
    }

    /// A source partition is not split in the destination when every destination partition expression is
    /// single-valued over it. That holds structurally when the expression is a deterministic function of the
    /// source partition key, because rows agreeing on the source key then agree on it as well; the remaining
    /// expressions have to be proven from the partition's min/max values. Without `parts`, only checks that
    /// such a proof is possible.
    void verifyPartitionKeyCompatibility(
        const KeyDescription & source_key,
        const KeyDescription & destination_key,
        const MergeTreeSettingsPtr & source_settings,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        /// An unpartitioned destination holds everything in a single partition.
        if (destination_key.column_names.empty())
            return;

        const auto & destination_dag = destination_key.expression->getActionsDAG();
        const auto source_dag = ActionsDAG::cloneSubDAG(
            source_key.expression->getActionsDAG().findInOutputs(source_key.column_names), /*remove_aliases=*/ true);

        /// ARRAY JOIN turns one row into many, which neither the tree matcher nor min/max models.
        if (source_dag.hasArrayJoin() || destination_dag.hasArrayJoin())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: a partition key containing ARRAY JOIN is not supported.");

        /// Injective functions do not group rows, so the values they are applied to are what a destination
        /// expression has to be a function of.
        const auto irreducible_source_nodes = removeInjectiveFunctionsFromResultsRecursively(source_dag);
        const auto matches = matchTrees(source_dag.getOutputs(), destination_dag);

        const auto minmax_columns = MergeTreeData::getMinMaxColumns(
            source_key, source_settings, MergeTreePartMinMaxIndexColumns::PARTITION_KEY_ONLY);
        const auto minmax_column_names = minmax_columns.getNames();
        const auto minmax_column_types = minmax_columns.getTypes();

        /// Compute the global min/max index of the parts
        IMergeTreeDataPart::MinMaxIndex minmax;
        for (const auto & part : parts)
            minmax.merge(*part->getMinMaxIndex());
        const auto * minmax_of_parts = parts.empty() ? nullptr : &minmax;

        /*
            1. If there is a structural match between the source and destination key, we accept it
            2. If there is not a structural match, we check if the destination expression maps to a single partition by checking its monotonicity over the source range.
        */
        NodeMap visited;
        for (const auto * destination_output : destination_dag.findInOutputs(destination_key.column_names))
        {
            if (allOutputsDependsOnlyOnAllowedNodes(irreducible_source_nodes, matches, destination_output, visited)
                && castCannotBreakStructuralMatch(destination_output, minmax_column_names, minmax_column_types))
                continue;

            verifyOutputMapsToSinglePartition(
                destination_output, minmax_column_names, minmax_column_types, minmax_of_parts, partition_id, context);
        }
    }
}

#if USE_AVRO
namespace
{
    /// The partition spec of an Iceberg destination as a ClickHouse partition key, nothing if it is unpartitioned.
    std::optional<KeyDescription> getIcebergDestinationPartitionKey(
        const Poco::JSON::Object::Ptr & metadata_object,
        const StorageMetadataPtr & destination_metadata,
        const ContextPtr & context)
    {
        const auto original_schema_id = metadata_object->getValue<Int64>(Iceberg::f_current_schema_id);
        const auto partition_spec_id  = metadata_object->getValue<Int64>(Iceberg::f_default_spec_id);

        Poco::JSON::Object::Ptr current_schema_json;
        {
            const auto schemas = metadata_object->getArray(Iceberg::f_schemas);
            for (size_t i = 0; i < schemas->size(); ++i)
            {
                auto s = schemas->getObject(static_cast<UInt32>(i));
                if (s->getValue<Int32>(Iceberg::f_schema_id) == static_cast<Int32>(original_schema_id))
                {
                    current_schema_json = s;
                    break;
                }
            }
        }

        Poco::JSON::Object::Ptr partition_spec_json;
        {
            const auto specs = metadata_object->getArray(Iceberg::f_partition_specs);
            for (size_t i = 0; i < specs->size(); ++i)
            {
                auto s = specs->getObject(static_cast<UInt32>(i));
                if (s->getValue<Int64>(Iceberg::f_spec_id) == partition_spec_id)
                {
                    partition_spec_json = s;
                    break;
                }
            }
        }

        if (!current_schema_json || !partition_spec_json)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition to Iceberg table: destination metadata is malformed, "
                "current-schema-id '{}' or default-spec-id '{}' does not resolve to a schema/spec.",
                original_schema_id, partition_spec_id);

        std::unordered_map<Int32, String> source_id_to_column_name;
        {
            const auto schema_fields = current_schema_json->getArray(Iceberg::f_fields);
            for (size_t i = 0; i < schema_fields->size(); ++i)
            {
                auto f = schema_fields->getObject(static_cast<UInt32>(i));
                source_id_to_column_name[f->getValue<Int32>(Iceberg::f_id)] = f->getValue<String>(Iceberg::f_name);
            }
        }

        const auto spec_fields = partition_spec_json->getArray(Iceberg::f_fields);
        const UInt32 spec_size = spec_fields ? static_cast<UInt32>(spec_fields->size()) : 0;
        if (spec_size == 0)
            return std::nullopt;

        /// Rebuild the destination spec as a ClickHouse partition key, the way the Iceberg read path does in
        /// ManifestFileIterator, so the same compatibility rule applies as for a plain object storage
        /// destination and the transform arguments keep the order the writer will use.
        const String partition_timezone = context->getSettingsRef()[Setting::iceberg_partition_timezone];
        auto partition_key_ast = make_intrusive<ASTFunction>();
        partition_key_ast->name = "tuple";
        partition_key_ast->arguments = make_intrusive<ASTExpressionList>();
        partition_key_ast->children.push_back(partition_key_ast->arguments);

        for (UInt32 i = 0; i < spec_size; ++i)
        {
            const auto field = spec_fields->getObject(i);
            const auto transform = field->getValue<String>(Iceberg::f_transform);
            const auto source_id = field->getValue<Int32>(Iceberg::f_source_id);

            const auto column_it = source_id_to_column_name.find(source_id);
            if (column_it == source_id_to_column_name.end())
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export partition to Iceberg table: destination partition spec refers to source_id "
                    "{}, which is not part of the current schema.", source_id);

            auto transform_ast = Iceberg::getASTFromTransform(transform, column_it->second, partition_timezone);
            if (!transform_ast)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export partition to Iceberg table: destination field on column '{}' uses transform "
                    "'{}', which has no ClickHouse equivalent.", column_it->second, transform);

            partition_key_ast->arguments->children.emplace_back(std::move(transform_ast));
        }

        const auto destination_columns = ColumnsDescription::fromNamesAndTypes(
            destination_metadata->getSampleBlockNonMaterialized().getNamesAndTypes());

        return KeyDescription::getKeyFromAST(partition_key_ast, destination_columns, /*virtuals=*/ {}, context);
    }
}

    void verifyIcebergPartitionCompatibility(
        const Poco::JSON::Object::Ptr & metadata_object,
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        if (const auto destination_key = getIcebergDestinationPartitionKey(metadata_object, destination_metadata, context))
            verifyPartitionKeyCompatibility(
                source_metadata->getPartitionKey(), *destination_key, parts.front()->storage.getSettings(), parts, partition_id, context);
    }
#endif

    void verifyPlainPartitionCompatibility(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        verifyPartitionKeyCompatibility(
            source_metadata->getPartitionKey(), destination_metadata->getPartitionKey(), parts.front()->storage.getSettings(),
            parts, partition_id, context);
    }

    void verifyPartitionKeyCanBeCompatible(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StoragePtr & destination_storage,
        const MergeTreeSettingsPtr & source_settings,
        const ContextPtr & context)
    {
        if (destination_storage->isDataLake())
        {
#if USE_AVRO
            const auto metadata_object = getIcebergMetadataObject(destination_storage, context);
            if (const auto destination_key = getIcebergDestinationPartitionKey(metadata_object, destination_metadata, context))
                verifyPartitionKeyCompatibility(source_metadata->getPartitionKey(), *destination_key, source_settings, {}, "", context);
#endif
            return;
        }

        verifyPartitionKeyCompatibility(
            source_metadata->getPartitionKey(), destination_metadata->getPartitionKey(), source_settings, {}, "", context);
    }

    namespace
    {
        bool haveSameTupleElementLayout(const DataTypePtr & source_type, const DataTypePtr & destination_type)
        {
            const auto source_type_unwrapped = removeNullable(removeLowCardinality(source_type));
            const auto destination_type_unwrapped = removeNullable(removeLowCardinality(destination_type));

            const auto * source_tuple = checkAndGetDataType<DataTypeTuple>(source_type_unwrapped.get());
            const auto * destination_tuple = checkAndGetDataType<DataTypeTuple>(destination_type_unwrapped.get());
            if (source_tuple || destination_tuple)
            {
                if (!source_tuple || !destination_tuple)
                    return false;

                if (source_tuple->hasExplicitNames() && destination_tuple->hasExplicitNames())
                {
                    if (source_tuple->getElementNames() != destination_tuple->getElementNames())
                        return false;
                }
                else if (source_tuple->getElements().size() != destination_tuple->getElements().size())
                    return false;

                const auto & source_elements = source_tuple->getElements();
                const auto & destination_elements = destination_tuple->getElements();
                for (size_t i = 0; i < source_elements.size(); ++i)
                    if (!haveSameTupleElementLayout(source_elements[i], destination_elements[i]))
                        return false;

                return true;
            }

            const auto * source_array = checkAndGetDataType<DataTypeArray>(source_type_unwrapped.get());
            const auto * destination_array = checkAndGetDataType<DataTypeArray>(destination_type_unwrapped.get());
            if (source_array || destination_array)
            {
                if (!source_array || !destination_array)
                    return false;

                return haveSameTupleElementLayout(source_array->getNestedType(), destination_array->getNestedType());
            }

            const auto * source_map = checkAndGetDataType<DataTypeMap>(source_type_unwrapped.get());
            const auto * destination_map = checkAndGetDataType<DataTypeMap>(destination_type_unwrapped.get());
            if (source_map || destination_map)
            {
                if (!source_map || !destination_map)
                    return false;

                return haveSameTupleElementLayout(source_map->getKeyType(), destination_map->getKeyType())
                    && haveSameTupleElementLayout(source_map->getValueType(), destination_map->getValueType());
            }

            return true;
        }

        /// `canBeSafelyCast` only widens numbers, but every `Date` fits in a `Date32`, every `DateTime` in a
        /// `DateTime64` of any scale, and every `DateTime64` in one of a larger scale unless that scale is 9,
        /// whose range ends in 2262. Iceberg stores them as `date` and `timestamp`, so an Iceberg table
        /// declares `Date32` and `DateTime64(6)` columns once it reloads its schema from its metadata.
        bool isDateOrTimeWidening(const DataTypePtr & source_type, const DataTypePtr & destination_type)
        {
            if (isNullableOrLowCardinalityNullable(source_type) && !isNullableOrLowCardinalityNullable(destination_type))
                return false;

            const auto source = removeNullable(removeLowCardinality(source_type));
            const auto destination = removeNullable(removeLowCardinality(destination_type));
            if (isDateTime64(source) && isDateTime64(destination))
            {
                const auto source_scale = assert_cast<const DataTypeDateTime64 &>(*source).getScale();
                const auto destination_scale = assert_cast<const DataTypeDateTime64 &>(*destination).getScale();
                return destination_scale >= source_scale && (destination_scale < 9 || source_scale == 9);
            }

            return (isDate(source) && isDate32(destination)) || (isDateTime(source) && isDateTime64(destination));
        }

        void verifyExportColumnCastIsSafe(
            const ColumnWithTypeAndName & source_column,
            const ColumnWithTypeAndName & destination_column,
            const StorageID & destination_storage_id)
        {
            if (canBeSafelyCast(source_column.type, destination_column.type)
                || isDateOrTimeWidening(source_column.type, destination_column.type))
                return;

            throw Exception(ErrorCodes::INCOMPATIBLE_COLUMNS,
                "Cannot export to {}: column '{}' requires a lossy cast from {} to {}, "
                "which may change values. Set `export_merge_tree_part_allow_lossy_cast = 1` "
                "to allow lossy casts during export.",
                destination_storage_id.getFullTableName(),
                destination_column.name,
                source_column.type->getName(),
                destination_column.type->getName());
        }

        void verifyPartitionKeyColumn(
            const ColumnWithTypeAndName & source_column,
            const ColumnWithTypeAndName & destination_column,
            size_t position,
            const StorageID & destination_storage_id,
            bool match_by_name)
        {
            if (source_column.name != destination_column.name)
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export to {}: partition key column '{}' is at position {} in the source "
                    "table, but the destination's column at that position is named '{}'. EXPORT "
                    "PART/PARTITION {} so partition key columns must be declared {} in both tables.",
                    destination_storage_id.getFullTableName(),
                    source_column.name,
                    position,
                    destination_column.name,
                    match_by_name ? "matches columns by name" : "matches columns by position",
                    match_by_name ? "with the same name" : "at the same position");

            if (!haveSameTupleElementLayout(source_column.type, destination_column.type))
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export to {}: partition key column '{}' has a different Tuple element "
                    "layout in the source ({}) and destination ({}). Tuple element names must be "
                    "declared in the same order in both tables.",
                    destination_storage_id.getFullTableName(),
                    source_column.name,
                    source_column.type->getName(),
                    destination_column.type->getName());
        }
    }

    void checkExportSchemaColumnsCount(
        size_t source_columns_count,
        size_t destination_columns_count,
        bool ignore_extra_source_columns)
    {
        if (source_columns_count < destination_columns_count)
            throw Exception(
                ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH,
                "Number of columns doesn't match (source: {} and result: {}): "
                "destination cannot have more columns than source",
                source_columns_count,
                destination_columns_count);

        if (source_columns_count > destination_columns_count && !ignore_extra_source_columns)
            throw Exception(
                ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH,
                "Number of columns doesn't match (source: {} and result: {}): "
                "source has extra columns and the `export_merge_tree_part_ignore_extra_source_columns` setting is disabled",
                source_columns_count,
                destination_columns_count);
    }

    void verifyExportSchemaCastable(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const StorageID & destination_storage_id,
        const ContextPtr & context)
    {
        /// Build (and discard) the same converting DAG the export worker will build
        /// later, to surface structural mismatches (column count, untyped casts) early.
        Block source_sample_block;
        for (const auto & column : source_metadata->getColumns().getReadable())
            source_sample_block.insert({column.type->createColumn(), column.type, column.name});

        const auto destination_sample_block = destination_metadata->getSampleBlockNonMaterialized();

        auto source_columns = source_sample_block.getColumnsWithTypeAndName();
        const auto & destination_columns = destination_sample_block.getColumnsWithTypeAndName();

        const auto schema_match_mode = context->getSettingsRef()[Setting::export_merge_tree_part_schema_match_mode].value;
        const bool ignore_extra_source_columns = context->getSettingsRef()[Setting::export_merge_tree_part_ignore_extra_source_columns];
        const bool src_has_extra_columns = source_columns.size() > destination_columns.size();

        checkExportSchemaColumnsCount(source_columns.size(), destination_columns.size(), ignore_extra_source_columns);

        const ActionsDAG::MatchColumnsMode mode = schema_match_mode == MergeTreePartExportSchemaMatchMode::NAME
            ? ActionsDAG::MatchColumnsMode::Name
            : ActionsDAG::MatchColumnsMode::Position;

        // makeConvertingActions with postitional mode requires equal columns count,
        if (ActionsDAG::MatchColumnsMode::Position == mode && ignore_extra_source_columns && src_has_extra_columns)
        {
            LOG_DEBUG(
                getLogger("ExportTaskUtils"),
                "Source has {} columns while destination has {} columns, "
                "the {} extra trailing source column(s) will be ignored",
                source_columns.size(),
                destination_columns.size(),
                source_columns.size() - destination_columns.size());

            source_columns.resize(destination_columns.size());
        }

        (void)ActionsDAG::makeConvertingActions(source_columns, destination_columns, mode, context);

        const auto & source_columns_description = source_metadata->getColumns();
        /// Collect the top-level columns that own columns or subcolumns required by `PARTITION BY`.
        /// For example, both `PARTITION BY t.a` and `PARTITION BY (t.a, t.b)` add `t`.
        std::unordered_set<String> partition_key_owner_columns;
        for (const auto & column_or_subcolumn_name : source_metadata->getColumnsRequiredForPartitionKey())
        {
            auto resolved = source_columns_description.tryGetColumnOrSubcolumn(GetColumnsOptions::All, column_or_subcolumn_name);
            const auto & column_name = resolved ? resolved->getNameInStorage() : column_or_subcolumn_name;
            partition_key_owner_columns.insert(column_name);
        }

        const bool allow_lossy_cast = context->getSettingsRef()[Setting::export_merge_tree_part_allow_lossy_cast];

        switch (schema_match_mode)
        {
            case MergeTreePartExportSchemaMatchMode::NAME: {
                std::unordered_map<String, size_t> source_positions_by_name;
                source_positions_by_name.reserve(source_columns.size());
                for (size_t i = 0; i < source_columns.size(); ++i)
                    source_positions_by_name.emplace(source_columns[i].name, i);

                for (const auto & destination_column : destination_columns)
                {
                    const auto source_it = source_positions_by_name.find(destination_column.name);
                    if (source_it == source_positions_by_name.end())
                        throw Exception(
                            ErrorCodes::THERE_IS_NO_COLUMN, "Cannot find column `{}` in source stream", destination_column.name);

                    const auto & source_column = source_columns[source_it->second];

                    if (partition_key_owner_columns.contains(destination_column.name))
                        verifyPartitionKeyColumn(
                            source_column,
                            destination_column,
                            source_it->second,
                            destination_storage_id,
                            /*match_by_name=*/true);

                    /// Lossy casts may silently change values, so reject them unless the user opts in.
                    if (!allow_lossy_cast)
                        verifyExportColumnCastIsSafe(source_column, destination_column, destination_storage_id);
                }
                break;
            }
            case MergeTreePartExportSchemaMatchMode::POSITION: {
                const size_t num_columns = std::min(source_columns.size(), destination_columns.size());
                for (size_t i = 0; i < num_columns; ++i)
                {
                    if (partition_key_owner_columns.contains(source_columns[i].name))
                        verifyPartitionKeyColumn(
                            source_columns[i],
                            destination_columns[i],
                            i,
                            destination_storage_id,
                            /*match_by_name=*/false);

                    /// Lossy casts may silently change values, so reject them unless the user opts in.
                    if (!allow_lossy_cast)
                        verifyExportColumnCastIsSafe(source_columns[i], destination_columns[i], destination_storage_id);
                }
                break;
            }
        }
    }
}

}
