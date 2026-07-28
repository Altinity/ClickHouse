#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ProfileEvents.h>
#include <Common/FailPoint.h>
#include <Common/escapeForFileName.h>
#include <Common/logger_useful.h>
#include "Storages/ExportReplicatedMergeTreePartitionManifest.h"
#include "Storages/ExportReplicatedMergeTreePartitionTaskEntry.h"
#include <Storages/MergeTree/MergeTreeData.h>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <Core/Block.h>
#include <Core/Settings.h>
#include <DataTypes/Utils.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTExpressionList.h>
#include <Functions/FunctionFactory.h>
#include <Functions/CastOverloadResolver.h>
#include <Interpreters/castColumn.h>
#include <DataTypes/DataTypeString.h>

#if USE_AVRO
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#endif

namespace ProfileEvents
{
    extern const Event ExportPartitionZooKeeperRequests;
    extern const Event ExportPartitionZooKeeperGet;
    extern const Event ExportPartitionZooKeeperGetChildren;
    extern const Event ExportPartitionZooKeeperSet;
    extern const Event ExportPartitionZooKeeperCreate;
    extern const Event ExportPartitionZooKeeperMulti;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int FAULT_INJECTED;
    extern const int BAD_ARGUMENTS;
    extern const int NO_SUCH_DATA_PART;
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
    extern const SettingsTimezone iceberg_partition_timezone;
#endif
}

namespace FailPoints
{
    extern const char iceberg_export_after_commit_before_zk_completed[];
    extern const char export_partition_commit_always_throw[];
}

namespace fs = std::filesystem;

