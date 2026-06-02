#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <filesystem>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}

ContentAddressedMetadataStorage::ContentAddressedMetadataStorage(ObjectStoragePtr object_storage_, String storage_path_prefix_, String server_id_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
{
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    return std::make_shared<ContentAddressedTransaction>(*this);
}

std::optional<std::string> ContentAddressedMetadataStorage::readSmallObjectIfExists(const std::string & key) const
{
    // TODO(phase3): honor object_storage->getCommonKeyPrefix() (bare keys for now).
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
        return std::nullopt;

    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

std::optional<std::string> ContentAddressedMetadataStorage::readRefPartId(const std::string & table_uuid, const std::string & part_name) const
{
    return readSmallObjectIfExists(ContentAddressed::refKey(server_id, table_uuid, part_name));
}

ContentAddressed::Footer ContentAddressedMetadataStorage::loadFooterOrThrow(const std::string & part_id) const
{
    auto bytes = readSmallObjectIfExists(ContentAddressed::partKey(part_id));
    if (!bytes)
        throw Exception(
            ErrorCodes::CORRUPTED_DATA,
            "ContentAddressed: live ref points at missing footer parts/{}",
            part_id);
    return ContentAddressed::Footer::deserialize(*bytes);
}

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        return false;
    auto footer = loadFooterOrThrow(*pid);
    return footer.blobs.contains(p->file);
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: existsDirectory implemented in Phase 2 Task 3/4");
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: existsFileOrDirectory implemented in Phase 2 Task 3/4");
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto footer = loadFooterOrThrow(*pid);
    auto it = footer.blobs.find(p->file);
    if (it == footer.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in footer of {}", p->file, path);
    return it->second.size;
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string & path) const
{
    // Mirror MetadataStorageFromPlainObjectStorage: report the resolved blob object's mtime.
    auto objects = getStorageObjects(path);
    chassert(!objects.empty());
    auto metadata = object_storage->getObjectMetadata(objects.front().remote_path, /*with_tags=*/false);
    return metadata.last_modified;
}

std::vector<std::string> ContentAddressedMetadataStorage::listDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: listDirectory implemented in Phase 2 Task 3/4");
}

DirectoryIteratorPtr ContentAddressedMetadataStorage::iterateDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: iterateDirectory implemented in Phase 2 Task 3/4");
}

StoredObjects ContentAddressedMetadataStorage::getStorageObjects(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto footer = loadFooterOrThrow(*pid);
    auto it = footer.blobs.find(p->file);
    if (it == footer.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in footer of {}", p->file, path);

    const auto & e = it->second;
    // BARE key (no common-prefix prepend); must match what the gtest seeds and what blobs are stored under.
    // TODO(phase3): honor object_storage->getCommonKeyPrefix().
    return {StoredObject(ContentAddressed::blobKey(e.key), path, e.size)};
}

}
