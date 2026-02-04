#pragma once

#include <Storages/MutationCommands.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/PatchParts/PatchPartInfo.h>
#include <Storages/MergeTree/PatchParts/PatchPartsUtils.h>
#include <Storages/MergeTree/AlterConversions.h>
#include <Core/Types.h>
#include <memory>
#include <unordered_map>

namespace DB
{

/// Type aliases for partition-related maps (also defined in MergeTreeData.h for backward compatibility)
using PartitionIdToMinBlock = std::unordered_map<String, Int64>;
using PartitionIdToMinBlockPtr = std::shared_ptr<const PartitionIdToMinBlock>;

/// A snapshot of pending mutations that weren't applied to some of the parts yet
/// and should be applied on the fly (i.e. when reading from the part).
/// Mutations not supported by AlterConversions (isSupported*Mutation) can be omitted.
struct IMutationsSnapshot
{
    /// Contains info that doesn't depend on state of mutations.
    struct Params
    {
        Int64 metadata_version = -1;
        Int64 min_part_metadata_version = -1;
        PartitionIdToMinBlockPtr min_part_data_versions = nullptr;
        PartitionIdToMaxBlockPtr max_mutation_versions = nullptr;
        bool need_data_mutations = false;
        bool need_alter_mutations = false;
        bool need_patch_parts = false;
    };

    static Int64 getMinPartDataVersionForPartition(const Params & params, const String & partition_id);
    static Int64 getMaxMutationVersionForPartition(const Params & params, const String & partition_id);

    static bool needIncludeMutationToSnapshot(const Params & params, const MutationCommands & commands);

    virtual ~IMutationsSnapshot() = default;
    virtual void addPatches(DataPartsVector patches_) = 0;

    /// Returns mutation commands that are required to be applied to the `part`.
    /// @return list of mutation commands in order: oldest to newest.
    virtual MutationCommands getOnFlyMutationCommandsForPart(const DataPartPtr & part) const = 0;
    virtual PatchParts getPatchesForPart(const DataPartPtr & part) const = 0;
    virtual std::shared_ptr<IMutationsSnapshot> cloneEmpty() const = 0;
    virtual NameSet getAllUpdatedColumns() const = 0;

    virtual bool hasPatchParts() const = 0;
    virtual bool hasDataMutations() const = 0;
    virtual bool hasAlterMutations() const = 0;
    virtual bool hasMetadataMutations() const = 0;
    bool hasAnyMutations() const { return hasDataMutations() || hasAlterMutations() || hasMetadataMutations(); }
};

struct MutationsSnapshotBase : public IMutationsSnapshot
{
public:
    Params params;
    MutationCounters counters;
    PatchesByPartition patches_by_partition;

    MutationsSnapshotBase() = default;
    MutationsSnapshotBase(Params params_, MutationCounters counters_, DataPartsVector patches_);

    void addPatches(DataPartsVector patches_) override;
    PatchParts getPatchesForPart(const DataPartPtr & part) const final;

    bool hasPatchParts() const final { return !patches_by_partition.empty(); }
    bool hasDataMutations() const final { return counters.num_data > 0; }
    bool hasAlterMutations() const final { return counters.num_alter > 0; }
    bool hasMetadataMutations() const final { return counters.num_metadata > 0; }

protected:
    NameSet getColumnsUpdatedInPatches() const;
    void addSupportedCommands(const MutationCommands & commands, UInt64 mutation_version, MutationCommands & result_commands) const;
};

using MutationsSnapshotPtr = std::shared_ptr<const IMutationsSnapshot>;

}

