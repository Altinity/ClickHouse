#include <Storages/ObjectStorage/DataLakes/Iceberg/AlterDropPartitionExecutor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SnapshotSummary.h>

#if USE_AVRO
/// NOLINTBEGIN(clang-analyzer-core.uninitialized.UndefReturn)
/// avro uses nasty '*std::any_cast' which triggers clang-tidy, the warning is false positive since
/// a type and value are consistent in avro::GenericDatum, and even more - all avro manifests in iceberg
/// consist only of AVRO_RECORDS

#include <Core/Block.h>
#include <Core/Settings.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/WriteBufferFromFileBase.h>
#include <Interpreters/Context.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTPartition.h>
#include <Processors/Formats/Impl/AvroRowInputFormat.h>
#include <Processors/Formats/Impl/AvroRowOutputFormat.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ChunkPartitioner.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ExternalPathResolver.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/FileNamesGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MetadataGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/StatelessMetadataFileGetter.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/FieldAccurateComparison.h>
#include <Common/logger_useful.h>

#include <base/defines.h>
#include <base/scope_guard.h>

#include <Poco/JSON/Array.h>
#include <Poco/String.h>

#include <fmt/ranges.h>

#include <limits>
#include <memory>
#include <set>
#include <string>

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int INVALID_PARTITION_VALUE;
extern const int LIMIT_EXCEEDED;
extern const int LOGICAL_ERROR;
extern const int NOT_IMPLEMENTED;
extern const int ICEBERG_SPECIFICATION_VIOLATION;
extern const int CONCURRENT_ACCESS_NOT_SUPPORTED;
extern const int METADATA_MISMATCH;
extern const int ACCESS_DENIED;
}

namespace Setting
{
extern const SettingsInt64 iceberg_snapshot_id;
extern const SettingsInt64 iceberg_timestamp_ms;
extern const SettingsBool data_lake_delete_data_on_drop;
}

namespace DataLakeStorageSetting
{
extern const DataLakeStorageSettingsString iceberg_metadata_file_path;
extern const DataLakeStorageSettingsBool iceberg_use_version_hint;
}

namespace FailPoints
{
extern const char iceberg_writes_cleanup[];
extern const char iceberg_drop_partition_pause_after_discovery[];
}

namespace Iceberg
{

/// One global retry cap shared with INSERT/UPDATE/DELETE.
static constexpr auto MAX_TRANSACTION_RETRIES = 100;

namespace
{

/// Find the partition spec object with the given spec-id inside a metadata JSON document.
/// Throws METADATA_MISMATCH if the spec is not found (indicates metadata/spec-id mismatch).
Poco::JSON::Object::Ptr lookupPartitionSpec(const Poco::JSON::Object::Ptr & meta, Int64 spec_id)
{
    auto specs = meta->getArray(f_partition_specs);
    for (size_t i = 0; i < specs->size(); ++i)
    {
        auto spec = specs->getObject(static_cast<UInt32>(i));
        if (spec->getValue<Int64>(f_spec_id) == spec_id)
            return spec;
    }
    throw Exception(ErrorCodes::METADATA_MISMATCH,
        "Partition spec with id {} not found in table metadata", spec_id);
}

/// One term per partition field in spec order
PartitionSpecSignature getPartitionSpecSignature(const Poco::JSON::Array::Ptr & spec_fields)
{
    PartitionSpecSignature signature;
    signature.reserve(spec_fields->size());
    for (UInt32 i = 0; i < spec_fields->size(); ++i)
    {
        auto spec_field = spec_fields->getObject(i);
        signature.emplace_back(
            spec_field->getValue<Int32>(f_source_id),
            Poco::toLower(spec_field->getValue<String>(f_partition_transform)));
    }
    return signature;
}

constexpr size_t PARTITION_FIELD_NOT_IN_SPEC = std::numeric_limits<size_t>::max();

/// Position of every target spec field inside a file's own partition tuple
std::vector<size_t> mapTargetFieldsToSpecPositions(
    const PartitionSpecification & entry_spec, const PartitionSpecSignature & signature)
{
    std::vector<size_t> positions(signature.size(), PARTITION_FIELD_NOT_IN_SPEC);
    for (size_t i = 0; i < signature.size(); ++i)
    {
        for (size_t j = 0; j < entry_spec.size(); ++j)
        {
            if (entry_spec[j].source_id == signature[i].column_id
                && Poco::toLower(entry_spec[j].transform_name) == signature[i].transform)
            {
                positions[i] = j;
                break;
            }
        }
    }
    return positions;
}

void validateDropPartitionAST(const ASTPartition & ast, const PartitionCommand & command)
{
    if (ast.all)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} ALL is not supported for Iceberg", command.typeToString());
    if (ast.id)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} ID is not supported for Iceberg", command.typeToString());
    if (!ast.value)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "{} doesn't have partition value", command.typeToString());
}

