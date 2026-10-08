#pragma once
#include <Core/Types.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeObjectMetadata.h>
#include <Processors/Formats/IInputFormat.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataObjectInfo.h>
#include <Storages/ObjectStorage/IObjectIterator.h>


namespace DB
{
class ReadBuffer;
class WriteBuffer;

/// A response send from initiator in Cluster functions (S3Cluster, etc)
struct ClusterFunctionReadTaskResponse
{
    ClusterFunctionReadTaskResponse() = default;
    explicit ClusterFunctionReadTaskResponse(const std::string & path_);
    explicit ClusterFunctionReadTaskResponse(ObjectInfoPtr object, const ContextPtr & context);

    /// Data path (object path, in case of object storage).
    String path;
    FileBucketInfoPtr file_bucket_info;
    /// Object metadata path, in case of data lake object.
    DataLakeObjectMetadata data_lake_metadata;
    /// Iceberg object metadata
    std::optional<Iceberg::IcebergObjectSerializableInfo> iceberg_info;
    /// File's columns info
    std::optional<DataFileMetaInfoPtr> file_meta_info;

    /// Convert received response into ObjectInfo.
    ObjectInfoPtr getObjectInfo() const;

    /// Whether response is empty.
    /// It is used to identify an end of processing.
    bool isEmpty() const { return path.empty(); }

    /// Serialize according to the cluster-processing protocol version.
    /// `antalya_protocol_version` is the negotiated Antalya version of this hop (`0` for an upstream peer).
    void serialize(WriteBuffer & out, size_t worker_protocol_version, size_t antalya_protocol_version = 0) const;
    /// Deserialize. The cluster-processing protocol version is read from `in`.
    /// `antalya_protocol_version` must be the same negotiated value `serialize` was given.
    void deserialize(ReadBuffer & in, size_t antalya_protocol_version = 0);
};

using ClusterFunctionReadTaskResponsePtr = std::shared_ptr<ClusterFunctionReadTaskResponse>;
using ClusterFunctionReadTaskCallback = std::function<ClusterFunctionReadTaskResponsePtr()>;

}
