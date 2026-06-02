#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>

namespace DB
{

// Transaction for the content-addressed metadata storage.
// Phase 2 only provides the skeleton: commit is a no-op and the write
// methods (createMetadataFile, etc.) are not implemented yet (Phase 3).
class ContentAddressedTransaction : public IMetadataTransaction
{
protected:
    ContentAddressedMetadataStorage & metadata_storage;

public:
    explicit ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_);

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;
};

}
