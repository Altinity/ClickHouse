#include <Disks/DiskObjectStorage/Replication/ClusterConfiguration.h>
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
#include <Disks/DiskObjectStorage/DiskObjectStorageTransaction.h>
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/IOSchedulingSettings.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/ForkWriteBuffer.h>
#include <IO/WriteBuffer.h>
#if ENABLE_DISTRIBUTED_CACHE
#include <DistributedCache/Utils.h>
#endif
#include <Core/Settings.h>
#include <Core/SettingsEnums.h>
#include <Disks/IO/WriteBufferWithFinalizeCallback.h>
#include <Disks/WriteMode.h>
#include <Disks/IDisk.h>

#include <Common/ElapsedTimeProfileEventIncrement.h>
#include <Common/Logger.h>
#include <Common/checkStackSize.h>
#include <Common/logger_useful.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/ProfileEvents.h>
#include <Common/setThreadName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <base/defines.h>

#include <cstddef>
#include <memory>
#include <ranges>
#include <vector>

namespace ProfileEvents
{
    extern const Event DiskObjectStorageWaitBlobRemovalMicroseconds;
}

namespace DB
{

namespace FailPoints
{
    extern const char smt_insert_fake_hardware_error[];
    extern const char disk_object_storage_fail_commit_metadata_transaction[];
    extern const char disk_object_storage_fail_precommit_metadata_transaction[];
}

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int FAULT_INJECTED;
    extern const int LOGICAL_ERROR;
}

namespace
{

/// A content-addressed mutable per-part file atomic-write rename (e.g. txn_version.txt.tmp ->
/// txn_version.txt, the MVCC creation-CSN / removal-TID protocol). Both endpoints are mutable
/// per-part files in the same part. Such a rename only re-keys the part's staged sidecar map, so it
/// must be dispatched EAGERLY (in program order, like writeFile / createHardLink / moveDirectory) and
/// NOT deferred to commit replay: the part-directory rename (moveDirectory) is eager and publishes the
/// part's manifest + sidecar, so a deferred rename here would replay AFTER that publish, stranding the
/// .tmp file in the published sidecar and failing the rename (B182 — merge/mutation inside an explicit
/// transaction).
bool isContentAddressedMutablePartFileRename(const std::string & from_path, const std::string & to_path)
{
    auto from = ContentAddressed::parsePartFilePath(from_path);
    if (!from || from->file.empty() || !ContentAddressed::isMutablePerPartFile(from->file))
        return false;
    auto to = ContentAddressed::parsePartFilePath(to_path);
    return to && !to->file.empty() && ContentAddressed::isMutablePerPartFile(to->file);
}

}

void DiskObjectStorageTransaction::waitBlobRemoval(const StoredObjects & blobs) const
{
    try
    {
        ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::DiskObjectStorageWaitBlobRemovalMicroseconds);
        for (size_t i = 0; i < 100 && metadata_storage->hasPendingRemovalBlobs(blobs); ++i)
            blob_killer->triggerAndWait();

        if (watch.elapsed() > 100'000)
            LOG_TRACE(getLogger("DiskObjectStorageTransaction"), "Waiting for blob removal took {} ms", watch.elapsed() / 1000);
    }
    catch (...)
    {
        tryLogCurrentException(getLogger("DiskObjectStorageTransaction"));
    }
}

DiskObjectStorageTransaction::DiskObjectStorageTransaction(
    ClusterConfigurationPtr cluster_,
    MetadataStoragePtr metadata_storage_,
    ObjectStorageRouterPtr object_storages_,
    BlobKillerThreadPtr blob_killer_,
    std::shared_ptr<ThreadPool> copy_object_pool_,
    bool wait_blob_removal_,
    String read_resource_name_,
    String write_resource_name_)
    : cluster(std::move(cluster_))
    , metadata_storage(std::move(metadata_storage_))
    , object_storages(std::move(object_storages_))
    , blob_killer(std::move(blob_killer_))
    , copy_object_pool(std::move(copy_object_pool_))
    , wait_blob_removal(wait_blob_removal_)
    , read_resource_name(std::move(read_resource_name_))
    , write_resource_name(std::move(write_resource_name_))
    , metadata_transaction(metadata_storage->createTransaction())
{
}