namespace ExportPartitionUtils
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

    Block getPartitionSourceBlockForIcebergCommit(
        MergeTreeData & storage, const String & partition_id, const std::vector<String> & exported_part_names)
    {
        auto lock = storage.readLockParts();
        const auto parts = storage.getDataPartsVectorInPartitionForInternalUsage(
            {MergeTreeDataPartState::Active, MergeTreeDataPartState::Outdated}, partition_id, lock);

        /// Only look at the parts being exported. These parts are guaranteed to map to a single partition.
        /// Parts that were later inserted shall be ignored
        const std::unordered_set<String> exported(exported_part_names.begin(), exported_part_names.end());
        MergeTreeData::DataPartsVector exported_parts;
        for (const auto & part : parts)
            if (exported.contains(part->name) && part->minmax_idx && part->minmax_idx->initialized)
                exported_parts.push_back(part);

        if (exported_parts.empty())
            throw Exception(ErrorCodes::NO_SUCH_DATA_PART,
                "Cannot find any of the exported parts for partition_id '{}' to derive Iceberg partition "
                "values. They may have been merged and cleaned up before this commit, or are not present "
                "on this replica. The commit will be retried.",
                partition_id);

        const auto metadata_snapshot = storage.getInMemoryMetadataPtr();
        const auto & partition_key = metadata_snapshot->getPartitionKey();
        const auto minmax_column_names = MergeTreeData::getMinMaxColumnsNames(partition_key);
        const auto minmax_column_types = MergeTreeData::getMinMaxColumnsTypes(partition_key);


        /// Grab the min/ max value from the partition. When the query was scheduled, we validated that
        /// dst_expression(min) == dst_expression(max). Therefore, we can use only the min value, no need for the max.
        Block block;
        for (size_t i = 0; i < minmax_column_types.size(); ++i)
        {
            std::optional<Field> min_value;
            for (const auto & part : exported_parts)
            {
                if (i >= part->minmax_idx->hyperrectangle.size())
                    continue;
                auto range = part->minmax_idx->hyperrectangle[i];
                range.shrinkToIncludedIfPossible();
                if (!min_value || range.left < min_value)
                {
                    min_value = range.left;
                }
            }

            if (!min_value)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "Cannot derive Iceberg partition value: no min/max statistics for partition-key column "
                    "'{}' in any exported part of partition '{}'.", minmax_column_names[i], partition_id);

            auto column = minmax_column_types[i]->createColumn();
            column->insert(*min_value);
            block.insert(ColumnWithTypeAndName(column->getPtr(), minmax_column_types[i], minmax_column_names[i]));
        }

        return block;
    }

    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ExportReplicatedMergeTreePartitionManifest & manifest)
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

    /// Collect all the exported paths from the processed parts
    /// If multiRead is supported by the keeper implementation, it is done in a single request
    /// Otherwise, multiple async requests are sent
    std::vector<std::string> getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path)
    {
        std::vector<std::string> exported_paths;

        LOG_DEBUG(log, "ExportPartition: Getting exported paths for {}", export_path);

        const auto processed_parts_path = fs::path(export_path) / "processed";

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGetChildren);
        std::vector<std::string> processed_parts;
        if (Coordination::Error::ZOK != zk->tryGetChildren(processed_parts_path, processed_parts))
        {
            /// todo arthur do something here
            LOG_WARNING(log, "ExportPartition: Failed to get parts children, exiting");
            return {};
        }

        std::vector<std::string> get_paths;

        for (const auto & processed_part : processed_parts)
        {
            get_paths.emplace_back(processed_parts_path / processed_part);
        }

        auto responses = zk->tryGet(get_paths);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGet, get_paths.size());

        responses.waitForResponses();

        for (size_t i = 0; i < responses.size(); ++i)
        {
            if (responses[i].error != Coordination::Error::ZOK)
            {
                /// todo arthur what to do in this case?
                /// It could be that zk is corrupt, in that case we should fail the task
                /// but it can also be some temporary network issue? not sure
                LOG_WARNING(log, "ExportPartition: Failed to get exported path, exiting");
                return {};
            }

            const auto processed_part_entry = ExportReplicatedMergeTreePartitionProcessedPartEntry::fromJsonString(responses[i].data);

            for (const auto & path_in_destination : processed_part_entry.paths_in_destination)
            {
                exported_paths.emplace_back(path_in_destination);
            }
        }

        return exported_paths;
    }

    void commit(
        const ExportReplicatedMergeTreePartitionManifest & manifest,
        const StoragePtr & destination_storage,
        const zkutil::ZooKeeperPtr & zk,
        const LoggerPtr & log,
        const std::string & entry_path,
        const ContextPtr & context_in,
        MergeTreeData & source_storage,
        const String & replica_name)
    {
        auto context = Context::createCopy(context_in);
        context->setSetting("write_full_path_in_iceberg_metadata", manifest.write_full_path_in_iceberg_metadata);

        if (manifest.iceberg_partition_timezone)
            context->setSetting("iceberg_partition_timezone", *manifest.iceberg_partition_timezone);

        /// Failpoint used by integration tests to force persistent commit failure and exercise
        /// the commit-attempts budget / FAILED state transition.
        fiu_do_on(FailPoints::export_partition_commit_always_throw,
        {
            throw Exception(ErrorCodes::FAULT_INJECTED,
                "Failpoint: export_partition_commit_always_throw");
        });

        /// Per-task ephemeral lock that serializes the commit phase across replicas.
        /// Without it, `handlePartExportSuccess` (post-last-part path) and `tryCleanup`
        /// (poll/recovery path) can drive `commitExportPartitionTransaction` concurrently
        /// for the same task.
        const auto commit_lock_path = fs::path(entry_path) / "commit_lock";
        auto commit_lock = zkutil::EphemeralNodeHolder::tryCreate(commit_lock_path, *zk, replica_name);
        if (!commit_lock)
        {
            LOG_DEBUG(log, "ExportPartition: commit_lock for {} is held by another replica, skipping commit on this replica", entry_path);
            return;
        }
        LOG_INFO(log, "ExportPartition: commit_lock for {} acquired by replica {}", entry_path, replica_name);

        /// Honor a concurrent KILL: commit_lock serializes us against killExportPartition,
        /// so a non-PENDING status here means cancel won the race.
        std::string status_str;
        if (!zk->tryGet(fs::path(entry_path) / "status", status_str))
            return;
        const auto status = magic_enum::enum_cast<ExportReplicatedMergeTreePartitionTaskEntry::Status>(status_str);
        if (!status || *status != ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING)
        {
            LOG_DEBUG(log, "ExportPartition: {} not PENDING, skipping commit", entry_path);
            return;
        }

        const auto exported_paths = ExportPartitionUtils::getExportedPaths(log, zk, entry_path);

        if (exported_paths.empty())
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ExportPartition: No exported paths found, will not commit export. This might be a bug");
        }

        //// not checking for an exact match because a single part might generate multiple files
        if (exported_paths.size() < manifest.parts.size())
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ExportPartition: Reached the commit phase, but exported paths size is less than the number of parts, will not commit export. This might be a bug");
        }

        IStorage::IcebergCommitExportPartitionArguments iceberg_args;

        if (!manifest.iceberg_metadata_json.empty())
        {
            iceberg_args.metadata_json_string = manifest.iceberg_metadata_json;
            if (source_storage.getInMemoryMetadataPtr()->hasPartitionKey())
                iceberg_args.partition_source_block =
                    getPartitionSourceBlockForIcebergCommit(source_storage, manifest.partition_id, manifest.parts);
        }

        const auto destination_commit_info = destination_storage->commitExportPartitionTransaction(
            manifest.transaction_id, manifest.partition_id, exported_paths, iceberg_args, context);

        /// Failpoint to simulate a crash after the Iceberg commit succeeds but before
        /// ZooKeeper is updated to COMPLETED. Used by idempotency integration tests.
        fiu_do_on(FailPoints::iceberg_export_after_commit_before_zk_completed,
        {
            LOG_INFO(log, "Failpoint: simulating crash after Iceberg commit, before ZK COMPLETED");
            std::this_thread::sleep_for(std::chrono::seconds(10));
            throw Exception(ErrorCodes::FAULT_INJECTED,
                "Failpoint: simulating crash after Iceberg commit, before ZK COMPLETED");
        });

        LOG_INFO(log, "ExportPartition: Committed export, mark as completed");

        const std::string status_path = fs::path(entry_path) / "status";
        const std::string completed_name = String(magic_enum::enum_name(ExportReplicatedMergeTreePartitionTaskEntry::Status::COMPLETED)).data();

        Coordination::Requests ops;
        ops.emplace_back(zkutil::makeSetRequest(status_path, completed_name, -1));

        ExportReplicatedMergeTreePartitionCommitInfoEntry commit_info_entry {
            destination_commit_info.iceberg_metadata_file,
            destination_commit_info.iceberg_manifest_list,
            destination_commit_info.iceberg_manifest_file,
            destination_commit_info.commit_marker_file};

        const std::string commit_info_path = fs::path(entry_path) / "commit_info";
        ops.emplace_back(zkutil::makeCreateRequest(commit_info_path, commit_info_entry.toJsonString(), zkutil::CreateMode::Persistent));

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperMulti);

        Coordination::Responses responses;
        const auto rc = zk->tryMulti(ops, responses);

        if (rc == Coordination::Error::ZOK)
        {
            LOG_INFO(log, "ExportPartition: Marked export as completed and persisted commit_info");
            return;
        }

        if (rc == Coordination::Error::ZNODEEXISTS)
        {
            LOG_INFO(log, "ExportPartition: commit_info already present (peer wrote it first); task already COMPLETED");
            return;
        }

        throw Exception(ErrorCodes::NETWORK_ERROR, "ExportPartition: Failed to mark export as completed (rc={}), will not try to fix it", rc);
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

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGet);
        if (!zk->tryGet(status_path, current_status, &status_stat))
        {
            /// Task was removed (TTL cleanup or force-overwrite). Nothing to do.
            LOG_DEBUG(log, "ExportPartition: /status missing for {}, skipping commit-failure bookkeeping", entry_path);
            return false;
        }

        const auto status = magic_enum::enum_cast<ExportReplicatedMergeTreePartitionTaskEntry::Status>(current_status);
        if (!status)
        {
            LOG_WARNING(log, "ExportPartition: Invalid status {} for task {}, skipping commit-failure bookkeeping", current_status, entry_path);
            return false;
        }

        if (status != ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING)
        {
            /// Another replica already reached a terminal state (COMPLETED or FAILED).
            /// Do NOT overwrite — a successful commit by a peer must win.
            LOG_DEBUG(log,
                "ExportPartition: /status for {} is {} (not PENDING), skipping commit-failure bookkeeping",
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
                String(magic_enum::enum_name(ExportReplicatedMergeTreePartitionTaskEntry::Status::FAILED)).data(),
                status_stat.version));
        }

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperMulti);
        Coordination::Responses responses;
        const auto rc = zk->tryMulti(ops, responses);
        if (rc != Coordination::Error::ZOK)
        {
            LOG_WARNING(log, "ExportPartition: Failed to persist commit failure bookkeeping for {}: {}", entry_path, rc);
            return false;
        }

        LOG_INFO(log,
            "ExportPartition: Commit failure recorded for {} (code {}){}",
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

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGet);
        const bool leaf_exists = zk->tryGet(last_exception_path, current_data);
        if (leaf_exists)
        {
            try
            {
                entry = LastExceptionEntry::fromJsonString(current_data);
            }
            catch (...)
            {
                LOG_WARNING(log, "ExportPartition: last_exception JSON at {} is malformed, resetting", last_exception_path.string());
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
            ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
            ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperCreate);
            const auto create_code = zk->tryCreate(last_exception_path, entry.toJsonString(), zkutil::CreateMode::Persistent);
            if (create_code != Coordination::Error::ZOK && create_code != Coordination::Error::ZNODEEXISTS)
                LOG_INFO(log, "ExportPartition: could not pre-create last_exception leaf {}: {}", last_exception_path.string(), create_code);
        }

        /// Always a version -1 Set: it can neither conflict with a peer's create nor abort the
        /// enclosing multi, so the lock-release / FAILED-set ops it accompanies always commit.
        ops.emplace_back(zkutil::makeSetRequest(last_exception_path, entry.toJsonString(), -1));
    }