/// Evaluate the user-supplied DROP PARTITION expression against the Iceberg
/// partition spec, following the same convention as `MergeTree`
/// (`MergeTreeData::getPartitionIDFromQuery`).
///
/// `ParserPartition` only accepts an `ASTLiteral` or an `ASTFunction` named
/// `tuple` here, so we only need to handle three shapes:
///   - `ASTLiteral` with a scalar `Field`           — e.g. `DROP PARTITION 7`
///   - `ASTLiteral` with `Field::Types::Tuple`      — e.g. `DROP PARTITION (3, '4')`
///   - `ASTFunction{name=="tuple"}`                 — e.g. `DROP PARTITION (icebergBucket(4, 'abc'))`
///
/// For the function form, each argument is constant-folded with
/// `evaluateConstantExpression` (so transforms like `icebergBucket(4, 'abc')`
/// or `toYearNumSinceEpoch(toDate('2025-01-01'))` evaluate to their
/// partition-key value). Each resulting `Field` is then coerced to the
/// corresponding partition-result type via `convertFieldToTypeOrThrow`.
Row parsePartitionTuple(const IAST & value_ast, const DataTypes & partition_types, ContextPtr context)
{
    const auto partitions_fields_count = partition_types.size();
    auto wrong_partition_fields_count = [&](size_t got)
    {
        return Exception(
            ErrorCodes::INVALID_PARTITION_VALUE,
            "Wrong number of fields in the partition expression: {}, must be: {}",
            got,
            partitions_fields_count);
    };

    Row out(partitions_fields_count);

    if (const auto * lit = value_ast.as<ASTLiteral>())
    {
        if (lit->value.getType() == Field::Types::Tuple)
        {
            const auto & tuple = lit->value.safeGet<Tuple>();
            if (tuple.size() != partitions_fields_count)
                throw wrong_partition_fields_count(tuple.size());
            for (size_t i = 0; i < partitions_fields_count; ++i)
                out[i] = convertFieldToTypeOrThrow(tuple[i], *partition_types[i]);
            return out;
        }

        if (partitions_fields_count != 1)
            throw wrong_partition_fields_count(1);
        out[0] = convertFieldToTypeOrThrow(lit->value, *partition_types[0]);
        return out;
    }

    const auto * fn = value_ast.as<ASTFunction>();
    if (!fn || fn->name != "tuple")
        throw Exception(ErrorCodes::INVALID_PARTITION_VALUE, "Expected literal or tuple for partition key, got {}", value_ast.getID());

    const auto & args = fn->arguments ? fn->arguments->children : ASTs{};
    if (args.size() != partitions_fields_count)
        throw wrong_partition_fields_count(args.size());

    for (size_t i = 0; i < partitions_fields_count; ++i)
    {
        Field value = evaluateConstantExpression(args[i], context).first;
        out[i] = convertFieldToTypeOrThrow(value, *partition_types[i]);
    }
    return out;
}

Block getPartitionSourceHeader(
    const Poco::JSON::Array::Ptr & spec_fields,
    const Poco::JSON::Array::Ptr & schema_fields,
    const IcebergSchemaProcessor & schema_processor,
    Int32 schema_id)
{
    std::unordered_map<Int32, String> column_name_by_source_id;
    for (UInt32 i = 0; i < schema_fields->size(); ++i)
    {
        auto schema_field = schema_fields->getObject(i);
        column_name_by_source_id[schema_field->getValue<Int32>(f_id)] = schema_field->getValue<String>(f_name);
    }

    ColumnsWithTypeAndName columns;
    std::unordered_set<String> used_column_names;
    for (UInt32 i = 0; i < spec_fields->size(); ++i)
    {
        auto spec_field = spec_fields->getObject(i);
        auto source_id = spec_field->getValue<Int32>(f_source_id);

        auto name_it = column_name_by_source_id.find(source_id);
        if (name_it == column_name_by_source_id.end())
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "DROP PARTITION is not supported for a partition spec whose source column (field id {}) is not a "
                "top-level column of the current table schema", source_id);

        const auto & column_name = name_it->second;

        if (!used_column_names.insert(column_name).second)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "DROP PARTITION is not supported for a partition spec that uses column '{}' in more than one "
                "partition field", column_name);

        auto name_and_type = schema_processor.tryGetFieldCharacteristics(schema_id, source_id);
        if (!name_and_type)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Partition source column '{}' (field id {}) is missing from Iceberg schema {}",
                column_name, source_id, schema_id);

        columns.emplace_back(name_and_type->type, column_name);
    }

    return Block(std::move(columns));
}

