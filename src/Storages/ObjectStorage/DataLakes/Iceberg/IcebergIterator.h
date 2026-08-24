#pragma once
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include "config.h"

#if USE_AVRO

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <Core/Types.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Interpreters/Context_fwd.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFileIterator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Snapshot.h>

#include <Common/ConcurrentBoundedQueue.h>
#include <Common/ThreadPool_fwd.h>

#include <atomic>
#include <optional>
#include <base/defines.h>

#include <Core/BackgroundSchedulePool.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataObjectInfo.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadataFilesCache.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergTableStateSnapshot.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PositionDeleteTransform.h>
#include <Storages/ObjectStorage/Utils.h>

namespace DB
{

namespace Iceberg
{

class SingleThreadIcebergKeysIterator
{
public:
    SingleThreadIcebergKeysIterator(
        ObjectStoragePtr object_storage_,
        ContextPtr local_context_,
        Iceberg::ManifestFileContentType manifest_file_content_type_,
        const ActionsDAG * filter_dag_,
        TableStateSnapshotPtr table_snapshot_,
        IcebergDataSnapshotPtr data_snapshot_,
        PersistentTableComponents persistent_components,
        std::shared_ptr<SecondaryStorages> secondary_storages_);

    std::optional<DB::Iceberg::ProcessedManifestFileEntryPtr> next();

    /// Grab the next manifest file of the matching content type and return an iterator over its
    /// entries, or nullptr when no manifest files are left. Thread-safe: the cursor is an atomic, so
    /// several parallel workers can call this concurrently and each gets a distinct manifest file,
    /// fetching (the S3 read) and pruning it independently - the manifest reads themselves run in
    /// parallel rather than being serialized behind a shared advance lock.
    Iceberg::ManifestIteratorPtr nextManifestFile();

private:
    ObjectStoragePtr object_storage;
    std::shared_ptr<const ActionsDAG> filter_dag;
    ContextPtr local_context;
    Iceberg::TableStateSnapshotPtr table_snapshot;
    Iceberg::IcebergDataSnapshotPtr data_snapshot;
    PersistentTableComponents persistent_components;
    LoggerPtr log;

    std::shared_ptr<SecondaryStorages> secondary_storages;

    /// Atomic so parallel workers (see IcebergIterator) can pull distinct manifest files without a
    /// lock; the legacy single-threaded next() path uses it too (uncontended).
    std::atomic<size_t> manifest_file_index{0};
    Iceberg::ManifestIteratorPtr current_manifest_file_iterator;

    const Iceberg::ManifestFileContentType manifest_file_content_type;
};

}

class IcebergIterator : public IObjectIterator
{
public:
    explicit IcebergIterator(
        ObjectStoragePtr object_storage_,
        ContextPtr local_context_,
        const ActionsDAG * filter_dag_,
        IDataLakeMetadata::FileProgressCallback callback_,
        Iceberg::TableStateSnapshotPtr table_snapshot_,
        Iceberg::IcebergDataSnapshotPtr data_snapshot_,
        Iceberg::PersistentTableComponents persistent_components_,
        std::shared_ptr<SecondaryStorages> secondary_storages_);

    ObjectInfoPtr next(size_t) override;

    size_t estimatedKeysCount() override;
    ~IcebergIterator() override;

private:
    LoggerPtr logger;
    std::shared_ptr<ActionsDAG> filter_dag;
    ObjectStoragePtr object_storage;
    ContextPtr local_context;
    const Iceberg::TableStateSnapshotPtr table_state_snapshot;
    Iceberg::PersistentTableComponents persistent_components;
    Iceberg::SingleThreadIcebergKeysIterator data_files_iterator;
    Iceberg::SingleThreadIcebergKeysIterator deletes_iterator;
    ConcurrentBoundedQueue<Iceberg::ProcessedManifestFileEntryPtr> blocking_queue;
    /// Legacy single-threaded producer (iceberg_metadata_processing_threads == 1).
    std::unique_ptr<ThreadFromGlobalPool> producer_task;
    /// Parallel producers (iceberg_metadata_processing_threads > 1). Each worker pulls a distinct
    /// manifest file from `data_files_iterator` (thread-safe atomic cursor), reads and drains it
    /// independently, then pulls the next - so the per-manifest S3 reads run in parallel.
    std::vector<ThreadFromGlobalPool> producer_threads;
    /// Number of parallel producer threads still running; the last one to finish closes the queue.
    std::atomic<size_t> producers_remaining{0};
    IDataLakeMetadata::FileProgressCallback callback;
    std::vector<Iceberg::ProcessedManifestFileEntryPtr> deletion_vector_files;
    std::vector<Iceberg::ProcessedManifestFileEntryPtr> parquet_position_deletes_files;
    std::vector<Iceberg::ProcessedManifestFileEntryPtr> equality_deletes_files;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::shared_ptr<SecondaryStorages> secondary_storages;  // Sometimes data or manifests can be located on another storage
    Int32 table_schema_id;
};
}


#endif
