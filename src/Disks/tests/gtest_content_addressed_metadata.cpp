#include <gtest/gtest.h>
#include <optional>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>

using namespace DB::ContentAddressed;

TEST(ContentAddressedPoolPaths, ContentKeysFanOut)
{
    EXPECT_EQ(blobKey("abcdef0123"), "blobs/ab/cd/abcdef0123");
    EXPECT_EQ(partKey("0011223344"), "parts/00/11/0011223344");
    EXPECT_EQ(blobKey("ab"), "blobs/ab"); // too short to fan out (test-only)
}

TEST(ContentAddressedPoolPaths, RefKeys)
{
    EXPECT_EQ(refsPrefix("srvA", "uuid-1"), "store/srvA/uuid-1/refs/");
    EXPECT_EQ(refKey("srvA", "uuid-1", "all_1_1_0"), "store/srvA/uuid-1/refs/all_1_1_0");
}

TEST(ContentAddressedPoolPaths, ParsePartFilePath)
{
    auto file = parsePartFilePath("123/uuid-1/all_1_1_0/columns.txt");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "uuid-1");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "columns.txt");

    auto part_dir = parsePartFilePath("123/uuid-1/all_1_1_0/"); // trailing slash, no file
    ASSERT_TRUE(part_dir.has_value());
    EXPECT_EQ(part_dir->part_name, "all_1_1_0");
    EXPECT_EQ(part_dir->file, "");

    EXPECT_FALSE(parsePartFilePath("123/uuid-1").has_value());   // table dir, not a part
    EXPECT_FALSE(parsePartFilePath("123").has_value());          // shallower
}

TEST(ContentAddressedPoolPaths, ParseTableUuid)
{
    EXPECT_EQ(parseTableUuid("123/uuid-1/"), std::optional<std::string>("uuid-1"));
    EXPECT_EQ(parseTableUuid("123/uuid-1"), std::optional<std::string>("uuid-1"));
    EXPECT_FALSE(parseTableUuid("123/uuid-1/all_1_1_0").has_value()); // part dir, not table dir
}
