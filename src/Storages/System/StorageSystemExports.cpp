#include <Access/ContextAccess.h>
#include <Interpreters/Context.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/System/StorageSystemExports.h>
#include "Columns/ColumnString.h"
#include "DataTypes/DataTypeString.h"
#include <Storages/MergeTree/MergeTreeExportStatus.h>
#include "Storages/VirtualColumnUtils.h"
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime.h>
#include <Interpreters/DatabaseCatalog.h>


namespace DB
{

ColumnsDescription StorageSystemExports::getColumnsDescription()
{
    return ColumnsDescription
    {
        {"source_database", std::make_shared<DataTypeString>(), "Name of the source database."},
        {"source_table", std::make_shared<DataTypeString>(), "Name of the source table."},
        {"destination_database", std::make_shared<DataTypeString>(), "Name of the destination database."},
        {"destination_table", std::make_shared<DataTypeString>(), "Name of the destination table."},
        {"create_time", std::make_shared<DataTypeDateTime>(), "Date and time when the export command was submitted for execution."},
        {"part_name", std::make_shared<DataTypeString>(), "Name of the part"}
    };
}

void StorageSystemExports::fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node * predicate, std::vector<UInt8>) const
{
    const auto access = context->getAccess();
    const bool check_access_for_databases = !access->isGranted(AccessType::SHOW_TABLES);
    
    /// Collect a set of *MergeTree tables.
    std::map<String, std::map<String, StoragePtr>> merge_tree_tables;
    for (const auto & db : DatabaseCatalog::instance().getDatabases())
    {
        /// Check if database can contain MergeTree tables
        if (!db.second->canContainMergeTreeTables())
            continue;

        const bool check_access_for_tables = check_access_for_databases && !access->isGranted(AccessType::SHOW_TABLES, db.first);

        for (auto iterator = db.second->getTablesIterator(context); iterator->isValid(); iterator->next())
        {
            const auto & table = iterator->table();
            if (!table)
                continue;

            if (!dynamic_cast<const MergeTreeData *>(table.get()))
                continue;

            if (check_access_for_tables && !access->isGranted(AccessType::SHOW_TABLES, db.first, iterator->name()))
                continue;

            merge_tree_tables[db.first][iterator->name()] = table;
        }
    }

    MutableColumnPtr col_source_database_export = ColumnString::create();
    MutableColumnPtr col_source_table_export = ColumnString::create();

    for (auto & db : merge_tree_tables)
    {
        for (auto & table : db.second)
        {
            col_source_database_export->insert(db.first);
            col_source_table_export->insert(table.first);
        }
    }

    ColumnPtr col_source_database = std::move(col_source_database_export);
    ColumnPtr col_source_table = std::move(col_source_table_export);

    /// Determine what tables are needed by the conditions in the query.
    {
        Block filtered_block
        {
            { col_source_database, std::make_shared<DataTypeString>(), "source_database" },
            { col_source_table, std::make_shared<DataTypeString>(), "source_table" },
        };

        VirtualColumnUtils::filterBlockWithPredicate(predicate, filtered_block, context);

        if (!filtered_block.rows())
            return;

        col_source_database = filtered_block.getByName("source_database").column;
        col_source_table = filtered_block.getByName("source_table").column;
    }

    for (size_t i_storage = 0; i_storage < col_source_database->size(); ++i_storage)
    {
        auto database = (*col_source_database)[i_storage].safeGet<String>();
        auto table = (*col_source_table)[i_storage].safeGet<String>();

        std::vector<MergeTreeExportStatus> statuses;
        {
            const IStorage * storage = merge_tree_tables[database][table].get();
            if (const auto * merge_tree = dynamic_cast<const MergeTreeData *>(storage))
                statuses = merge_tree->getExportsStatus();
        }

        for (const MergeTreeExportStatus & status : statuses)
        {
            size_t col_num = 0;
            res_columns[col_num++]->insert(status.source_database);
            res_columns[col_num++]->insert(status.source_table);
            res_columns[col_num++]->insert(status.destination_database);
            res_columns[col_num++]->insert(status.destination_table);
            res_columns[col_num++]->insert(status.create_time);
            res_columns[col_num++]->insert(status.part_name);
        }
    }
}

}
