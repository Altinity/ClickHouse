#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;

/// The generation history is reset to a flat `{1, 1}` baseline for every class: CAS is pre-release and
/// carries no persisted data, so there is no compatibility cost to starting the count over. Pinned
/// because the decision is invisible otherwise — nothing consults `changePoints` at decode time yet, so
/// a wrong entry here would sit unnoticed until the day a per-class reader floor is wired and starts
/// admitting objects it should refuse.
TEST(CASFormat, EveryClassResetToTheBaselineGeneration)
{
    for (auto id : allRegisteredFormatIds())
    {
        const auto cps = changePoints(id);
        ASSERT_EQ(cps.size(), 1u) << "FormatId " << static_cast<int>(id);
        EXPECT_EQ(cps.front().generation, 1u);
        EXPECT_EQ(cps.front().min_reader, 1u);
    }

    const auto roster_cps = changePoints(FormatId::Roster);
    ASSERT_EQ(roster_cps.size(), 1u);
    EXPECT_EQ(roster_cps.front().generation, 1u);
    EXPECT_EQ(roster_cps.front().min_reader, 1u);
}

TEST(CASFormat, CurrentVersionsAreGBuild)
{
    EXPECT_EQ(currentWriterVersion(), G_BUILD);
    EXPECT_EQ(currentCompatibilityVersion(), G_BUILD);
}

TEST(CASFormat, CheckCompatibilityPassesWhenKnown)
{
    EXPECT_NO_THROW(checkCompatibility(1u, "manifest"));
    EXPECT_NO_THROW(checkCompatibility(G_BUILD, "manifest"));
}

TEST(CASFormat, CheckCompatibilityFailsClosedOnFuture)
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