namespace
{
    /// Terms of a given partition key expression.
    /// E.g. for "PARTITION BY (toYYYYMM(ts), country)", the terms are "toYYYYMM(ts)" and "country".
    struct PartitionTerm
    {
        String column;
        String function;
        std::optional<Int64> argument;
        std::optional<String> time_zone;

        bool operator==(const PartitionTerm & other) const
        {
            return column == other.column && function == other.function && argument == other.argument
                && time_zone == other.time_zone;
        }
    };

    std::vector<PartitionTerm> parsePartitionTerms(const ASTPtr & partition_key_ast)
    {
        std::vector<PartitionTerm> terms;
        if (!partition_key_ast)
            return terms;

        auto parse_one = [&](const ASTPtr & element)
        {
            if (const auto * ident = element->as<ASTIdentifier>())
            {
                terms.push_back({ident->name(), "", std::nullopt, std::nullopt});
                return;
            }

            const auto * func = element->as<ASTFunction>();
            if (!func)
                return;

            PartitionTerm term;
            term.function = func->name;
            for (const auto & child : func->children)
            {
                const auto * expression_list = child->as<ASTExpressionList>();
                if (!expression_list)
                    continue;
                /// A nested function such as toYYYYMM(toDate(ts)) is not unwrapped: it yields no column and
                /// stays unmatched, as the monotonicity proof only handles a single function of a single column.
                for (const auto & arg : expression_list->children)
                {
                    if (const auto * id = arg->as<ASTIdentifier>())
                        term.column = id->name();
                    /// Integer literal is the bucket/truncate width; a string literal is a date transform's timezone.
                    /// In theory, the string literal could mean a different thing, but among the partition expressions iceberg supports
                    /// time_zone is the only option
                    else if (const auto * lit = arg->as<ASTLiteral>())
                    {
                        if (lit->value.getType() == Field::Types::String)
                            term.time_zone = lit->value.safeGet<String>();
                        else
                            term.argument = lit->value.safeGet<Int64>();
                    }
                }
            }
            terms.push_back(std::move(term));
        };

        if (const auto * func = partition_key_ast->as<ASTFunction>(); func && func->name == "tuple")
        {
            for (const auto & child : func->children)
                if (const auto * expression_list = child->as<ASTExpressionList>())
                    for (const auto & element : expression_list->children)
                        parse_one(element);
        }
        else
            parse_one(partition_key_ast);

        return terms;
    }

