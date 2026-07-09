#pragma once

#include <Storages/System/IStorageSystemOneBlock.h>

namespace DB
{

class Context;

/// system.partition_exports: progress of EXPORT PARTITION tasks on plain (non-replicated) MergeTree
/// tables. Backed by each table's on-disk task descriptors mirrored in memory; querying it does not
/// touch disk. Each export task is represented by a single row. (ReplicatedMergeTree tasks live in
/// system.replicated_partition_exports instead.)
class StorageSystemPartitionExports final : public IStorageSystemOneBlock
{
public:
    std::string getName() const override { return "SystemPartitionExports"; }

    static ColumnsDescription getColumnsDescription();

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const override;
};

}
