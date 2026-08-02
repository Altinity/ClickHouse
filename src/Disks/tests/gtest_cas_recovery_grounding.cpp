#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Common/Exception.h>
#include <functional>
#include <limits>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int INVALID_STATE;
}

using namespace DB::Cas;

namespace
{

CatalogEntry catalog(NsState state)
{
    return CatalogEntry{.ns = RootNamespace{"srv1/recovery_grounding"}, .state = state, .incarnation = 1};
}

RefCkpt ckpt(uint64_t life_epoch, std::optional<RefTxnId> committed_through,
             std::optional<RefTxnId> checkpoint_snapshot_id = std::nullopt,
             std::optional<RefTxnId> last_epoch_seal = std::nullopt)
{
    return RefCkpt{.life_epoch = life_epoch,
                   .committed_through = committed_through,
                   .checkpoint_snapshot_id = checkpoint_snapshot_id,
                   .last_epoch_seal = last_epoch_seal};
}

void expectCode(const std::function<void()> & f, int code)
{
    try
    {
        f();
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), code);
    }
}

TEST(CasRecoveryGrounding, CreatingAndAbsentCatalogEntriesAreNotRecovered)
{
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Creating), ckpt(7, RefTxnId{7, 3}), std::nullopt); },
               DB::ErrorCodes::INVALID_STATE);
    expectCode([&] { chooseRecoveryGrounding(std::nullopt, ckpt(7, RefTxnId{7, 3}), std::nullopt); },
               DB::ErrorCodes::INVALID_STATE);
}

TEST(CasRecoveryGrounding, LiveAndRemovingRequireCheckpointAndLifeEpoch)
{
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Live), std::nullopt, std::nullopt); },
               DB::ErrorCodes::CORRUPTED_DATA);
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Removing), RefCkpt{}, std::nullopt); },
               DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, MissingFrontierMeansNoCommittedTransaction)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, std::nullopt), RefTxnId{7, 2});
    EXPECT_FALSE(grounding.base);
    EXPECT_FALSE(grounding.committed_through);
    EXPECT_TRUE(grounding.ignored_hinted_snapshot_above_frontier);
}

TEST(CasRecoveryGrounding, ChoosesGreatestEligibleBaseAndArithmeticWalkStart)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 8}, RefTxnId{7, 4}), RefTxnId{7, 6});
    EXPECT_EQ(grounding.base, (RefTxnId{7, 6}));
    EXPECT_EQ(grounding.walk_from, (RefTxnId{7, 7}));
    EXPECT_EQ(grounding.committed_through, (RefTxnId{7, 8}));
}

TEST(CasRecoveryGrounding, BaseAtFrontierStillStartsAtItsExactSuccessor)
{
    /// A writer recovery probes exactly this slot for its sole possible unfrontiered successor. The
    /// grounding contract must supply the arithmetic start even when the committed replay tail is empty.
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 8}, RefTxnId{7, 8}), std::nullopt);

    EXPECT_EQ(grounding.base, (RefTxnId{7, 8}));
    EXPECT_EQ(grounding.walk_from, (RefTxnId{7, 9}));
    EXPECT_EQ(grounding.committed_through, (RefTxnId{7, 8}));
}

TEST(CasRecoveryGrounding, IgnoresHintAboveFrontierAndRecordsDiagnostic)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 5}, RefTxnId{7, 3}), RefTxnId{7, 6});
    EXPECT_EQ(grounding.base, (RefTxnId{7, 3}));
    EXPECT_TRUE(grounding.ignored_hinted_snapshot_above_frontier);
}

TEST(CasRecoveryGrounding, WalksFromLifeEpochWithoutBaseAndNeverFromHintedLog)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(catalog(NsState::Removing), ckpt(9, RefTxnId{9, 3}), std::nullopt);
    EXPECT_EQ(grounding.walk_from, (RefTxnId{9, 1}));
}

TEST(CasRecoveryGrounding, RejectsBaseWithoutARepresentableSuccessor)
{
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live),
            ckpt(7, RefTxnId{8, 1}, RefTxnId{7, std::numeric_limits<uint64_t>::max()}, RefTxnId{8, 1}),
            std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, RejectsCheckpointFieldsAboveCommittedFrontier)
{
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, RefTxnId{7, 3}, RefTxnId{7, 4}), std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, RefTxnId{7, 3}, std::nullopt, RefTxnId{7, 4}), std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
}

}