    std::optional<std::pair<Field, Field>> calculatePartitionColumnMinMax(
        const MergeTreeData::DataPartsVector & parts, size_t slot)
    {
        std::optional<std::pair<Field, Field>> bounds;
        for (const auto & part : parts)
        {
            if (!part->minmax_idx || !part->minmax_idx->initialized || slot >= part->minmax_idx->hyperrectangle.size())
                continue;
            auto range = part->minmax_idx->hyperrectangle[slot];
            range.shrinkToIncludedIfPossible();
            if (!bounds)
            {
                bounds.emplace(range.left, range.right);
            }
            else
            {
                if (range.left < bounds->first)
                    bounds->first = range.left;
                if (bounds->second < range.right)
                    bounds->second = range.right;
            }
        }
        return bounds;
    }


    /// asserts that the source column maps to a single destination partition by checking the monotonicity on the min/max ranges
    void verifyColumnMapsToSinglePartition(
        const String & column,
        const String & function_name,
        std::optional<Int64> argument,
        const std::optional<String> & time_zone,
        const Names & minmax_column_names,
        const DataTypes & minmax_column_types,
        const Block & destination_sample,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        const auto slot_it = std::find(minmax_column_names.begin(), minmax_column_names.end(), column);
        if (slot_it == minmax_column_names.end())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: the destination partition expression uses column '{}', which is "
                "not part of the source MergeTree partition key.", column);
        const size_t slot = static_cast<size_t>(slot_it - minmax_column_names.begin());
        const auto & source_type = minmax_column_types[slot];

