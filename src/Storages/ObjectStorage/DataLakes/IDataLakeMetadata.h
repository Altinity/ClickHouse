#pragma once
#include <boost/noncopyable.hpp>

#include <Core/NamesAndTypes.h>
#include <Core/Types.h>
#include <Core/Range.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/ISimpleTransform.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/AlterCommands.h>
#include <Storages/MutationCommands.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Formats/FormatFilterInfo.h>
#include <Formats/FormatParserSharedResources.h>
#include <Storages/MutationCommands.h>
#include <Interpreters/StorageID.h>
#include <Databases/DataLake/ICatalog.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/AlterCommands.h>

#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>

namespace DataLake
{
class ICatalog;
}

namespace DB
{

namespace ErrorCodes
{
extern const int UNSUPPORTED_METHOD;
};

namespace Iceberg
{
struct ColumnInfo;
};

class DataFileMetaInfo
{
public:
    DataFileMetaInfo() = default;

    // subset of Iceberg::ColumnInfo now
    struct ColumnInfo
    {
        std::optional<Int64> rows_count;
        std::optional<Int64> nulls_count;
        std::optional<DB::Range> hyperrectangle;
    };

    // Extract metadata from Iceberg structure
    explicit DataFileMetaInfo(
        const Iceberg::IcebergSchemaProcessor & schema_processor,
        Int32 schema_id,
        const std::unordered_map<Int32, Iceberg::ColumnInfo> & columns_info_);

    void serialize(WriteBuffer & out) const;
    static DataFileMetaInfo deserialize(ReadBuffer & in);

    bool empty() const { return columns_info.empty(); }

    std::unordered_map<std::string, ColumnInfo> columns_info;
};

using DataFileMetaInfoPtr = std::shared_ptr<DataFileMetaInfo>;

struct DataFileInfo
{
    std::string file_path;
    std::optional<DataFileMetaInfoPtr> file_meta_info;
    std::optional<std::string> absolute_uri;

    explicit DataFileInfo(const std::string & file_path_)
        : file_path(file_path_) {}

    explicit DataFileInfo(std::string && file_path_)
        : file_path(std::move(file_path_)) {}

    bool operator==(const DataFileInfo & rhs) const
    {
        return file_path == rhs.file_path;
    }
};

class SinkToStorage;
using SinkToStoragePtr = std::shared_ptr<SinkToStorage>;
class StorageObjectStorageConfiguration;
using StorageObjectStorageConfigurationPtr = std::shared_ptr<StorageObjectStorageConfiguration>;
struct StorageID;
struct IObjectIterator;
struct RelativePathWithMetadata;
class IObjectStorage;
using ObjectIterator = std::shared_ptr<IObjectIterator>;
using ObjectStoragePtr = std::shared_ptr<IObjectStorage>;
using ObjectInfoPtr = std::shared_ptr<PathWithMetadata>;

class IDataLakeMetadata : boost::noncopyable
{
public:
    virtual ~IDataLakeMetadata() = default;

    virtual bool operator==(const IDataLakeMetadata & other) const = 0;

    /// Return iterator to `data files`.
    using FileProgressCallback = std::function<void(FileProgress)>;
    virtual ObjectIterator iterate(
        const ActionsDAG * /* filter_dag */,
        FileProgressCallback /* callback */,
        size_t /* list_batch_size */,
        ContextPtr context) const = 0;

    /// Table schema from data lake metadata.
    virtual NamesAndTypesList getTableSchema() const = 0;
    /// Read schema is the schema of actual data files,
    /// which can differ from table schema from data lake metadata.
    /// Return nothing if read schema is the same as table schema.
    virtual ReadFromFormatInfo prepareReadingFromFormat(
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context,
        bool supports_subset_of_columns,
        bool supports_tuple_elements);

    virtual std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(ContextPtr, ObjectInfoPtr) const { return {}; }
    virtual std::shared_ptr<const ActionsDAG> getSchemaTransformer(ContextPtr, ObjectInfoPtr) const { return {}; }

    /// Whether metadata is updateable (instead of recreation from scratch)
    /// to the latest version of table state in data lake.
    virtual bool supportsUpdate() const { return false; }
    /// Update metadata to the latest version.
    virtual bool update(const ContextPtr &) { return false; }

    virtual bool supportsSchemaEvolution() const { return false; }
    virtual bool supportsWrites() const { return false; }

    virtual void modifyFormatSettings(FormatSettings &, const Context &) const {}

    static constexpr bool supportsTotalRows() { return false; }
    virtual std::optional<size_t> totalRows(ContextPtr) const { return {}; }
    static constexpr bool supportsTotalBytes() { return false; }
    virtual std::optional<size_t> totalBytes(ContextPtr) const { return {}; }

    /// Some data lakes specify information for reading files from disks.
    /// For example, Iceberg has Parquet schema field ids in its metadata for reading files.
    virtual ColumnMapperPtr getColumnMapperForObject(ObjectInfoPtr /**/) const { return nullptr; }
    virtual ColumnMapperPtr getColumnMapperForCurrentSchema() const { return nullptr; }

    virtual SinkToStoragePtr write(
        SharedHeader /*sample_block*/,
        const StorageID & /*table_id*/,
        ObjectStoragePtr /*object_storage*/,
        StorageObjectStorageConfigurationPtr /*configuration*/,
        const std::optional<FormatSettings> & /*format_settings*/,
        ContextPtr /*context*/,
        std::shared_ptr<DataLake::ICatalog> /*catalog*/) { throwNotImplemented("write"); }

    virtual bool optimize(const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr /*context*/, const std::optional<FormatSettings> & /*format_settings*/)
    {
        return false;
    }

    virtual bool supportsDelete() const { return false; }
    virtual void mutate(const MutationCommands & /*commands*/,
        ContextPtr /*context*/,
        const StorageID & /*storage_id*/,
        StorageMetadataPtr /*metadata_snapshot*/,
        std::shared_ptr<DataLake::ICatalog> /*catalog*/,
        const std::optional<FormatSettings> & /*format_settings*/) { throwNotImplemented("mutations"); }

    virtual void checkMutationIsPossible(const MutationCommands & /*commands*/) { throwNotImplemented("mutations"); }

    virtual void addDeleteTransformers(ObjectInfoPtr, QueryPipelineBuilder &, const std::optional<FormatSettings> &, ContextPtr) const {}
    virtual void checkAlterIsPossible(const AlterCommands & /*commands*/) { throwNotImplemented("alter"); }
    virtual void alter(const AlterCommands & /*params*/, ContextPtr /*context*/) { throwNotImplemented("alter"); }

    virtual std::optional<String> partitionKey(ContextPtr) const { return {}; }
    virtual std::optional<String> sortingKey(ContextPtr) const { return {}; }

protected:
    virtual ObjectIterator createKeysIterator(
        Strings && data_files_,
        ObjectStoragePtr object_storage_,
        IDataLakeMetadata::FileProgressCallback callback_) const;

    ObjectIterator createKeysIterator(
        Strings && data_files_,
        ObjectStoragePtr object_storage_,
        IDataLakeMetadata::FileProgressCallback callback_,
        UInt64 snapshot_version_) const;

    [[noreturn]] void throwNotImplemented(std::string_view method) const
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Method `{}` is not implemented", method);
    }
};

using DataLakeMetadataPtr = std::unique_ptr<IDataLakeMetadata>;

}