MultipleDisksObjectStorageTransaction::MultipleDisksObjectStorageTransaction(
    ClusterConfigurationPtr source_cluster_,
    MetadataStoragePtr source_metadata_storage_,
    ObjectStorageRouterPtr source_object_storages_,
    ClusterConfigurationPtr destination_cluster_,
    MetadataStoragePtr destination_metadata_storage_,
    ObjectStorageRouterPtr destination_object_storages_,
    std::shared_ptr<ThreadPool> copy_object_pool_,
    std::string read_resource_name_,
    std::string write_resource_name_)
    : DiskObjectStorageTransaction(destination_cluster_, destination_metadata_storage_, destination_object_storages_, /*blob_killer=*/nullptr, std::move(copy_object_pool_), /*wait_blob_removal=*/false, std::move(read_resource_name_), std::move(write_resource_name_))
    , source_cluster(std::move(source_cluster_))
    , source_metadata_storage(std::move(source_metadata_storage_))
    , source_object_storages(std::move(source_object_storages_))
{
}

void DiskObjectStorageTransaction::createDirectory(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->createDirectory(path);
    });
}

void DiskObjectStorageTransaction::createDirectories(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->createDirectoryRecursive(path);
    });
}

void DiskObjectStorageTransaction::moveDirectory(const std::string & from_path, const std::string & to_path)
{
    /// CA: dispatch the rename EAGERLY (mirroring the createHardLink CA early-dispatch) instead of
    /// queuing a deferred lambda that fires inside commit() under the data_parts lock. For a
    /// content-addressed disk this is where a freshly-written part is published to its FINAL manifest
    /// ref; renameParts() runs LOCK-FREE in the replicated paths, so the publish happens off the
    /// data_parts lock (B151).
    if (metadata_storage->isContentAddressed())
    {
        metadata_transaction->moveDirectory(from_path, to_path);
        return;
    }

    operations_to_execute.push_back([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->moveDirectory(from_path, to_path);
    });
}

void DiskObjectStorageTransaction::moveFile(const String & from_path, const String & to_path)
{
    /// CA: a mutable per-part file's atomic-write rename re-keys the staged sidecar and must run in
    /// program order, BEFORE the eager moveDirectory publish — dispatch it eagerly (see
    /// isContentAddressedMutablePartFileRename, B182).
    if (metadata_storage->isContentAddressed() && isContentAddressedMutablePartFileRename(from_path, to_path))
    {
        metadata_transaction->moveFile(from_path, to_path);
        return;
    }

    operations_to_execute.push_back([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->moveFile(from_path, to_path);
    });
}

void DiskObjectStorageTransaction::truncateFile(const String & path, size_t size)
{
    operations_to_execute.push_back([path, size](MetadataTransactionPtr tx)
    {
        tx->truncateFile(path, size);
    });
}

void DiskObjectStorageTransaction::replaceFile(const std::string & from_path, const std::string & to_path)
{
    /// CA: a mutable per-part file's atomic-write replace re-keys the staged sidecar and must run in
    /// program order, BEFORE the eager moveDirectory publish — dispatch it eagerly (see
    /// isContentAddressedMutablePartFileRename, B182).
    if (metadata_storage->isContentAddressed() && isContentAddressedMutablePartFileRename(from_path, to_path))
    {
        metadata_transaction->replaceFile(from_path, to_path);
        return;
    }

    operations_to_execute.push_back([from_path, to_path](MetadataTransactionPtr tx)
    {
        tx->replaceFile(from_path, to_path);
    });
}

void DiskObjectStorageTransaction::removeFile(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/false, /*should_remove_objects=*/true);
    });
}

void DiskObjectStorageTransaction::removeSharedFile(const std::string & path, bool keep_shared_data)
{
    operations_to_execute.push_back([path, keep_shared_data](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/false, /*should_remove_objects=*/!keep_shared_data);
    });
}

void DiskObjectStorageTransaction::removeSharedRecursive(
    const std::string & path, bool keep_all_shared_data, const NameSet & file_names_remove_metadata_only)
{
    if (!keep_all_shared_data && file_names_remove_metadata_only.empty())
    {
        operations_to_execute.push_back([path](MetadataTransactionPtr tx)
        {
            tx->removeRecursive(path, /*should_remove_objects=*/nullptr);
        });
    }
    else
    {
        operations_to_execute.push_back([path, keep_all_shared_data, file_names_remove_metadata_only](MetadataTransactionPtr tx)
        {
            tx->removeRecursive(path, /*should_remove_objects=*/[keep_all_shared_data, file_names_remove_metadata_only](const std::string & relative_path)
            {
                return !keep_all_shared_data && !file_names_remove_metadata_only.contains(relative_path);
            });
        });
    }
}

