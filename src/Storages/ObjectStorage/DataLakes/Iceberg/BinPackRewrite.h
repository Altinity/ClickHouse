#pragma once

#include "config.h"

#if USE_AVRO

#include <Interpreters/Context_fwd.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadataFilesCache.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>

namespace DataLake
{
class ICatalog;
}

namespace DB
{
struct SecondaryStorages;
}

namespace DB::Iceberg
{

/// Execute bin-packing compaction for an Iceberg table: merge small data files into
/// larger ones, producing a `replace` snapshot that atomically swaps the old files
/// for the merged results.  Only data files smaller than `iceberg_min_data_file_size_bytes`
/// are candidates; each bin targets `iceberg_target_data_file_size_bytes`.
///
/// Leaves all other files, manifests, and snapshot history untouched.
///
/// Returns true on successful commit, false on commit conflict (caller should retry).
bool executeBinPackCompaction(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    SharedHeader sample_block,
    ContextPtr context,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const StorageID & table_id);

}

#endif
