#include <Storages/ObjectStorage/DataLakes/Iceberg/BinPackRewrite.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <Core/Settings.h>
#include <Databases/DataLake/Common.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatParserSharedResources.h>
#include <IO/CompressionMethod.h>
#include <Interpreters/Context.h>
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
#include <Common/FieldVisitorDump.h>
#include <Common/Logger.h>

#if USE_AVRO

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int ICEBERG_SPECIFICATION_VIOLATION;
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

/// A single small data file selected for bin-packing.
struct SmallFileEntry
{
    IcebergPathFromMetadata file_path;
    Int64 record_count;
    Int64 file_size_in_bytes;
    String file_format;
    Row partition_key;
    std::optional<Int32> sort_order_id;
    /// Lineage from the source manifest entry.
    std::optional<Int64> snapshot_id;
    std::optional<Int64> sequence_number;
    std::optional<Int64> file_sequence_number;
    /// Per-column statistics from the source manifest.
    DataFileColumnStatistics column_stats;
};

/// A bin: a group of small files from the same partition to be merged.
struct Bin
{
    Row partition_key;
    std::vector<SmallFileEntry> files;
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
    /// Manifest paths to carry forward unchanged (delete manifests + data manifests with no small files).
    std::unordered_set<String> carry_forward_manifest_paths;
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

    /// Collect files per partition.
    using PartitionFiles = std::vector<SmallFileEntry>;
    std::unordered_map<Row, PartitionFiles, PartitionKeyHash, PartitionKeyEqual> partition_files;
    /// Track manifest paths with no small files to carry forward.
    std::unordered_set<String> manifests_with_only_large_files;

    for (const auto & manifest_file : manifest_list)
    {
        if (manifest_file.content_type == ManifestFileContentType::DELETE)
        {
            plan.carry_forward_manifest_paths.insert(manifest_file.manifest_file_path.serialize());
            continue;
        }

        auto files_handle = getManifestFileEntriesHandle(
            object_storage, persistent_table_components, context, log,
            manifest_file, static_cast<Int32>(current_schema_id), secondary_storages);

        bool has_small_files = false;
        for (const auto & data_file : files_handle.getFilesWithoutDeleted(FileContentType::DATA))
        {
            const auto & entry = data_file->parsed_entry;
            if (static_cast<UInt64>(entry->file_size_in_bytes) < min_file_size)
            {
                has_small_files = true;
                SmallFileEntry small_entry;
                small_entry.file_path = entry->file_path_key;
                small_entry.record_count = entry->record_count;
                small_entry.file_size_in_bytes = entry->file_size_in_bytes;
                small_entry.file_format = entry->file_format;
                small_entry.partition_key = entry->partition_key_value;
                small_entry.sort_order_id = entry->sort_order_id;
                small_entry.snapshot_id = entry->parsed_snapshot_id;
                if (!small_entry.snapshot_id.has_value())
                    small_entry.snapshot_id = manifest_file.added_snapshot_id;
                small_entry.sequence_number = entry->parsed_sequence_number;
                if (!small_entry.sequence_number.has_value())
                    small_entry.sequence_number = manifest_file.added_sequence_number;
                small_entry.file_sequence_number = entry->parsed_file_sequence_number;
                if (!small_entry.file_sequence_number.has_value())
                    small_entry.file_sequence_number = manifest_file.added_sequence_number;

                /// Carry over per-column stats.
                for (const auto & [field_id, col_info] : entry->columns_infos)
                {
                    if (col_info.bytes_size.has_value())
                        small_entry.column_stats.column_sizes.emplace_back(field_id, *col_info.bytes_size);
                    if (col_info.rows_count.has_value())
                        small_entry.column_stats.value_counts.emplace_back(field_id, *col_info.rows_count);
                    if (col_info.nulls_count.has_value())
                        small_entry.column_stats.null_value_counts.emplace_back(field_id, *col_info.nulls_count);
                }
                for (const auto & [field_id, bounds] : entry->value_bounds)
                {
                    if (!bounds.first.isNull())
                        small_entry.column_stats.lower_bounds.emplace_back(field_id, bounds.first.safeGet<String>());
                    if (!bounds.second.isNull())
                        small_entry.column_stats.upper_bounds.emplace_back(field_id, bounds.second.safeGet<String>());
                }

                partition_files[entry->partition_key_value].push_back(std::move(small_entry));
            }
        }

        if (!has_small_files)
            manifests_with_only_large_files.insert(manifest_file.manifest_file_path.serialize());
    }

