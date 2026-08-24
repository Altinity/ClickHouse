#include <Poco/String.h>
#include "config.h"

#include <Core/Field.h>
#include <Common/FieldVisitorToString.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Interpreters/Context_fwd.h>

#include <Storages/ObjectStorage/DataLakes/Iceberg/PositionDeleteTransform.h>

#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataObjectInfo.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Common/Exception.h>

#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Core/ProtocolDefines.h>

#if USE_PARQUET
#include <Processors/Formats/Impl/Parquet/ReadCommon.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>
#endif

namespace DB::ErrorCodes
{
extern const int NOT_IMPLEMENTED;
extern const int UNKNOWN_PROTOCOL;
extern const int PROTOCOL_VERSION_MISMATCH;
}


using namespace DB::Iceberg;

namespace DB::Iceberg
{

void requireParquetDataFileForRowDeletes(const String & file_format, std::string_view feature_name)
{
    if (Poco::toUpper(file_format) != "PARQUET")
    {
        throw Exception(
            DB::ErrorCodes::NOT_IMPLEMENTED,
            "{} are only supported for data files of Parquet format in Iceberg, but got {}",
            feature_name,
            file_format);
    }
}

}

namespace DB
{

namespace Setting
{
extern const SettingsBool use_roaring_bitmap_iceberg_positional_deletes;
};

namespace Iceberg
{
String computePartitionId(const Row & partition_key_value)
{
    if (partition_key_value.empty())
        return {};
    String result;
    for (const auto & val : partition_key_value)
    {
        if (!result.empty())
            result += '_';
        result += applyVisitor(FieldVisitorToString{}, val);
    }
    return result;
}
}

#if USE_AVRO

#if USE_PARQUET
/// Rough byte size of a decoded manifest bound value, used only to size the parquet footer read.
static size_t estimateBoundFieldBytes(const Field & f)
{
    if (f.getType() == Field::Types::String)
        return f.safeGet<String>().size();
    return 16; /// conservative for numeric / decimal / date / uuid bounds
}
#endif

IcebergDataObjectInfo::IcebergDataObjectInfo(
    Iceberg::ProcessedManifestFileEntryPtr data_manifest_file_entry_, const String & metadata_path_, Int32 schema_id_relevant_to_iterator_, ObjectStoragePtr resolved_storage_, const String & resolved_key_)
    : ObjectInfo(RelativePathWithMetadata(resolved_key_.empty() ? metadata_path_ : resolved_key_))
    , info{
          data_manifest_file_entry_->parsed_entry->file_path_key,
          metadata_path_,
          data_manifest_file_entry_->resolved_schema_id,
          schema_id_relevant_to_iterator_,
          data_manifest_file_entry_->sequence_number,
          data_manifest_file_entry_->parsed_entry->file_format,
          /* manifest_file */ data_manifest_file_entry_->manifest_file_path,
          /* partition_id */ Iceberg::computePartitionId(data_manifest_file_entry_->parsed_entry->partition_key_value),
          /* position_deletes_objects */ {},
          /* equality_deletes_objects */ {},
          data_manifest_file_entry_->parsed_entry->record_count,
          data_manifest_file_entry_->parsed_entry->file_size_in_bytes}
    , resolved_storage(std::move(resolved_storage_))
{
    /// resolved_storage and resolved_key must be provided together or neither must be provided
    /// (default-constructed, meaning the path has not been resolved yet).
    chassert(resolved_key_.empty() == (resolved_storage == nullptr));

    /// Note: object identity (size / etag / multipart part offsets) is intentionally NOT pre-populated
    /// from the manifest here. It is fetched once via GetObjectAttributes and cached by the
    /// object-storage identity cache (see S3ObjectStorage::getObjectMetadata / ObjectStorageIdentityCache),
    /// so all object reads - Iceberg data files included - go through one path that yields the real
    /// ETag (needed by the filesystem / page / parquet-metadata caches) and the multipart layout
    /// (needed for read alignment). Pre-populating metadata here would skip that path and lose both.
#if USE_PARQUET
    /// Precompute a footer-size hint from the manifest stats so the parquet reader can fetch the
    /// FileMetaData in a single tail read (see Parquet::estimateParquetFooterSize). Parquet only;
    /// the hint is ignored by other formats. The row-group count comes from the manifest's
    /// split_offsets (one per row group) when present - accurate even for byte-split row groups -
    /// and falls back to a record_count/1M guess otherwise. Only affects read count, never correctness.
    const auto & entry = *data_manifest_file_entry_->parsed_entry;
    if (entry.file_format == "PARQUET")
    {
        size_t num_columns = std::max(entry.columns_infos.size(), entry.value_bounds.size());
        if (num_columns > 0)
        {
            constexpr size_t rows_per_row_group_guess = 1'000'000;
            size_t rows = size_t(std::max<Int64>(entry.record_count, 0));
            size_t num_row_groups = !entry.split_offsets.empty()
                ? entry.split_offsets.size()
                : std::max<size_t>(1, (rows + rows_per_row_group_guess - 1) / rows_per_row_group_guess);
            size_t bounds_bytes = 0;
            for (const auto & [field_id, bounds] : entry.value_bounds)
                bounds_bytes += estimateBoundFieldBytes(bounds.first) + estimateBoundFieldBytes(bounds.second);
            relative_path_with_metadata.footer_size_hint
                = Parquet::estimateParquetFooterSize(num_columns, num_row_groups, bounds_bytes);
        }
    }
#endif
}

IcebergDataObjectInfo::IcebergDataObjectInfo(const RelativePathWithMetadata & path_)
    : ObjectInfo(path_)
{
}

IcebergDataObjectInfo::IcebergDataObjectInfo(const RelativePathWithMetadata & path_, const Iceberg::IcebergObjectSerializableInfo & info_)
    : ObjectInfo(path_)
    , info(info_)
{
}

std::shared_ptr<ISimpleTransform> IcebergDataObjectInfo::getPositionDeleteTransformer(
    ObjectStoragePtr object_storage,
    const SharedHeader & header,
    const std::optional<FormatSettings> & format_settings,
    FormatParserSharedResourcesPtr parser_shared_resources,
    ContextPtr context_,
    const Iceberg::IcebergPathResolver & path_resolver,
    std::shared_ptr<SecondaryStorages> secondary_storages)
{
    IcebergDataObjectInfoPtr self = shared_from_this();
    if (!context_->getSettingsRef()[Setting::use_roaring_bitmap_iceberg_positional_deletes].value)
        return std::make_shared<IcebergStreamingPositionDeleteTransform>(header, self, object_storage, format_settings, parser_shared_resources, context_, path_resolver, secondary_storages);
    else
        return std::make_shared<IcebergBitmapPositionDeleteTransform>(header, self, object_storage, format_settings, parser_shared_resources, context_, path_resolver, secondary_storages);
}

void IcebergDataObjectInfo::addPositionDeleteObject(Iceberg::ProcessedManifestFileEntryPtr position_delete_object, const String & resolved_storage_path)
{
    Iceberg::requireParquetDataFileForRowDeletes(info.file_format, "Position deletes");
    info.position_deletes_objects.emplace_back(
        resolved_storage_path, position_delete_object->parsed_entry->file_format, std::nullopt,
        position_delete_object->sequence_number);
}

void IcebergDataObjectInfo::addEqualityDeleteObject(const Iceberg::ProcessedManifestFileEntryPtr & equality_delete_object, const String & resolved_storage_path)
{
    info.equality_deletes_objects.emplace_back(
        resolved_storage_path,
        equality_delete_object->parsed_entry->file_format,
        equality_delete_object->parsed_entry->equality_ids,
        equality_delete_object->resolved_schema_id);
}

bool hasIcebergEqualityDeletes(const ObjectInfoPtr & object_info)
{
    const auto * iceberg = dynamic_cast<const IcebergDataObjectInfo *>(object_info.get());
    return iceberg && !iceberg->info.equality_deletes_objects.empty();
}

bool hasIcebergPositionDeletes(const ObjectInfoPtr & object_info)
{
    const auto * iceberg = dynamic_cast<const IcebergDataObjectInfo *>(object_info.get());
    return iceberg && !iceberg->info.position_deletes_objects.empty();
}

#endif

void IcebergObjectSerializableInfo::serializeForClusterFunctionProtocol(WriteBuffer & out, size_t protocol_version) const
{
    checkVersion(protocol_version);

    if (requires_external_storage && protocol_version < DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH)
    {
        throw Exception(
            ErrorCodes::PROTOCOL_VERSION_MISMATCH,
            "Iceberg data file '{}' is outside of the table location, "
            "worker needs to have protocol version >= {}, but has {}. ",
            data_object_file_metadata_path,
            DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH,
            protocol_version);
    }

    auto path_for_protocol = [&](const String & path) -> String
    {
        if (protocol_version < DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH)
            return SchemeAuthorityKey(path).key;
        return path;
    };

    writeStringBinary(data_object_file_path_key.serialize(), out);
    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH)
    {
        writeStringBinary(data_object_file_metadata_path, out);
    }
    writeVarInt(underlying_format_read_schema_id, out);
    writeVarInt(schema_id_relevant_to_iterator, out);
    writeVarInt(sequence_number, out);
    writeStringBinary(file_format, out);
    {
        writeVarUInt(position_deletes_objects.size(), out);
        for (const auto & pos_delete_obj : position_deletes_objects)
        {
            writeStringBinary(path_for_protocol(pos_delete_obj.file_path), out);
            writeStringBinary(pos_delete_obj.file_format, out);
            if (pos_delete_obj.reference_data_file_path.has_value())
            {
                writeVarUInt(1, out);
                writeStringBinary(path_for_protocol(pos_delete_obj.reference_data_file_path.value()), out);
            }
            else
            {
                writeVarUInt(0, out);
            }
        }
    }
    {
        writeVarUInt(equality_deletes_objects.size(), out);
        for (const auto & eq_delete_obj : equality_deletes_objects)
        {
            writeStringBinary(path_for_protocol(eq_delete_obj.file_path), out);
            writeStringBinary(eq_delete_obj.file_format, out);
            writeVarInt(eq_delete_obj.schema_id, out);
            if (eq_delete_obj.equality_ids.has_value())
            {
                writeVarUInt(1, out);
                writeVarUInt(eq_delete_obj.equality_ids->size(), out);
                for (const auto & equality_id : *eq_delete_obj.equality_ids)
                {
                    writeVarInt(equality_id, out);
                }
            }
            else
            {
                writeVarUInt(0, out);
            }
        }
    }
    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_FILE_STATS)
    {
        if (record_count.has_value())
        {
            writeVarUInt(1, out);
            writeVarInt(*record_count, out);
        }
        else
        {
            writeVarUInt(0, out);
        }
        if (file_size_in_bytes.has_value())
        {
            writeVarUInt(1, out);
            writeVarInt(*file_size_in_bytes, out);
        }
        else
        {
            writeVarUInt(0, out);
        }
    }
}

