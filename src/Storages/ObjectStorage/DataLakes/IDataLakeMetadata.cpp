#include "IDataLakeMetadata.h"
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>

#include <Common/logger_useful.h>

namespace DB
{

namespace
{

class KeysIterator : public IObjectIterator
{
public:
    KeysIterator(
        DataFileInfos && data_files_,
        ObjectStoragePtr object_storage_,
        IDataLakeMetadata::FileProgressCallback callback_)
        : data_files(data_files_)
        , object_storage(object_storage_)
        , callback(callback_)
    {
    }

    size_t estimatedKeysCount() override
    {
        return data_files.size();
    }

    ObjectInfoPtr next(size_t) override
    {
        while (true)
        {
            size_t current_index = index.fetch_add(1, std::memory_order_relaxed);
            if (current_index >= data_files.size())
                return nullptr;

            auto key = data_files[current_index].file_path;

            if (callback)
            {
                /// Too expencive to load size for metadata always
                /// because it requires API call to external storage.
                /// In many cases only keys are needed.
                callback(FileProgress(0, 1));
            }

            auto result = std::make_shared<ObjectInfo>(key, std::nullopt);
            result->setFileMetaInfo(data_files[current_index].file_meta_info);
            return result;
        }
    }

private:
    DataFileInfos data_files;
    ObjectStoragePtr object_storage;
    std::atomic<size_t> index = 0;
    IDataLakeMetadata::FileProgressCallback callback;
};

}

ObjectIterator IDataLakeMetadata::createKeysIterator(
    DataFileInfos && data_files_,
    ObjectStoragePtr object_storage_,
    IDataLakeMetadata::FileProgressCallback callback_) const
{
    return std::make_shared<KeysIterator>(std::move(data_files_), object_storage_, callback_);
}

DB::ReadFromFormatInfo IDataLakeMetadata::prepareReadingFromFormat(
    const Strings & requested_columns,
    const DB::StorageSnapshotPtr & storage_snapshot,
    const ContextPtr & context,
    bool supports_subset_of_columns)
{
    return DB::prepareReadingFromFormat(requested_columns, storage_snapshot, context, supports_subset_of_columns);
}

DataFileMetaInfo::DataFileMetaInfo(Poco::JSON::Object::Ptr file_info)
{
    if (!file_info)
        return;

    auto log = getLogger("DataFileMetaInfo");

    if (file_info->has("columns"))
    {
        auto columns = file_info->getArray("columns");
        for (size_t i = 0; i < columns->size(); ++i)
        {
            auto column = columns->getObject(static_cast<UInt32>(i));

            Int32 id;
            if (column->has("id"))
                id = column->get("id");
            else
            {
                LOG_WARNING(log, "Can't read column id, ignored");
                continue;
            }

            DB::DataFileMetaInfo::ColumnInfo column_info;
            if (column->has("rows"))
                column_info.rows_count = column->get("rows");
            if (column->has("nulls"))
                column_info.nulls_count = column->get("nulls");
            if (column->has("range"))
            {
                Range range("");
                std::string r = column->get("range");
                try
                {
                    range.deserialize(r);
                    column_info.hyperrectangle = std::move(range);
                }
                catch (const Exception & e)
                {
                    LOG_WARNING(log, "Can't read range for column {}, range '{}' ignored, error: {}", id, r, e.what());
                }
            }

            columns_info[id] = column_info;
        }
    }
}

DataFileMetaInfo::DataFileMetaInfo(const std::unordered_map<Int32, Iceberg::ColumnInfo> & columns_info_)
{
    for (const auto & column : columns_info_)
    {
        columns_info[column.first] = {column.second.rows_count, column.second.nulls_count, column.second.hyperrectangle};
    }
}

Poco::JSON::Object::Ptr DataFileMetaInfo::toJson() const
{
    Poco::JSON::Object::Ptr file_info = new Poco::JSON::Object();

    if (!columns_info.empty())
    {
        Poco::JSON::Array::Ptr columns = new Poco::JSON::Array();

        for (const auto & column : columns_info)
        {
            Poco::JSON::Object::Ptr column_info = new Poco::JSON::Object();
            column_info->set("id", column.first);
            if (column.second.rows_count.has_value())
                column_info->set("rows", column.second.rows_count.value());
            if (column.second.nulls_count.has_value())
                column_info->set("nulls", column.second.nulls_count.value());
            if (column.second.hyperrectangle.has_value())
                column_info->set("range", column.second.hyperrectangle.value().serialize());

            columns->add(column_info);
        }

        file_info->set("columns", columns);
    }

    return file_info;
}

}