    /// Carry forward manifests that only have large files.
    for (const auto & path : manifests_with_only_large_files)
        plan.carry_forward_manifest_paths.insert(path);

    if (partition_files.empty())
    {
        LOG_INFO(log, "No small files found below threshold {} bytes; nothing to compact", min_file_size);
        return plan;
    }

    /// Group small files into bins per partition.
    for (auto & [partition_key, files] : partition_files)
    {
        /// Need at least 2 files to make compaction worthwhile.
        if (files.size() < 2)
        {
            /// Not enough files to merge — the manifest containing these is NOT carried forward as-is
            /// because it also holds the small files.  The existing compaction logic will write a
            /// data manifest for the untouched partition in the new manifest list below.
            continue;
        }

        Bin current_bin;
        current_bin.partition_key = partition_key;

        for (auto & entry : files)
        {
            if (current_bin.total_bytes + entry.file_size_in_bytes > static_cast<Int64>(target_file_size)
                && !current_bin.files.empty())
            {
                plan.bins.push_back(std::move(current_bin));
                current_bin = Bin{};
                current_bin.partition_key = partition_key;
            }

            current_bin.total_bytes += entry.file_size_in_bytes;
            current_bin.total_records += entry.record_count;
            plan.removed_data_files++;
            plan.removed_records += entry.record_count;
            plan.removed_files_size += entry.file_size_in_bytes;
            current_bin.files.push_back(std::move(entry));
        }

        if (current_bin.files.size() >= 2)
            plan.bins.push_back(std::move(current_bin));
        else
        {
            /// Undo the stats for a single-file bin (not worth merging).
            for (const auto & f : current_bin.files)
            {
                plan.removed_data_files--;
                plan.removed_records -= f.record_count;
                plan.removed_files_size -= f.file_size_in_bytes;
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

} // anonymous namespace


bool executeBinPackCompaction(
    const PersistentTableComponents & persistent_table_components,
    ObjectStoragePtr object_storage,
    SecondaryStorages & secondary_storages,
    const DataLakeStorageSettings & data_lake_settings,
    SharedHeader sample_block,
    ContextPtr context,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const StorageID & table_id)
{
    LoggerPtr log = getLogger("IcebergBinPack");

    const auto & settings = context->getSettingsRef();
    UInt64 target_size = settings[Setting::iceberg_target_data_file_size_bytes];
    UInt64 min_size = settings[Setting::iceberg_min_data_file_size_bytes];

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

    const Int32 format_version = metadata_object->getValue<Int32>(f_format_version);
    if (format_version < 2)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Bin-packing compaction is supported only for Iceberg format_version >= 2");

    /// Build the plan.
    auto plan = buildBinPackPlan(
        metadata_object, persistent_table_components, object_storage,
        secondary_storages, context, min_size, target_size);

    if (plan.bins.empty())
    {
        LOG_INFO(log, "No bins to compact; table is already optimally packed");
        return true;
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

                auto input_format = FormatFactory::instance().getInput(
                    file_entry.file_format.empty() ? write_format : file_entry.file_format,
                    *read_buffer,
                    *sample_block,
                    context,
                    8192,
                    std::nullopt, /// format_settings
                    parser_shared_resources,
                    std::make_shared<FormatFilterInfo>(nullptr, context, nullptr, nullptr, nullptr),
                    true, /// is_remote_fs
                    CompressionMethod::None,
                    false);

                while (true)
                {
                    auto chunk = input_format->read();
                    if (chunk.empty())
                        break;
                    writer.consume(chunk);
                }

                /// Track old file info for the delete manifest.
                result.old_file_paths.push_back(file_entry.file_path);
                result.old_file_row_counts.push_back(static_cast<UInt64>(file_entry.record_count));
                result.old_file_byte_counts.push_back(static_cast<UInt64>(file_entry.file_size_in_bytes));
                result.old_file_formats.push_back(file_entry.file_format);
                result.old_file_sort_order_ids.push_back(file_entry.sort_order_id);
                result.old_file_stats.push_back(std::move(file_entry.column_stats));

                DataFileEntryLineage lineage;
                lineage.added_snapshot_id = file_entry.snapshot_id;
                lineage.sequence_number = file_entry.sequence_number;
                lineage.file_sequence_number = file_entry.file_sequence_number;
                lineage.status_override = ManifestEntryStatus::DELETED;
                result.old_file_lineage.push_back(lineage);
            }

            writer.finalize();

            result.merged_file_paths = writer.getDataFiles();
            result.merged_file_row_counts = writer.getDataFileRowCounts();
            result.merged_file_byte_counts = writer.getDataFileByteCounts();

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
        /// We write two types of manifests:
        /// - Delete manifests: DELETED entries for old files (one per bin)
        /// - Add manifests: ADDED entries for new merged files (one per bin)
        std::vector<IcebergPathFromMetadata> all_new_manifest_paths;
        std::vector<Int64> all_manifest_sizes;
        std::vector<ManifestListEntryExistingCounts> all_existing_counts;
        std::vector<Int64> all_entry_partition_spec_ids;

        for (auto & bin_result : bin_results)
        {
            /// Delete manifest: old files marked DELETED.
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
                    bin_result.partition_key,
                    plan.partition_types,
                    bin_result.old_file_paths,
                    bin_result.old_file_row_counts,
                    bin_result.old_file_byte_counts,
                    std::nullopt, /// data_file_statistics
                    sample_block,
                    snapshot_result.snapshot,
                    write_format,
                    plan.partition_spec,
                    plan.partition_spec_id,
                    *buf,
                    FileContentType::DATA,
                    std::nullopt, /// user_defined_sequence_number
                    {}, /// per_file_stats
                    bin_result.old_file_formats,
                    bin_result.old_file_stats,
                    bin_result.old_file_sort_order_ids,
                    bin_result.old_file_lineage);

                buf->finalize();
                Int64 manifest_size = buf->count();
                if (manifest_size == 0)
                    manifest_size = object_storage->getObjectMetadata(storage_path, false).size_bytes;

                all_new_manifest_paths.push_back(manifest_path);
                all_manifest_sizes.push_back(manifest_size);
                /// The DELETED manifest has existing (really: deleted) file counts for manifest-list accounting.
                Int64 min_seq = std::numeric_limits<Int64>::max();
                for (const auto & lineage : bin_result.old_file_lineage)
                    min_seq = std::min(min_seq, lineage.sequence_number.value_or(0));
                all_existing_counts.push_back(
                    {static_cast<Int64>(bin_result.old_file_paths.size()),
                     static_cast<Int64>(std::accumulate(bin_result.old_file_row_counts.begin(), bin_result.old_file_row_counts.end(), 0UL)),
                     min_seq});
                all_entry_partition_spec_ids.push_back(plan.partition_spec_id);
            }

            /// Add manifest: new merged files.
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
                    bin_result.partition_key,
                    plan.partition_types,
                    bin_result.merged_file_paths,
                    bin_result.merged_file_row_counts,
                    bin_result.merged_file_byte_counts,
                    std::nullopt, /// data_file_statistics
                    sample_block,
                    snapshot_result.snapshot,
                    write_format,
                    plan.partition_spec,
                    plan.partition_spec_id,
                    *buf,
                    FileContentType::DATA);

                buf->finalize();
                Int64 manifest_size = buf->count();
                if (manifest_size == 0)
                    manifest_size = object_storage->getObjectMetadata(storage_path, false).size_bytes;

                all_new_manifest_paths.push_back(manifest_path);
                all_manifest_sizes.push_back(manifest_size);
                all_existing_counts.push_back({0, 0, 0}); /// New files: no existing counts.
                all_entry_partition_spec_ids.push_back(plan.partition_spec_id);
            }
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

