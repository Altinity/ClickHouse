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
    for (auto id : {FormatId::Blob, FormatId::Tree, FormatId::Manifest, FormatId::GcSnap,
                    FormatId::GcState, FormatId::RetiredSet, FormatId::Watermark,
                    FormatId::PoolMeta, FormatId::Roster,
                    FormatId::RootsRegistry, FormatId::GcOutcomes})
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
    EXPECT_NO_THROW(checkCompatibility(1u, "tree"));
    EXPECT_NO_THROW(checkCompatibility(G_BUILD, "tree"));
}

TEST(CasFormat, CheckCompatibilityFailsClosedOnFuture)
{
    try
    {
        checkCompatibility(G_BUILD + 1, "tree");
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
    EXPECT_EQ(le32toStr(magicFor(FormatId::Tree)),          "CATR");
    EXPECT_EQ(le32toStr(magicFor(FormatId::Manifest)),      "CARS");
    EXPECT_EQ(le32toStr(magicFor(FormatId::PoolMeta)),      "CAPM");
    EXPECT_EQ(le32toStr(magicFor(FormatId::Watermark)),     "CAWM");
    EXPECT_EQ(le32toStr(magicFor(FormatId::GcState)),       "CAGT");
    EXPECT_EQ(le32toStr(magicFor(FormatId::RetiredSet)),    "CART");
    EXPECT_EQ(le32toStr(magicFor(FormatId::RootsRegistry)), "CARR");
    EXPECT_EQ(le32toStr(magicFor(FormatId::GcOutcomes)),    "CAGO");
}

TEST(CasFormat, MagicForUndefinedClassThrowsLogicalError)
{
    /// GcSnap and Roster have no mutable protobuf magic defined — they must throw LOGICAL_ERROR.
    try
    {
        magicFor(FormatId::GcSnap);
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
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
        magicFor(FormatId::Tree),
        magicFor(FormatId::Manifest),
        magicFor(FormatId::PoolMeta),
        magicFor(FormatId::Watermark),
        magicFor(FormatId::GcState),
        magicFor(FormatId::RetiredSet),
        magicFor(FormatId::RootsRegistry),
        magicFor(FormatId::GcOutcomes),
    };
    for (size_t i = 0; i < std::size(magics); ++i)
        for (size_t j = i + 1; j < std::size(magics); ++j)
            EXPECT_NE(magics[i], magics[j]) << "duplicate magic at indices " << i << " and " << j;
}
