#pragma once


#include <Interpreters/Context_fwd.h>
#include "config.h"

#include <map>
#include <string>
#include <string_view>

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#if USE_AVRO

#include <Storages/ColumnsDescription.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Snapshot.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <IO/CompressedReadBufferWrapper.h>
#include <IO/CompressionMethod.h>

namespace DB::Iceberg
{

void writeMessageToFile(
    const String & data,
    const String & filename,
    DB::ObjectStoragePtr object_storage,
    DB::ContextPtr context,
    std::function<void()> cleanup,
    DB::CompressionMethod compression_method = DB::CompressionMethod::None);

struct TransformAndArgument
{
    String transform_name;
    std::optional<size_t> argument;
    /// When Iceberg table is partitioned by time, splitting by partitions can be made using different timezone
    /// (UTC in most cases). This timezone can be set with setting `iceberg_partition_timezone`, value is in this member.
    /// When Iceberg partition condition converted to ClickHouse function in `parseTransformAndArgument` method
    /// `time_zone` added as second argument to functions like `toRelativeDayNum`, `toYearNumSinceEpoch`, etc.
    std::optional<String> time_zone;
};

std::optional<TransformAndArgument> parseTransformAndArgument(const String & transform_name_src, const String & time_zone);

Poco::JSON::Object::Ptr getMetadataJSONObject(
    const String & metadata_file_path,
    ObjectStoragePtr object_storage,
    StorageObjectStorageConfigurationPtr configuration_ptr,
    IcebergMetadataFilesCachePtr cache_ptr,
    const ContextPtr & local_context,
    LoggerPtr log,
    CompressionMethod compression_method);

struct MetadataFileWithInfo
{
    Int32 version;
    String path;
    CompressionMethod compression_method;
};

std::pair<Poco::Dynamic::Var, bool> getIcebergType(DataTypePtr type, Int32 & iter);

/// Spec: https://iceberg.apache.org/spec/?h=metadata.json#table-metadata-fields
std::pair<Poco::JSON::Object::Ptr, String> createEmptyMetadataFile(
    String path_location,
    const ColumnsDescription & columns,
    ASTPtr partition_by,
    UInt64 format_version = 2);

MetadataFileWithInfo getLatestOrExplicitMetadataFileAndVersion(
    const ObjectStoragePtr & object_storage,
    StorageObjectStorageConfigurationPtr configuration_ptr,
    IcebergMetadataFilesCachePtr cache_ptr,
    const ContextPtr & local_context,
    Poco::Logger * log);

}

#endif
