#include "Interpreters/Context_fwd.h"
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/AddDefaultDatabaseVisitor.h>
#include <Processors/Transforms/AddingDefaultsTransform.h>
#include <Processors/Sources/RemoteSource.h>
#include <QueryPipeline/RemoteQueryExecutor.h>
#include <Storages/StorageFileCluster.h>
#include <Storages/IStorage.h>
#include <Storages/StorageFile.h>
#include <Storages/extractTableFunctionFromSelectQuery.h>
#include <Storages/VirtualColumnUtils.h>
#include <TableFunctions/TableFunctionFileCluster.h>

#include <memory>
#include <Storages/HivePartitioningUtils.h>
#include <Core/Settings.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace Setting
{
    extern const SettingsBool use_hive_partitioning;
}

StorageFileCluster::StorageFileCluster(
    const ContextPtr & context,
    const String & cluster_name_,
    const String & filename_,
    const String & format_name_,
    const String & compression_method,
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_)
    : IStorageCluster(cluster_name_, table_id_, getLogger("StorageFileCluster (" + table_id_.getFullTableName() + ")"))
    , filename(filename_)
    , format_name(format_name_)
{
    StorageInMemoryMetadata storage_metadata;

    size_t total_bytes_to_read; // its value isn't used as we are not reading files (just listing them). But it is required by getPathsList
    paths = StorageFile::getPathsList(filename_, context->getUserFilesPath(), context, total_bytes_to_read);

    if (columns_.empty())
    {
        ColumnsDescription columns;
        if (format_name == "auto")
            std::tie(columns, format_name) = StorageFile::getTableStructureAndFormatFromFile(paths, compression_method, std::nullopt, context);
        else
            columns = StorageFile::getTableStructureFromFile(format_name, paths, compression_method, std::nullopt, context);

        storage_metadata.setColumns(columns);
    }
    else
    {
        if (format_name == "auto")
            format_name = StorageFile::getTableStructureAndFormatFromFile(paths, compression_method, std::nullopt, context).second;
        storage_metadata.setColumns(columns_);
    }

    auto & storage_columns = storage_metadata.columns;

    /// Not grabbing the file_columns because it is not necessary to do it here.
    std::tie(hive_partition_columns_to_read_from_file_path, std::ignore) = HivePartitioningUtils::setupHivePartitioningForFileURLLikeStorage(
        storage_columns,
        paths.empty() ? "" : paths.front(),
        columns_.empty(),
        std::nullopt,
        context);

    storage_metadata.setConstraints(constraints_);
    setVirtuals(VirtualColumnUtils::getVirtualsForFileLikeStorage(storage_metadata.columns));
    setInMemoryMetadata(storage_metadata);
}

void StorageFileCluster::updateQueryToSendIfNeeded(DB::ASTPtr & query, const StorageSnapshotPtr & storage_snapshot, const DB::ContextPtr & context)
{
    auto * table_function = extractTableFunctionFromSelectQuery(query);
    if (!table_function)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Expected SELECT query from table function fileCluster, got '{}'", query->formatForErrorMessage());

    TableFunctionFileCluster::updateStructureAndFormatArgumentsIfNeeded(
        table_function,
        storage_snapshot->metadata->getColumns().getAll().toNamesAndTypesDescription(),
        format_name,
        context
    );
}

class FileTaskIterator : public TaskIterator
{
public:
    FileTaskIterator(const Strings & files,
        std::optional<StorageFile::ArchiveInfo> archive_info,
        const ActionsDAG::Node * predicate,
        const NamesAndTypesList & virtual_columns,
        const NamesAndTypesList & hive_partition_columns_to_read_from_file_path,
        const ContextPtr & context,
        bool distributed_processing = false)
        : iterator(files
            , archive_info
            , predicate
            , virtual_columns
            , hive_partition_columns_to_read_from_file_path
            , context
            , distributed_processing) {}

    ~FileTaskIterator() override = default;

    std::string operator()(size_t /* number_of_current_replica */) const override
    {
        return iterator.next();
    }

private:
    mutable StorageFileSource::FilesIterator iterator;
};

RemoteQueryExecutor::Extension StorageFileCluster::getTaskIteratorExtension(
    const ActionsDAG::Node * predicate,
    const std::optional<ActionsDAG> & /* filter_actions_dag */,
    const ContextPtr & context,
    ClusterPtr) const
{
    auto callback = std::make_shared<FileTaskIterator>(
        paths,
        std::nullopt,
        predicate,
        getVirtualsList(),
        hive_partition_columns_to_read_from_file_path,
        context
    );
    return RemoteQueryExecutor::Extension{.task_iterator = std::move(callback)};
}

}
