#include <Storages/ObjectStorage/DataLakes/Iceberg/BinPackRewrite.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <Core/Settings.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatParserSharedResources.h>
#include <IO/CompressionMethod.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/ProcessList.h>
#include <Storages/ObjectStorage/DataLakes/Common/Common.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ChunkPartitioner.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Compaction.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/FileNamesGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MetadataGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MultipleFileWriter.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/StatelessMetadataFileGetter.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/String.h>
#include <Common/FieldVisitorDump.h>
#include <Common/Logger.h>

#if USE_AVRO

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_WRITE_TO_FILE_BUFFER;
    extern const int LOGICAL_ERROR;
    extern const int ICEBERG_SPECIFICATION_VIOLATION;
    extern const int QUERY_WAS_CANCELLED;
}

namespace DB::Setting
{
    extern const SettingsUInt64 iceberg_target_data_file_size_bytes;
    extern const SettingsUInt64 iceberg_min_data_file_size_bytes;
}

namespace DB::DataLakeStorageSetting
{
    extern const DataLakeStorageSettingsBool iceberg_use_version_hint;
}

namespace DB::Iceberg
{

namespace
{

SharedHeader makeHeaderFromSchema(IcebergSchemaProcessor & schema_processor, Int32 schema_id)
{
    Block header;
    for (const auto & column : *schema_processor.getClickhouseTableSchemaById(schema_id))
        header.insert(ColumnWithTypeAndName(column.type->createColumn(), column.type, column.name));
    return std::make_shared<const Block>(std::move(header));
}

/// A single live data file recorded from a data manifest.
struct DataFileRecord
{
    IcebergPathFromMetadata file_path;
    Int64 record_count;
    Int64 file_size_in_bytes;
    String file_format;
    /// The schema the file was written with.
    Int32 schema_id = 0;
    Row partition_key;
    std::optional<Int32> sort_order_id;
    /// Lineage from the source manifest entry.
    std::optional<Int64> snapshot_id;
    std::optional<Int64> sequence_number;
    std::optional<Int64> file_sequence_number;
    /// Per-column statistics from the source manifest.
    DataFileColumnStatistics column_stats;
    /// The manifest this entry was read from.
    String source_manifest_path;
    /// Whether this file may be rewritten: small enough and no live delete file can apply to it.
    bool is_candidate = false;
};

/// A live delete-file entry from a delete manifest. Bin-packing must not rewrite a data file
/// the delete could still apply to: rewriting changes the file's path and re-stamps its
/// sequence numbers, so the carried-forward delete would silently stop applying.
struct DeleteRecord
{
    FileContentType content_type;
    /// Resolved data sequence number of the delete entry.
    Int64 sequence_number;
    Row partition_key;
    /// Partition spec the delete manifest was written with. When it differs from the table's
    /// default spec, the partition value is not comparable and the delete is treated as global.
    Int32 partition_spec_id;
    /// For position deletes / deletion vectors that reference exactly one data file.
    std::optional<IcebergPathFromMetadata> lower_reference_data_file_path;
    std::optional<IcebergPathFromMetadata> upper_reference_data_file_path;
};

/// A bin: a group of small files from the same partition to be merged.
struct Bin
{
    Row partition_key;
    std::vector<DataFileRecord> files;
    Int64 total_bytes = 0;
    Int64 total_records = 0;
};

/// Key for grouping files by partition.
struct PartitionKeyHash
{
    std::hash<String> hasher;
    size_t operator()(const Row & row) const
    {
        size_t result = 0;
        FieldVisitorDump dump_visitor;
        for (const auto & value : row)
            result ^= hasher(applyVisitor(dump_visitor, value));
        return result;
    }
};

struct PartitionKeyEqual
{
    bool operator()(const Row & a, const Row & b) const
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }
};

/// The plan: which files to rewrite, which manifests to keep.
struct BinPackPlan
{
    /// Bins of small files to merge.
    std::vector<Bin> bins;
    /// Manifest paths to carry forward unchanged (delete manifests, manifests written under a
    /// non-default partition spec, and data manifests with no rewritten files).
    std::unordered_set<String> carry_forward_manifest_paths;
    /// Live data files from manifests that also contain rewritten files. They are not merged
    /// (large files, single-file partitions, files excluded because of deletes) and stay in the
    /// new snapshot as EXISTING entries, grouped by partition key.
    std::unordered_map<Row, std::vector<DataFileRecord>, PartitionKeyHash, PartitionKeyEqual> kept_groups;
    /// Total statistics across removed files.
    Int64 removed_data_files = 0;
    Int64 removed_records = 0;
    Int64 removed_files_size = 0;
    /// Number of distinct partitions affected.
    Int64 num_partitions = 0;
    /// The current snapshot id to use as parent.
    Int64 current_snapshot_id = -1;
    /// The partition spec to use for new manifests.
    Poco::JSON::Object::Ptr partition_spec;
    Int64 partition_spec_id = 0;
    std::vector<String> partition_columns;
    DataTypes partition_types;
};


BinPackPlan buildBinPackPlan(
    Poco::JSON::Object::Ptr metadata_object,
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    ContextPtr context,
    UInt64 min_file_size,
    UInt64 target_file_size)
{
    LoggerPtr log = getLogger("IcebergBinPack::buildPlan");
    BinPackPlan plan;

    if (!metadata_object->has(f_current_snapshot_id))
        return plan;
    Int64 current_snapshot_id = metadata_object->getValue<Int64>(f_current_snapshot_id);
    if (current_snapshot_id < 0)
        return plan;
    plan.current_snapshot_id = current_snapshot_id;

    String current_manifest_list_path;
    auto snapshots = metadata_object->get(f_snapshots).extract<Poco::JSON::Array::Ptr>();
    for (size_t i = 0; i < snapshots->size(); ++i)
    {
        const auto snapshot = snapshots->getObject(static_cast<UInt32>(i));
        if (snapshot->getValue<Int64>(f_metadata_snapshot_id) == current_snapshot_id)
        {
            current_manifest_list_path = snapshot->getValue<String>(f_manifest_list);
            break;
        }
    }
    if (current_manifest_list_path.empty())
        return plan;

    auto current_schema_id = metadata_object->getValue<Int64>(f_current_schema_id);

    /// Resolve partition spec.
    auto partition_spec_id = metadata_object->getValue<Int32>(f_default_spec_id);
    auto partitions_specs = metadata_object->getArray(f_partition_specs);
    Poco::JSON::Object::Ptr partition_spec;
    for (size_t i = 0; i < partitions_specs->size(); ++i)
    {
        auto candidate = partitions_specs->getObject(static_cast<UInt32>(i));
        if (candidate->getValue<Int64>(f_spec_id) == partition_spec_id)
        {
            partition_spec = candidate;
            break;
        }
    }
    if (!partition_spec)
        throw Exception(
            ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "Iceberg metadata does not contain partition spec matching default-spec-id {}",
            partition_spec_id);

    plan.partition_spec = partition_spec;
    plan.partition_spec_id = partition_spec_id;

    /// Spec ids with no partition fields; deletes written under them apply to every partition.
    std::unordered_set<Int32> unpartitioned_spec_ids;
    for (UInt32 i = 0; i < partitions_specs->size(); ++i)
    {
        auto candidate = partitions_specs->getObject(static_cast<UInt32>(i));
        if (candidate->getArray(f_fields)->size() == 0)
            unpartitioned_spec_ids.insert(candidate->getValue<Int32>(f_spec_id));
    }

    auto spec_fields = partition_spec->getArray(f_fields);
    std::vector<String> partition_columns;
    for (UInt32 i = 0; i < spec_fields->size(); ++i)
        partition_columns.push_back(spec_fields->getObject(i)->getValue<String>(f_name));
    plan.partition_columns = partition_columns;

    /// Resolve partition types.
    auto schemas = metadata_object->getArray(f_schemas);
    Poco::JSON::Object::Ptr current_schema;
    for (size_t i = 0; i < schemas->size(); ++i)
    {
        if (schemas->getObject(static_cast<UInt32>(i))->getValue<Int32>(f_schema_id) == current_schema_id)
        {
            current_schema = schemas->getObject(static_cast<UInt32>(i));
            break;
        }
    }
    if (!current_schema)
        throw Exception(
            ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "Iceberg metadata does not contain schema matching current-schema-id {}",
            current_schema_id);

    /// Build partition types from the partitioner.
    for (UInt32 i = 0; i < schemas->size(); ++i)
        persistent_table_components.schema_processor->addIcebergTableSchema(schemas->getObject(i), context);

    auto fields_characteristics = persistent_table_components.schema_processor->tryGetFieldsCharacteristics(
        static_cast<Int32>(current_schema_id), {});
    Block spec_sample_block;
    for (const auto & nt : fields_characteristics)
        spec_sample_block.insert(ColumnWithTypeAndName(nt.type, nt.name));
    auto shared_sample = std::make_shared<const Block>(std::move(spec_sample_block));
    if (!partition_columns.empty())
        plan.partition_types = ChunkPartitioner(spec_fields, current_schema->getArray(f_fields), context, shared_sample).getResultTypes();

    /// Scan the current manifest list.
    auto manifest_list = getManifestList(
        object_storage, persistent_table_components, context,
        IcebergPathFromMetadata::deserialize(current_manifest_list_path),
        log, secondary_storages);

    /// Every live data file, grouped by the manifest that lists it.
    std::unordered_map<String, std::vector<DataFileRecord>> files_by_manifest;
    /// Live delete entries, used to keep the rewrite away from files they can apply to.
    std::vector<DeleteRecord> delete_records;
    size_t small_files_total = 0;

    for (const auto & manifest_file : manifest_list)
    {
        if (manifest_file.content_type == ManifestFileContentType::DELETE)
        {
            plan.carry_forward_manifest_paths.insert(manifest_file.manifest_file_path.serialize());

            /// Record live delete entries so data files they can apply to are left out of the rewrite.
            auto deletes_handle = getManifestFileEntriesHandle(
                object_storage, persistent_table_components, context, log,
                manifest_file, static_cast<Int32>(current_schema_id), secondary_storages);

            for (const auto delete_content_type : {FileContentType::POSITION_DELETE, FileContentType::EQUALITY_DELETE})
            {
                for (const auto & delete_file : deletes_handle.getFilesWithoutDeleted(delete_content_type))
                {
                    const auto & entry = delete_file->parsed_entry;
                    DeleteRecord record;
                    record.content_type = delete_content_type;
                    record.sequence_number = delete_file->sequence_number;
                    record.partition_key = entry->partition_key_value;
                    record.partition_spec_id = manifest_file.partition_spec_id;
                    record.lower_reference_data_file_path = entry->lower_reference_data_file_path;
                    record.upper_reference_data_file_path = entry->upper_reference_data_file_path;
                    delete_records.push_back(std::move(record));
                }
            }
            continue;
        }

        if (manifest_file.partition_spec_id != partition_spec_id)
        {
            /// New manifests are written with the default partition spec and a single partition
            /// tuple per manifest; entries written under an older spec would be recorded with
            /// wrong partition values, so such manifests are carried forward unchanged.
            plan.carry_forward_manifest_paths.insert(manifest_file.manifest_file_path.serialize());
            continue;
        }

        auto files_handle = getManifestFileEntriesHandle(
            object_storage, persistent_table_components, context, log,
            manifest_file, static_cast<Int32>(current_schema_id), secondary_storages);

        auto & manifest_records = files_by_manifest[manifest_file.manifest_file_path.serialize()];
        for (const auto & data_file : files_handle.getFilesWithoutDeleted(FileContentType::DATA))
        {
            const auto & entry = data_file->parsed_entry;
            DataFileRecord record;
            record.file_path = entry->file_path_key;
            record.record_count = entry->record_count;
            record.file_size_in_bytes = entry->file_size_in_bytes;
            record.file_format = entry->file_format;
            record.schema_id = data_file->resolved_schema_id;
            record.partition_key = entry->partition_key_value;
            record.sort_order_id = entry->sort_order_id;
            record.snapshot_id = entry->parsed_snapshot_id;
            if (!record.snapshot_id.has_value())
                record.snapshot_id = manifest_file.added_snapshot_id;
            record.sequence_number = entry->parsed_sequence_number;
            if (!record.sequence_number.has_value())
                record.sequence_number = manifest_file.added_sequence_number;
            record.file_sequence_number = entry->parsed_file_sequence_number;
            if (!record.file_sequence_number.has_value())
                record.file_sequence_number = manifest_file.added_sequence_number;

            /// Carry over per-column stats.
            for (const auto & [field_id, col_info] : entry->columns_infos)
            {
                if (col_info.bytes_size.has_value())
                    record.column_stats.column_sizes.emplace_back(field_id, *col_info.bytes_size);
                if (col_info.rows_count.has_value())
                    record.column_stats.value_counts.emplace_back(field_id, *col_info.rows_count);
                if (col_info.nulls_count.has_value())
                    record.column_stats.null_value_counts.emplace_back(field_id, *col_info.nulls_count);
            }
            for (const auto & [field_id, bounds] : entry->value_bounds)
            {
                if (!bounds.first.isNull())
                    record.column_stats.lower_bounds.emplace_back(field_id, bounds.first.safeGet<String>());
                if (!bounds.second.isNull())
                    record.column_stats.upper_bounds.emplace_back(field_id, bounds.second.safeGet<String>());
            }

            record.source_manifest_path = manifest_file.manifest_file_path.serialize();
            record.is_candidate = static_cast<UInt64>(entry->file_size_in_bytes) < min_file_size;
            if (record.is_candidate)
                ++small_files_total;
            manifest_records.push_back(std::move(record));
        }
    }

    /// Exclude from the rewrite every small file that a live delete could still apply to,
    /// following the Iceberg scan-planning rules: a position delete applies to data files with
    /// data sequence number <= its own, an equality delete to those strictly lower.
    size_t excluded_by_deletes = 0;
    if (!delete_records.empty())
    {
        for (auto & [manifest_path, records] : files_by_manifest)
        {
            for (auto & record : records)
            {
                if (!record.is_candidate)
                    continue;

                const Int64 data_sequence_number = record.sequence_number.value_or(0);
                for (const auto & delete_record : delete_records)
                {
                    const bool sequence_applies = delete_record.content_type == FileContentType::EQUALITY_DELETE
                        ? data_sequence_number < delete_record.sequence_number
                        : data_sequence_number <= delete_record.sequence_number;
                    if (!sequence_applies)
                        continue;

                    /// Partition match is checked only when the delete is partitioned under the
                    /// table's default spec. A delete under an unpartitioned spec is global, and
                    /// a delete under a different partitioned spec cannot be compared by
                    /// partition value; both are conservatively treated as matching.
                    if (!unpartitioned_spec_ids.contains(delete_record.partition_spec_id)
                        && delete_record.partition_spec_id == partition_spec_id
                        && !PartitionKeyEqual{}(delete_record.partition_key, record.partition_key))
                        continue;

                    /// A position delete or deletion vector naming exactly one data file applies
                    /// only to that file.
                    if (delete_record.content_type == FileContentType::POSITION_DELETE
                        && delete_record.lower_reference_data_file_path.has_value()
                        && delete_record.upper_reference_data_file_path.has_value()
                        && *delete_record.lower_reference_data_file_path == *delete_record.upper_reference_data_file_path
                        && *delete_record.lower_reference_data_file_path != record.file_path)
                        continue;

                    record.is_candidate = false;
                    ++excluded_by_deletes;
                    break;
                }
            }
        }
        if (excluded_by_deletes > 0)
            LOG_INFO(log, "Excluded {} small files from bin-packing because live delete files may apply to them", excluded_by_deletes);
    }

    /// Group candidate files into bins per partition (by pointer; the records stay in
    /// files_by_manifest until the plan is materialized below).
    std::unordered_map<Row, std::vector<const DataFileRecord *>, PartitionKeyHash, PartitionKeyEqual> partition_candidates;
    for (const auto & [manifest_path, records] : files_by_manifest)
        for (const auto & record : records)
            if (record.is_candidate)
                partition_candidates[record.partition_key].push_back(&record);

    if (partition_candidates.empty())
    {
        if (small_files_total == 0)
            LOG_INFO(log, "No small files found below threshold {} bytes; nothing to compact", min_file_size);
        else
            LOG_INFO(log, "No files eligible for bin-packing; nothing to compact");
        return plan;
    }

    /// A bin: pointers to candidate files of one partition, up to `target_file_size` bytes.
    struct BinPointers
    {
        Row partition_key;
        std::vector<const DataFileRecord *> files;
        Int64 total_bytes = 0;
        Int64 total_records = 0;
    };
    std::vector<BinPointers> bin_ptrs;

    for (auto & [partition_key, files] : partition_candidates)
    {
        /// Need at least 2 files to make compaction worthwhile.
        if (files.size() < 2)
            continue;

        std::sort(files.begin(), files.end(), [](const DataFileRecord * lhs, const DataFileRecord * rhs)
        {
            if (lhs->file_size_in_bytes != rhs->file_size_in_bytes)
                return lhs->file_size_in_bytes < rhs->file_size_in_bytes;
            return lhs->file_path.serialize() < rhs->file_path.serialize();
        });

        BinPointers current_bin;
        current_bin.partition_key = partition_key;

        /// Rewriting a single file only copies it to a new file of the same size, which would stay
        /// a candidate and be rewritten again by every subsequent OPTIMIZE. Such a file stays as is.
        auto flush_bin = [&]
        {
            if (current_bin.files.size() >= 2)
                bin_ptrs.push_back(std::move(current_bin));
            current_bin = BinPointers{};
            current_bin.partition_key = partition_key;
        };

        for (const auto * entry : files)
        {
            if (current_bin.total_bytes + entry->file_size_in_bytes > static_cast<Int64>(target_file_size)
                && !current_bin.files.empty())
                flush_bin();

            current_bin.total_bytes += entry->file_size_in_bytes;
            current_bin.total_records += entry->record_count;
            current_bin.files.push_back(entry);
        }

        flush_bin();
    }

    /// Materialize the plan: move binned records into bins, carry forward manifests with no
    /// rewritten files, and keep every other file of a touched manifest as an EXISTING entry.
    std::unordered_map<String, size_t> bin_index_by_path;
    for (size_t i = 0; i < bin_ptrs.size(); ++i)
        for (const auto * entry : bin_ptrs[i].files)
            bin_index_by_path.emplace(entry->file_path.serialize(), i);

    plan.bins.resize(bin_ptrs.size());
    for (size_t i = 0; i < bin_ptrs.size(); ++i)
    {
        plan.bins[i].partition_key = bin_ptrs[i].partition_key;
        plan.bins[i].total_bytes = bin_ptrs[i].total_bytes;
        plan.bins[i].total_records = bin_ptrs[i].total_records;
    }

    for (auto & [manifest_path, records] : files_by_manifest)
    {
        const bool manifest_touched = std::any_of(
            records.begin(), records.end(),
            [&](const auto & record) { return bin_index_by_path.contains(record.file_path.serialize()); });

        if (!manifest_touched)
        {
            plan.carry_forward_manifest_paths.insert(manifest_path);
            continue;
        }

        for (auto & record : records)
        {
            auto it = bin_index_by_path.find(record.file_path.serialize());
            if (it != bin_index_by_path.end())
            {
                plan.removed_data_files++;
                plan.removed_records += record.record_count;
                plan.removed_files_size += record.file_size_in_bytes;
                plan.bins[it->second].files.push_back(std::move(record));
            }
            else
            {
                plan.kept_groups[record.partition_key].push_back(std::move(record));
            }
        }
    }

    plan.num_partitions = 0;
    {
        std::unordered_set<Row, PartitionKeyHash, PartitionKeyEqual> affected_partitions;
        for (const auto & bin : plan.bins)
            affected_partitions.insert(bin.partition_key);
        plan.num_partitions = static_cast<Int64>(affected_partitions.size());
    }

    LOG_INFO(log, "Bin-pack plan: {} bins across {} partitions, {} small files totalling {} bytes",
             plan.bins.size(), plan.num_partitions, plan.removed_data_files, plan.removed_files_size);

    return plan;
}

struct LatestMetadata
{
    Int32 version;
    Poco::JSON::Object::Ptr object;
};

LatestMetadata readLatestMetadata(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context,
    LoggerPtr log)
{
    const auto [metadata_version, metadata_file_path, _] = getLatestOrExplicitMetadataFileAndVersion(
        object_storage,
        persistent_table_components.table_path,
        data_lake_settings,
        persistent_table_components.metadata_cache,
        context,
        log.get(),
        persistent_table_components.table_uuid,
        persistent_table_components.metadata_compression_method,
        /* force_fetch_latest_metadata */ true,
        /* ignore_explicit_metadata_file_path */ true);

    auto metadata_object = getMetadataJSONObject(
        metadata_file_path,
        object_storage,
        persistent_table_components.metadata_cache,
        context,
        log,
        persistent_table_components.metadata_compression_method,
        persistent_table_components.table_uuid);

    return {metadata_version, metadata_object};
}

enum class MetadataFileOwner : uint8_t
{
    Absent,
    Ours,
    Other,
};

/// Who wrote the metadata file at `metadata_path`, judged by its current snapshot id.
MetadataFileOwner getMetadataFileOwner(
    const IcebergPathFromMetadata & metadata_path,
    CompressionMethod compression_method,
    Int64 snapshot_id,
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    ContextPtr context,
    LoggerPtr log)
{
    const auto storage_path = persistent_table_components.path_resolver.resolve(metadata_path);
    if (!object_storage->exists(StoredObject(storage_path)))
        return MetadataFileOwner::Absent;

    /// Bypass the metadata cache: the outcome of our own write is what is being checked.
    auto metadata_object = getMetadataJSONObject(
        storage_path, object_storage, /* metadata_cache */ nullptr, context, log, compression_method, std::nullopt);
    const bool ours
        = metadata_object->has(f_current_snapshot_id) && metadata_object->getValue<Int64>(f_current_snapshot_id) == snapshot_id;
    return ours ? MetadataFileOwner::Ours : MetadataFileOwner::Other;
}

} // anonymous namespace