void DiskObjectStorageTransaction::removeSharedFileIfExists(const std::string & path, bool keep_shared_data)
{
    operations_to_execute.push_back([path, keep_shared_data](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/true, /*should_remove_objects=*/!keep_shared_data);
    });
}

void DiskObjectStorageTransaction::removeDirectory(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->removeDirectory(path);
    });
}

void DiskObjectStorageTransaction::removeRecursive(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->removeRecursive(path, /*should_remove_objects=*/nullptr);
    });
}

void DiskObjectStorageTransaction::removeFileIfExists(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->unlinkFile(path, /*if_exists=*/true, /*should_remove_objects*/true);
    });
}

void DiskObjectStorageTransaction::removeSharedFiles(const RemoveBatchRequest & files, bool keep_all_batch_data, const NameSet & file_names_remove_metadata_only)
{
    for (const auto & [path, if_exists] : files)
    {
        const bool should_remove_objects = !keep_all_batch_data && !file_names_remove_metadata_only.contains(fs::path(path).filename());
        operations_to_execute.push_back([path, if_exists, should_remove_objects](MetadataTransactionPtr tx)
        {
            tx->unlinkFile(path, if_exists, should_remove_objects);
        });
    }
}

std::unique_ptr<WriteBufferFromFileBase> DiskObjectStorageTransaction::writeFileWithAutoCommit(
    const std::string & path,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings)
{
    return writeFileImpl(/*autocommit=*/true, path, buf_size, mode, settings);
}

std::unique_ptr<WriteBufferFromFileBase> DiskObjectStorageTransaction::writeFile(
    const std::string & path,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings)
{
    return writeFileImpl(/*autocommit=*/false, path, buf_size, mode, settings);
}

