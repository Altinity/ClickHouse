#pragma once

#include <Storages/MergeTree/Compaction/MergePredicates/IMergePredicate.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/StorageMergeTree.h>
#include <Storages/MergeTree/MergeTreeCommittingBlock.h>
#include <Storages/MergeTree/ExportTTLDeleteGate.h>
#include <Storages/MergeTree/ExportedBlocks.h>

namespace DB
{

class MergeTreeMergePredicate final : public IMergePredicate
{
public:
    explicit MergeTreeMergePredicate(const StorageMergeTree & storage_, std::unique_lock<std::mutex> & merge_mutate_lock_);
    ~MergeTreeMergePredicate() override = default;

    std::expected<void, PreformattedMessage> canMergeParts(const PartProperties & left, const PartProperties & right) const override;
    std::expected<void, PreformattedMessage> canUsePartInMerges(const MergeTreeDataPartPtr & part) const;
    PartsRange getPatchesToApplyOnMerge(const PartsRange & range) const override;

private:
    const StorageMergeTree & storage;
    std::unique_lock<std::mutex> & merge_mutate_lock;
    PatchInfosByPartition patches_by_partition;
    CommittingBlocksSet committing_blocks;
    std::optional<Int64> min_update_block;
    ExportedBlocksPtr exported_blocks;
    ExportTTLDeleteGate delete_gate;
};

using MergeTreeMergePredicatePtr = std::shared_ptr<const MergeTreeMergePredicate>;

}