bool hasLivePositionDeletes(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context)
{
    LoggerPtr log = getLogger("IcebergBinPack");
    const auto metadata_object = readLatestMetadata(persistent_table_components, object_storage, data_lake_settings, context, log).object;

    if (!metadata_object->has(f_current_snapshot_id))
        return false;
    const Int64 current_snapshot_id = metadata_object->getValue<Int64>(f_current_snapshot_id);
    if (current_snapshot_id < 0)
        return false;

    String current_manifest_list_path;
    auto snapshots = metadata_object->get(f_snapshots).extract<Poco::JSON::Array::Ptr>();
    for (size_t i = 0; i < snapshots->size(); ++i)
    {
        const auto snapshot = snapshots->getObject(static_cast<UInt32>(i));
        if (snapshot->getValue<Int64>(f_metadata_snapshot_id) == current_snapshot_id)
        {
            current_manifest_list_path = snapshot->getValue<String>(f_manifest_list);
            break;
        }
    }
    if (current_manifest_list_path.empty())
        return false;

    const auto current_schema_id = metadata_object->getValue<Int32>(f_current_schema_id);
    auto schemas = metadata_object->getArray(f_schemas);
    for (UInt32 i = 0; i < schemas->size(); ++i)
        persistent_table_components.schema_processor->addIcebergTableSchema(schemas->getObject(i), context);

    auto manifest_list = getManifestList(
        object_storage, persistent_table_components, context,
        IcebergPathFromMetadata::deserialize(current_manifest_list_path),
        log, secondary_storages);

    for (const auto & manifest_file : manifest_list)
    {
        if (manifest_file.content_type != ManifestFileContentType::DELETE)
            continue;
        auto deletes_handle = getManifestFileEntriesHandle(
            object_storage, persistent_table_components, context, log,
            manifest_file, current_schema_id, secondary_storages);
        if (!deletes_handle.getFilesWithoutDeleted(FileContentType::POSITION_DELETE).empty())
            return true;
    }
    return false;
}

