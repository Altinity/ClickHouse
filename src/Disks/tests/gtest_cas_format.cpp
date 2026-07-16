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
    /// Every registered class has a non-empty, gen-1 baseline.
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
