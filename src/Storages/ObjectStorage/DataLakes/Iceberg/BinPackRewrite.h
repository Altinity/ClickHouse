#pragma once

#include "config.h"

#if USE_AVRO

#include <Interpreters/Context_fwd.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadataFilesCache.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>

namespace DB
{
struct SecondaryStorages;
}

namespace DB::Iceberg
{

enum class BinPackCommitResult : uint8_t
{
    Committed,
    /// Another writer committed first, or the catalog did not accept the snapshot; the attempt can be retried.
    Conflict,
};

/// Whether the latest snapshot has live position delete files. Bin-packing leaves the files they
/// apply to untouched, so such tables are compacted by `compactIcebergTable` instead.
/// When `catalog` is set, the latest snapshot is the one the catalog points to.
bool hasLivePositionDeletes(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context,
    const std::shared_ptr<DataLake::ICatalog> & catalog,
    const StorageID & storage_id);

/// Execute bin-packing compaction for an Iceberg table: merge small data files into
/// larger ones, producing a `replace` snapshot that atomically swaps the old files
/// for the merged results.  Only data files smaller than `iceberg_min_data_file_size_bytes`
/// are candidates; each bin targets `iceberg_target_data_file_size_bytes`.
/// Files are rewritten in the current schema of the latest metadata.
///
/// Leaves all other files, manifests, and snapshot history untouched.
///
/// When `catalog` is set, the new metadata is committed to it as well.
///
/// Throws, without removing the files it wrote, when the outcome of the metadata commit cannot be determined.
BinPackCommitResult executeBinPackCompaction(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const StorageID & storage_id);

}

#endif