        /// A NULL value forms its own destination partition, so a Nullable column may split the source
        /// partition; min/max cannot rule that out. Require a structural match for such columns.
        if (source_type->isNullable())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: column '{}' is Nullable, so a NULL forms a separate destination "
                "partition; partition the source by the matching destination partition expression.", column);

        const auto bounds = calculatePartitionColumnMinMax(parts, slot);
        if (!bounds)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: no min/max statistics available for column '{}' in partition "
                "'{}'; cannot validate partitioning.", column, partition_id);
        const auto & [min_value, max_value] = *bounds;

        if (!destination_sample.has(column))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition: destination column '{}' not found.", column);
        const auto destination_type = destination_sample.getByName(column).type;

        /// The written value is transform(cast(source)), and the endpoints only bound the interior rows if
        /// the cast preserves order. Preserving every value is not enough: Int -> String loses nothing, yet
        /// "10" sorts before "2", so an interior row can fall outside the endpoints. Without a cast the
        /// order holds trivially; otherwise CAST must prove it over the partition's actual range.
        const bool is_cast_needed = !source_type->equals(*destination_type);
        if (is_cast_needed)
        {
            const auto cast_function
                = createInternalCast({source_type, column}, destination_type, CastType::nonAccurate, {}, context);
            if (!cast_function->hasInformationAboutMonotonicity()
                || !cast_function->getMonotonicityForRange(*source_type, min_value, max_value).is_monotonic)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export partition '{}': values of column '{}' cross a non-monotonic cast boundary to "
                    "the destination type {}, so it spans multiple destination partitions.",
                    partition_id, column, destination_type->getName());
        }

        auto values_column = source_type->createColumn();
        values_column->insert(min_value);
        values_column->insert(max_value);
        const auto cast_column = castColumn({std::move(values_column), source_type, column}, destination_type);

        Field cast_min;
        Field cast_max;
        cast_column->get(0, cast_min);
        cast_column->get(1, cast_max);

        /// A bare destination column is single-valued iff its two cast endpoints are equal; otherwise the
        /// destination transform applied to [cast(min), cast(max)] must be monotonic (so the endpoints
        /// bound every interior row; a hash such as bucket is not) and map both endpoints to one value.
        bool spans_multiple_partitions;
        if (function_name.empty() || function_name == "identity")
        {
            spans_multiple_partitions = cast_min != cast_max;
        }
        else
        {
            auto resolver = FunctionFactory::instance().get(function_name, context);
            ColumnsWithTypeAndName arguments;
            if (argument)
                arguments.push_back({DataTypeUInt64().createColumnConst(2, *argument), std::make_shared<DataTypeUInt64>(), "width"});
            arguments.push_back({cast_column, destination_type, column});
            if (time_zone)
                arguments.push_back({DataTypeString().createColumnConst(2, *time_zone), std::make_shared<DataTypeString>(), "timezone"});

            const auto function = resolver->build(arguments);
            if (!function->hasInformationAboutMonotonicity()
                || !function->getMonotonicityForRange(*destination_type, cast_min, cast_max).is_monotonic)
            {
                spans_multiple_partitions = true;
            }
            else
            {
                const auto result = function->execute(arguments, function->getResultType(), /*input_rows_count=*/ 2, /*dry_run=*/ false);
                Field first;
                Field second;
                result->get(0, first);
                result->get(1, second);
                spans_multiple_partitions = first != second;
            }
        }

        if (spans_multiple_partitions)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Cannot export partition '{}': the source partition might span multiple destination partitions "
                "for column '{}'. A source MergeTree partition must map to a single destination partition.",
                partition_id, column);
    }

    bool sourceHasMatchingPartitionTerm(
        const PartitionTerm & term,
        const std::vector<PartitionTerm> & source_terms,
        const Names & minmax_column_names,
        const DataTypes & minmax_column_types,
        const Block & destination_sample)
    {
        if (std::find(source_terms.begin(), source_terms.end(), term) == source_terms.end())
            return false;

        if (term.function.empty())
            return true;

        const auto it = std::find(minmax_column_names.begin(), minmax_column_names.end(), term.column);
        return it != minmax_column_names.end() && destination_sample.has(term.column)
            && minmax_column_types[it - minmax_column_names.begin()]->equals(*destination_sample.getByName(term.column).type);
    }

    /// Asserts every destination partition term maps the whole source partition to a single destination
    /// partition, either because the source already partitions by that term or because the min/max proof holds.
    void verifyPartitionTermsCompatibility(
        const std::vector<PartitionTerm> & destination_terms,
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        const auto source_terms = parsePartitionTerms(source_metadata->getPartitionKeyAST());

        const auto & source_partition_key = source_metadata->getPartitionKey();
        const auto minmax_column_names = MergeTreeData::getMinMaxColumnsNames(source_partition_key);
        const auto minmax_column_types = MergeTreeData::getMinMaxColumnsTypes(source_partition_key);
        const auto destination_sample = destination_metadata->getSampleBlockNonMaterialized();

        for (const auto & term : destination_terms)
        {
            if (sourceHasMatchingPartitionTerm(term, source_terms, minmax_column_names, minmax_column_types, destination_sample))
                continue;

            verifyColumnMapsToSinglePartition(term.column, term.function, term.argument, term.time_zone,
                minmax_column_names, minmax_column_types, destination_sample, parts, partition_id, context);
        }
    }
}

