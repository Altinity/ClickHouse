#pragma once

#include "config.h"

#if USE_AVRO

#include <unordered_set>
#include <utility>
#include <vector>

#include <Common/Logger_fwd.h>
#include <Core/Types.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <Interpreters/Context_fwd.h>
#include <Poco/JSON/Array.h>

#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergPath.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ExternalPathResolver.h>

namespace DB::Iceberg
{

/// Paths collected from Iceberg snapshot metadata, stored as opaque
/// IcebergPathFromMetadata values that must be resolved through
/// IcebergPathResolver before I/O operations.
struct SnapshotReferencedFiles
{
    std::unordered_set<IcebergPathFromMetadata> manifest_list_paths;
    std::unordered_set<IcebergPathFromMetadata> manifest_paths;
    std::unordered_set<IcebergPathFromMetadata> data_file_paths;
};

SnapshotReferencedFiles collectSnapshotReferencedFiles(
    const Poco::JSON::Array::Ptr & snapshots,
    ObjectStoragePtr object_storage,
    const PersistentTableComponents & persistent_table_components,
    ContextPtr context,
    LoggerPtr log,
    Int32 current_schema_id,
    ExternalStorageCache & external_storages);

struct ExternalFile
{
    ObjectStoragePtr storage;
    String key;
    IcebergPathFromMetadata path;
};

struct ReachableFilesResult
{
    std::unordered_set<String> files;
    Int32 metadata_version;
    std::vector<ExternalFile> external_files;
};

/// Collect all files reachable through the metadata graph.
///
/// Traverses: metadata JSON files (from metadata-log), manifest lists (from snapshots),
/// manifest files (from manifest lists), data/delete files (from manifest files),
/// and statistics files. Paths inside the table directory are returned as resolved storage
/// paths in `files`; files outside it (on any storage) are returned in `external_files`.
/// Also returns the metadata version used, for TOCTOU detection.
/// `scan_metadata_log_history` adds surviving external references without making expired in-table files reachable.
/// `ignore_explicit_metadata_file_path` selects the latest head (for `remove_orphan_files`) instead of the configured one (for `drop`).
ReachableFilesResult collectReachableFiles(
    ObjectStoragePtr object_storage,
    const PersistentTableComponents & persistent_table_components,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context,
    LoggerPtr log,
    ExternalStorageCache & external_storages,
    bool scan_metadata_log_history,
    bool ignore_explicit_metadata_file_path);

}

#endif
