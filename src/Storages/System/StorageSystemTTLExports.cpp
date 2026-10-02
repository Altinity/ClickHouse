#include <Storages/System/StorageSystemTTLExports.h>

#include <Access/ContextAccess.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Storages/MergeTree/ExportTTLScheduler.h>
#include <Storages/MergeTree/MergeTreeData.h>

namespace DB
{

ColumnsDescription StorageSystemTTLExports::getColumnsDescription()
{
    return ColumnsDescription
    {
        {"database", std::make_shared<DataTypeString>(), "Name of the source database."},
        {"table", std::make_shared<DataTypeString>(), "Name of the source table."},
        {"partition_id", std::make_shared<DataTypeString>(), "ID of the partition."},
        {"destination_database", std::make_shared<DataTypeString>(), "Name of the destination database of the EXPORT TTL."},
        {"destination_table", std::make_shared<DataTypeString>(), "Name of the destination table of the EXPORT TTL."},
        {"exported_parts", std::make_shared<DataTypeUInt64>(), "Number of active parts whose rows were committed to the destination."},
        {"claimed_parts", std::make_shared<DataTypeUInt64>(), "Number of active parts being exported, or waiting to be exported again after a failed task."},
        {"eligible_parts", std::make_shared<DataTypeUInt64>(), "Number of active parts that are due for export and not exported yet."},
        {"eligible_bytes", std::make_shared<DataTypeUInt64>(), "Size on disk of the eligible parts."},
        {"parts_held_by_delete_gate", std::make_shared<DataTypeUInt64>(),
            "Number of parts whose delete or column TTL is due, but that are kept from merges until they are exported."},
        {"current_transaction_id", std::make_shared<DataTypeString>(), "Transaction id of the task exporting the partition now, see `system.distributed_exports`. Empty if there is none."},
        {"last_error", std::make_shared<DataTypeString>(), "Error of the last attempt to export the partition, empty if it succeeded."},
        {"scheduler_replica", std::make_shared<DataTypeString>(),
            "The replica of a Replicated*MergeTree table that schedules its TTL exports; the other replicas show the state it stored. Empty for a plain MergeTree."},
    };
}

void StorageSystemTTLExports::fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const
{
    const auto access = context->getAccess();
    const bool check_access_for_databases = !access->isGranted(AccessType::SHOW_TABLES);

    for (const auto & [database_name, database] : DatabaseCatalog::instance().getDatabases(GetDatabasesOptions{.with_datalake_catalogs = false, .with_remote_databases = false}))
    {
        if (database->isExternal())
            continue;

        const bool check_access_for_tables = check_access_for_databases && !access->isGranted(AccessType::SHOW_TABLES, database_name);

        for (auto iterator = database->getTablesIterator(context); iterator->isValid(); iterator->next())
        {
            const auto & table = iterator->table();
            const auto * merge_tree = dynamic_cast<const MergeTreeData *>(table.get());
            if (!merge_tree)
                continue;

            if (check_access_for_tables && !access->isGranted(AccessType::SHOW_TABLES, database_name, iterator->name()))
                continue;

            for (const auto & info : merge_tree->getExportTTLInfo())
            {
                size_t i = 0;
                res_columns[i++]->insert(database_name);
                res_columns[i++]->insert(iterator->name());
                res_columns[i++]->insert(info.partition_id);
                res_columns[i++]->insert(info.destination_database);
                res_columns[i++]->insert(info.destination_table);
                res_columns[i++]->insert(info.exported_parts);
                res_columns[i++]->insert(info.claimed_parts);
                res_columns[i++]->insert(info.eligible_parts);
                res_columns[i++]->insert(info.eligible_bytes);
                res_columns[i++]->insert(info.parts_held_by_delete_gate);
                res_columns[i++]->insert(info.current_transaction_id);
                res_columns[i++]->insert(info.last_error);
                res_columns[i++]->insert(info.scheduler_replica);
            }
        }
    }
}

}