#if USE_AVRO
    void verifyIcebergPartitionCompatibility(
        const Poco::JSON::Object::Ptr & metadata_object,
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
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
        auto source_id_to_name = [&](Int32 id) -> String
        {
            auto it = source_id_to_column_name.find(id);
            return it != source_id_to_column_name.end() ? it->second : fmt::format("<unknown source_id={}>", id);
        };

        const auto actual_fields = partition_spec_json->getArray(Iceberg::f_fields);
        const size_t actual_size = actual_fields ? actual_fields->size() : 0;
        const String partition_timezone = context->getSettingsRef()[Setting::iceberg_partition_timezone];

        /// Rebuild the destination spec as ClickHouse partition terms, mirroring ChunkPartitioner and the commit
        /// path, so the same compatibility rule applies as for a plain object storage destination. An empty spec
        /// yields no terms: an unpartitioned table has a single partition that every source part maps to.
        std::vector<PartitionTerm> destination_terms;
        destination_terms.reserve(actual_size);
        for (UInt32 i = 0; i < actual_size; ++i)
        {
            const auto af = actual_fields->getObject(i);
            const auto dest_transform = af->getValue<String>(Iceberg::f_transform);
            const String column = source_id_to_name(af->getValue<Int32>(Iceberg::f_source_id));
            const auto transform_and_argument = Iceberg::parseTransformAndArgument(dest_transform, partition_timezone);

            if (!transform_and_argument)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Cannot export partition to Iceberg table: destination field on column '{}' uses transform "
                    "'{}', which has no ClickHouse equivalent.", column, dest_transform);

            /// A bare source column parses to an empty function name, which is what Iceberg calls identity.
            destination_terms.push_back({
                column,
                transform_and_argument->transform_name == "identity" ? "" : transform_and_argument->transform_name,
                transform_and_argument->argument
                    ? std::optional<Int64>(static_cast<Int64>(*transform_and_argument->argument)) : std::nullopt,
                transform_and_argument->time_zone});
        }

        verifyPartitionTermsCompatibility(
            destination_terms, source_metadata, destination_metadata, parts, partition_id, context);
    }