DataTypes resolvePartitionTypes(
    const Poco::JSON::Object & partition_spec,
    const Poco::JSON::Object & current_schema,
    const IcebergSchemaProcessor & schema_processor,
    Int32 schema_id,
    ContextPtr context)
{
    auto partition_fields = partition_spec.getArray(f_fields);
    auto schema_fields = current_schema.getArray(f_fields);

    if (!schema_fields || schema_fields->size() == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Could not find key '{}' in schema {} or fields is empty", f_fields, schema_id);

    SharedHeader sample_block
        = std::make_shared<const Block>(getPartitionSourceHeader(partition_fields, schema_fields, schema_processor, schema_id));

    ChunkPartitioner partitioner(partition_fields, schema_fields, context, sample_block);
    return partitioner.getResultTypes();
}

struct ManifestRewriteResult
{
    Int32 surviving_entries = 0;
    Int64 surviving_rows = 0;
    Int64 removed_entries = 0;
    Int64 min_sequence_number = 0;
};

/// Copy a manifest file without the entries whose `data_file.file_path` is in excluded_file_paths
ManifestRewriteResult rewriteManifestFileExcludingFiles(
    ReadBuffer & source,
    const std::unordered_set<String> & excluded_file_paths,
    Int64 inherited_sequence_number,
    Int64 inherited_snapshot_id,
    WriteBuffer & out)
{
    auto input_stream = std::make_unique<AvroInputStreamReadBufferAdapter>(source);
    auto reader_base = std::make_unique<avro::DataFileReaderBase>(std::move(input_stream), MAX_AVRO_SCHEMA_DEPTH);

    auto * reader_base_ptr = reader_base.get();
    avro::DataFileReader<avro::GenericDatum> reader(std::move(reader_base));

    const avro::ValidSchema & schema = reader.dataSchema();

    auto adapter = std::make_unique<OutputStreamWriteBufferAdapter>(out);
    avro::DataFileWriter<avro::GenericDatum> writer(std::move(adapter), schema);

    for (const auto & [key, value] : reader_base_ptr->metadata())
    {
        if (key.starts_with("avro.") && key != "avro.schema")
            continue;
        writer.setMetadata(key, String(value.begin(), value.end()));
    }

    ManifestRewriteResult result;
    Int64 min_sequence_number = std::numeric_limits<Int64>::max();

    avro::GenericDatum datum(schema);
    while (reader.read(datum))
    {
        avro::GenericRecord & entry = datum.value<avro::GenericRecord>();
        avro::GenericRecord & data_file = entry.field(f_data_file).value<avro::GenericRecord>();

        /// Copying a DELETED entry would revive it, since every entry below is restamped EXISTING.
        if (entry.field(f_status).value<Int32>() == static_cast<Int32>(ManifestEntryStatus::DELETED))
            continue;

        if (excluded_file_paths.contains(data_file.field(f_file_path).value<std::string>()))
        {
            ++result.removed_entries;
            continue;
        }

        auto materialize = [&](const String & field_name, Int64 inherited_value) -> Int64
        {
            if (!entry.hasField(field_name))
                return inherited_value;

            auto & field = entry.field(field_name);
            if (!field.isUnion())
                return field.value<Int64>();
            if (field.unionBranch() != 0)
                return field.value<Int64>();

            field.selectBranch(1);
            field.value<Int64>() = inherited_value;
            return inherited_value;
        };

        const Int64 sequence_number = materialize(f_sequence_number, inherited_sequence_number);
        materialize(f_file_sequence_number, inherited_sequence_number);
        materialize(f_snapshot_id, inherited_snapshot_id);

        entry.field(f_status) = avro::GenericDatum(static_cast<Int32>(ManifestEntryStatus::EXISTING));

        ++result.surviving_entries;
        result.surviving_rows += data_file.field(f_record_count).value<Int64>();
        min_sequence_number = std::min(min_sequence_number, sequence_number);

        writer.write(datum);
    }

    writer.close();

    result.min_sequence_number = result.surviving_entries > 0 ? min_sequence_number : inherited_sequence_number;
    return result;
}

}

AlterDropPartitionExecutor::AlterDropPartitionExecutor(
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
    const StorageID & storage_id_)
    : command(command_)
    , metadata(metadata_)
    , context(context_)
    , object_storage(std::move(object_storage_))
    , external_storages(external_storages_)
    , components(components_)
    , data_lake_settings(data_lake_settings_)
    , write_format(std::move(write_format_))
    , log(std::move(log_))
    , catalog(std::move(catalog_))
    , storage_id(storage_id_)
    , purge(context->getSettingsRef()[Setting::data_lake_delete_data_on_drop])
{
}

