#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
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
    for (auto id : {FormatId::Blob, FormatId::Manifest,
                    FormatId::GcState, FormatId::RetiredSet,
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

TEST(CasFormat, MagicForEachMutableObjectClass)
{
    /// The per-type magic is the 4 ASCII bytes stored as little-endian uint32.
    /// Verify each magic encodes the documented ASCII string.
    auto le32toStr = [](uint32_t v) -> std::string
    {
        char buf[4];
        buf[0] = static_cast<char>(v & 0xFF);
        buf[1] = static_cast<char>((v >> 8) & 0xFF);
        buf[2] = static_cast<char>((v >> 16) & 0xFF);
        buf[3] = static_cast<char>((v >> 24) & 0xFF);
        return std::string(buf, 4);
    };

    EXPECT_EQ(le32toStr(magicFor(FormatId::Blob)),          "CABL");
    EXPECT_EQ(le32toStr(magicFor(FormatId::Manifest)),      "CARS");
    EXPECT_EQ(le32toStr(magicFor(FormatId::PoolMeta)),      "CAPM");
    EXPECT_EQ(le32toStr(magicFor(FormatId::GcState)),       "CAGT");
    EXPECT_EQ(le32toStr(magicFor(FormatId::RetiredSet)),    "CART");
    EXPECT_EQ(le32toStr(magicFor(FormatId::GcOutcomes)),    "CAGO");
    EXPECT_EQ(le32toStr(magicFor(FormatId::PartManifest)),    "CAPT");
    EXPECT_EQ(le32toStr(magicFor(FormatId::RunFile)),         "CARN");
    EXPECT_EQ(le32toStr(magicFor(FormatId::FoldSeal)),        "CAFS");
    /// Guard the documented collision: PartManifest must NOT reuse PoolMeta's "CAPM".
    EXPECT_NE(magicFor(FormatId::PartManifest), magicFor(FormatId::PoolMeta));
}

TEST(CasFormat, MagicForUndefinedClassThrowsLogicalError)
{
    /// Roster has no mutable protobuf magic defined — it must throw LOGICAL_ERROR. (Tree and GcSnap
    /// were removed from the enum entirely in the rev. 15 part-manifest redesign.)
    try
    {
        magicFor(FormatId::Roster);
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}

TEST(CasFormat, MagicsAreDistinct)
{
    /// All per-type magics must be unique — a collision would make bad-magic detection useless.
    const uint32_t magics[] = {
        magicFor(FormatId::Blob),
        magicFor(FormatId::Manifest),
        magicFor(FormatId::PoolMeta),
        magicFor(FormatId::GcState),
        magicFor(FormatId::RetiredSet),
        magicFor(FormatId::GcOutcomes),
        magicFor(FormatId::PartManifest),
        magicFor(FormatId::RunFile),
        magicFor(FormatId::FoldSeal),
    };
    for (size_t i = 0; i < std::size(magics); ++i)
        for (size_t j = i + 1; j < std::size(magics); ++j)
            EXPECT_NE(magics[i], magics[j]) << "duplicate magic at indices " << i << " and " << j;
}
