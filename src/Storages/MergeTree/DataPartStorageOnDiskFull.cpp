#include <Storages/MergeTree/DataPartStorageOnDiskFull.h>

#include <Disks/IDiskTransaction.h>
#include <Disks/SingleDiskVolume.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadPipeline.h>
#include <IO/WriteBufferFromFileBase.h>
#include <Interpreters/Context.h>
#include <Common/typeid_cast.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
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
    return std::shared_ptr<DataPartStorageOnDiskFull>(new DataPartStorageOnDiskFull(volume, std::string(fs::path(root_path) / part_dir), name, use_parent_transaction ? transaction : nullptr));
}

DataPartStoragePtr DataPartStorageOnDiskFull::getProjection(const std::string & name) const
{
    return std::make_shared<DataPartStorageOnDiskFull>(volume, std::string(fs::path(root_path) / part_dir), name);
}

bool DataPartStorageOnDiskFull::exists() const
{
    return volume->getDisk()->existsDirectory(fs::path(root_path) / part_dir);
}

bool DataPartStorageOnDiskFull::existsFile(const std::string & name) const
{
    auto path = fs::path(root_path) / part_dir / name;
    /// B59: a part still being assembled by this transaction can have staged-but-uncommitted files
    /// (e.g. projection temp blocks on a content-addressed disk). Consult the held transaction first.
    if (transaction && transaction->tryGetInFlightFileSize(path).has_value())
        return true;
    return volume->getDisk()->existsFile(path);
}

bool DataPartStorageOnDiskFull::existsDirectory(const std::string & name) const
{
    return volume->getDisk()->existsDirectory(fs::path(root_path) / part_dir / name);
}

class DataPartStorageIteratorOnDisk final : public IDataPartStorageIterator
{
public:
    DataPartStorageIteratorOnDisk(DiskPtr disk_, DirectoryIteratorPtr it_)
        : disk(std::move(disk_)), it(std::move(it_))
    {
    }

    void next() override { it->next(); }
    bool isValid() const override { return it->isValid(); }
    bool isFile() const override { return isValid() && disk->existsFile(it->path()); }
    std::string name() const override { return it->name(); }
    std::string path() const override { return it->path(); }

private:
    DiskPtr disk;
    DirectoryIteratorPtr it;
};

DataPartStorageIteratorPtr DataPartStorageOnDiskFull::iterate() const
{
    return std::make_unique<DataPartStorageIteratorOnDisk>(
        volume->getDisk(),
        volume->getDisk()->iterateDirectory(fs::path(root_path) / part_dir));
}

Poco::Timestamp DataPartStorageOnDiskFull::getFileLastModified(const String & file_name) const
{
    return volume->getDisk()->getLastModified(fs::path(root_path) / part_dir / file_name);
}

size_t DataPartStorageOnDiskFull::getFileSize(const String & file_name) const
{
    auto path = fs::path(root_path) / part_dir / file_name;
    /// B59: see existsFile — the merge stats the staged temp files before reading them back.
    if (transaction)
        if (auto size = transaction->tryGetInFlightFileSize(path))
            return *size;
    return volume->getDisk()->getFileSize(path);
}

UInt32 DataPartStorageOnDiskFull::getRefCount(const String & file_name) const
{
    return volume->getDisk()->getRefCount(fs::path(root_path) / part_dir / file_name);
}

std::vector<std::string> DataPartStorageOnDiskFull::getRemotePaths(const std::string & file_name) const
{
    const std::string path = fs::path(root_path) / part_dir / file_name;

    /// B59: a file staged by this transaction resolves to its already-uploaded blob object(s) before commit.
    StoredObjects objects;
    if (transaction)
        if (auto inflight = transaction->tryGetInFlightStorageObjects(path))
            objects = std::move(*inflight);
    if (objects.empty())
        objects = volume->getDisk()->getStorageObjects(path);

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

void DataPartStorageOnDiskFull::prepareRead(
    const std::string & name,
    const ReadSettings & settings,
    std::optional<size_t> read_hint,
    ReadPipeline & pipeline) const
{
    auto path = fs::path(root_path) / part_dir / name;

    /// B59: read-your-writes for a part still being assembled by this transaction. A projection
    /// spill-and-merge reads its own temp blocks back before the parent part's single commit; on a
    /// content-addressed disk those files are staged in the transaction (blob uploaded, no ref yet),
    /// so the committed metadata path can't see them. If the held transaction resolves the file
    /// in-flight, serve it via a custom pipeline source that reads through the transaction. Gated on
    /// `transaction != nullptr` so committed-part reads (no open transaction) are unchanged.
    if (transaction)
    {
        StoredObjects inflight_objects;
        if (auto objs = transaction->tryGetInFlightStorageObjects(path))
            inflight_objects = std::move(*objs);
        else if (auto size = transaction->tryGetInFlightFileSize(path))
            /// Mutable per-part file staged inline (no blob object); synthesize a placeholder so the
            /// single-object pipeline is satisfied — the custom creator below ignores it and reads the
            /// inline bytes through the transaction.
            inflight_objects = StoredObjects{StoredObject(path, path, *size)};

        if (!inflight_objects.empty())
        {
            auto * tx = transaction.get();
            pipeline.setSource(
                [tx, path](const StoredObject &, const ReadSettings & read_settings, bool /*use_external_buffer*/, bool /*restrict_seek*/)
                {
                    return tx->tryReadFileInFlight(path, read_settings, std::nullopt);
                },
                std::move(inflight_objects),
                settings);
            return;
        }
    }

    volume->getDisk()->prepareRead(path, settings, read_hint, pipeline);
}

std::unique_ptr<ReadBufferFromFileBase> DataPartStorageOnDiskFull::readFileIfExists(
    const std::string & name,
    const ReadSettings & settings,
    std::optional<size_t> read_hint) const
{
    auto path = fs::path(root_path) / part_dir / name;
    /// B59: serve a file staged by this transaction (uploaded blob or inline mutable bytes) before commit.
    /// This direct delegate bypasses prepareRead, so the in-flight guard must be repeated here; it is the
    /// only path that reaches the inline-mutable case via a returned buffer.
    if (transaction)
        if (auto rb = transaction->tryReadFileInFlight(path, settings, read_hint))
            return rb;
    return volume->getDisk()->readFileIfExists(path, settings, read_hint);
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
    if (transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Uncommitted{}transaction already exists", has_shared_transaction ? " shared " : " ");

    transaction = volume->getDisk()->createTransaction();
}

void DataPartStorageOnDiskFull::commitTransaction()
{
    if (!transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is no uncommitted transaction");

    if (has_shared_transaction)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot commit shared transaction");

    transaction->commit();
    transaction.reset();
}

}
