#pragma once

#include <base/types.h>
#include <vector>
#include <ctime>
#include <Storages/MergeTree/MergeTreeExportManifest.h>


namespace DB
{

struct MergeTreeExportStatus
{
    String source_database;
    String source_table;
    String destination_database;
    String destination_table;
    String transaction_id;
    time_t create_time = 0;
    std::vector<String> parts_to_do_names;
    MergeTreeExportManifest::Status status;
};

}

