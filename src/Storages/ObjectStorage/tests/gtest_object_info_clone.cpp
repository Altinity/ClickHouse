#include <gtest/gtest.h>

#include <IO/Archives/IArchiveReader.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueSource.h>

using namespace DB;

TEST(ObjectInfoClone, ArchiveEntryKeepsArchiveIdentity)
{
    auto archive = std::make_shared<ObjectInfo>("bucket/file.zip");
    IArchiveReader::FileInfo file_info;
    file_info.uncompressed_size = 42;
    file_info.compressed_size = 20;

    auto original = std::make_shared<StorageObjectStorageSource::ArchiveIterator::ObjectInfoInArchive>(
        archive, "inner.parquet", /*archive_reader=*/ nullptr, std::move(file_info));

    ObjectInfo sliced_value = *original;
    auto sliced = std::make_shared<ObjectInfo>(sliced_value);
    EXPECT_FALSE(sliced->isArchive());
    EXPECT_FALSE(std::dynamic_pointer_cast<StorageObjectStorageSource::ArchiveIterator::ObjectInfoInArchive>(sliced));

    auto cloned = original->clone();
    auto archive_cloned = std::dynamic_pointer_cast<StorageObjectStorageSource::ArchiveIterator::ObjectInfoInArchive>(cloned);
    ASSERT_TRUE(archive_cloned);
    EXPECT_TRUE(archive_cloned->isArchive());
    EXPECT_EQ(archive_cloned->getPathToArchive(), "bucket/file.zip");
    EXPECT_EQ(archive_cloned->getFileName(), "inner.parquet");
    EXPECT_EQ(archive_cloned->fileSizeInArchive(), 42u);
    EXPECT_EQ(archive_cloned->archive_object.get(), archive.get());
    EXPECT_EQ(archive_cloned->file_info.compressed_size, 20u);
}

TEST(ObjectInfoClone, QueueEntryKeepsFileMetadataType)
{
    ObjectInfo listed("queue/file.parquet");
    auto original = std::make_shared<ObjectStorageQueueSource::ObjectStorageQueueObjectInfo>(listed, /*file_metadata=*/ nullptr);
    original->data_lake_metadata.emplace();

    ObjectInfo sliced_value = *original;
    auto sliced = std::make_shared<ObjectInfo>(sliced_value);
    EXPECT_FALSE(std::dynamic_pointer_cast<ObjectStorageQueueSource::ObjectStorageQueueObjectInfo>(sliced));

    auto cloned = original->clone();
    auto queue_cloned = std::dynamic_pointer_cast<ObjectStorageQueueSource::ObjectStorageQueueObjectInfo>(cloned);
    ASSERT_TRUE(queue_cloned);
    EXPECT_EQ(queue_cloned->getPath(), "queue/file.parquet");
    EXPECT_FALSE(queue_cloned->file_metadata);
    EXPECT_TRUE(queue_cloned->data_lake_metadata.has_value());
    EXPECT_NE(queue_cloned.get(), original.get());
}
