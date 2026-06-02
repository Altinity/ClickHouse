#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <string>

namespace DB
{

// Content-addressed metadata storage: resolves part files via ref to part_id to footer to blob.
// Read/resolve only in Phase 2; the write path is Phase 3.
class ContentAddressedMetadataStorage final : public IMetadataStorage
{
public:
    ContentAddressedMetadataStorage(ObjectStoragePtr object_storage_, String storage_path_prefix_, String server_id_);

    MetadataStorageType getType() const override { return MetadataStorageType::ContentAddressed; }
    const std::string & getPath() const override { return storage_path_full; }
    bool supportsChmod() const override { return false; }
    bool supportsStat() const override { return false; }
    bool isReadOnly() const override { return false; }
    bool areBlobPathsRandom() const override { return false; }
    uint32_t getHardlinkCount(const std::string &) const override { return 0; }

    MetadataTransactionPtr createTransaction() override;

    bool existsFile(const std::string & path) const override;
    bool existsDirectory(const std::string & path) const override;
    bool existsFileOrDirectory(const std::string & path) const override;
    uint64_t getFileSize(const std::string & path) const override;
    Poco::Timestamp getLastModified(const std::string & path) const override;
    std::vector<std::string> listDirectory(const std::string & path) const override;
    DirectoryIteratorPtr iterateDirectory(const std::string & path) const override;
    StoredObjects getStorageObjects(const std::string & path) const override;

    const std::string & serverIdForTest() const { return server_id; }

private:
    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
};

}