void IcebergObjectSerializableInfo::deserializeForClusterFunctionProtocol(ReadBuffer & in, size_t protocol_version)
{
    checkVersion(protocol_version);
    {
        String raw_path;
        readStringBinary(raw_path, in);
        data_object_file_path_key = IcebergPathFromMetadata::deserialize(std::move(raw_path));
    }
    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH)
    {
        readStringBinary(data_object_file_metadata_path, in);
    }
    readVarInt(underlying_format_read_schema_id, in);
    readVarInt(schema_id_relevant_to_iterator, in);
    readVarInt(sequence_number, in);
    readStringBinary(file_format, in);

    {
        size_t pos_delete_obj_size = 0;
        readVarUInt(pos_delete_obj_size, in);
        position_deletes_objects.resize(pos_delete_obj_size);
        for (size_t i = 0; i < pos_delete_obj_size; ++i)
        {
            Iceberg::PositionDeleteObject & pos_delete_obj = position_deletes_objects[i];
            readStringBinary(pos_delete_obj.file_path, in);
            readStringBinary(pos_delete_obj.file_format, in);
            size_t has_reference_path = 0;
            readVarUInt(has_reference_path, in);
            if (has_reference_path)
            {
                String reference_path;
                readStringBinary(reference_path, in);
                pos_delete_obj.reference_data_file_path = reference_path;
            }
        }
    }
    {
        size_t eq_delete_obj_size = 0;
        readVarUInt(eq_delete_obj_size, in);
        equality_deletes_objects.resize(eq_delete_obj_size);
        for (size_t i = 0; i < eq_delete_obj_size; ++i)
        {
            Iceberg::EqualityDeleteObject & eq_delete_obj = equality_deletes_objects[i];
            readStringBinary(eq_delete_obj.file_path, in);
            readStringBinary(eq_delete_obj.file_format, in);
            readVarInt(eq_delete_obj.schema_id, in);
            size_t has_equality_ids = 0;
            readVarUInt(has_equality_ids, in);
            if (has_equality_ids)
            {
                size_t equality_ids_size = 0;
                readVarUInt(equality_ids_size, in);
                eq_delete_obj.equality_ids = std::vector<Int32>{};
                for (size_t j = 0; j < equality_ids_size; ++j)
                {
                    Int32 equality_id = 0;
                    readVarInt(equality_id, in);
                    eq_delete_obj.equality_ids->push_back(equality_id);
                }
            }
        }
    }
    if (protocol_version >= DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_FILE_STATS)
    {
        size_t has_record_count = 0;
        readVarUInt(has_record_count, in);
        if (has_record_count)
        {
            Int64 value = 0;
            readVarInt(value, in);
            record_count = value;
        }
        else
        {
            record_count = std::nullopt;
        }
        size_t has_file_size = 0;
        readVarUInt(has_file_size, in);
        if (has_file_size)
        {
            Int64 value = 0;
            readVarInt(value, in);
            file_size_in_bytes = value;
        }
        else
        {
            file_size_in_bytes = std::nullopt;
        }
    }
}

void IcebergObjectSerializableInfo::checkVersion(size_t protocol_version) const
{
    if (protocol_version < DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_METADATA)
    {
        throw Exception(
            ErrorCodes::UNKNOWN_PROTOCOL,
            "IcebergObjectSerializableInfo serialization is supported since protocol version {}, got: {}",
            DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION_WITH_ICEBERG_METADATA,
            protocol_version);
    }
}
}