BinPackCommitResult executeBinPackCompaction(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    ContextPtr context,
    const String & write_format)
{
    LoggerPtr log = getLogger("IcebergBinPack");

    const auto & settings = context->getSettingsRef();
    UInt64 target_size = settings[Setting::iceberg_target_data_file_size_bytes];
    UInt64 min_size = settings[Setting::iceberg_min_data_file_size_bytes];

    const auto [metadata_version, metadata_object]
        = readLatestMetadata(persistent_table_components, object_storage, data_lake_settings, context, log);

    /// Format version 3 requires row lineage (`_row_id`, `_last_updated_sequence_number`) to be carried
    /// into rewritten files, which the rewrite does not do yet.
    const Int32 format_version = metadata_object->getValue<Int32>(f_format_version);
    if (format_version != 2)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Bin-packing compaction is supported only for Iceberg format_version 2, got {}",
            format_version);

    /// Build the plan.
    auto plan = buildBinPackPlan(
        metadata_object, persistent_table_components, object_storage,
        secondary_storages, context, min_size, target_size);

    if (plan.bins.empty())
    {
        LOG_INFO(log, "No bins to compact; table is already optimally packed");
        return BinPackCommitResult::Committed;
    }

    const auto & path_resolver = persistent_table_components.path_resolver;
    CompressionMethod compression_method = persistent_table_components.metadata_compression_method;

    FileNamesGenerator generator(
        path_resolver.getTableLocation(), false, compression_method, write_format);
    generator.setVersion(metadata_version + 1);

    MetadataGenerator metadata_generator(metadata_object);

    /// Track new files for cleanup on failure.
    std::vector<IcebergPathFromMetadata> new_data_file_paths;
    std::vector<IcebergPathFromMetadata> new_manifest_paths;
    IcebergPathFromMetadata manifest_list_path;
    bool keep_files_on_error = false;

    auto cleanup = [&]()
    {
        for (const auto & p : new_data_file_paths)
        {
            try { object_storage->removeObjectIfExists(StoredObject(path_resolver.resolve(p))); }
            catch (...) { tryLogCurrentException(log, "Cleanup: failed to remove data file"); }
        }
        for (const auto & p : new_manifest_paths)
        {
            try { object_storage->removeObjectIfExists(StoredObject(path_resolver.resolve(p))); }
            catch (...) { tryLogCurrentException(log, "Cleanup: failed to remove manifest file"); }
        }
        if (!manifest_list_path.empty())
        {
            try { object_storage->removeObjectIfExists(StoredObject(path_resolver.resolve(manifest_list_path))); }
            catch (...) { tryLogCurrentException(log, "Cleanup: failed to remove manifest list"); }
        }
    };

    try
    {
        /// Resolve schema for the column mapper.
        auto current_schema_id = metadata_object->getValue<Int64>(f_current_schema_id);
        auto schemas = metadata_object->getArray(f_schemas);
        Poco::JSON::Object::Ptr current_schema;
        for (size_t i = 0; i < schemas->size(); ++i)
        {
            if (schemas->getObject(static_cast<UInt32>(i))->getValue<Int32>(f_schema_id) == current_schema_id)
            {
                current_schema = schemas->getObject(static_cast<UInt32>(i));
                break;
            }
        }
        if (!current_schema)
            throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
                "Missing schema for current-schema-id {}", current_schema_id);

        auto & schema_processor = *persistent_table_components.schema_processor;
        const Int32 current_schema_id_int = static_cast<Int32>(current_schema_id);

        /// The rewrite reads and writes the schema of the metadata it commits against, not the
        /// storage's in-memory columns, which may be older than that metadata.
        const SharedHeader sample_block = makeHeaderFromSchema(schema_processor, current_schema_id_int);
        const ColumnMapperPtr current_schema_column_mapper = createColumnMapper(current_schema);

        /// Phase 1: Read small files and write merged data files.
        /// For each bin, read all source files and write a merged file via MultipleFileWriter.
        Int64 total_added_files = 0;
        Int64 total_added_records = 0;
        Int64 total_added_files_size = 0;

        /// Track info per bin for manifest writing.
        struct BinResult
        {
            Row partition_key;
            std::vector<IcebergPathFromMetadata> merged_file_paths;
            std::vector<UInt64> merged_file_row_counts;
            std::vector<UInt64> merged_file_byte_counts;
            std::vector<DataFileStatisticsPtr> merged_file_stats;
            /// The old files that were replaced.
            std::vector<IcebergPathFromMetadata> old_file_paths;
            std::vector<UInt64> old_file_row_counts;
            std::vector<UInt64> old_file_byte_counts;
            std::vector<DataFileEntryLineage> old_file_lineage;
            std::vector<DataFileColumnStatistics> old_file_stats;
            std::vector<String> old_file_formats;
            std::vector<std::optional<Int32>> old_file_sort_order_ids;
        };
        std::vector<BinResult> bin_results;

        for (auto & bin : plan.bins)
        {
            BinResult result;
            result.partition_key = bin.partition_key;

            /// Prepare the MultipleFileWriter for this bin.
            MultipleFileWriter writer(
                /* max_data_file_num_rows */ 0,  /// no row limit; size limit is used
                /* max_data_file_num_bytes */ target_size,
                current_schema->getArray(f_fields),
                generator,
                path_resolver,
                object_storage,
                context,
                std::nullopt, /// format_settings
                write_format,
                sample_block,
                [&](const std::string & path)
                {
                    new_data_file_paths.push_back(IcebergPathFromMetadata::deserialize(path));
                });

            /// Read each source file and feed into the writer.
            for (auto & file_entry : bin.files)
            {
                auto [resolved_storage, resolved_key] = resolveObjectStorageForPath(
                    persistent_table_components.table_location,
                    file_entry.file_path.serialize(),
                    object_storage, secondary_storages, context,
                    path_resolver);

                RelativePathWithMetadata object_info(resolved_key);
                ObjectStoragePtr storage_to_use = resolved_storage ? resolved_storage : object_storage;
                auto read_buffer = createReadBuffer(object_info, storage_to_use, context, log);

                auto parser_shared_resources = std::make_shared<FormatParserSharedResources>(
                    settings, /*num_streams_=*/1);

                const String source_format = file_entry.file_format.empty() ? write_format : file_entry.file_format;
                const bool is_parquet = Poco::toLower(source_format) == "parquet";

                /// Like `SELECT`, read a file with the schema it was written with (Parquet columns are
                /// matched by field id when present, by name otherwise) and then evolve it to the
                /// current schema, so renamed, retyped, added and dropped columns are handled.
                SharedHeader read_header = sample_block;
                ColumnMapperPtr column_mapper = is_parquet ? current_schema_column_mapper : nullptr;
                std::shared_ptr<ExpressionActions> schema_transform;
                if (file_entry.schema_id != current_schema_id_int)
                {
                    read_header = makeHeaderFromSchema(schema_processor, file_entry.schema_id);
                    column_mapper = is_parquet ? schema_processor.getColumnMapperById(file_entry.schema_id) : nullptr;
                    auto dag = schema_processor.getSchemaTransformationDagByIds(context, file_entry.schema_id, current_schema_id_int);
                    schema_transform = std::make_shared<ExpressionActions>(dag->clone());
                }

                auto input_format = FormatFactory::instance().getInput(
                    source_format,
                    *read_buffer,
                    *read_header,
                    context,
                    8192,
                    std::nullopt, /// format_settings
                    parser_shared_resources,
                    std::make_shared<FormatFilterInfo>(nullptr, context, column_mapper, nullptr, nullptr),
                    true, /// is_remote_fs
                    CompressionMethod::None,
                    false);

                while (true)
                {
                    if (auto elem = context->getProcessListElement(); elem && elem->isKilled())
                        throw Exception(ErrorCodes::QUERY_WAS_CANCELLED, "OPTIMIZE TABLE cancelled during bin-pack rewrite");

                    auto chunk = input_format->read();
                    if (chunk.empty())
                        break;

                    if (schema_transform)
                    {
                        size_t num_rows = chunk.getNumRows();
                        Block block = read_header->cloneWithColumns(chunk.detachColumns());
                        schema_transform->execute(block, num_rows);

                        Columns columns;
                        columns.reserve(sample_block->columns());
                        for (const auto & column : *sample_block)
                            columns.push_back(block.getByName(column.name).column->convertToFullColumnIfConst());
                        chunk = Chunk(std::move(columns), num_rows);
                    }

                    writer.consume(chunk);
                }

                /// Track old file info for the delete manifest.
                result.old_file_paths.push_back(file_entry.file_path);
                result.old_file_row_counts.push_back(static_cast<UInt64>(file_entry.record_count));
                result.old_file_byte_counts.push_back(static_cast<UInt64>(file_entry.file_size_in_bytes));
                result.old_file_formats.push_back(file_entry.file_format);
                result.old_file_sort_order_ids.push_back(file_entry.sort_order_id);
                result.old_file_stats.push_back(std::move(file_entry.column_stats));

                /// Per the spec, the `snapshot_id` of a DELETED entry is the snapshot that deleted the file,
                /// so `added_snapshot_id` stays unset and the new `replace` snapshot id is written.
                DataFileEntryLineage lineage;
                lineage.sequence_number = file_entry.sequence_number;
                lineage.file_sequence_number = file_entry.file_sequence_number;
                lineage.status_override = ManifestEntryStatus::DELETED;
                result.old_file_lineage.push_back(lineage);
            }

            writer.finalize();

            result.merged_file_paths = writer.getDataFiles();
            result.merged_file_row_counts = writer.getDataFileRowCounts();
            result.merged_file_byte_counts = writer.getDataFileByteCounts();
            result.merged_file_stats = writer.getPerFileStatistics();
            if (result.merged_file_stats.size() != result.merged_file_paths.size())
                throw Exception(
                    ErrorCodes::LOGICAL_ERROR,
                    "Bin-pack writer returned {} statistics for {} merged files",
                    result.merged_file_stats.size(), result.merged_file_paths.size());

            for (size_t i = 0; i < result.merged_file_paths.size(); ++i)
            {
                total_added_files++;
                total_added_records += static_cast<Int64>(result.merged_file_row_counts[i]);
                total_added_files_size += static_cast<Int64>(result.merged_file_byte_counts[i]);
            }

            bin_results.push_back(std::move(result));
        }

        /// Phase 2: Generate the replace snapshot.
        auto generated_metadata_info = generator.generateMetadataPathWithInfo();
        auto snapshot_result = metadata_generator.generateReplaceSnapshot(
            generator,
            generated_metadata_info.path,
            plan.current_snapshot_id,
            total_added_files,
            total_added_records,
            total_added_files_size,
            plan.removed_data_files,
            plan.removed_records,
            plan.removed_files_size,
            plan.num_partitions);

        /// Phase 3: Write manifest files.
        /// We write three types of manifests:
        /// - Delete manifests: DELETED entries for old files (one per bin)
        /// - Add manifests: ADDED entries for new merged files (one per bin)
        /// - Kept manifests: EXISTING entries for files of touched manifests that were not merged
        std::vector<IcebergPathFromMetadata> all_new_manifest_paths;
        std::vector<Int64> all_manifest_sizes;
        std::vector<ManifestListEntryExistingCounts> all_existing_counts;
        std::vector<Int64> all_entry_partition_spec_ids;

        /// Write one data manifest for a set of entries sharing a partition key and record it
        /// for the manifest list. `existing_counts` must match the statuses of the written entries.
        auto write_manifest_for_entries = [&](
            const Row & partition_key,
            const std::vector<IcebergPathFromMetadata> & paths,
            const std::vector<UInt64> & row_counts,
            const std::vector<UInt64> & byte_counts,
            const std::vector<String> & formats,
            const std::vector<DataFileColumnStatistics> & stats,
            const std::vector<std::optional<Int32>> & sort_order_ids,
            const std::vector<DataFileEntryLineage> & lineage,
            const std::optional<DataFileStatistics> & data_file_statistics,
            ManifestListEntryExistingCounts existing_counts)
        {
            auto manifest_path = generator.generateManifestEntryName();
            auto storage_path = path_resolver.resolve(manifest_path);
            new_manifest_paths.push_back(manifest_path);

            auto buf = object_storage->writeObject(
                StoredObject(storage_path), WriteMode::Rewrite, std::nullopt,
                DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

            generateManifestFile(
                metadata_object,
                plan.partition_columns,
                partition_key,
                plan.partition_types,
                paths,
                row_counts,
                byte_counts,
                data_file_statistics,
                sample_block,
                snapshot_result.snapshot,
                write_format,
                plan.partition_spec,
                plan.partition_spec_id,
                *buf,
                FileContentType::DATA,
                std::nullopt, /// user_defined_sequence_number
                {}, /// per_file_stats
                formats,
                stats,
                sort_order_ids,
                lineage);

            buf->finalize();
            Int64 manifest_size = buf->count();
            if (manifest_size == 0)
                manifest_size = object_storage->getObjectMetadata(storage_path, false).size_bytes;

            all_new_manifest_paths.push_back(manifest_path);
            all_manifest_sizes.push_back(manifest_size);
            all_existing_counts.push_back(existing_counts);
            all_entry_partition_spec_ids.push_back(plan.partition_spec_id);
        };

        const Int64 new_sequence_number = snapshot_result.snapshot->getValue<Int64>(f_metadata_sequence_number);

        for (auto & bin_result : bin_results)
        {
            /// Delete manifest: old files marked DELETED.
            Int64 deleted_min_seq = std::numeric_limits<Int64>::max();
            for (const auto & lineage : bin_result.old_file_lineage)
                deleted_min_seq = std::min(deleted_min_seq, lineage.sequence_number.value_or(0));

            ManifestListEntryExistingCounts deleted_counts;
            deleted_counts.min_sequence_number = deleted_min_seq;
            deleted_counts.deleted_files_count = static_cast<Int64>(bin_result.old_file_paths.size());
            deleted_counts.deleted_rows_count = static_cast<Int64>(
                std::accumulate(bin_result.old_file_row_counts.begin(), bin_result.old_file_row_counts.end(), UInt64{0}));

            write_manifest_for_entries(
                bin_result.partition_key,
                bin_result.old_file_paths,
                bin_result.old_file_row_counts,
                bin_result.old_file_byte_counts,
                bin_result.old_file_formats,
                bin_result.old_file_stats,
                bin_result.old_file_sort_order_ids,
                bin_result.old_file_lineage,
                std::nullopt,
                deleted_counts);

            /// Add manifests: one per merged file, because `generateManifestFile` applies a single
            /// statistics object to every entry of a manifest.
            for (size_t i = 0; i < bin_result.merged_file_paths.size(); ++i)
            {
                std::optional<DataFileStatistics> merged_stats;
                if (bin_result.merged_file_stats[i])
                    merged_stats = *bin_result.merged_file_stats[i];

                ManifestListEntryExistingCounts added_counts;
                added_counts.min_sequence_number = new_sequence_number;
                added_counts.added_files_count = 1;
                added_counts.added_rows_count = static_cast<Int64>(bin_result.merged_file_row_counts[i]);

                write_manifest_for_entries(
                    bin_result.partition_key,
                    {bin_result.merged_file_paths[i]},
                    {bin_result.merged_file_row_counts[i]},
                    {bin_result.merged_file_byte_counts[i]},
                    {}, /// formats
                    {}, /// stats
                    {}, /// sort_order_ids
                    {}, /// lineage
                    merged_stats,
                    added_counts);
            }
        }

        /// Kept manifests: live data files from touched manifests that were not merged (large
        /// files, single-file partitions, files excluded because of deletes). They stay in the
        /// new snapshot as EXISTING entries with their original snapshot id and sequence numbers.
        for (const auto & [partition_key, kept_files] : plan.kept_groups)
        {
            std::vector<IcebergPathFromMetadata> kept_paths;
            std::vector<UInt64> kept_row_counts;
            std::vector<UInt64> kept_byte_counts;
            std::vector<String> kept_formats;
            std::vector<DataFileColumnStatistics> kept_stats;
            std::vector<std::optional<Int32>> kept_sort_order_ids;
            std::vector<DataFileEntryLineage> kept_lineage;
            kept_paths.reserve(kept_files.size());
            kept_row_counts.reserve(kept_files.size());
            kept_byte_counts.reserve(kept_files.size());
            kept_formats.reserve(kept_files.size());
            kept_stats.reserve(kept_files.size());
            kept_sort_order_ids.reserve(kept_files.size());
            kept_lineage.reserve(kept_files.size());

            Int64 kept_min_seq = std::numeric_limits<Int64>::max();
            Int64 kept_total_rows = 0;
            for (const auto & kept : kept_files)
            {
                kept_paths.push_back(kept.file_path);
                kept_row_counts.push_back(static_cast<UInt64>(kept.record_count));
                kept_byte_counts.push_back(static_cast<UInt64>(kept.file_size_in_bytes));
                kept_formats.push_back(kept.file_format);
                kept_stats.push_back(kept.column_stats);
                kept_sort_order_ids.push_back(kept.sort_order_id);

                DataFileEntryLineage lineage;
                lineage.added_snapshot_id = kept.snapshot_id;
                lineage.sequence_number = kept.sequence_number;
                lineage.file_sequence_number = kept.file_sequence_number;
                kept_lineage.push_back(lineage);

                kept_min_seq = std::min(kept_min_seq, kept.sequence_number.value_or(0));
                kept_total_rows += kept.record_count;
            }

            write_manifest_for_entries(
                partition_key,
                kept_paths,
                kept_row_counts,
                kept_byte_counts,
                kept_formats,
                kept_stats,
                kept_sort_order_ids,
                kept_lineage,
                std::nullopt,
                {static_cast<Int64>(kept_files.size()), kept_total_rows, kept_min_seq});
        }

        /// Phase 4: Write manifest list.
        {
            auto storage_manifest_list_path = path_resolver.resolve(snapshot_result.manifest_list_path);
            manifest_list_path = snapshot_result.manifest_list_path;

            auto buf = object_storage->writeObject(
                StoredObject(storage_manifest_list_path), WriteMode::Rewrite, std::nullopt,
                DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

            generateManifestList(
                path_resolver,
                metadata_object,
                object_storage,
                secondary_storages,
                context,
                all_new_manifest_paths,
                snapshot_result.snapshot,
                all_manifest_sizes,
                *buf,
                FileContentType::DATA,
                false, /// use_previous_snapshots
                {}, /// per_entry_content_types
                all_existing_counts,
                plan.carry_forward_manifest_paths,
                all_entry_partition_spec_ids);

            buf->finalize();
        }

        /// Phase 5: Commit metadata.
        {
            std::ostringstream oss; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
            Poco::JSON::Stringifier::stringify(metadata_object, oss, 4);
            std::string json_representation = removeEscapedSlashes(oss.str());

            auto hint_path = generator.generateVersionHint();

            const auto commit_result = tryWriteMetadataFileAndVersionHint(
                path_resolver,
                generated_metadata_info,
                json_representation,
                hint_path,
                object_storage,
                context,
                data_lake_settings[DataLakeStorageSetting::iceberg_use_version_hint]);

            if (commit_result == MetadataCommitResult::Conflict)
            {
                LOG_INFO(log, "Bin-pack commit conflict detected, cleaning up");
                cleanup();
                return BinPackCommitResult::Conflict;
            }

            if (commit_result == MetadataCommitResult::Unknown)
            {
                /// From here on the new metadata may be live and reference the new files,
                /// so they are removed only once another writer is known to own this version.
                keep_files_on_error = true;

                const Int64 new_snapshot_id = snapshot_result.snapshot->getValue<Int64>(f_metadata_snapshot_id);
                MetadataFileOwner owner = MetadataFileOwner::Absent;
                String verification_error;
                try
                {
                    owner = getMetadataFileOwner(
                        generated_metadata_info.path, generated_metadata_info.compression_method, new_snapshot_id,
                        persistent_table_components, object_storage, context, log);
                }
                catch (...)
                {
                    verification_error = getCurrentExceptionMessage(false);
                }

                if (!verification_error.empty() || owner == MetadataFileOwner::Absent)
                    throw Exception(
                        ErrorCodes::CANNOT_WRITE_TO_FILE_BUFFER,
                        "Outcome of writing Iceberg metadata file {} for bin-pack snapshot {} is unknown{}. "
                        "Files written by this attempt were left in place",
                        generated_metadata_info.path.serialize(), new_snapshot_id,
                        verification_error.empty() ? "" : fmt::format(" and could not be verified: {}", verification_error));

                if (owner == MetadataFileOwner::Other)
                {
                    LOG_INFO(log, "Bin-pack commit conflict detected after a failed metadata write, cleaning up");
                    keep_files_on_error = false;
                    cleanup();
                    return BinPackCommitResult::Conflict;
                }
                LOG_INFO(log, "Bin-pack metadata write reported an error, but snapshot {} is committed", new_snapshot_id);
            }
        }

        LOG_INFO(log, "Bin-pack compaction committed: {} bins, {} new files ({} records, {} bytes), "
                 "{} old files removed ({} records, {} bytes)",
                 plan.bins.size(), total_added_files, total_added_records, total_added_files_size,
                 plan.removed_data_files, plan.removed_records, plan.removed_files_size);
        return BinPackCommitResult::Committed;
    }
    catch (...)
    {
        if (!keep_files_on_error)
            cleanup();
        throw;
    }
}

}

#endif
