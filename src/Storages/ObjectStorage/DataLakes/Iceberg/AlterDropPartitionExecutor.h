#pragma once

#include <Storages/ObjectStorage/DataLakes/Iceberg/SnapshotSummary.h>
#include "config.h"

#if USE_AVRO

#include <Core/Field.h>
#include <Databases/DataLake/ICatalog.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/StorageID.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/FileNamesGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergPath.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergTableStateSnapshot.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFile.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MetadataGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Snapshot.h>
#include <Storages/PartitionCommands.h>
#include <Common/Logger.h>

#include <Poco/JSON/Object.h>

#include <optional>
#include <unordered_set>
#include <vector>

namespace DB
{

class FileNamesGenerator;
class IcebergMetadata;
struct ExternalStorageCache;

namespace Iceberg
{

struct PartitionTerm
{
    Int32 column_id;
    String transform;
};

using PartitionSpecSignature = std::vector<PartitionTerm>;

class AlterDropPartitionExecutor
{
public:
    AlterDropPartitionExecutor(
        const PartitionCommand & command_,
        const IcebergMetadata & metadata_,
        ContextPtr context_,
        ObjectStoragePtr object_storage_,
        ExternalStorageCache & external_storages_,
        const PersistentTableComponents & components_,
        const DataLakeStorageSettings & data_lake_settings_,
        String write_format_,
        LoggerPtr log_,
        std::shared_ptr<DataLake::ICatalog> catalog_,
        const StorageID & storage_id_);

    void run();

private:
    struct SnapshotState
    {
        IcebergDataSnapshotPtr snapshot;
        TableStateSnapshot table_state;
        Poco::JSON::Object::Ptr metadata_object;
        Poco::JSON::Object::Ptr partition_spec;
        Int64 partition_spec_id = 0;
        Int32 schema_id = 0;
        std::vector<String> partition_columns;
        DataTypes partition_types;
        PartitionSpecSignature spec_signature;
    };

    using SpecFieldPositions = std::unordered_map<const PartitionSpecification *, std::vector<size_t>>;

    struct TargetManifest
    {
        ManifestFileCacheKey manifest_key;
        std::unordered_set<String> excluded_file_paths;
    };

    struct TargetManifests
    {
        std::vector<TargetManifest> fully_matched;
        std::vector<TargetManifest> partially_matched;

        bool empty() const { return fully_matched.empty() && partially_matched.empty(); }
    };

    struct DropPlan
    {
        TargetManifests target_manifests;
        Iceberg::SnapshotSummaryUpdateDelete snapshot_summary_update;
        std::vector<Iceberg::IcebergPathFromMetadata> files_to_be_purged;
    };

    using TargetFilePaths = std::unordered_set<String>;

    std::optional<SnapshotState> fetchSnapshotState();

    const std::vector<size_t> & getTargetFieldPositions(
        const ProcessedManifestFileEntry & entry, const SnapshotState & state, SpecFieldPositions & spec_field_positions) const;
    bool isInTargetPartition(
        const ProcessedManifestFileEntry & entry,
        const SnapshotState & state,
        const Row & target_partition,
        SpecFieldPositions & spec_field_positions) const;

    TargetFilePaths discoverTargetFilePaths(const SnapshotState & state, const Row & target_partition) const;
    DropPlan buildDropPlan(const SnapshotState & state, const TargetFilePaths & targets, const Row & target_partition) const;
    bool tryCommit(SnapshotState & state, const DropPlan & plan);

    struct ManifestListWriteResult
    {
        GeneratedMetadataFileWithInfo metadata_info;
        Poco::JSON::Object::Ptr new_snapshot;
    };

    ManifestListWriteResult writeManifestList(
        SnapshotState & state, const DropPlan & plan, FileNamesGenerator & filename_generator, std::vector<String> & files_for_cleanup);

    PreviousManifestActions rewritePartiallyMatchedManifests(
        const DropPlan & plan, FileNamesGenerator & filename_generator, std::vector<String> & files_for_cleanup);

    bool commitMetadataJSON(
        SnapshotState & state,
        FileNamesGenerator & filename_generator,
        const ManifestListWriteResult & manifest_list_result,
        std::vector<String> & files_for_cleanup,
        bool & published,
        bool * version_hint_names_metadata_file);

    bool catalogCommittedSnapshot(Int64 snapshot_id) const;

    void cleanupNotCommited(std::vector<std::string> files);

    void checkPurgeIsGranted(const DropPlan & plan) const;

    void purgeDroppedFiles(const DropPlan & plan, const GeneratedMetadataFileWithInfo & metadata_info, bool version_hint_names_metadata_file);

    const PartitionCommand & command;
    const IcebergMetadata & metadata;
    ContextPtr context;
    ObjectStoragePtr object_storage;
    ExternalStorageCache & external_storages;
    const PersistentTableComponents & components;
    const DataLakeStorageSettings & data_lake_settings;
    String write_format;
    LoggerPtr log;
    std::shared_ptr<DataLake::ICatalog> catalog;
    StorageID storage_id;
    bool purge = false;
};

}
}

#endif