std::optional<AlterDropPartitionExecutor::SnapshotState> AlterDropPartitionExecutor::fetchSnapshotState()
{
    SnapshotState state;

    {
        const auto [metadata_version, metadata_file_path, compression_method] = getLatestMetadataFileAndVersionWithCatalog(
            object_storage,
            catalog,
            storage_id.getTableName(),
            components.table_path,
            data_lake_settings,
            components.metadata_cache,
            context,
            log.get(),
            components.table_uuid,
            components.metadata_compression_method);

        auto [snapshot, table_state] = metadata.getState(context, metadata_file_path, metadata_version);
        if (!snapshot)
            return std::nullopt;

        /// FIXME: in all other places schema_id is int32
        if (snapshot->schema_id_on_snapshot_commit > std::numeric_limits<Int32>::max())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Iceberg schema_id {} exceeds Int32 range", snapshot->schema_id_on_snapshot_commit);

        state.snapshot = std::move(snapshot);
        state.table_state = std::move(table_state);
    }

    auto compression_method = DB::Iceberg::getCompressionMethodFromMetadataFile(state.table_state.metadata_file_path);
    auto metadata_object = getMetadataJSONObject(
        state.table_state.metadata_file_path,
        object_storage,
        components.metadata_cache,
        context,
        log,
        compression_method,
        components.table_uuid);

    const auto format_version = metadata_object->getValue<Int32>(f_format_version);
    if (format_version != 2)
        throw Exception(
            ErrorCodes::NOT_IMPLEMENTED,
            "DROP PARTITION is supported only for Iceberg format-version 2, but the table has format-version {}",
            format_version);

    auto specs = metadata_object->getArray(f_partition_specs);
    if (!specs || specs->size() == 0)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS, "No 'partition-specs' or empty in metadata file {}", state.table_state.metadata_file_path);

    state.metadata_object = metadata_object;
    state.schema_id = static_cast<Int32>(state.snapshot->schema_id_on_snapshot_commit);
    state.partition_spec_id = metadata_object->getValue<Int64>(f_default_spec_id);

    auto partition_spec = lookupPartitionSpec(metadata_object, state.partition_spec_id);

    if (!partition_spec->has(f_fields))
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Default partition spec {} doesn't have '{}' key, metadata {}",
            state.partition_spec_id,
            f_fields,
            state.table_state.metadata_file_path);

    state.partition_spec = partition_spec;

    auto partition_fields = state.partition_spec->getArray(f_fields);
    for (size_t i = 0; i < partition_fields->size(); ++i)
    {
        auto field_object = partition_fields->getObject(static_cast<UInt32>(i));
        auto field_name = field_object->getValue<String>(f_name);
        state.partition_columns.emplace_back(std::move(field_name));
    }

    if (state.partition_columns.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "DROP PARTITION is not supported on unpartitioned Iceberg tables");

    state.spec_signature = getPartitionSpecSignature(partition_fields);

    auto schemas = metadata_object->getArray(f_schemas);
    if (!schemas || schemas->size() == 0)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Iceberg '{}' key not found in metadata {} or empty",
            f_schemas,
            state.table_state.metadata_file_path);

    const auto current_schema_id = IcebergMetadata::parseTableSchema(metadata_object, *components.schema_processor, context, log);

    Poco::JSON::Object::Ptr current_schema;
    for (size_t i = 0; schemas && i < schemas->size(); ++i)
    {
        auto schema = schemas->getObject(static_cast<UInt32>(i));
        if (!schema || schema->getValue<Int32>(f_schema_id) != current_schema_id)
            continue;

        current_schema = schema;
        break;
    }

    if (!current_schema)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Iceberg schema '{}' not found in metadata {} or empty",
            current_schema_id,
            state.table_state.metadata_file_path);

    state.partition_types
        = resolvePartitionTypes(*state.partition_spec, *current_schema, *components.schema_processor, current_schema_id, context);

    if (state.partition_types.size() != state.partition_columns.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Partitions types count doesn't match with number of partition columns");

    return state;
}

const std::vector<size_t> & AlterDropPartitionExecutor::getTargetFieldPositions(
    const ProcessedManifestFileEntry & entry, const SnapshotState & state, SpecFieldPositions & spec_field_positions) const
{
    const auto & entry_spec = entry.common_partition_specification;
    if (!entry_spec)
        throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "File {} has no partition specification", entry.parsed_entry->file_path_key.serialize());

    const auto & partition_key_value = entry.parsed_entry->partition_key_value;
    if (partition_key_value.size() != entry_spec->size())
        throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "File {} has {} partition values, but its partition spec has {} fields",
            entry.parsed_entry->file_path_key.serialize(), partition_key_value.size(), entry_spec->size());

    auto [it, inserted] = spec_field_positions.try_emplace(entry_spec.get());
    if (inserted)
        it->second = mapTargetFieldsToSpecPositions(*entry_spec, state.spec_signature);

    return it->second;
}

bool AlterDropPartitionExecutor::isInTargetPartition(
    const ProcessedManifestFileEntry & entry,
    const SnapshotState & state,
    const Row & target_partition,
    SpecFieldPositions & spec_field_positions) const
{
    const auto & positions = getTargetFieldPositions(entry, state, spec_field_positions);
    const auto & partition_key_value = entry.parsed_entry->partition_key_value;

    bool constrains_every_target_field = true;
    for (size_t i = 0; i < state.spec_signature.size(); ++i)
    {
        const auto position = positions[i];
        if (position == PARTITION_FIELD_NOT_IN_SPEC)
        {
            constrains_every_target_field = false;
            continue;
        }
        if (!accurateEquals(partition_key_value[position], target_partition[i]))
            return false;
    }

    if (!constrains_every_target_field)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "DROP PARTITION is not supported for Iceberg table {}: file {} was written under a partition spec "
            "that does not contain every field of the current spec ({}), so it spans more than the dropped partition",
            components.table_path, entry.parsed_entry->file_path_key.serialize(), state.partition_spec_id);

    return true;
}

AlterDropPartitionExecutor::TargetFilePaths
AlterDropPartitionExecutor::discoverTargetFilePaths(const SnapshotState & state, const Row & target_partition) const
{
    TargetFilePaths targets;
    SpecFieldPositions spec_field_positions;
    auto collect = [&](const std::vector<ProcessedManifestFileEntryPtr> & entries)
    {
        for (const auto & entry : entries)
        {
            const auto & parsed = entry->parsed_entry;

            if (!parsed)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Manifest file entry is not parsed");

            if (isInTargetPartition(*entry, state, target_partition, spec_field_positions))
                targets.emplace(components.path_resolver.resolve(parsed->file_path_key));
        }
    };

    for (const auto & manifest_key : state.snapshot->manifest_list_entries)
    {
        auto handle = getManifestFileEntriesHandle(
            object_storage, components, context, log, manifest_key, state.schema_id, external_storages);

        collect(handle.getFilesWithoutDeleted(FileContentType::DATA));
        collect(handle.getFilesWithoutDeleted(FileContentType::POSITION_DELETE));
        collect(handle.getFilesWithoutDeleted(FileContentType::EQUALITY_DELETE));
    }

    return targets;
}

