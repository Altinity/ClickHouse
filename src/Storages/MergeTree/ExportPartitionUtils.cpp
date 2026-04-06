#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ProfileEvents.h>
#include <Common/FailPoint.h>
#include <Common/logger_useful.h>
#include "Storages/ExportReplicatedMergeTreePartitionManifest.h"
#include "Storages/ExportReplicatedMergeTreePartitionTaskEntry.h"
#include <filesystem>
#include <thread>
#include <Interpreters/Context.h>

namespace ProfileEvents
{
    extern const Event ExportPartitionZooKeeperRequests;
    extern const Event ExportPartitionZooKeeperGet;
    extern const Event ExportPartitionZooKeeperGetChildren;
    extern const Event ExportPartitionZooKeeperSet;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int FAULT_INJECTED;
}

namespace FailPoints
{
    extern const char iceberg_export_after_commit_before_zk_completed[];
}

namespace fs = std::filesystem;

namespace ExportPartitionUtils
{
    ContextPtr getContextCopyWithTaskSettings(const ContextPtr & context, const ExportReplicatedMergeTreePartitionManifest & manifest)
    {
        auto context_copy = Context::createCopy(context);
        context_copy->makeQueryContextForExportPart();
        context_copy->setCurrentQueryId(manifest.query_id);
        context_copy->setSetting("output_format_parallel_formatting", manifest.parallel_formatting);
        context_copy->setSetting("output_format_parquet_parallel_encoding", manifest.parquet_parallel_encoding);
        context_copy->setSetting("max_threads", manifest.max_threads);
        context_copy->setSetting("export_merge_tree_part_file_already_exists_policy", String(magic_enum::enum_name(manifest.file_already_exists_policy)));
        context_copy->setSetting("export_merge_tree_part_max_bytes_per_file", manifest.max_bytes_per_file);
        context_copy->setSetting("export_merge_tree_part_max_rows_per_file", manifest.max_rows_per_file);

        /// always skip pending mutations and patch parts because we already validated the parts during query processing
        context_copy->setSetting("export_merge_tree_part_throw_on_pending_mutations", false);
        context_copy->setSetting("export_merge_tree_part_throw_on_pending_patch_parts", false);

        context_copy->setSetting("export_merge_tree_part_filename_pattern", manifest.filename_pattern);
        context_copy->setSetting("write_full_path_in_iceberg_metadata", manifest.write_full_path_in_iceberg_metadata);

	    return context_copy;
    }

    /// Collect all the exported paths from the processed parts
    /// If multiRead is supported by the keeper implementation, it is done in a single request
    /// Otherwise, multiple async requests are sent
    std::vector<std::string> getExportedPaths(const LoggerPtr & log, const zkutil::ZooKeeperPtr & zk, const std::string & export_path)
    {
        std::vector<std::string> exported_paths;

        LOG_INFO(log, "ExportPartition: Getting exported paths for {}", export_path);

        const auto processed_parts_path = fs::path(export_path) / "processed";

        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperGetChildren);
        std::vector<std::string> processed_parts;
        if (Coordination::Error::ZOK != zk->tryGetChildren(processed_parts_path, processed_parts))
        {
            /// todo arthur do something here
            LOG_INFO(log, "ExportPartition: Failed to get parts children, exiting");
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
                LOG_INFO(log, "ExportPartition: Failed to get exported path, exiting");
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
        const ContextPtr & context_in)
    {
        auto context = Context::createCopy(context_in);
        context->setSetting("write_full_path_in_iceberg_metadata", manifest.write_full_path_in_iceberg_metadata);

        const auto exported_paths = ExportPartitionUtils::getExportedPaths(log, zk, entry_path);

        if (exported_paths.empty())
        {
            LOG_WARNING(log, "ExportPartition: No exported paths found, will not commit export. This might be a bug");
            return;
        }

        //// not checking for an exact match because a single part might generate multiple files
        if (exported_paths.size() < manifest.parts.size())
        {
            LOG_WARNING(log, "ExportPartition: Reached the commit phase, but exported paths size is less than the number of parts, will not commit export. This might be a bug");
            return;
        }

        IStorage::IcebergCommitExportPartitionArguments iceberg_args;

        if (!manifest.iceberg_metadata_json.empty())
        {
            iceberg_args.metadata_json_string = manifest.iceberg_metadata_json;
            iceberg_args.partition_values = manifest.partition_values;
        }

        destination_storage->commitExportPartitionTransaction(manifest.transaction_id, manifest.partition_id, exported_paths, iceberg_args, context);

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
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperRequests);
        ProfileEvents::increment(ProfileEvents::ExportPartitionZooKeeperSet);
        if (Coordination::Error::ZOK == zk->trySet(fs::path(entry_path) / "status", String(magic_enum::enum_name(ExportReplicatedMergeTreePartitionTaskEntry::Status::COMPLETED)).data(), -1))
        {
            LOG_INFO(log, "ExportPartition: Marked export as completed");
        }
        else
        {
            LOG_INFO(log, "ExportPartition: Failed to mark export as completed, will not try to fix it");
        }
    }
}

}
