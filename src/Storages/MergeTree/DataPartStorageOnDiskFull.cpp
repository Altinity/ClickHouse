#include <Storages/MergeTree/DataPartStorageOnDiskFull.h>

#include <Disks/IDiskTransaction.h>
#include <Disks/SingleDiskVolume.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/PackedFilesReader.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadPipeline.h>
#include <IO/WriteBufferFromFileBase.h>
#include <Interpreters/Context.h>
#include <Storages/MergeTree/MergeTreeIndicesSerialization.h>
#include <Common/FailPoint.h>
#include <Common/typeid_cast.h>

#include <set>
#include <vector>

namespace DB
{

namespace FailPoints
{
    extern const char part_storage_fail_commit_transaction[];
    extern const char part_storage_pause_before_commit_transaction[];
    extern const char part_storage_pause_after_commit_transaction[];
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int FAULT_INJECTED;
}

DataPartStorageOnDiskFull::DataPartStorageOnDiskFull(VolumePtr volume_, std::string root_path_, std::string part_dir_)
    : DataPartStorageOnDiskBase(std::move(volume_), std::move(root_path_), std::move(part_dir_))
{
}

DataPartStorageOnDiskFull::DataPartStorageOnDiskFull(
    VolumePtr volume_, std::string root_path_, std::string part_dir_, DiskTransactionPtr transaction_)
    : DataPartStorageOnDiskBase(std::move(volume_), std::move(root_path_), std::move(part_dir_), std::move(transaction_))
{
}

MutableDataPartStoragePtr DataPartStorageOnDiskFull::create(
    VolumePtr volume_, std::string root_path_, std::string part_dir_, bool /*initialize_*/) const
{
    return std::make_shared<DataPartStorageOnDiskFull>(std::move(volume_), std::move(root_path_), std::move(part_dir_));
}

MutableDataPartStoragePtr DataPartStorageOnDiskFull::getProjection(const std::string & name, bool use_parent_transaction) // NOLINT
{
    /// Not arena-scoped: most callers use this only as a short-lived filesystem handle (CHECK TABLE,
    /// mutation hardlink/copy, existence probes). The part-lifetime projection storage is created via
    /// `getProjectionPartBuilder`, which scopes the arena itself.
    return std::shared_ptr<DataPartStorageOnDiskFull>(new DataPartStorageOnDiskFull(volume, std::string(fs::path(root_path) / part_dir), name, use_parent_transaction ? transaction : nullptr));
}

DataPartStoragePtr DataPartStorageOnDiskFull::getProjection(const std::string & name) const
{
    return std::make_shared<DataPartStorageOnDiskFull>(volume, std::string(fs::path(root_path) / part_dir), name);
}

bool DataPartStorageOnDiskFull::exists() const
{
    /// CAS does not report the top-level part directory through the transaction overlay. A temporary
    /// part whose transaction was never committed then takes the early return in `removeIfNeeded`
    /// instead of removing a path that committed metadata does not have. A projection storage asks for
    /// an inner directory of its parent part, so a staged projection directory is visible here.
    return existsDirectoryInTransactionOrDisk(fs::path(root_path) / part_dir);
}

bool DataPartStorageOnDiskFull::existsFileImpl(const std::string & name) const
{
    /// A part being assembled can have staged files that committed metadata cannot see, for example
    /// projection temporary blocks on CAS. The helper consults the held transaction before the disk.
    return existsFileInTransactionOrDisk(fs::path(root_path) / part_dir / name);
}

bool DataPartStorageOnDiskFull::existsDirectory(const std::string & name) const
{
    /// A carried-forward projection may be hard-linked into the open part transaction before committed
    /// metadata exposes its directory. The helper detects staged files below it before falling back to the disk.
    return existsDirectoryInTransactionOrDisk(fs::path(root_path) / part_dir / name);
}

DataPartStorageIteratorPtr DataPartStorageOnDiskFull::iterate() const
{
    return iterateDirectoryInTransactionOrDisk(fs::path(root_path) / part_dir);
}

Poco::Timestamp DataPartStorageOnDiskFull::getFileLastModified(const String & file_name) const
{
    return volume->getDisk()->getLastModified(fs::path(root_path) / part_dir / file_name);
}

size_t DataPartStorageOnDiskFull::getFileSizeImpl(const String & file_name) const
{
    /// A merge gets the size of staged projection temporary files before reading them back. Committed
    /// metadata cannot expose their sizes until the parent part commits, so consult the transaction first.
    return getFileSizeInTransactionOrDisk(fs::path(root_path) / part_dir / file_name);
}

std::optional<UInt64> DataPartStorageOnDiskFull::getPackedFileUncompressedSize(const std::string & file_name) const
{
    if (looksLikePackedSkipIndexFile(file_name))
        if (auto reader = getSkipIndicesPackedReader(); reader && reader->exists(file_name))
            return reader->getFileUncompressedSize(file_name);
    return {};
}

UInt32 DataPartStorageOnDiskFull::getRefCount(const String & file_name) const
{
    return volume->getDisk()->getRefCount(fs::path(root_path) / part_dir / file_name);
}

std::vector<std::string> DataPartStorageOnDiskFull::getRemotePaths(const std::string & file_name) const
{
    const std::string path = fs::path(root_path) / part_dir / file_name;

    /// A staged blob that is not pending upload, for example one carried forward by `createHardLink` from a
    /// committed part, resolves to its pool object. Blobs written by this transaction stay pending until
    /// commit publishes them. A pending blob has no storage object yet, so do not fall back to a committed
    /// object at the same logical path. An inline entry yields one synthetic object with an empty remote
    /// path, the same as a committed inline entry.
    auto objects = getStorageObjectsInTransactionOrDisk(path);

    std::vector<std::string> remote_paths;
    remote_paths.reserve(objects.size());

    for (const auto & object : objects)
        remote_paths.push_back(object.remote_path);

    return remote_paths;
}

String DataPartStorageOnDiskFull::getUniqueId() const
{
    auto disk = volume->getDisk();
    if (!disk->supportZeroCopyReplication())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Disk {} doesn't support zero-copy replication", disk->getName());

    return disk->getUniqueId(fs::path(getRelativePath()) / "checksums.txt");
}

void DataPartStorageOnDiskFull::prepareReadImpl(
    const std::string & name,
    const ReadSettings & settings,
    std::optional<size_t> read_hint,
    ReadPipeline & pipeline) const
{
    /// A projection spill-and-merge can read its own temporary blocks before the parent part commits.
    /// On CAS a staged blob may still be pending upload or may already be in the pool, for example when
    /// carried forward from a committed part, while inline content is stored in the staged entry. Committed
    /// metadata cannot see that state, so the helper installs a source that reads through the held transaction.
    /// Without a transaction, or if it does not know the path, the helper uses the disk pipeline unchanged.
    prepareReadInTransactionOrDisk(fs::path(root_path) / part_dir / name, settings, read_hint, pipeline);
}

std::unique_ptr<ReadBufferFromFileBase> DataPartStorageOnDiskFull::readFileIfExistsImpl(
    const std::string & name,
    const ReadSettings & settings,
    std::optional<size_t> read_hint) const
{
    /// `readFileIfExists` does not go through `prepareRead`, so it must apply the same transaction overlay.
    /// Staged blobs, including pending uploads, and inline bytes are served through the transaction before
    /// commit. If the transaction knows the path but cannot read it, the helper does not expose a
    /// committed version.
    return readFileIfExistsInTransactionOrDisk(fs::path(root_path) / part_dir / name, settings, read_hint);
}

std::unique_ptr<WriteBufferFromFileBase> DataPartStorageOnDiskFull::writeFile(
    const String & name,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings)
{
    if (transaction)
        return transaction->writeFile(fs::path(root_path) / part_dir / name, buf_size, mode, settings);
    return volume->getDisk()->writeFile(fs::path(root_path) / part_dir / name, buf_size, mode, settings);
}

void DataPartStorageOnDiskFull::createFile(const String & name)
{
    executeWriteOperation([&](auto & disk) { disk.createFile(fs::path(root_path) / part_dir / name); });
}

void DataPartStorageOnDiskFull::moveFile(const String & from_name, const String & to_name)
{
    executeWriteOperation([&](auto & disk)
    {
        auto relative_path = fs::path(root_path) / part_dir;
        disk.moveFile(relative_path / from_name, relative_path / to_name);
    });
}

void DataPartStorageOnDiskFull::replaceFile(const String & from_name, const String & to_name)
{
    executeWriteOperation([&](auto & disk)
    {
        auto relative_path = fs::path(root_path) / part_dir;
        disk.replaceFile(relative_path / from_name, relative_path / to_name);
    });
}

void DataPartStorageOnDiskFull::removeFile(const String & name)
{
    executeWriteOperation([&](auto & disk) { disk.removeFile(fs::path(root_path) / part_dir / name); });
}

void DataPartStorageOnDiskFull::removeFileIfExists(const String & name)
{
    executeWriteOperation([&](auto & disk) { disk.removeFileIfExists(fs::path(root_path) / part_dir / name); });
}

void DataPartStorageOnDiskFull::createHardLinkFrom(const IDataPartStorage & source, const std::string & from, const std::string & to)
{
    const auto * source_on_disk = typeid_cast<const DataPartStorageOnDiskFull *>(&source);
    if (!source_on_disk)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Cannot create hardlink from different storage. Expected DataPartStorageOnDiskFull, got {}",
            typeid(source).name());

