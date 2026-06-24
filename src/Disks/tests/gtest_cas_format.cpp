#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromMemory.h>

namespace DB::ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;

TEST(CasFormat, ChangePointsExistForEveryClass)
{
    /// Every registered class has a non-empty, gen-1 baseline.
    for (auto id : {FormatId::Blob, FormatId::Tree, FormatId::Manifest, FormatId::GcSnap,
                    FormatId::GcState, FormatId::RetiredSet, FormatId::Watermark,
                    FormatId::PoolMeta, FormatId::Roster})
    {
        auto cps = changePoints(id);
        ASSERT_FALSE(cps.empty());
        EXPECT_EQ(cps.front().generation, 1u);
        EXPECT_EQ(cps.front().min_reader, 1u);
    }
}

TEST(CasFormat, CurrentWriterVersionIsGen1Baseline)
{
    auto s = currentWriterVersion(FormatId::Tree);
    EXPECT_EQ(s.writer_version, 1u);
    EXPECT_EQ(s.min_reader_version, 1u);
}

TEST(CasFormat, CurrentWriterVersionPicksNewestAtOrBelowFloor)
{
    /// With only generation 1 defined, any floor >= 1 yields {1,1}.
    auto s = currentWriterVersion(FormatId::Manifest, /*floor=*/5);
    EXPECT_EQ(s.writer_version, 1u);
    EXPECT_EQ(s.min_reader_version, 1u);
}

TEST(CasFormat, GateOnReadPassesWhenKnown)
{
    EXPECT_NO_THROW(gateOnRead(/*min_reader=*/1, "tree"));
    EXPECT_NO_THROW(gateOnRead(G_BUILD, "tree"));
}

TEST(CasFormat, GateOnReadFailsClosedOnFuture)
{
    try
    {
        gateOnRead(G_BUILD + 1, "tree");
        FAIL() << "expected UNKNOWN_FORMAT_VERSION";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
    }
}
