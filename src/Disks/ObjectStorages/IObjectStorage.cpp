#include <Disks/IO/ThreadPoolRemoteFSReader.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <Disks/ObjectStorages/ObjectStorageIterator.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/copyData.h>
#include <Interpreters/Context.h>
#include <Common/Exception.h>
#include <Common/ObjectStorageKeyGenerator.h>

/// TODO: move DataFileInfo into separate file
#include <Storages/ObjectStorage/DataLakes/IDataLakeMetadata.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/JSONException.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}

const MetadataStorageMetrics & IObjectStorage::getMetadataStorageMetrics() const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method 'getMetadataStorageMetrics' is not implemented");
}

bool IObjectStorage::existsOrHasAnyChild(const std::string & path) const
{
    RelativePathsWithMetadata files;
    listObjects(path, files, 1);
    return !files.empty();
}

void IObjectStorage::listObjects(const std::string &, RelativePathsWithMetadata &, size_t) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "listObjects() is not supported");
}


ObjectStorageIteratorPtr IObjectStorage::iterate(const std::string & path_prefix, size_t max_keys) const
{
    RelativePathsWithMetadata files;
    listObjects(path_prefix, files, max_keys);

    return std::make_shared<ObjectStorageIteratorFromList>(std::move(files));
}

std::optional<ObjectMetadata> IObjectStorage::tryGetObjectMetadata(const std::string & path) const
{
    try
    {
        return getObjectMetadata(path);
    }
    catch (...)
    {
        return {};
    }
}

ThreadPool & IObjectStorage::getThreadPoolWriter()
{
    auto context = Context::getGlobalContextInstance();
    if (!context)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Global context not initialized");

    return context->getThreadPoolWriter();
}

void IObjectStorage::copyObjectToAnotherObjectStorage( // NOLINT
    const StoredObject & object_from,
    const StoredObject & object_to,
    const ReadSettings & read_settings,
    const WriteSettings & write_settings,
    IObjectStorage & object_storage_to,
    std::optional<ObjectAttributes> object_to_attributes)
{
    if (&object_storage_to == this)
        copyObject(object_from, object_to, read_settings, write_settings, object_to_attributes);

    auto in = readObject(object_from, read_settings);
    auto out = object_storage_to.writeObject(object_to, WriteMode::Rewrite, /* attributes= */ {}, /* buf_size= */ DBMS_DEFAULT_BUFFER_SIZE, write_settings);
    copyData(*in, *out);
    out->finalize();
}

const std::string & IObjectStorage::getCacheName() const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "getCacheName is not implemented for object storage");
}

ReadSettings IObjectStorage::patchSettings(const ReadSettings & read_settings) const
{
    return read_settings;
}

WriteSettings IObjectStorage::patchSettings(const WriteSettings & write_settings) const
{
    return write_settings;
}

RelativePathWithMetadata::RelativePathWithMetadata(const String & task_string, std::optional<ObjectMetadata> metadata_)
    : metadata(std::move(metadata_))
    , command(task_string)
{
    if (!command.isParsed())
        relative_path = task_string;
    else
    {
        auto file_path = command.getFilePath();
        if (file_path.has_value())
            relative_path = file_path.value();
        file_meta_info = command.getFileMetaInfo();
    }
}

RelativePathWithMetadata::RelativePathWithMetadata(const DataFileInfo & info, std::optional<ObjectMetadata> metadata_)
    : metadata(std::move(metadata_))
{
    relative_path = info.file_path;
    file_meta_info = info.file_meta_info;
}

void RelativePathWithMetadata::loadMetadata(ObjectStoragePtr object_storage, bool ignore_non_existent_file)
{
    if (!metadata)
    {
        const auto & path = isArchive() ? getPathToArchive() : getPath();

        if (ignore_non_existent_file)
        {
            metadata = object_storage->tryGetObjectMetadata(path);
        }
        else
        {
            metadata = object_storage->getObjectMetadata(path);
        }
    }
}

RelativePathWithMetadata::CommandInTaskResponse::CommandInTaskResponse(const std::string & task)
{
    Poco::JSON::Parser parser;
    try
    {
        auto json = parser.parse(task).extract<Poco::JSON::Object::Ptr>();
        if (!json)
            return;

        successfully_parsed = true;

        if (json->has("file_path"))
            file_path = json->getValue<std::string>("file_path");
        if (json->has("retry_after_us"))
            retry_after_us = json->getValue<size_t>("retry_after_us");
        if (json->has("meta_info"))
            file_meta_info = std::make_shared<DataFileMetaInfo>(json->getObject("meta_info"));
    }
    catch (const Poco::JSON::JSONException &)
    { /// Not a JSON
        return;
    }
}

std::string RelativePathWithMetadata::CommandInTaskResponse::toString() const
{
    Poco::JSON::Object json;

    if (file_path.has_value())
        json.set("file_path", file_path.value());
    if (retry_after_us.has_value())
        json.set("retry_after_us", retry_after_us.value());
    if (file_meta_info.has_value())
        json.set("meta_info", file_meta_info.value()->toJson());

    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);
    return oss.str();
}

}