AlterDropPartitionExecutor::DropPlan
AlterDropPartitionExecutor::buildDropPlan(const SnapshotState & state, const TargetFilePaths & targets, const Row & target_partition) const
{
    DropPlan result;
    auto unprocessed_target_file_paths = targets;

    UInt64 removed_data_files = 0;
    UInt64 removed_records = 0;
    UInt64 removed_files_size = 0;
    UInt64 removed_position_deletes = 0;
    UInt64 removed_position_delete_files = 0;
    UInt64 removed_equality_deletes = 0;
    UInt64 removed_equality_delete_files = 0;

    /// Paranoia. Partition's key values are converted from iceberg's manifest to clickhouse's representation
    /// We're trying to protect ourselves against ambiguous conversions which might lead to several partitions matches a single clickhouse's defintion
    Row partition_key_value_for_assert;
    SpecFieldPositions spec_field_positions;
    auto assert_on_ambigious_partition_key = [&](const ProcessedManifestFileEntry & entry)
    {
        const auto & parsed_entry = *entry.parsed_entry;
        if (!isInTargetPartition(entry, state, target_partition, spec_field_positions))
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Manifest file entry partition value ({}) is different from target ({})",
                fmt::join(parsed_entry.partition_key_value, ", "),
                fmt::join(target_partition, ", "));

        Row target_field_values;
        for (const auto position : getTargetFieldPositions(entry, state, spec_field_positions))
            target_field_values.push_back(parsed_entry.partition_key_value[position]);

        if (partition_key_value_for_assert.empty())
            partition_key_value_for_assert = std::move(target_field_values);
        else if (!std::ranges::equal(partition_key_value_for_assert, target_field_values))
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Manifest file entry partition value ({}) is different from previous one ({})",
                fmt::join(partition_key_value_for_assert, ", "),
                fmt::join(target_field_values, ", "));
        }
    };

    auto process_entries = [&](const auto & entries,
                               size_t & entries_to_keep,
                               size_t & entries_to_remove,
                               std::unordered_set<String> & excluded_file_paths)
    {
        for (const auto & entry : entries)
        {
            if (!entry->parsed_entry)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Manifest file entry is not parsed");

            const auto & parsed_entry = *entry->parsed_entry;
            const String storage_path = components.path_resolver.resolve(parsed_entry.file_path_key);
            if (!targets.contains(storage_path))
            {
                ++entries_to_keep;
                continue;
            }

            assert_on_ambigious_partition_key(*entry);

            ++entries_to_remove;
            unprocessed_target_file_paths.erase(storage_path);
            excluded_file_paths.insert(parsed_entry.file_path_key.serialize());
            if (purge)
                result.files_to_be_purged.push_back(parsed_entry.file_path_key);
            switch (parsed_entry.content_type)
            {
                case FileContentType::DATA:
                    ++removed_data_files;
                    removed_records += parsed_entry.record_count;
                    removed_files_size += parsed_entry.file_size_in_bytes;
                    break;
                case FileContentType::POSITION_DELETE:
                    ++removed_position_delete_files;
                    removed_position_deletes += parsed_entry.record_count;
                    removed_files_size += parsed_entry.file_size_in_bytes;
                    break;
                case FileContentType::EQUALITY_DELETE:
                    ++removed_equality_delete_files;
                    removed_equality_deletes += parsed_entry.record_count;
                    removed_files_size += parsed_entry.file_size_in_bytes;
                    break;
            }
        }
    };

    for (const auto & manifest_key : state.snapshot->manifest_list_entries)
    {
        auto handle = getManifestFileEntriesHandle(
            object_storage, components, context, log, manifest_key, state.schema_id, external_storages);

        size_t entries_to_keep = 0;
        size_t entries_to_remove = 0;
        std::unordered_set<String> excluded_file_paths;

        process_entries(handle.getFilesWithoutDeleted(FileContentType::DATA), entries_to_keep, entries_to_remove, excluded_file_paths);
        process_entries(
            handle.getFilesWithoutDeleted(FileContentType::POSITION_DELETE), entries_to_keep, entries_to_remove, excluded_file_paths);
        process_entries(
            handle.getFilesWithoutDeleted(FileContentType::EQUALITY_DELETE), entries_to_keep, entries_to_remove, excluded_file_paths);

        if (entries_to_remove == 0)
            continue;

        TargetManifest target_manifest{.manifest_key = manifest_key, .excluded_file_paths = std::move(excluded_file_paths)};
        if (entries_to_keep == 0)
            result.target_manifests.fully_matched.emplace_back(std::move(target_manifest));
        else
            result.target_manifests.partially_matched.emplace_back(std::move(target_manifest));
    }

    if (!unprocessed_target_file_paths.empty())
        throw Exception(
            ErrorCodes::CONCURRENT_ACCESS_NOT_SUPPORTED,
            "DROP PARTITION lost a race with a concurrent operation that replaced files in the "
            "target partition; please retry");

    result.snapshot_summary_update = Iceberg::SnapshotSummaryUpdateDelete{
        .deleted_data_files = removed_data_files,
        .removed_records = removed_records,
        .removed_files_size = removed_files_size,
        .removed_position_delete_files = removed_position_delete_files,
        .removed_position_deletes = removed_position_deletes,
        .removed_equality_delete_files = removed_equality_delete_files,
        .removed_equality_deletes = removed_equality_deletes,
        .num_partitions = 1};
    return result;
}

