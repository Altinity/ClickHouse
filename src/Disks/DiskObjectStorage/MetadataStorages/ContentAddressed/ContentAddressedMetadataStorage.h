#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <optional>
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
    friend class ContentAddressedTransaction;

    // Resolve a part-file path to its footer BlobEntry (carry-forward source for mutations).
    // Throws if the path is not a part file, the ref is absent, or the file is not in the footer.
    ContentAddressed::BlobEntry resolveBlobEntry(const std::string & path) const;

    // Resolve helpers: part file -> ref -> part_id -> footer -> blob.
    // TODO(phase3): honor object_storage->getCommonKeyPrefix() (bare keys for now).
    // No footer cache in M1 (read each time); caching is a later optimization.

    // Read the ref object at refKey(server_id, table_uuid, part_name).
    // Returns nullopt if the ref object is absent, else its content (the part_id).
    std::optional<std::string> readRefPartId(const std::string & table_uuid, const std::string & part_name) const;

    // Load and deserialize the footer at partKey(part_id).
    // B18 fail-close: if the footer object is absent, throw CORRUPTED_DATA (a live ref must
    // never point at a missing footer); never treat it as "file doesn't exist".
    ContentAddressed::Footer loadFooterOrThrow(const std::string & part_id) const;

    // Read a small object (ref/footer) into a string. Returns nullopt if the object is absent.
    std::optional<std::string> readSmallObjectIfExists(const std::string & key) const;

    const ObjectStoragePtr object_storage;
    const std::string storage_path_prefix;
    const std::string storage_path_full;
    const std::string server_id;
};

}
