#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;

TEST(CasFormat, ChangePointsExistForEveryClass)
{
    /// Every class that existed from the start has a non-empty, gen-1 baseline.
    for (auto id : {FormatId::Blob,
                    FormatId::GcState,
                    FormatId::PoolMeta, FormatId::Roster,
                    FormatId::GcOutcomes,
                    FormatId::PartManifest, FormatId::RunFile,
                    FormatId::FoldSeal})
    {
        auto cps = changePoints(id);
        ASSERT_FALSE(cps.empty());
        EXPECT_EQ(cps.front().generation, 1u);
        EXPECT_EQ(cps.front().min_reader, 1u);
    }
}

/// A class BORN after generation 1 begins its history at its birth generation, not at 1. `RefCkpt`
/// (spec INV-4) was introduced at generation 4, so `{4, 4}` is its whole history: there is no such
/// thing as a generation-1 `_ckpt`, and a `{1, 1}` baseline would assert that a generation-1 reader
/// could read one. Pinned because the decision is invisible otherwise — nothing consults
/// `changePoints` at decode time yet, so a wrong entry here would sit unnoticed until the day a
/// per-class reader floor is wired and starts admitting objects it should refuse.
TEST(CasFormat, ChangePointsOfAClassBornAfterGenerationOneStartAtItsBirth)
{
    const auto cps = changePoints(FormatId::RefCkpt);
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps.front().generation, kContiguousRefStreamsGeneration);
    EXPECT_EQ(cps.front().min_reader, kContiguousRefStreamsGeneration);
    EXPECT_GT(cps.front().generation, 1u) << "the point of this test is that it is NOT the gen-1 baseline";
}

TEST(CasFormat, CurrentVersionsAreGBuild)
{
    EXPECT_EQ(currentWriterVersion(), G_BUILD);
    EXPECT_EQ(currentCompatibilityVersion(), G_BUILD);
}

TEST(CasFormat, CheckCompatibilityPassesWhenKnown)
{
    EXPECT_NO_THROW(checkCompatibility(1u, "manifest"));
    EXPECT_NO_THROW(checkCompatibility(G_BUILD, "manifest"));
}

TEST(CasFormat, CheckCompatibilityFailsClosedOnFuture)
{
    try
    {
        checkCompatibility(G_BUILD + 1, "manifest");
        FAIL() << "expected UNKNOWN_FORMAT_VERSION";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
    }
}