std::unique_ptr<WriteBufferFromFileBase> DiskObjectStorageTransaction::writeFileImpl(
    bool autocommit,
    const std::string & path,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings)
{
    LOG_TEST(getLogger("DiskObjectStorageTransaction"), "write file {} mode {} autocommit {}", path, mode, autocommit);

    WriteSettings enriched_settings = updateIOSchedulingSettings(settings, read_resource_name, write_resource_name);

    /// NOTE: We check it here and not after writing blob because in case of plain/plain-rewritable metadata storages
    ///       undo of disk tx will actually remove existing data.
    /// A content-addressed metadata storage reports no native append, but CAN service an append on a
    /// non-part / table-level VERBATIM file (e.g. the mutation entry mutation_<n>.txt, to which the MVCC
    /// commit appends the CSN line in afterCommit) by read-modify-rewrite: the file is a verbatim object
    /// at a stable key and is fully readable, so the existing bytes are carried forward and the new bytes
    /// written after them (handled in the CA write path below). Append on a CA PART file stays rejected.
    if (mode == WriteMode::Append && !metadata_storage->supportWritingWithAppend()
        && !metadata_storage->isContentAddressed())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append");

    /// Gated content-addressed write path. For a content-addressed metadata storage the blob key
    /// is the content hash, which is only known after all bytes have been written, so the up-front
    /// `generateObjectKeyForPath` + streaming `writeObject` path below cannot be used. Instead we
    /// delegate to the content-addressed transaction's own buffer, which spills + hashes + uploads
    /// the content on finalize and records the resulting blob. The manifest + ref are then published
    /// when `commit` invokes `metadata_transaction->commit` (no `operations_to_execute` entry is
    /// needed here). This branch leaves every other metadata type's behavior unchanged.
    if (metadata_storage->isContentAddressed())
    {
        auto * content_addressed_transaction = dynamic_cast<ContentAddressedTransaction *>(metadata_transaction.get());
        if (!content_addressed_transaction)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Content-addressed metadata storage did not produce a ContentAddressedTransaction");

        /// Append is serviceable (read-modify-rewrite) only for a non-part / table-level verbatim file.
        /// A part file is either a content blob (the key is the content hash — append is meaningless) or a
        /// mutable per-part file (always rewritten whole), so append on a part-file path is unsupported.
        if (mode == WriteMode::Append && ContentAddressed::isPartFilePath(path))
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append for content part files");

        /// Autocommit cannot work for CONTENT part files: their manifest + ref are published only
        /// when `commit` invokes `metadata_transaction->commit`, which the buffer finalize does not
        /// trigger. Non-part / table-level files (e.g. format_version.txt) are written verbatim to a
        /// direct object key and are durable on finalize with no commit involvement, so autocommit is
        /// fine for them.
        ///
        /// A MUTABLE per-part file (txn_version.txt / metadata_version.txt and their atomic-write
        /// .tmp siblings) IS autocommittable: it stages inline into recorded_mutable and the
        /// mutable-only commit branch publishes it to the part's per-ref sidecar WITHOUT touching the
        /// manifest or ref. The MVCC layer writes txn_version.txt this way through the bare disk (no
        /// part transaction), so each create/write/replace is its own autocommit one-shot — this is
        /// the path that makes transactional INSERT's creation-CSN fill-in, removal-TID rewrite, and
        /// rollback work on a content-addressed disk. The buffer finalize does not trigger commit, so
        /// wrap the inline buffer in a finalize callback that commits this one-shot transaction (the
        /// inner buffer's own finalize stages the bytes into recorded_mutable first; the outer
        /// callback then publishes the sidecar).
        if (autocommit && ContentAddressed::isPartFilePath(path))
        {
            auto p = ContentAddressed::parsePartFilePath(path);
            if (!p || p->file.empty() || !ContentAddressed::isMutablePerPartFile(p->file))
                throw Exception(
                    ErrorCodes::NOT_IMPLEMENTED,
                    "Autocommit writes are not supported for content part files on a content-addressed disk");

            auto inner = content_addressed_transaction->writeFile(path, buf_size, mode, enriched_settings);
            auto commit_callback = [disk_tx = shared_from_this()](size_t) mutable { disk_tx->commit(); };
            return std::make_unique<WriteBufferWithFinalizeCallback>(
                std::move(inner), std::move(commit_callback), path, /*create_blob_if_empty=*/true);
        }

        /// The returned buffer (a ContentAddressedWriteBuffer for a content blob, or a
        /// ContentAddressedInlineWriteBuffer for a non-autocommit mutable per-part file) captures a bare
        /// `[this]` of `content_addressed_transaction` in its finalize / pin-blob callbacks
        /// (on_finalized -> recorded[...], on_pin_blob -> recordBlobInSession -> persistSession reading
        /// `this->session`). These buffers are deferred-finalized: MergedBlockOutputStream's Finalizer
        /// stores them and calls finish() LATER, possibly from another thread or after the part storage /
        /// transaction would otherwise be torn down on an async-insert / cancel / exception-unwind path. If
        /// the buffer outlives the transaction, that `[this]` dangles -> persistSession touches a freed
        /// session -> heap corruption (the suspected CA-S3 SIGSEGV). Mirror the mutable-file branch above
        /// (and the verbatim branch below): pin the DiskObjectStorageTransaction via
        /// `disk_tx = shared_from_this()` for the lifetime of the returned buffer. Because this transaction
        /// owns `metadata_transaction` (the ContentAddressedTransaction) by shared_ptr, holding `disk_tx`
        /// keeps that `[this]` valid until the buffer (and so this callback) is destroyed after finalize.
        /// No cycle: the transaction does not hold the buffer, so releasing the callback releases the
        /// transaction.
        auto inner = content_addressed_transaction->writeFile(path, buf_size, mode, enriched_settings);
        auto keep_alive_callback = [disk_tx = shared_from_this()](size_t) mutable {};
        return std::make_unique<WriteBufferWithFinalizeCallback>(
            std::move(inner), std::move(keep_alive_callback), path, /*create_blob_if_empty=*/true);
    }

    StoredObject object(metadata_transaction->generateObjectKeyForPath(path).serialize(), path);
    ForkWriteBuffer::WriteBufferPtrs writers;
    auto enabled_locations = cluster->getEnabledLocations();
    for (const auto & location : enabled_locations)
    {
        size_t use_buffer_size = buf_size;
        std::unique_ptr<WriteBufferFromFileBase> writer;

        if (location == cluster->getLocalLocation())
        {
            ObjectStoragePtr object_storage = object_storages->takePointingTo(location);

            #if ENABLE_DISTRIBUTED_CACHE
                bool use_distributed_cache = DistributedCache::canUseDistributedCacheForWrite(enriched_settings, *object_storage);

                if (use_distributed_cache && enriched_settings.distributed_cache_settings.write_through_cache_buffer_size)
                    use_buffer_size = enriched_settings.distributed_cache_settings.write_through_cache_buffer_size;
            #endif

            writer = object_storage->writeObject(
                object,
                /// We always use mode Rewrite because we simulate append using metadata and different files
                WriteMode::Rewrite,
                /*attributes=*/std::nullopt,
                use_buffer_size,
                enriched_settings);

            #if ENABLE_DISTRIBUTED_CACHE
                if (use_distributed_cache)
                    writer = DistributedCache::writeWithDistributedCache(path, object, enriched_settings, *object_storage, std::move(writer));
            #endif
        }
        else
        {
            writer = object_storages->takePointingTo(location)->writeObject(
                object,
                /// We always use mode Rewrite because we simulate append using metadata and different files
                WriteMode::Rewrite,
                /*attributes=*/std::nullopt,
                use_buffer_size,
                enriched_settings);
        }

        writers.push_back(std::move(writer));
        written_blobs[location].push_back(object);
    }

    auto buffer_to_enabled_locations = std::make_unique<ForkWriteBuffer>(std::move(writers));

    /// Does metadata_storage support empty files without actual blobs in the object_storage?
    const bool create_blob_if_empty = !metadata_storage->supportsEmptyFilesWithoutBlobs();

    /// This callback called in WriteBuffer finalize method -- only there we actually know
    /// how many bytes were written. We don't control when this finalize method will be called
    /// so here we just modify operation itself, but don't execute anything (and don't modify metadata transaction).
    /// Otherwise it's possible to get reorder of operations, like:
    /// tx->createDirectory(xxx) -- will add metadata operation in execute
    /// buf1 = tx->writeFile(xxx/yyy.bin)
    /// buf2 = tx->writeFile(xxx/zzz.bin)
    /// ...
    /// buf1->finalize() // shouldn't do anything with metadata operations, just memorize what to do
    /// tx->commit()
    const auto create_metadata_callback = [disk_tx = shared_from_this(), replicated_locations = std::move(enabled_locations), mode, object, autocommit, create_blob_if_empty](size_t count) mutable
    {
        object.bytes_size = count;

        /// Locations to which blobs were not originally copied should be marked as missing.
        auto missing_locations = disk_tx->cluster->findComplement(replicated_locations);
        disk_tx->operations_to_execute.push_back([object, mode, create_blob_if_empty, blob_replication = std::move(missing_locations)](MetadataTransactionPtr tx)
        {
            if (mode == WriteMode::Rewrite)
            {
                if (object.bytes_size > 0 || create_blob_if_empty)
                {
                    LOG_TEST(getLogger("DiskObjectStorageTransaction"), "Writing blob for path {}, key {}, size {}", object.local_path, object.remote_path, object.bytes_size);
                    tx->recordBlobsReplication(object, blob_replication);
                    tx->createMetadataFile(object.local_path, {object});
                }
                else
                {
                    LOG_TRACE(getLogger("DiskObjectStorageTransaction"), "Skipping writing empty blob for path {}, key {}", object.local_path, object.remote_path);
                    tx->createMetadataFile(object.local_path, {});
                }
            }
            else
            {
                if (object.bytes_size > 0 || create_blob_if_empty)
                    tx->recordBlobsReplication(object, blob_replication);

                /// Even if not create_blob_if_empty and size is 0, we still need to add metadata just to make sure that a file gets created if this is the 1st append
                tx->addBlobToMetadata(object.local_path, object);
            }
        });

        if (autocommit)
            disk_tx->commit();
    };

    return std::make_unique<WriteBufferWithFinalizeCallback>(std::move(buffer_to_enabled_locations), std::move(create_metadata_callback), object.remote_path, create_blob_if_empty);
}

