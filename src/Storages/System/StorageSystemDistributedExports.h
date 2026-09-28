#pragma once

#include <Storages/System/IStorageSystemOneBlock.h>

namespace DB
{

class Context;

/// system.distributed_exports: progress of the export tasks of every MergeTree-family table, created by
/// `EXPORT PARTITION` or by the `EXPORT` TTL, both of plain `MergeTree` (backed by on-disk task
/// descriptors) and `Replicated*MergeTree` (backed by the ZooKeeper manifest mirror). Both are read
/// from memory, so querying it touches neither disk nor ZooKeeper. Each export task is represented
/// by a single row.
class StorageSystemDistributedExports final : public IStorageSystemOneBlock
{
public:
    std::string getName() const override { return "SystemDistributedExports"; }

    static ColumnsDescription getColumnsDescription();

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const override;
};

}
