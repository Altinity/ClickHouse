#pragma once

#include <Storages/MergeTree/MergeTreeData.h>

namespace DB
{

void exportMTPartToParquet(const MergeTreeData & data, const MergeTreeData::DataPartPtr & data_part, ContextPtr context);

}