            const bool catalog_writes_metadata_file = catalog && catalog->isTransactional();
            if (!catalog_writes_metadata_file
                && !writeMetadataFileAndVersionHint(
                    path_resolver,
                    generated_metadata_info,
                    json_representation,
                    hint_path,
                    object_storage,
                    context,
                    data_lake_settings[DataLakeStorageSetting::iceberg_use_version_hint]))
            {
                LOG_INFO(log, "Bin-pack commit conflict detected, cleaning up");
                cleanup();
                return false;
            }

            if (catalog)
            {
                auto catalog_filename = path_resolver.resolveForCatalog(generated_metadata_info.path);
                const auto & [namespace_name, table_name] = DataLake::parseTableName(table_id.getTableName());
                if (!catalog->updateMetadata(namespace_name, table_name, catalog_filename, snapshot_result.snapshot))
                {
                    LOG_INFO(log, "Bin-pack commit conflict via catalog, cleaning up");
                    cleanup();
                    return false;
                }
            }
        }

        LOG_INFO(log, "Bin-pack compaction committed: {} bins, {} new files ({} records, {} bytes), "
                 "{} old files removed ({} records, {} bytes)",
                 plan.bins.size(), total_added_files, total_added_records, total_added_files_size,
                 plan.removed_data_files, plan.removed_records, plan.removed_files_size);
        return true;
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}

}

#endif