/// This function is a simplified and adapted version of DiskObjectStorageTransaction::writeFile().
/// TODO: This function is incorrect for multiple object storages, we need to remove it and use everywhere regular writeFile
void DiskObjectStorageTransaction::writeFileUsingBlobWritingFunction(
    const String & path, WriteMode mode, WriteBlobFunction && write_blob_function)
{
    if (cluster->getConfiguration().size() > 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "writeFileUsingBlobWritingFunction not supported for replicated setup");

    StoredObject object(metadata_transaction->generateObjectKeyForPath(path).serialize(), path);
    written_blobs[cluster->getLocalLocation()].push_back(object);

    /// See DiskObjectStorage::getBlobPath().
    Strings blob_path;
    blob_path.reserve(2);
    blob_path.emplace_back(object.remote_path);
    String objects_namespace = object_storages->takePointingTo(cluster->getLocalLocation())->getObjectsNamespace();
    if (!objects_namespace.empty())
        blob_path.emplace_back(objects_namespace);

    /// We always use mode Rewrite because we simulate append using metadata and different files
    object.bytes_size = std::move(write_blob_function)(blob_path, WriteMode::Rewrite, /*object_attributes=*/std::nullopt);

    operations_to_execute.push_back([object, mode](MetadataTransactionPtr tx)
    {
        if (mode == WriteMode::Rewrite)
        {
            LOG_TEST(getLogger("DiskObjectStorageTransaction"), "Writing blob for path {}, key {}, size {}", object.local_path, object.remote_path, object.bytes_size);
            tx->recordBlobsReplication(object, /*missing_locations=*/{});
            tx->createMetadataFile(object.local_path, {object});
        }
        else
        {
            tx->recordBlobsReplication(object, /*missing_locations=*/{});
            tx->addBlobToMetadata(object.local_path, object);
        }
    });
}

