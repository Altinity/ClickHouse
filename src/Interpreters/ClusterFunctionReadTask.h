#pragma once
#include <Core/Types.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeObjectMetadata.h>
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
    std::optional<size_t> read_source_index;
    FileBucketInfoPtr file_bucket_info;
    /// Object metadata path, in case of data lake object.
    DataLakeObjectMetadata data_lake_metadata;
    /// Iceberg object metadata
    std::optional<Iceberg::IcebergObjectSerializableInfo> iceberg_info;
    /// File's columns info
    std::optional<DataFileMetaInfoPtr> file_meta_info;
    /// Microseconds the worker should wait before asking for another task.
    /// Set only when `path` is empty. Written when the negotiated Antalya version is at least
    /// `DBMS_ANTALYA_PROTOCOL_VERSION_WITH_READ_TASK_EXTENSIONS`.
    std::optional<UInt64> retry_after_us;

    /// Convert received response into ObjectInfo.
    ObjectInfoPtr getObjectInfo() const;

    /// Whether response is empty.
    /// It is used to identify an end of processing. A retry request has no path and is not the end.
    bool isEmpty() const { return path.empty() && !retry_after_us; }

    /// Serialize according to the protocol version.
    /// `worker_antalya_protocol_version` is the Antalya protocol version negotiated with the worker.
    void serialize(WriteBuffer & out, size_t worker_protocol_version, UInt64 worker_antalya_protocol_version) const;
    /// Deserialize. Protocol version will be received from `in`
    /// and the result will be deserialized accordingly.
    /// `antalya_protocol_version` is the version negotiated with the peer that wrote the packet.
    void deserialize(ReadBuffer & in, UInt64 antalya_protocol_version = 0);
};

using ClusterFunctionReadTaskResponsePtr = std::shared_ptr<ClusterFunctionReadTaskResponse>;
using ClusterFunctionReadTaskCallback = std::function<ClusterFunctionReadTaskResponsePtr()>;

}
