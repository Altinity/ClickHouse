#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <gtest/gtest.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeTableStateSnapshot.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataObjectInfo.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergTableStateSnapshot.h>
#include <Core/AntalyaProtocol.h>
#include <Core/ProtocolDefines.h>
#if USE_AVRO && USE_AWS_S3
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ExternalPathResolver.h>
#endif

namespace DB::ErrorCodes
{
extern const int PROTOCOL_VERSION_MISMATCH;
}

using namespace DB;

namespace
{
std::vector<Iceberg::TableStateSnapshot> example_iceberg_states = {
    Iceberg::TableStateSnapshot{
        .metadata_file_path = "abacaba",
        .metadata_version = std::numeric_limits<Int32>::min(),
        .schema_id = std::numeric_limits<Int32>::max(),
        .snapshot_id = std::optional{std::numeric_limits<Int64>::max()},
    },
    Iceberg::TableStateSnapshot{
        .metadata_file_path = "",
        .metadata_version = 0,
        .schema_id = std::numeric_limits<Int32>::max(),
        .snapshot_id = std::nullopt,
    },
};

Iceberg::IcebergObjectSerializableInfo makeObjectInfo()
{
    Iceberg::IcebergObjectSerializableInfo info;
    info.data_object_file_path_key = DB::Iceberg::IcebergPathFromMetadata::deserialize("s3://bucket/path/to/file.parquet");
    info.underlying_format_read_schema_id = 1;
    info.schema_id_relevant_to_iterator = 1;
    info.sequence_number = 0;
    info.file_format = "PARQUET";
    return info;
}
}

TEST(DatalakeStateSerde, IcebergStateSerde)
{

    for (const auto & iceberg_state : example_iceberg_states)
    {
        String str;
        {
            WriteBufferFromString write_buffer{str};
            iceberg_state.serialize(write_buffer);
        }
        ReadBufferFromMemory read_buffer(str) ;
        Iceberg::TableStateSnapshot serde_state = Iceberg::TableStateSnapshot::deserialize(read_buffer, DATA_LAKE_TABLE_STATE_SNAPSHOT_PROTOCOL_VERSION);
        ASSERT_TRUE(read_buffer.eof());
        ASSERT_EQ(iceberg_state, serde_state);
    }
}


TEST(DatalakeStateSerde, DataLakeStateSerde)
{

    for (const auto & iceberg_state : example_iceberg_states)
    {
        DataLakeTableStateSnapshot initial_state = iceberg_state;
        String str;
        {
            WriteBufferFromString write_buffer{str};
            serializeDataLakeTableStateSnapshot(initial_state, write_buffer);
        }
        ReadBufferFromMemory read_buffer(str);
        DataLakeTableStateSnapshot serde_state = deserializeDataLakeTableStateSnapshot(read_buffer);
        ASSERT_TRUE(read_buffer.eof());
        ASSERT_EQ(initial_state, serde_state);
    }
}


