#pragma once

#include "config.h"

#if USE_AVRO

#include <Databases/DataLake/ICatalog.h>
#include <Interpreters/Context_fwd.h>
#include <Parsers/IAST_fwd.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ExternalPathResolver.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include <Storages/ObjectStorage/Utils.h>

namespace DB::Iceberg
{

/// Result of an add_files execution, surfaced as (metric_name, metric_value) rows.
struct AddFilesResult
{
    Int64 files_registered = 0;
    Int64 rows_registered = 0;
    Int64 bytes_registered = 0;
    Int64 partitions_touched = 0;
    Int64 manifests_written = 0;
};

/// Register existing Parquet files as a new append snapshot, like iceberg-java's
/// `add_files`. Each file is read once (footer only) to derive its record count,
/// column sizes, null counts and bounds, and to prove that it belongs to a single
/// partition of the table's current default partition spec.
AddFilesResult addFiles(
    const std::vector<String> & paths,
    bool check_duplicate_files,
    bool allow_missing_columns,
    ContextPtr context,
    ObjectStoragePtr object_storage,
    const DataLakeStorageSettings & data_lake_settings,
    const PersistentTableComponents & persistent_table_components,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const String & table_name,
    ExternalStorageCache & external_storages);

Pipe executeAddFiles(
    const ASTPtr & args,
    ContextPtr context,
    ObjectStoragePtr object_storage,
    const DataLakeStorageSettings & data_lake_settings,
    const PersistentTableComponents & persistent_components,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const String & table_name,
    ExternalStorageCache & external_storages);

}

#endif
