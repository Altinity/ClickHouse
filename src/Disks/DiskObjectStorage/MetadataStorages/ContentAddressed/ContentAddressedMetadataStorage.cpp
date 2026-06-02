#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>

#include <filesystem>

#include <Common/Exception.h>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
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

bool ContentAddressedMetadataStorage::existsFile(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: existsFile implemented in Phase 2 Task 3/4");
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: existsDirectory implemented in Phase 2 Task 3/4");
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: existsFileOrDirectory implemented in Phase 2 Task 3/4");
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: getFileSize implemented in Phase 2 Task 3/4");
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: getLastModified implemented in Phase 2 Task 3/4");
}

std::vector<std::string> ContentAddressedMetadataStorage::listDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: listDirectory implemented in Phase 2 Task 3/4");
}

DirectoryIteratorPtr ContentAddressedMetadataStorage::iterateDirectory(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: iterateDirectory implemented in Phase 2 Task 3/4");
}

StoredObjects ContentAddressedMetadataStorage::getStorageObjects(const std::string &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: getStorageObjects implemented in Phase 2 Task 3/4");
}

}