#endif

    void verifyPlainPartitionCompatibility(
        const StorageMetadataPtr & source_metadata,
        const StorageMetadataPtr & destination_metadata,
        const MergeTreeData::DataPartsVector & parts,
        const String & partition_id,
        const ContextPtr & context)
    {
        const auto source_key_ast = source_metadata->getPartitionKeyAST();
        const auto destination_key_ast = destination_metadata->getPartitionKeyAST();

        auto ast_to_string = [](const ASTPtr & ast) { return ast ? ast->formatWithSecretsOneLine() : String{}; };

        /// Fast path: identical partition keys are single-valued per source partition by construction.
        if (ast_to_string(source_key_ast) == ast_to_string(destination_key_ast))
            return;

        /// If the partition keys are not identical, we need to verify that the source partition maps to a single destination partition
        verifyPartitionTermsCompatibility(
            parsePartitionTerms(destination_key_ast), source_metadata, destination_metadata, parts, partition_id, context);
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

        const auto source_columns = source_sample_block.getColumnsWithTypeAndName();
        const auto destination_columns = destination_sample_block.getColumnsWithTypeAndName();

        (void) ActionsDAG::makeConvertingActions(
            source_columns,
            destination_columns,
            ActionsDAG::MatchColumnsMode::Position,
            context);

        /// Lossy casts may silently change values, so reject them unless the user opts in.
        if (context->getSettingsRef()[Setting::export_merge_tree_part_allow_lossy_cast])
            return;

        const size_t num_columns = std::min(source_columns.size(), destination_columns.size());
        for (size_t i = 0; i < num_columns; ++i)
        {
            const auto & source_column = source_columns[i];
            const auto & destination_column = destination_columns[i];
            if (!canBeSafelyCast(source_column.type, destination_column.type))
                throw Exception(ErrorCodes::INCOMPATIBLE_COLUMNS,
                    "Cannot export to {}: column '{}' requires a lossy cast from {} to {}, "
                    "which may change values. Set `export_merge_tree_part_allow_lossy_cast = 1` "
                    "to allow lossy casts during export.",
                    destination_storage_id.getFullTableName(),
                    destination_column.name,
                    source_column.type->getName(),
                    destination_column.type->getName());
        }
    }
}

}
