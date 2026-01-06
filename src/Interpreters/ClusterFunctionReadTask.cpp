#include <Interpreters/ClusterFunctionReadTask.h>
#include <Interpreters/SetSerialization.h>
#include <Interpreters/Context.h>
#include <Core/Settings.h>
#include <Core/ProtocolDefines.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <Interpreters/ActionsDAG.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Formats/FormatFactory.h>
#include <Processors/Formats/Impl/ParquetBlockInputFormat.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_PROTOCOL;
    extern const int LOGICAL_ERROR;
}
namespace Setting
{
    extern const SettingsBool cluster_function_process_archive_on_multiple_nodes;
    extern const SettingsBool allow_experimental_iceberg_read_optimization;
}

ClusterFunctionReadTaskResponse::ClusterFunctionReadTaskResponse(ObjectInfoPtr object, const ContextPtr & context)
    : iceberg_read_optimization_enabled(context->getSettingsRef()[Setting::allow_experimental_iceberg_read_optimization])
{
    if (!object)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "`object` cannot be null");

    if (object->data_lake_metadata.has_value())
        data_lake_metadata = object->data_lake_metadata.value();

    file_meta_info = object->file_meta_info;

    if (object->getCommand().isValid())
        path = object->getCommand().toString();
    else
    {
        const bool send_over_whole_archive = !context->getSettingsRef()[Setting::cluster_function_process_archive_on_multiple_nodes];
        path = send_over_whole_archive ? object->getPathOrPathToArchiveIfArchive() : object->getPath();
        absolute_path = object->getAbsolutePath();
        file_bucket_info = object->file_bucket_info;
    }
}

ClusterFunctionReadTaskResponse::ClusterFunctionReadTaskResponse(const std::string & path_)
    : path(path_)
{
}

ObjectInfoPtr ClusterFunctionReadTaskResponse::getObjectInfo() const
{
    if (isEmpty())
        return {};

    auto object = std::make_shared<ObjectInfo>(path);
    object->data_lake_metadata = data_lake_metadata;
    object->file_meta_info = file_meta_info;
    if (absolute_path.has_value() && !absolute_path.value().empty())
        object->absolute_path = absolute_path;

    object->file_bucket_info = file_bucket_info;

    return object;
}

void ClusterFunctionReadTaskResponse::serialize(WriteBuffer & out, size_t protocol_version) const
{
    writeVarUInt(protocol_version, out);
    writeStringBinary(path, out);

    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_DATA_LAKE_METADATA)
    {
        SerializedSetsRegistry registry;
        if (data_lake_metadata.transform)
            data_lake_metadata.transform->serialize(out, registry);
        else
            ActionsDAG().serialize(out, registry);
    }

    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_FILE_BUCKETS_INFO)
    {
        if (file_bucket_info)
        {
            /// Write format name so we can create appropriate file bucket info during deserialization.
            writeStringBinary(file_bucket_info->getFormatName(), out);
            file_bucket_info->serialize(out);
        }
        else
        {
            /// Write empty string as format name if file_bucket_info is not set.
            writeStringBinary("", out);
        }
    }

    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_DATA_LAKE_COLUMNS_METADATA)
    {
        /// This info is not used when optimization is disabled, so there is no need to send it.
        if (iceberg_read_optimization_enabled && file_meta_info.has_value())
            file_meta_info.value()->serialize(out);
        else
            DataFileMetaInfo().serialize(out);
    }
}

void ClusterFunctionReadTaskResponse::deserialize(ReadBuffer & in)
{
    size_t protocol_version = 0;
    readVarUInt(protocol_version, in);
    if (protocol_version < DBMS_CLUSTER_INITIAL_PROCESSING_PROTOCOL_VERSION
        || protocol_version > DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION)
    {
        throw Exception(
            ErrorCodes::UNKNOWN_PROTOCOL, "Supported protocol versions are in range [{}, {}], got: {}",
            DBMS_CLUSTER_INITIAL_PROCESSING_PROTOCOL_VERSION, DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION,
            protocol_version);
    }

    readStringBinary(path, in);
    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_DATA_LAKE_METADATA)
    {
        DeserializedSetsRegistry registry;
        auto transform = std::make_shared<ActionsDAG>(ActionsDAG::deserialize(in, registry, Context::getGlobalContextInstance()));

        if (!path.empty() && !transform->getInputs().empty())
        {
            data_lake_metadata.transform = std::move(transform);
        }
    }

    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_FILE_BUCKETS_INFO)
    {
        String format;
        readStringBinary(format, in);
        if (!format.empty())
        {
            file_bucket_info = FormatFactory::instance().getFileBucketInfo(format);
            file_bucket_info->deserialize(in);
        }
    }

    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_DATA_LAKE_COLUMNS_METADATA)
    {
        auto info = std::make_shared<DataFileMetaInfo>(DataFileMetaInfo::deserialize(in));

        if (!path.empty() && !info->empty())
            file_meta_info = info;
    }
}

}