AlterDropPartitionExecutor::ManifestListWriteResult AlterDropPartitionExecutor::writeManifestList(
    SnapshotState & state, const DropPlan & plan, FileNamesGenerator & filename_generator, std::vector<String> & files_for_cleanup)
{
    auto parent_snapshot_id = state.metadata_object->getValue<Int64>(f_current_snapshot_id);
    auto metadata_info = filename_generator.generateMetadataPathWithInfo();

    /// TODO: make generateNextMetadata to accept SnapshotSummaryUpdate
    auto [new_snapshot, manifest_list_path] = MetadataGenerator{state.metadata_object}.generateNextMetadata(
        filename_generator,
        components.path_resolver.reverseResolve(state.table_state.metadata_file_path),
        parent_snapshot_id,
        /*added_files=*/0,
        /*added_records=*/0,
        /*added_files_size=*/0,
        /*num_partitions=*/0,
        /*added_delete_files=*/0,
        /*num_deleted_rows=*/0);

    Poco::JSON::Object::Ptr parent_snapshot;
    auto snapshots = state.metadata_object->getArray(f_snapshots);
    for (size_t i = 0; snapshots && i < snapshots->size(); ++i)
    {
        auto snapshot = snapshots->getObject(static_cast<UInt32>(i));
        if (snapshot && snapshot->getValue<Int64>(f_metadata_snapshot_id) == parent_snapshot_id)
        {
            parent_snapshot = snapshot;
            break;
        }
    }

    if (!parent_snapshot || !parent_snapshot->has(f_summary))
        throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION, "Parent snapshot {} has no summary", parent_snapshot_id);

    auto parent_summary = parent_snapshot->getObject(f_summary);
    auto parsed_summary = SnapshotSummary::fromJSON(*parent_summary, /*with_extra_fields=*/true, /*require_totals=*/true);
    if (!parsed_summary)
        throw Exception(
            ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "Cannot use summary of parent snapshot {} for DROP PARTITION: {}",
            parent_snapshot_id,
            parsed_summary.error());

    /// TODO: copy refresh cursor explicitly
    auto summary = SnapshotSummary{plan.snapshot_summary_update, parsed_summary->getTotals(), parsed_summary->getExtraFields()}.toJSON();
    new_snapshot->set(f_summary, summary);

    const String storage_manifest_list_path = components.path_resolver.resolve(manifest_list_path);
    files_for_cleanup.push_back(storage_manifest_list_path);

    std::unordered_set<String> removed_manifest_paths;
    for (const auto & tm : plan.target_manifests.fully_matched)
        removed_manifest_paths.insert(tm.manifest_key.manifest_file_path.serialize());

    std::unordered_set<String> carry_forward_manifest_paths;
    for (const auto & manifest_key : state.snapshot->manifest_list_entries)
    {
        const auto path = manifest_key.manifest_file_path.serialize();
        if (!removed_manifest_paths.contains(path))
            carry_forward_manifest_paths.insert(path);
    }

    auto manifest_actions = rewritePartiallyMatchedManifests(plan, filename_generator, files_for_cleanup);

    LOG_TRACE(log, "ALTER DROP PARTITION writing new manifest list {}", storage_manifest_list_path);

    auto buf = object_storage->writeObject(
        StoredObject(storage_manifest_list_path),
        WriteMode::Rewrite,
        /*attributes=*/std::nullopt,
        DBMS_DEFAULT_BUFFER_SIZE,
        context->getWriteSettings());

    generateManifestList(
        components.path_resolver,
        state.metadata_object,
        object_storage,
        external_storages,
        context,
        /*manifest_entry_names=*/{},
        new_snapshot,
        /*manifest_entry_sizes=*/{},
        *buf,
        FileContentType::DATA,
        /*use_previous_snapshots=*/false,
        /*per_entry_content_types=*/{},
        /*entry_counts=*/{},
        carry_forward_manifest_paths,
        /*entry_partition_spec_ids=*/{},
        /*entry_partition_summaries=*/{},
        manifest_actions);

    buf->finalize();

    return ManifestListWriteResult{.metadata_info = metadata_info, .new_snapshot = new_snapshot};
}