void DiskObjectStorageTransaction::createHardLink(const std::string & src_path, const std::string & dst_path)
{
    /// CA read-your-writes: a content-addressed transaction stages a hardlinked file into its per-part
    /// `recorded` map so a reader holding the transaction can resolve it before commit — exactly as
    /// `writeFile` stages eagerly (it has no `operations_to_execute` entry; commit publishes from the
    /// staging). A carried-forward projection is hardlinked into the open whole-part transaction during a
    /// mutation, and `loadProjections` (which runs in the SAME finalize, before commit) must see it via
    /// the directory overlay. Deferring the hardlink to commit replay (the default below) would hide it
    /// until after `loadProjections` ran, so the carried projection registered empty (B58/B63). The
    /// metadata-level `createHardLink` is a map assignment (idempotent), so staging it eagerly and NOT
    /// queuing it is equivalent to the queued replay — commit publishes the manifest from the staging.
    if (metadata_storage->isContentAddressed())
    {
        metadata_transaction->createHardLink(src_path, dst_path);
        return;
    }

    operations_to_execute.push_back([src_path, dst_path](MetadataTransactionPtr tx)
    {
        tx->createHardLink(src_path, dst_path);
    });
}

std::optional<StoredObjects> DiskObjectStorageTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    return metadata_transaction->tryGetInFlightStorageObjects(path);
}

std::unique_ptr<ReadBufferFromFileBase> DiskObjectStorageTransaction::tryReadFileInFlight(
    const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const
{
    return metadata_transaction->tryReadFileInFlight(path, settings, read_hint);
}

std::optional<uint64_t> DiskObjectStorageTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    return metadata_transaction->tryGetInFlightFileSize(path);
}

bool DiskObjectStorageTransaction::hasInFlightDirectory(const std::string & path) const
{
    return metadata_transaction->hasInFlightDirectory(path);
}

std::vector<std::string> DiskObjectStorageTransaction::listInFlightDirectory(const std::string & path) const
{
    return metadata_transaction->listInFlightDirectory(path);
}

void DiskObjectStorageTransaction::setReadOnly(const std::string & path)
{
    operations_to_execute.push_back([path](MetadataTransactionPtr tx)
    {
        tx->setReadOnly(path);
    });
}

void DiskObjectStorageTransaction::setLastModified(const std::string & path, const Poco::Timestamp & timestamp)
{
    operations_to_execute.push_back([path, timestamp](MetadataTransactionPtr tx)
    {
        tx->setLastModified(path, timestamp);
    });
}

void DiskObjectStorageTransaction::chmod(const String & path, mode_t mode)
{
    operations_to_execute.push_back([path, mode](MetadataTransactionPtr tx)
    {
        tx->chmod(path, mode);
    });
}

