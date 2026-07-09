#include <Storages/System/StorageSystemPartitionExports.h>
#include <Access/ContextAccess.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Storages/MergeTree/MergeTreePartitionExportScheduler.h>
#include <Storages/StorageMergeTree.h>
#include <Storages/VirtualColumnUtils.h>

namespace DB
{

ColumnsDescription StorageSystemPartitionExports::getColumnsDescription()
{
    return ColumnsDescription
    {
        {"source_database", std::make_shared<DataTypeString>(), "Name of the source database."},
        {"source_table", std::make_shared<DataTypeString>(), "Name of the source table."},
        {"destination_database", std::make_shared<DataTypeString>(), "Name of the destination database."},
        {"destination_table", std::make_shared<DataTypeString>(), "Name of the destination table."},
        {"create_time", std::make_shared<DataTypeDateTime>(), "Date and time when the export command was submitted."},
        {"partition_id", std::make_shared<DataTypeString>(), "ID of the partition being exported."},
        {"transaction_id", std::make_shared<DataTypeString>(), "ID of the export transaction (used to KILL it)."},
        {"query_id", std::make_shared<DataTypeString>(), "Query ID of the export operation."},
        {"parts", std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>()), "List of part names to be exported."},
        {"parts_count", std::make_shared<DataTypeUInt64>(), "Number of parts in the export."},
        {"parts_to_do", std::make_shared<DataTypeUInt64>(), "Number of parts still pending to be exported."},
        {"status", std::make_shared<DataTypeString>(), "Status of the export (PENDING, COMPLETED, FAILED, KILLED)."},
        {"last_exception", std::make_shared<DataTypeString>(), "Message of the most recent exception for this task, if any."},
        {"last_exception_part", std::make_shared<DataTypeString>(), "Part associated with the last exception (empty for task-level failures such as commit)."},
        {"last_exception_time", std::make_shared<DataTypeDateTime>(), "Time of the most recent exception."},
        {"exception_count", std::make_shared<DataTypeUInt64>(), "Total number of failures recorded for this task."},
    };
}

void StorageSystemPartitionExports::fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node * predicate, std::vector<UInt8>) const
{
    const auto access = context->getAccess();
    const bool check_access_for_databases = !access->isGranted(AccessType::SHOW_TABLES);

    std::map<String, std::map<String, StoragePtr>> merge_tree_tables;
    for (const auto & db : DatabaseCatalog::instance().getDatabases(GetDatabasesOptions{.with_remote_databases = false}))
    {
        /// skip data lakes
        if (db.second->isExternal())
            continue;

        const bool check_access_for_tables = check_access_for_databases && !access->isGranted(AccessType::SHOW_TABLES, db.first);

        for (auto iterator = db.second->getTablesIterator(context); iterator->isValid(); iterator->next())
        {
            const auto & table = iterator->table();
            if (!table)
                continue;

            /// Only plain (non-replicated) MergeTree tables expose partition exports here.
            if (!dynamic_cast<StorageMergeTree *>(table.get()))
                continue;

            if (check_access_for_tables && !access->isGranted(AccessType::SHOW_TABLES, db.first, iterator->name()))
                continue;

            merge_tree_tables[db.first][iterator->name()] = table;
        }
    }

    MutableColumnPtr col_database_mut = ColumnString::create();
    MutableColumnPtr col_table_mut = ColumnString::create();

    for (auto & db : merge_tree_tables)
    {
        for (auto & table : db.second)
        {
            col_database_mut->insert(db.first);
            col_table_mut->insert(table.first);
        }
    }

    ColumnPtr col_database = std::move(col_database_mut);
    ColumnPtr col_table = std::move(col_table_mut);

    /// Determine what tables are needed by the conditions in the query.
    {
        Block filtered_block
        {
            { col_database, std::make_shared<DataTypeString>(), "database" },
            { col_table, std::make_shared<DataTypeString>(), "table" },
        };

        VirtualColumnUtils::filterBlockWithPredicate(predicate, filtered_block, context);

        if (!filtered_block.rows())
            return;

        col_database = filtered_block.getByName("database").column;
        col_table = filtered_block.getByName("table").column;
    }

    for (size_t i_storage = 0; i_storage < col_database->size(); ++i_storage)
    {
        const auto database = (*col_database)[i_storage].safeGet<String>();
        const auto table = (*col_table)[i_storage].safeGet<String>();

        std::vector<PartitionExportInfo> partition_exports_info;
        {
            const IStorage * storage = merge_tree_tables[database][table].get();
            if (const auto * merge_tree = dynamic_cast<const StorageMergeTree *>(storage))
                partition_exports_info = merge_tree->getPartitionExportsInfo();
        }

        for (const PartitionExportInfo & info : partition_exports_info)
        {
            std::size_t i = 0;
            res_columns[i++]->insert(info.source_database);
            res_columns[i++]->insert(info.source_table);
            res_columns[i++]->insert(info.destination_database);
            res_columns[i++]->insert(info.destination_table);
            res_columns[i++]->insert(info.create_time);
            res_columns[i++]->insert(info.partition_id);
            res_columns[i++]->insert(info.transaction_id);
            res_columns[i++]->insert(info.query_id);

            Array parts_array;
            parts_array.reserve(info.parts.size());
            for (const auto & part : info.parts)
                parts_array.push_back(part);
            res_columns[i++]->insert(parts_array);

            res_columns[i++]->insert(info.parts_count);
            res_columns[i++]->insert(info.parts_to_do);
            res_columns[i++]->insert(info.status);
            res_columns[i++]->insert(info.last_exception_message);
            res_columns[i++]->insert(info.last_exception_part);
            res_columns[i++]->insert(info.last_exception_time);
            res_columns[i++]->insert(info.exception_count);
        }
    }
}

}
