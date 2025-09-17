#pragma once
#include <Core/NamesAndTypes.h>
#include <Core/Types.h>
#include <Core/Range.h>
#include <boost/noncopyable.hpp>
#include <Interpreters/ActionsDAG.h>
#include <Storages/ObjectStorage/IObjectIterator.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Poco/JSON/Object.h>

namespace Iceberg
{

struct ColumnInfo;

};

namespace DB
{

namespace ErrorCodes
{
extern const int UNSUPPORTED_METHOD;
};

class DataFileMetaInfo
{
public:
    DataFileMetaInfo() = default;

    // Extract metadata from Iceberg structure
    explicit DataFileMetaInfo(const std::unordered_map<Int32, Iceberg::ColumnInfo> & columns_info_);

    // Deserialize from json in distributed requests
    explicit DataFileMetaInfo(const Poco::JSON::Object::Ptr file_info);

    // Serialize to json in distributed requests
    Poco::JSON::Object::Ptr toJson() const;

    struct ColumnInfo
    {
        std::optional<Int64> rows_count;
        std::optional<Int64> nulls_count;
        std::optional<DB::Range> hyperrectangle;
    };

    std::unordered_map<Int32, ColumnInfo> columns_info;
};

using DataFileMetaInfoPtr = std::shared_ptr<DataFileMetaInfo>;

struct DataFileInfo
{
    std::string file_path;
    std::optional<DataFileMetaInfoPtr> file_meta_info;

    explicit DataFileInfo(const std::string & file_path_)
        : file_path(file_path_) {}

    explicit DataFileInfo(std::string && file_path_)
        : file_path(std::move(file_path_)) {}

    bool operator==(const DataFileInfo & rhs) const
    {
        return file_path == rhs.file_path;
    }
};

using DataFileInfos = std::vector<DataFileInfo>;

class IDataLakeMetadata : boost::noncopyable
{
public:
    virtual ~IDataLakeMetadata() = default;

    virtual bool operator==(const IDataLakeMetadata & other) const = 0;

    /// List all data files.
    /// For better parallelization, iterate() method should be used.
    virtual DataFileInfos getDataFiles() const = 0;
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
    virtual DB::ReadFromFormatInfo prepareReadingFromFormat(
        const Strings & requested_columns,
        const DB::StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context,
        bool supports_subset_of_columns);

    virtual std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(ContextPtr, const String & /* path */) const { return {}; }
    virtual std::shared_ptr<const ActionsDAG> getSchemaTransformer(ContextPtr, const String & /* path */) const { return {}; }

    /// Whether metadata is updateable (instead of recreation from scratch)
    /// to the latest version of table state in data lake.
    virtual bool supportsUpdate() const { return false; }
    /// Update metadata to the latest version.
    virtual bool update(const ContextPtr &) { return false; }

    virtual bool supportsSchemaEvolution() const { return false; }
    virtual bool supportsWrites() const { return false; }

    virtual void modifyFormatSettings(FormatSettings &) const {}

    virtual std::optional<size_t> totalRows(ContextPtr) const { return {}; }
    virtual std::optional<size_t> totalBytes(ContextPtr) const { return {}; }

protected:
    ObjectIterator createKeysIterator(
        DataFileInfos && data_files_,
        ObjectStoragePtr object_storage_,
        IDataLakeMetadata::FileProgressCallback callback_) const;

    [[noreturn]] void throwNotImplemented(std::string_view method) const
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Method `{}` is not implemented", method);
    }
};

using DataLakeMetadataPtr = std::unique_ptr<IDataLakeMetadata>;

}