void DiskObjectStorageTransaction::createFile(const std::string & path)
{
    if (metadata_storage->supportsEmptyFilesWithoutBlobs())
    {
        operations_to_execute.push_back([path](MetadataTransactionPtr tx)
        {
            tx->createMetadataFile(path, /*objects=*/{});
        });
    }
    else
    {
        writeFile(path, 0, WriteMode::Rewrite, getWriteSettings())->finalize();
    }
}

void DiskObjectStorageTransaction::copyFileImpl(
    const MetadataStoragePtr & src_metadata_storage,
    const ClusterConfigurationPtr & src_cluster,
    const ObjectStorageRouterPtr & src_object_storages,
    const std::string & from_file_path,
    const std::string & to_file_path,
    const ReadSettings & read_settings,
    const WriteSettings & write_settings)
{
    /// Share the enriched settings via shared_ptr so each task lambda captures a cheap refcount bump
    /// rather than a full copy of ReadSettings / WriteSettings.
    const auto enriched_read_settings = std::make_shared<const ReadSettings>(
        updateIOSchedulingSettings(read_settings, read_resource_name, write_resource_name));
    const auto enriched_write_settings = std::make_shared<const WriteSettings>(
        updateIOSchedulingSettings(write_settings, read_resource_name, write_resource_name));

    const auto blobs_to_copy = src_metadata_storage->getStorageObjects(from_file_path);
    const auto blobs_to_create = blobs_to_copy
                        | std::views::transform([&](const auto & from) { return StoredObject(metadata_transaction->generateObjectKeyForPath(to_file_path).serialize(), to_file_path, from.bytes_size); })
                        | std::ranges::to<StoredObjects>();

    const auto locations_for_writing = cluster->getEnabledLocations();
    const auto missing_locations = cluster->findComplement(locations_for_writing);
    const auto src_local_location = src_cluster->getLocalLocation();

    /// Pre-populate `written_blobs` sequentially so the parallel section does not race on it.
    for (const auto & location : locations_for_writing)
        for (const auto & dst_blob : blobs_to_create)
            written_blobs[location].push_back(dst_blob);

    /// Dispatch `copyObjectToAnotherObjectStorage` calls in parallel onto the disk-level pool.
    /// We can't reuse `IObjectStorage::getThreadPoolWriter()` here because the copy implementation
    /// itself submits onto that pool via `writeObject`, which would risk pool self-deadlock.
    /// `ThreadPoolCallbackRunnerLocal` drains tasks in its destructor, so on exception unwinding
    /// the captured state stays alive until in-flight workers complete.
    ThreadPoolCallbackRunnerLocal<void> runner(*copy_object_pool, ThreadName::DISK_OBJECT_STORAGE_COPY);

    for (const auto & location : locations_for_writing)
    {
        for (const auto [src_blob, dst_blob] : std::views::zip(blobs_to_copy, blobs_to_create))
        {
            runner.enqueueAndKeepTrack(
                [this, src_object_storages, src_blob, dst_blob, location, src_local_location, enriched_read_settings, enriched_write_settings]
                {
                    src_object_storages->takePointingTo(src_local_location)->copyObjectToAnotherObjectStorage(
                        src_blob, dst_blob, *enriched_read_settings, *enriched_write_settings, *object_storages->takePointingTo(location));
                });
        }
    }

    runner.waitForAllToFinishAndRethrowFirstError();

    operations_to_execute.push_back([blobs_to_create, missing_locations, to_file_path](MetadataTransactionPtr tx)
    {
        for (const auto & blob : blobs_to_create)
            tx->recordBlobsReplication(blob, missing_locations);

        tx->createMetadataFile(to_file_path, blobs_to_create);
    });
}

void DiskObjectStorageTransaction::copyFile(const std::string & from_file_path, const std::string & to_file_path, const ReadSettings & read_settings, const WriteSettings & write_settings)
{
    copyFileImpl(metadata_storage, cluster, object_storages, from_file_path, to_file_path, read_settings, write_settings);
}

void MultipleDisksObjectStorageTransaction::copyFile(const std::string & from_file_path, const std::string & to_file_path, const ReadSettings & read_settings, const WriteSettings & write_settings)
{
    copyFileImpl(source_metadata_storage, source_cluster, source_object_storages, from_file_path, to_file_path, read_settings, write_settings);
}

