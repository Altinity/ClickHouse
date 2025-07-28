#include <Access/ContextAccess.h>
#include <Interpreters/Context.h>
#include <Storages/ExportsList.h>
#include <Storages/System/StorageSystemExports.h>
#include "DataTypes/DataTypeString.h"
#include "DataTypes/DataTypesNumber.h"


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
        {"elapsed", std::make_shared<DataTypeFloat64>(), "Time elapsed (in seconds) since data part movement started."},
        {"destination_path", std::make_shared<DataTypeString>(), "Path to the destination file in the destination storage."},
        {"part_name", std::make_shared<DataTypeString>(), "Name of the data part being moved."},
        {"thread_id", std::make_shared<DataTypeUInt64>(), "Identifier of a thread performing the movement."},
    };
}

void StorageSystemExports::fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const
{
    const auto access = context->getAccess();
    const bool check_access_for_tables = !access->isGranted(AccessType::SHOW_TABLES);
    const auto & move_list = context->getExportsList();

    for (const auto & move : move_list.get())
    {
        if (check_access_for_tables && !access->isGranted(AccessType::SHOW_TABLES, move.source_database, move.source_table))
            continue;

        size_t i = 0;
        res_columns[i++]->insert(move.source_database);
        res_columns[i++]->insert(move.source_table);
        res_columns[i++]->insert(move.destination_database);
        res_columns[i++]->insert(move.destination_table);
        res_columns[i++]->insert(move.elapsed);
        res_columns[i++]->insert(move.destination_path);
        res_columns[i++]->insert(move.part_name);
        res_columns[i++]->insert(move.thread_id);
    }
}

}