TEST(DatalakeStateSerde, IcebergObjectSerializableInfoRoundTrip)
{
    Iceberg::IcebergObjectSerializableInfo info;
    info.data_object_file_path_key = DB::Iceberg::IcebergPathFromMetadata::deserialize("s3://bucket/path/to/file.parquet");
    info.underlying_format_read_schema_id = 42;
    info.schema_id_relevant_to_iterator = 7;
    info.sequence_number = 123456;
    info.file_format = "PARQUET";
    info.position_deletes_objects = {{"s3://bucket/deletes/pos1.parquet", "PARQUET", "s3://bucket/path/to/file.parquet"}};
    info.equality_deletes_objects = {{"s3://bucket/deletes/eq1.parquet", "PARQUET", std::vector<Int32>{1, 2, 3}, 42}};
    info.record_count = 100500;
    info.file_size_in_bytes = 999888777;

    String str;
    {
        WriteBufferFromString write_buffer{str};
        info.serializeForClusterFunctionProtocol(write_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION, DBMS_ANTALYA_PROTOCOL_VERSION);
    }
    ReadBufferFromMemory read_buffer(str.data(), str.size());
    Iceberg::IcebergObjectSerializableInfo deserialized;
    deserialized.deserializeForClusterFunctionProtocol(read_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION);

    ASSERT_TRUE(read_buffer.eof());
    ASSERT_EQ(deserialized.data_object_file_path_key, info.data_object_file_path_key);
    ASSERT_EQ(deserialized.underlying_format_read_schema_id, info.underlying_format_read_schema_id);
    ASSERT_EQ(deserialized.schema_id_relevant_to_iterator, info.schema_id_relevant_to_iterator);
    ASSERT_EQ(deserialized.sequence_number, info.sequence_number);
    ASSERT_EQ(deserialized.file_format, info.file_format);
    ASSERT_EQ(deserialized.position_deletes_objects.size(), 1);
    ASSERT_EQ(deserialized.equality_deletes_objects.size(), 1);
    ASSERT_TRUE(deserialized.record_count.has_value());
    ASSERT_EQ(*deserialized.record_count, 100500);
    ASSERT_TRUE(deserialized.file_size_in_bytes.has_value());
    ASSERT_EQ(*deserialized.file_size_in_bytes, 999888777);
}


TEST(DatalakeStateSerde, IcebergObjectSerializableInfoWithoutFileStats)
{
    Iceberg::IcebergObjectSerializableInfo info;
    info.data_object_file_path_key = DB::Iceberg::IcebergPathFromMetadata::deserialize("s3://bucket/path/to/file.parquet");
    info.underlying_format_read_schema_id = 1;
    info.schema_id_relevant_to_iterator = 1;
    info.sequence_number = 0;
    info.file_format = "PARQUET";
    info.record_count = 42;
    info.file_size_in_bytes = 1024;

    /// Serialize at protocol version 5 (before file stats were added)
    String str;
    {
        WriteBufferFromString write_buffer{str};
        info.serializeForClusterFunctionProtocol(write_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_EXCLUDED_ROWS, DBMS_ANTALYA_PROTOCOL_VERSION);
    }

    /// Deserialize at protocol version 5 — the new fields should be absent
    ReadBufferFromMemory read_buffer(str.data(), str.size());
    Iceberg::IcebergObjectSerializableInfo deserialized;
    deserialized.deserializeForClusterFunctionProtocol(read_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_EXCLUDED_ROWS);

    ASSERT_TRUE(read_buffer.eof());
    ASSERT_EQ(deserialized.data_object_file_path_key, info.data_object_file_path_key);
    ASSERT_FALSE(deserialized.record_count.has_value());
    ASSERT_FALSE(deserialized.file_size_in_bytes.has_value());
}


TEST(DatalakeStateSerde, IcebergObjectSerializableInfoNulloptFileStats)
{
    /// Test round-trip when the fields are explicitly nullopt
    Iceberg::IcebergObjectSerializableInfo info;
    info.data_object_file_path_key = DB::Iceberg::IcebergPathFromMetadata::deserialize("s3://bucket/path/to/file.parquet");
    info.underlying_format_read_schema_id = 1;
    info.schema_id_relevant_to_iterator = 1;
    info.sequence_number = 0;
    info.file_format = "PARQUET";
    info.record_count = std::nullopt;
    info.file_size_in_bytes = std::nullopt;

    String str;
    {
        WriteBufferFromString write_buffer{str};
        info.serializeForClusterFunctionProtocol(write_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION, DBMS_ANTALYA_PROTOCOL_VERSION);
    }
    ReadBufferFromMemory read_buffer(str.data(), str.size());
    Iceberg::IcebergObjectSerializableInfo deserialized;
    deserialized.deserializeForClusterFunctionProtocol(read_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION);

    ASSERT_TRUE(read_buffer.eof());
    ASSERT_FALSE(deserialized.record_count.has_value());
    ASSERT_FALSE(deserialized.file_size_in_bytes.has_value());
}


