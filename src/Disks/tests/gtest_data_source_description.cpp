#include <gtest/gtest.h>

#include <Disks/DiskType.h>

using namespace DB;

namespace
{

DataSourceDescription makeS3Description(bool files_are_whole_objects)
{
    DataSourceDescription description;
    description.type = DataSourceType::ObjectStorage;
    description.object_storage_type = ObjectStorageType::S3;
    description.description = "http://storage.example.com/bucket/";
    description.files_are_whole_objects = files_are_whole_objects;
    return description;
}

}

TEST(DataSourceDescription, NativeCopyNeedsWholeObjectsOnBothSides)
{
    const auto whole = makeS3Description(true);
    const auto windowed = makeS3Description(false);

    EXPECT_TRUE(whole.canUseNativeCopyWith(whole));
    EXPECT_FALSE(whole.canUseNativeCopyWith(windowed));
    EXPECT_FALSE(windowed.canUseNativeCopyWith(whole));
    EXPECT_FALSE(windowed.canUseNativeCopyWith(windowed));
}

TEST(DataSourceDescription, EqualityStaysReflexiveForWindowedFiles)
{
    const auto windowed = makeS3Description(false);

    EXPECT_TRUE(windowed == windowed);
    EXPECT_TRUE(windowed.sameKind(windowed));
}

TEST(DataSourceDescription, NativeCopyStillNeedsTheSameKind)
{
    auto one = makeS3Description(true);
    auto other = makeS3Description(true);
    other.description = "http://other.example.com/bucket/";

    EXPECT_FALSE(one.canUseNativeCopyWith(other));
}