PreviousManifestActions AlterDropPartitionExecutor::rewritePartiallyMatchedManifests(
    const DropPlan & plan, FileNamesGenerator & filename_generator, std::vector<String> & files_for_cleanup)
{
    PreviousManifestActions manifest_actions;
    const auto & resolver = components.path_resolver;

    for (const auto & [source_manifest, excluded_file_paths] : plan.target_manifests.partially_matched)
    {
        auto rewritten_path = filename_generator.generateManifestEntryName();
        auto rewritten_storage_path = resolver.resolve(rewritten_path);
        files_for_cleanup.push_back(rewritten_storage_path);

        auto [source_storage, source_key] = resolveObjectStorageForPath(
            components.table_location,
            source_manifest.manifest_file_path.serialize(),
            object_storage,
            external_storages,
            context,
            resolver);

        RelativePathWithMetadata source_object_info(source_key);
        auto source_buf = createReadBuffer(source_object_info, source_storage, context, log);
        auto rewritten_buf = object_storage->writeObject(
            StoredObject(rewritten_storage_path),
            WriteMode::Rewrite, std::nullopt,
            DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

        auto rewrite_result = rewriteManifestFileExcludingFiles(
            *source_buf,
            excluded_file_paths,
            source_manifest.added_sequence_number,
            source_manifest.added_snapshot_id,
            *rewritten_buf);
        rewritten_buf->finalize();

        if (rewrite_result.removed_entries != static_cast<Int64>(excluded_file_paths.size()))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Rewriting manifest {} removed {} entries, expected {}",
                source_manifest.manifest_file_path.serialize(), rewrite_result.removed_entries,
                excluded_file_paths.size());

        auto rewritten_bytes = rewritten_buf->count();
        if (rewritten_bytes == 0)
        {
            rewritten_bytes = object_storage->getObjectMetadata(rewritten_storage_path, /*with_tags=*/false).size_bytes;
        }

        LOG_TRACE(log, "Rewrote manifest {} as {}: {} entries kept, {} removed",
            source_manifest.manifest_file_path.serialize(), rewritten_path.serialize(),
            rewrite_result.surviving_entries, rewrite_result.removed_entries);

        manifest_actions.emplace(
            source_manifest.manifest_file_path.serialize(),
            PreviousManifestAction{
                .new_path = rewritten_path,
                .new_length = static_cast<Int64>(rewritten_bytes),
                .new_existing_files_count = rewrite_result.surviving_entries,
                .new_existing_rows_count = rewrite_result.surviving_rows,
                .new_min_sequence_number = rewrite_result.min_sequence_number,
            });
    }

    return manifest_actions;
}

bool AlterDropPartitionExecutor::commitMetadataJSON(
    SnapshotState & state,
    FileNamesGenerator & filename_generator,
    const ManifestListWriteResult & manifest_list_result,
    std::vector<String> & files_for_cleanup,
    bool & published,
    bool * version_hint_names_metadata_file)
{
    const auto & metadata_info = manifest_list_result.metadata_info;
    std::string json_representation = stringifyJSON(state.metadata_object, 4);

    fiu_do_on(FailPoints::iceberg_writes_cleanup, { throw Exception(ErrorCodes::BAD_ARGUMENTS, "Failpoint for cleanup enabled"); });

    auto hint_path = filename_generator.generateVersionHint();

    const bool catalog_writes_metadata_file = catalog && catalog->isTransactional();
    if (!catalog_writes_metadata_file)
    {
        if (!writeMetadataFileAndVersionHint(
                components.path_resolver,
                metadata_info,
                json_representation,
                hint_path,
                object_storage,
                context,
                data_lake_settings[DataLakeStorageSetting::iceberg_use_version_hint],
                version_hint_names_metadata_file))
            return false;
    }

    if (!catalog)
        return true;

    if (!catalog_writes_metadata_file)
        files_for_cleanup.push_back(components.path_resolver.resolve(metadata_info.path));

    const auto & [namespace_name, table_name] = DataLake::parseTableName(storage_id.getTableName());
    const auto new_snapshot_id = manifest_list_result.new_snapshot->getValue<Int64>(f_metadata_snapshot_id);

    /// Assume readers can reach the snapshot until the catalog says it rejected the commit.
    published = true;
    if (!catalog->updateMetadata(
            namespace_name, table_name, components.path_resolver.resolveForCatalog(metadata_info.path), manifest_list_result.new_snapshot))
    {
        if (!catalogCommittedSnapshot(new_snapshot_id))
        {
            published = false;
            return false;
        }

        LOG_INFO(log, "Iceberg DROP PARTITION reported a failed catalog commit that the catalog accepted");
    }

    if (version_hint_names_metadata_file)
        *version_hint_names_metadata_file = true;

    return true;
}

bool AlterDropPartitionExecutor::catalogCommittedSnapshot(Int64 snapshot_id) const
{
    const auto [metadata_version, metadata_file_path, compression_method] = getLatestMetadataFileAndVersionWithCatalog(
        object_storage,
        catalog,
        storage_id.getTableName(),
        components.table_path,
        data_lake_settings,
        components.metadata_cache,
        context,
        log.get(),
        components.table_uuid,
        components.metadata_compression_method);

    auto metadata_object = getMetadataJSONObject(
        metadata_file_path, object_storage, components.metadata_cache, context, log, compression_method, components.table_uuid);

    auto snapshots = metadata_object->getArray(f_snapshots);
    for (size_t i = 0; snapshots && i < snapshots->size(); ++i)
    {
        auto snapshot = snapshots->getObject(static_cast<UInt32>(i));
        if (snapshot && snapshot->getValue<Int64>(f_metadata_snapshot_id) == snapshot_id)
            return true;
    }

    return false;
}

void AlterDropPartitionExecutor::cleanupNotCommited(std::vector<std::string> files)
{
    for (const auto & path : files)
    {
        try
        {
            object_storage->removeObjectIfExists(StoredObject(path));
        }
        catch (...)
        {
            tryLogCurrentException(log, fmt::format("Failed to clean up partially-written manifest {}", path));
        }
    }
}

