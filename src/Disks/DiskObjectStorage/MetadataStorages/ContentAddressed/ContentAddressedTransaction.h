#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/WriteMode.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>
#include <memory>
#include <string>

namespace DB
{

/// The M-W write-path wiring: accumulates ClickHouse operations and maps them to ONE Cas::Build
/// per written part at commit (design 2026-06-11 section 4; plan D-W2).
///
/// T2 SKELETON: every mutating operation throws NOT_IMPLEMENTED. The operations land task by task
/// (T3 content blobs, T4 mutable files, T5 carry-forward/renames, T6 removals, T7
/// detached/ATTACH/FREEZE, T8 read-your-writes, T9 disk-seam glue) — each with wiring tests; the
/// SQL suites gate the completed milestone (T13).
class ContentAddressedTransaction : public IMetadataTransaction
{
public:
    explicit ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_);

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;
    bool hasInFlightDirectory(const std::string & path) const override;
    std::vector<std::string> listInFlightDirectory(const std::string & path) const override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    /// The disk-layer write entry (DiskObjectStorageTransaction's CA branch calls this directly).
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    void createDirectory(const std::string & path) override;
    void createDirectoryRecursive(const std::string & path) override;
    void removeDirectory(const std::string & path) override;
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;
    void createHardLink(const std::string & path_from, const std::string & path_to) override;
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;
    void moveFile(const std::string & path_from, const std::string & path_to) override;
    void replaceFile(const std::string & path_from, const std::string & path_to) override;
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;
    void truncateFile(const std::string & path, size_t size) override;

protected:
    ContentAddressedMetadataStorage & metadata_storage;
};

}