TEST(DatalakeStateSerde, IcebergObjectSerializableInfoExternalPathRejectedBeforeAbsolutePathProtocol)
{
    auto info = makeObjectInfo();
    info.requires_external_storage = true;
    for (UInt64 antalya_version : {0, 1})
    {
        SCOPED_TRACE(antalya_version);
        String str;
        WriteBufferFromString write_buffer{str};
        try
        {
            info.serializeForClusterFunctionProtocol(write_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION, antalya_version);
            FAIL() << "External storage accepted by the old protocol";
        }
        catch (const DB::Exception & e)
        {
            ASSERT_EQ(e.code(), ErrorCodes::PROTOCOL_VERSION_MISMATCH);
        }
    }
}


TEST(DatalakeStateSerde, IcebergObjectSerializableInfoDeleteFilePathsByAntalyaVersion)
{
    auto info = makeObjectInfo();
    info.position_deletes_objects = {{"s3://bucket/deletes/pos1.parquet", "PARQUET", "s3://bucket/path/to/file.parquet", 0}};
    info.equality_deletes_objects = {{"s3://bucket/deletes/eq1.parquet", "PARQUET", std::vector<Int32>{1}, 1}};

    for (UInt64 antalya_version : {0, 1, 2})
    {
        SCOPED_TRACE(antalya_version);
        info.requires_external_storage = (antalya_version == 2);
        String str;
        {
            WriteBufferFromString write_buffer{str};
            info.serializeForClusterFunctionProtocol(write_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION, antalya_version);
        }
        ReadBufferFromMemory read_buffer(str.data(), str.size());
        Iceberg::IcebergObjectSerializableInfo deserialized;
        deserialized.deserializeForClusterFunctionProtocol(read_buffer, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION);

        ASSERT_TRUE(read_buffer.eof());
        ASSERT_EQ(deserialized.data_object_file_path_key, info.data_object_file_path_key);
        ASSERT_EQ(deserialized.position_deletes_objects.size(), 1);
        ASSERT_EQ(deserialized.equality_deletes_objects.size(), 1);
        const String prefix = antalya_version == 2 ? "s3://bucket/" : "";
        EXPECT_EQ(deserialized.position_deletes_objects[0].file_path, prefix + "deletes/pos1.parquet");
        EXPECT_EQ(deserialized.position_deletes_objects[0].reference_data_file_path, prefix + "path/to/file.parquet");
        EXPECT_EQ(deserialized.equality_deletes_objects[0].file_path, prefix + "deletes/eq1.parquet");
    }
}

#if USE_AVRO && USE_AWS_S3
TEST(DatalakeStateSerde, CopiedTableDeletePathsRequireAbsolutePathProtocol)
{
    const auto storage = std::make_shared<LocalObjectStorage>(LocalObjectStorageSettings("test", "./", true));
    ExternalStorageCache external_storages;
    const String location = "s3://old-bucket/prefix/table";
    const String path = location + "/data/delete.parquet";
    const Iceberg::IcebergPathResolver resolver(location, "table");

    const auto [resolved_storage, key] = resolveObjectStorageForPath(location, path, storage, external_storages, {}, resolver);
    ASSERT_EQ(resolved_storage, storage);
    ASSERT_EQ(key, "table/data/delete.parquet");
    EXPECT_TRUE(requiresAbsolutePathForCluster(location, path, storage, external_storages, {}, resolver));
    EXPECT_FALSE(requiresAbsolutePathForCluster(location, key, storage, external_storages, {}, resolver));

    const Iceberg::IcebergPathResolver original_resolver(location, "prefix/table");
    EXPECT_FALSE(requiresAbsolutePathForCluster(location, path, storage, external_storages, {}, original_resolver));
}
#endif
