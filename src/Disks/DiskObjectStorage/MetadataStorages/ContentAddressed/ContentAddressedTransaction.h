#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <Disks/WriteMode.h>
#include <IO/WriteSettings.h>

#include <map>
#include <memory>

namespace DB
{

// Transaction for the content-addressed metadata storage.
//
// As part files are written, each ContentAddressedWriteBuffer reports its content hash back
// through a finalize callback, and the transaction accumulates a (logical_file -> BlobEntry) map.
// Carried-forward files (mutations / hardlinks) copy their source BlobEntry without re-uploading.
// At commit the transaction derives the part_id from the accumulated blobs, writes the
// parts/<part_id> footer (put-if-absent) and publishes the ref.
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

    // Open a content-addressed write buffer for a part file. The buffer hashes + uploads the
    // content on finalize and, via its callback, records the resulting blob under the path's
    // logical file name. The buffer-size and settings arguments mirror the disk-layer writeFile
    // signature (they are forwarded to the underlying buffer in Task 3); content addressing
    // ignores append mode, so only Rewrite is meaningful here.
    std::unique_ptr<ContentAddressed::ContentAddressedWriteBuffer> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    // Carry a file forward from an already-committed (or in-flight) source part without
    // re-uploading: resolve the source blob and record it under the destination logical file.
    //
    // This is the production carry-forward entry point: the disk layer's
    // DiskObjectStorageTransaction::createHardLink delegates to metadata_transaction->createHardLink,
    // so this override is what real mutations / ATTACH reach when hardlinking unchanged files.
    void createHardLink(const std::string & path_from, const std::string & path_to) override;

    // Backwards-compatible alias used by the direct-call tests; forwards to createHardLink.
    void createHardLinkFrom(const std::string & from, const std::string & to) { createHardLink(from, to); }

private:
    // Record (logical_file -> blob) for the part being written; the part_id is derived from this.
    void recordBlob(const std::string & path, ContentAddressed::BlobEntry entry);
    // Pin/verify the (table_uuid, part_name) all files of one commit must agree on.
    void rememberTarget(const std::string & path);

    std::map<std::string, ContentAddressed::BlobEntry> recorded;
    std::string table_uuid;
    std::string part_name;
};

}