bool AlterDropPartitionExecutor::tryCommit(SnapshotState & state, const DropPlan & plan)
{
    /// Match the table's current metadata compression instead of the table-init default: a long-lived
    /// table can move from uncompressed to e.g. `vN.gz.metadata.json` (external writer or changed
    /// setting), and the replacement metadata must follow the same convention.
    const auto compression_method = DB::Iceberg::getCompressionMethodFromMetadataFile(state.table_state.metadata_file_path);
    FileNamesGenerator filename_generator(
        components.path_resolver.getTableLocation(),
        catalog != nullptr && catalog->isTransactional(),
        compression_method,
        write_format);
    filename_generator.setVersion(state.table_state.metadata_version + 1);
    filename_generator.setCompressionMethod(compression_method);

    std::vector<std::string> files_for_cleanup;
    bool committed = false;
    /// Set once the new snapshot is reachable by readers, after which its files must survive.
    bool published = false;

    SCOPE_EXIT({
        if (!committed && !published)
            cleanupNotCommited(std::move(files_for_cleanup));
    });

    auto manifest_list_result = writeManifestList(state, plan, filename_generator, files_for_cleanup);
    const auto & metadata_info = manifest_list_result.metadata_info;

    bool version_hint_names_metadata_file = false;
    committed = commitMetadataJSON(
        state, filename_generator, manifest_list_result, files_for_cleanup, published, &version_hint_names_metadata_file);
    if (!committed)
        return false;

    LOG_INFO(
        log,
        "DROP PARTITION committed: removed {} data files ({} rows), {} position-delete files",
        plan.snapshot_summary_update.deleted_data_files,
        plan.snapshot_summary_update.removed_records,
        plan.snapshot_summary_update.removed_position_delete_files);

    if (purge)
        purgeDroppedFiles(plan, metadata_info, version_hint_names_metadata_file);

    return true;
}

void AlterDropPartitionExecutor::purgeDroppedFiles(
    const DropPlan & plan, const GeneratedMetadataFileWithInfo & metadata_info, bool version_hint_names_metadata_file)
{
    if (!version_hint_names_metadata_file)
    {
        LOG_WARNING(log, "Skipping the purge after DROP PARTITION: version-hint.text does not name {}, so readers "
            "that follow it still reference the dropped files", components.path_resolver.resolve(metadata_info.path));
        return;
    }

    std::unordered_map<ObjectStoragePtr, StoredObjects> batches;
    for (const auto & file_to_purge : plan.files_to_be_purged)
    {
        auto [storage, key] = resolveObjectStorageForPath(
            components.table_location,
            file_to_purge.serialize(),
            object_storage,
            external_storages,
            context,
            components.path_resolver);

        batches[storage].emplace_back(key);
    }

    for (auto & [storage, batch] : batches)
    {
        try
        {
            storage->removeObjectsIfExist(batch);
        }
        catch (...)
        {
            Strings surviving_paths;
            surviving_paths.reserve(batch.size());
            for (const auto & object : batch)
                surviving_paths.push_back(object.remote_path);

            tryLogCurrentException(log, fmt::format(
                "Failed to purge data files after DROP PARTITION, these files may still exist in storage {} and "
                "are not retried by re-running the query: {}",
                storage->getDescription(), fmt::join(surviving_paths, ", ")));
        }
    }
}

void AlterDropPartitionExecutor::checkPurgeIsGranted(const DropPlan & plan) const
{
    for (const auto & file_to_purge : plan.files_to_be_purged)
    {
        if (!isPathWriteGranted(components.table_location, file_to_purge.serialize(), object_storage, context))
            throw Exception(
                ErrorCodes::ACCESS_DENIED,
                "Not enough privileges to drop the Iceberg partition: `data_lake_delete_data_on_drop` deletes the file '{}', "
                "which requires the `WRITE` grant on it",
                file_to_purge);
    }
}

void AlterDropPartitionExecutor::run()
{
    if (!catalog && data_lake_settings[DataLakeStorageSetting::iceberg_metadata_file_path].changed)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "DROP PARTITION does not support the iceberg_metadata_file_path setting");

    const auto & settings = context->getSettingsRef();
    if (settings[Setting::iceberg_snapshot_id].changed || settings[Setting::iceberg_timestamp_ms].changed)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "DROP PARTITION does not support iceberg_snapshot_id or iceberg_timestamp_ms settings");

    const auto & partition_ast = command.partition->as<ASTPartition &>();
    validateDropPartitionAST(partition_ast, command);

    TargetFilePaths targets;

    for (int attempt = 0; attempt < MAX_TRANSACTION_RETRIES; ++attempt)
    {
        auto state_opt = fetchSnapshotState();
        if (!state_opt)
        {
            LOG_DEBUG(log, "Table has no snapshot, nothing to drop");
            return;
        }
        SnapshotState & state = *state_opt;

        const auto target_partition = parsePartitionTuple(*partition_ast.value, state.partition_types, context);

        if (attempt == 0)
        {
            targets = discoverTargetFilePaths(state, target_partition);
            if (targets.empty())
            {
                LOG_INFO(log, "No data files match the requested partition; DROP PARTITION is a no-op");
                return;
            }
            FailPointInjection::pauseFailPoint(FailPoints::iceberg_drop_partition_pause_after_discovery);
        }

        auto plan = buildDropPlan(state, targets, target_partition);
        if (purge)
            checkPurgeIsGranted(plan);

        if (tryCommit(state, plan))
            return;
    }

    throw Exception(ErrorCodes::LIMIT_EXCEEDED, "Too many unsuccessful retries to drop partition in Iceberg table");
}

}
}

// NOLINTEND(clang-analyzer-core.uninitialized.UndefReturn)
#endif