    executeWriteOperation([&](auto & disk)
    {
        disk.createHardLink(
            fs::path(source_on_disk->getRelativePath()) / from,
            fs::path(root_path) / part_dir / to);
    });
}

void DataPartStorageOnDiskFull::copyFileFrom(const IDataPartStorage & source, const std::string & from, const std::string & to)
{
    const auto * source_on_disk = typeid_cast<const DataPartStorageOnDiskFull *>(&source);
    if (!source_on_disk)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Cannot create copy file from different storage. Expected DataPartStorageOnDiskFull, got {}",
            typeid(source).name());

    /// Copying files between different disks is
    /// not supported in disk transactions.
    source_on_disk->getDisk()->copyFile(
        fs::path(source_on_disk->getRelativePath()) / from,
        *volume->getDisk(),
        fs::path(root_path) / part_dir / to,
        getReadSettings());
}

void DataPartStorageOnDiskFull::createProjection(const std::string & name)
{
    executeWriteOperation([&](auto & disk) { disk.createDirectory(fs::path(root_path) / part_dir / name); });
}

void DataPartStorageOnDiskFull::beginTransaction()
{
    /// A borrowed projection sub-part shares the parent part's transaction and must not open its own.
    if (has_shared_transaction)
        return;

    if (transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Uncommitted transaction already exists");

    transaction = volume->getDisk()->createTransaction();
}

void DataPartStorageOnDiskFull::commitTransaction()
{
    /// The mirror of beginTransaction: a borrowed projection sub-part rides the parent's transaction and
    /// is published by the parent's single commit. Committing here would be committing someone else's
    /// transaction, so it is a no-op.
    if (has_shared_transaction)
        return;

    if (!transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is no uncommitted transaction");

    /// Regression gate for the part-durability-before-Keeper-commit invariant: lets a test fail the
    /// close of the PART's deferred disk transaction specifically (autocommit one-shot disk ops are
    /// not affected, unlike disk_object_storage_fail_commit_metadata_transaction).
    fiu_do_on(FailPoints::part_storage_fail_commit_transaction,
    {
        throw Exception(ErrorCodes::FAULT_INJECTED, "part_storage_fail_commit_transaction");
    });

    /// Lets a test hold the part inside its storage commit and prove that no MergeTree lock is held here.
    FailPointInjection::pauseFailPoint(FailPoints::part_storage_pause_before_commit_transaction);

    transaction->commit();
    transaction.reset();

    /// Lets a test stop the process between the durable commit and the rename that follows.
    FailPointInjection::pauseFailPoint(FailPoints::part_storage_pause_after_commit_transaction);
}

void DataPartStorageOnDiskFull::checkpointTransaction()
{
    /// A content-addressed part is published once, as a whole, by its final commit: a mid-build
    /// commit would publish the files written so far as a separate partial part.
    if (isContentAddressed())
        return;

    IDataPartStorage::checkpointTransaction();
}

}
