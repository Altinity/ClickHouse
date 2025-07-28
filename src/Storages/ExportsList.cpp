#include <Storages/ExportsList.h>
#include "base/getThreadId.h"

namespace DB
{

ExportsListElement::ExportsListElement(
    const StorageID & source_table_id_,
    const StorageID & destination_table_id_,
    const std::string & part_name_,
    const std::string & destination_path_)
: source_table_id(source_table_id_),
    destination_table_id(destination_table_id_),
    part_name(part_name_),
    destination_path(destination_path_),
    thread_id(getThreadId())
{
}

ExportInfo ExportsListElement::getInfo() const
{
    ExportInfo res;
    res.source_database = source_table_id.database_name;
    res.source_table = source_table_id.table_name;
    res.destination_database = destination_table_id.database_name;
    res.destination_table = destination_table_id.table_name;
    res.part_name = part_name;
    res.destination_path = destination_path;
    res.elapsed = watch.elapsedSeconds();
    res.thread_id = thread_id;
    return res;
}

}
