#pragma once

#include <Storages/System/IStorageSystemOneBlock.h>

namespace DB
{

class Context;

/// system.ttl_exports: state of the `TTL ... EXPORT TO TABLE` expression of every MergeTree-family
/// table, one row per partition, as of the last tick of the table's TTL export scheduler. Every
/// replica of a `Replicated*MergeTree` table has the same rows. Read from memory, so querying it
/// touches neither disk nor ZooKeeper.
class StorageSystemTTLExports final : public IStorageSystemOneBlock
{
public:
    std::string getName() const override { return "SystemTTLExports"; }

    static ColumnsDescription getColumnsDescription();

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const override;
};

}
