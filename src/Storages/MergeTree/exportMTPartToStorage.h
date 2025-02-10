#pragma once

#include <Storages/MergeTree/MergeTreeData.h>

namespace DB
{

void exportMTPartToStorage(const MergeTreeData & data, const MergeTreeData::DataPartPtr & data_part, SinkToStoragePtr dst_storage_sink, ContextPtr context);

}