void DiskObjectStorageTransaction::commit()
{
    auto component_guard = Coordination::setCurrentComponent("DiskObjectStorageTransaction::commit");
    for (size_t i = 0; i < operations_to_execute.size(); ++i)
    {
        try
        {
            operations_to_execute[i](metadata_transaction);
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("DiskObjectStorageTransaction"),
                fmt::format("An error occurred while executing transaction's operation #{}", i));

            undo();

            throw;
        }
    }

    try
    {
        fiu_do_on(FailPoints::disk_object_storage_fail_commit_metadata_transaction,
        {
            throw Exception(ErrorCodes::FAULT_INJECTED, "disk_object_storage_fail_commit_metadata_transaction");
        });

        metadata_transaction->commit(NoCommitOptions{});
    }
    catch (...)
    {
        undo();
        throw;
    }

    if (wait_blob_removal)
        waitBlobRemoval(metadata_transaction->getSubmittedForRemovalBlobs());

    operations_to_execute.clear();
    written_blobs.clear();
    LOG_TEST(getLogger("DiskObjectStorageTransaction"), "Transaction committed successfully");
}

TransactionCommitOutcomeVariant DiskObjectStorageTransaction::tryCommit(const TransactionCommitOptionsVariant & options)
{
    for (size_t i = 0; i < operations_to_execute.size(); ++i)
    {
        try
        {
            operations_to_execute[i](metadata_transaction);

            fiu_do_on(FailPoints::disk_object_storage_fail_precommit_metadata_transaction,
            {
                throw Coordination::Exception(Coordination::Error::ZOPERATIONTIMEOUT, "disk_object_storage_fail_precommit_metadata_transaction");
            });
        }
        catch (Exception & ex)
        {
            /// Reset metadata transaction, it will be refilled in operations_to_execute[i]->execute on the next retry if needed
            metadata_transaction = metadata_storage->createTransaction();

            ex.addMessage(fmt::format("While executing operation #{}", i));

            if (needRollbackBlobs(options))
                undo();

            throw;
        }
    }

    // disk_object_storage_fail_commit_metadata_transaction injects a fake hardware error before the commit attempt
    TransactionCommitOutcomeVariant outcome;
    fiu_do_on(FailPoints::disk_object_storage_fail_commit_metadata_transaction,
    {
        MetaInKeeperCommitOutcome result;
        result.code = Coordination::Error::ZOPERATIONTIMEOUT;
        outcome = result;
        LOG_ERROR(getLogger("DiskObjectStorageTransaction"), "Failpoint smt_insert_fake_hardware_error triggered");
    });

    if (std::get_if<MetaInKeeperCommitOutcome>(&outcome) == nullptr)
        outcome = metadata_transaction->tryCommit(options);

    // smt_insert_fake_hardware_error injects a fake hardware error after the commit attempt
    fiu_do_on(FailPoints::smt_insert_fake_hardware_error,
    {
        auto * result = std::get_if<MetaInKeeperCommitOutcome>(&outcome);
        result->code = Coordination::Error::ZOPERATIONTIMEOUT;
    });

    if (!isSuccessfulOutcome(outcome))
    {
        /// Reset metadata transaction, it will be refilled in operations_to_execute[i]->execute on the next retry if needed
        metadata_transaction = metadata_storage->createTransaction();

        if (canRollbackBlobs(options, outcome))
        {
            undo();
        }
        else
        {
            LOG_DEBUG(getLogger("DiskObjectStorageTransaction"),
                "Commit failed, but rollback of blobs is not needed. "
                "Transaction will be retried without rolling back blobs.");
        }

        return outcome;
    }

    if (wait_blob_removal)
        waitBlobRemoval(metadata_transaction->getSubmittedForRemovalBlobs());

    operations_to_execute.clear();
    written_blobs.clear();
    LOG_TEST(getLogger("DiskObjectStorageTransaction"), "Transaction committed successfully");

    return outcome;
}

void DiskObjectStorageTransaction::undo() noexcept
{
    for (const auto & [location, blobs] : written_blobs)
    {
        try
        {
            object_storages->takePointingTo(location)->removeObjectsIfExist(blobs);
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("DiskObjectStorageTransaction"), fmt::format("An error occurred during transaction cleanup from location '{}'", location));
        }
    }

    operations_to_execute.clear();
    written_blobs.clear();
}

}
