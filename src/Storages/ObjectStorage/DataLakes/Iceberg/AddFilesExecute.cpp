#include <Storages/ObjectStorage/DataLakes/Iceberg/AddFilesExecute.h>

#include "config.h"

#if USE_AVRO

#include <Columns/ColumnString.h>
#include <Core/Block.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <Databases/DataLake/Common.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteSettings.h>
#include <Interpreters/Context.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/ObjectStorage/DataLakes/Common/AvroForIcebergDeserializer.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ExecuteCommandArgs.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/FileNamesGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataFileEntry.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MetadataGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/StatelessMetadataFileGetter.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Common/logger_useful.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/String.h>

#include <arrow/api.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>
#include <Processors/Formats/Impl/ArrowBufferedStreams.h>
#include <Common/FieldVisitorToString.h>

#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_set>

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int ICEBERG_SPECIFICATION_VIOLATION;
extern const int LOGICAL_ERROR;
extern const int METADATA_MISMATCH;
extern const int SUPPORT_IS_DISABLED;
}

namespace DataLakeStorageSetting
{
extern const DataLakeStorageSettingsBool iceberg_use_version_hint;
}

namespace Iceberg
{

namespace
{

ExecuteCommandArgs makeSchema()
{
    ExecuteCommandArgs schema("add_files");
    schema.addPositional("paths");
    schema.addNamed("check_duplicate_files", Field::Types::UInt64);
    schema.addNamed("allow_missing_columns", Field::Types::UInt64);
    schema.addDefault("check_duplicate_files", Field(UInt64(1)));
    schema.addDefault("allow_missing_columns", Field(UInt64(0)));
    return schema;
}

std::vector<String> parsePathsArgument(const Field & field)
{
    std::vector<String> paths;
    if (field.getType() == Field::Types::String)
    {
        paths.push_back(field.safeGet<String>());
    }
    else if (field.getType() == Field::Types::Array)
    {
        for (const auto & item : field.safeGet<Array>())
            paths.push_back(item.safeGet<String>());
    }
    else
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "add_files expects 'paths' to be a String or an Array of Strings, got {}", field.getTypeName());
    }
    if (paths.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "add_files expects at least one path");
    return paths;
}

bool hasHiddenPathComponent(const String & path)
{
    size_t start = 0;
    while (start <= path.size())
    {
        size_t slash = path.find('/', start);
        String component = path.substr(start, slash == String::npos ? String::npos : slash - start);
        if (!component.empty() && component[0] == '.')
            return true;
        if (slash == String::npos)
            break;
        start = slash + 1;
    }
    return false;
}

bool endsWithParquet(const String & path)
{
    return Poco::toLower(path).ends_with(".parquet");
}

/// Mirrors the anonymous-namespace helper in IcebergMetadata.cpp: find the partition spec
/// object with the given spec-id inside a metadata JSON document; throws if absent.
Poco::JSON::Object::Ptr lookupPartitionSpec(const Poco::JSON::Object::Ptr & metadata, Int64 spec_id)
{
    auto specs = metadata->getArray(Iceberg::f_partition_specs);
    for (size_t i = 0; i < specs->size(); ++i)
    {
        auto spec = specs->getObject(static_cast<UInt32>(i));
        if (spec->getValue<Int64>(Iceberg::f_spec_id) == spec_id)
            return spec;
    }
    throw Exception(ErrorCodes::METADATA_MISMATCH,
        "Partition spec with id {} not found in table metadata", spec_id);
}

/// Find the schema object with the given schema-id inside a metadata JSON document; throws if absent.
Poco::JSON::Object::Ptr lookupSchema(const Poco::JSON::Object::Ptr & metadata, Int64 schema_id)
{
    auto schemas = metadata->getArray(Iceberg::f_schemas);
    for (size_t i = 0; i < schemas->size(); ++i)
    {
        auto schema = schemas->getObject(static_cast<UInt32>(i));
        if (schema->getValue<Int32>(Iceberg::f_schema_id) == schema_id)
            return schema;
    }
    throw Exception(ErrorCodes::METADATA_MISMATCH,
        "Schema with id {} not found in table metadata", schema_id);
}

/// Expand trailing-slash paths into concrete *.parquet files below them.
std::vector<String> expandPaths(
    const std::vector<String> & raw_paths,
    const IcebergPathResolver & resolver,
    const ObjectStoragePtr & object_storage,
    LoggerPtr log)
{
    std::vector<String> result;
    for (const auto & raw : raw_paths)
    {
        if (!raw.ends_with('/'))
        {
            result.push_back(raw);
            continue;
        }
        RelativePathsWithMetadata children;
        object_storage->listObjects(resolver.resolve(IcebergPathFromMetadata::deserialize(raw)), children, /* max_keys */ 0);
        for (const auto & child : children)
        {
            if (!endsWithParquet(child->relative_path) || hasHiddenPathComponent(child->relative_path))
                continue;
            /// Listed keys are storage paths; convert to the metadata (location-prefixed) form
            /// that native writes register in manifests.
            result.push_back(resolver.reverseResolve(child->relative_path).serialize());
        }
        LOG_DEBUG(log, "add_files: path {} expanded to {} parquet file(s)", raw, result.size());
    }
    return result;
}

Int64 floorDiv(Int64 a, Int64 b)
{
    Int64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

/// Howard Hinnant's civil-from-days algorithm.
void civilFromDays(Int64 z, Int64 & y, unsigned & m, unsigned & d)
{
    z += 719468;
    Int64 era = floorDiv(z, 146097);
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    Int64 year = static_cast<Int64>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));
    y = year + (m <= 2 ? 1 : 0);
}

enum class SourceValueKind
{
    Integer,   /// int32 / int64 (also date days and timestamp micros)
    String,
    Unsupported
};

/// Decoded min/max of one parquet column across all row groups, in encoded-bytes form
/// for equality and in decoded form for transforms.
struct ColumnBounds
{
    bool has_bounds = false;
    /// For Integer bounds: whether the parquet physical type is INT32 (4-byte LE serialization)
    /// rather than INT64 (8-byte). Decided by the physical type, never by the value range.
    bool int_is_32bit = false;
    Int64 min_int = 0;
    Int64 max_int = 0;
    String min_str;
    String max_str;
    SourceValueKind kind = SourceValueKind::Unsupported;
};

template <typename T>
T decodeLE(const String & bytes)
{
    T value = 0;
    memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(T)));
    return value;
}

/// Statistics of one data file derived from its parquet footer.
struct AnalyzedFile
{
    String registration_path;   /// path form stored in the manifest
    String storage_path;        /// resolved path used for reads
    IcebergSerializedFileStats stats;
    std::unordered_map<Int32, ColumnBounds> bounds_by_field_id;
};

ColumnBounds aggregateColumnBounds(
    const parquet::ColumnChunkMetaData & column,
    ColumnBounds acc)
{
    if (!column.is_stats_set())
        return acc;
    auto stats = column.statistics();
    if (!stats || !stats->HasMinMax())
        return acc;

    String encoded_min = stats->EncodeMin();
    String encoded_max = stats->EncodeMax();
    auto physical_type = stats->physical_type();

    SourceValueKind kind = SourceValueKind::Unsupported;
    if (physical_type == parquet::Type::INT32 || physical_type == parquet::Type::INT64)
        kind = SourceValueKind::Integer;
    else if (physical_type == parquet::Type::BYTE_ARRAY || physical_type == parquet::Type::FIXED_LEN_BYTE_ARRAY)
        kind = SourceValueKind::String;

    if (kind == SourceValueKind::Unsupported)
        return acc;

    if (!acc.has_bounds)
    {
        acc.kind = kind;
        if (kind == SourceValueKind::Integer)
        {
            acc.int_is_32bit = physical_type == parquet::Type::INT32;
            acc.min_int = physical_type == parquet::Type::INT32 ? Int64(decodeLE<Int32>(encoded_min)) : decodeLE<Int64>(encoded_min);
            acc.max_int = physical_type == parquet::Type::INT32 ? Int64(decodeLE<Int32>(encoded_max)) : decodeLE<Int64>(encoded_max);
        }
        else
        {
            acc.min_str = encoded_min;
            acc.max_str = encoded_max;
        }
        acc.has_bounds = true;
        return acc;
    }

    if (kind != acc.kind)
        return acc;

    if (kind == SourceValueKind::Integer)
    {
        Int64 this_min = physical_type == parquet::Type::INT32 ? Int64(decodeLE<Int32>(encoded_min)) : decodeLE<Int64>(encoded_min);
        Int64 this_max = physical_type == parquet::Type::INT32 ? Int64(decodeLE<Int32>(encoded_max)) : decodeLE<Int64>(encoded_max);
        acc.min_int = std::min(acc.min_int, this_min);
        acc.max_int = std::max(acc.max_int, this_max);
    }
    else
    {
        acc.min_str = std::min(acc.min_str, encoded_min);
        acc.max_str = std::max(acc.max_str, encoded_max);
    }
    return acc;
}

/// Read the parquet footer of one file and derive its Iceberg statistics and per-column bounds.
AnalyzedFile analyzeFile(
    const String & registration_path,
    const IcebergPathResolver & resolver,
    const ObjectStoragePtr & object_storage,
    const std::unordered_map<String, Int64> & column_path_to_field_id,
    const ContextPtr & context,
    const FormatSettings & format_settings)
{
    AnalyzedFile result;
    result.registration_path = registration_path;

    IcebergPathFromMetadata metadata_path = IcebergPathFromMetadata::deserialize(registration_path);
    result.storage_path = resolver.resolve(metadata_path);

    auto object_metadata = object_storage->tryGetObjectMetadata(result.storage_path, /* with_tags */ false);
    if (!object_metadata.has_value())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "add_files: file {} does not exist", registration_path);
    result.stats.file_size_in_bytes = static_cast<Int64>(object_metadata->size_bytes);

    std::shared_ptr<parquet::FileMetaData> parquet_metadata;
    try
    {
        std::atomic<int> is_stopped = 0;
        auto buffer = object_storage->readObject(StoredObject(result.storage_path), context->getReadSettings());
        auto arrow_file = asArrowFile(*buffer, format_settings, is_stopped, "Parquet", PARQUET_MAGIC_BYTES, /* avoid_buffering */ true);
        parquet_metadata = parquet::ReadMetaData(arrow_file);
    }
    catch (...)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "add_files: failed to read the parquet footer of {} ({}). The file may be truncated "
            "or still being written by another process; registering such a file is the caller's responsibility. "
            "Original error: {}",
            registration_path, result.storage_path, getCurrentExceptionMessage(false));
    }

    result.stats.record_count = parquet_metadata->num_rows();

    std::unordered_map<Int32, Int64> column_sizes;
    std::unordered_map<Int32, Int64> null_value_counts;
    std::unordered_map<Int32, ColumnBounds> bounds;

    for (int rg = 0; rg < parquet_metadata->num_row_groups(); ++rg)
    {
        auto row_group = parquet_metadata->RowGroup(rg);
        for (int col = 0; col < row_group->num_columns(); ++col)
        {
            auto column = row_group->ColumnChunk(col);
            String dot_path = column->path_in_schema()->ToDotString();
            auto field_it = column_path_to_field_id.find(dot_path);
            if (field_it == column_path_to_field_id.end())
                continue;
            Int32 field_id = static_cast<Int32>(field_it->second);

            column_sizes[field_id] += column->total_compressed_size();
            if (column->is_stats_set())
            {
                auto stats = column->statistics();
                if (stats && stats->HasNullCount())
                    null_value_counts[field_id] += stats->null_count();
            }
            bounds[field_id] = aggregateColumnBounds(*column, std::move(bounds[field_id]));
        }
    }

    for (const auto & [field_id, size] : column_sizes)
        result.stats.column_sizes.emplace_back(field_id, size);
    for (const auto & [field_id, count] : null_value_counts)
        result.stats.null_value_counts.emplace_back(field_id, count);
    for (const auto & [field_id, bound] : bounds)
    {
        if (!bound.has_bounds)
            continue;
        if (bound.kind == SourceValueKind::Integer)
        {
            /// Pass the parquet plain encoding through: both parquet statistics and Iceberg
            /// bounds store fixed-width numerics little-endian, with the width of the parquet
            /// physical type (INT32 -> 4 bytes, INT64 -> 8 bytes).
            std::vector<uint8_t> lower_bytes;
            std::vector<uint8_t> upper_bytes;
            if (bound.int_is_32bit)
            {
                Int32 lo = static_cast<Int32>(bound.min_int);
                Int32 hi = static_cast<Int32>(bound.max_int);
                lower_bytes.resize(sizeof(Int32));
                upper_bytes.resize(sizeof(Int32));
                memcpy(lower_bytes.data(), &lo, sizeof(Int32));
                memcpy(upper_bytes.data(), &hi, sizeof(Int32));
            }
            else
            {
                Int64 lo = bound.min_int;
                Int64 hi = bound.max_int;
                lower_bytes.resize(sizeof(Int64));
                upper_bytes.resize(sizeof(Int64));
                memcpy(lower_bytes.data(), &lo, sizeof(Int64));
                memcpy(upper_bytes.data(), &hi, sizeof(Int64));
            }
            result.stats.lower_bounds.emplace_back(field_id, std::move(lower_bytes));
            result.stats.upper_bounds.emplace_back(field_id, std::move(upper_bytes));
        }
        else
        {
            std::vector<uint8_t> lower_bytes(bound.min_str.begin(), bound.min_str.end());
            std::vector<uint8_t> upper_bytes(bound.max_str.begin(), bound.max_str.end());
            result.stats.lower_bounds.emplace_back(field_id, std::move(lower_bytes));
            result.stats.upper_bounds.emplace_back(field_id, std::move(upper_bytes));
        }
    }

    result.bounds_by_field_id = std::move(bounds);
    return result;
}

/// The value one partition field takes for a file, proven identical for every row in the file.
struct PartitionFieldValue
{
    String column_name;
    Field value;        /// Null when the transform is void or the column has only nulls
    DataTypePtr type;   /// ClickHouse type used for manifest tuple serialization
};

/// Derive the partition tuple of one file from its per-column bounds and prove that every row
/// in the file belongs to that single partition. Fails with an error naming the file and the
/// offending column when a file spans partitions.
PartitionFieldValue derivePartitionField(
    const Poco::JSON::Object::Ptr & spec_field,
    const AnalyzedFile & file,
    const std::unordered_map<Int64, NameAndTypePair> & source_columns,
    const std::unordered_map<Int64, String> & iceberg_type_by_field_id)
{
    Int64 source_id = spec_field->getValue<Int64>(Iceberg::f_source_id);
    String transform = spec_field->getValue<String>(Iceberg::f_transform);
    String field_name = spec_field->getValue<String>(Iceberg::f_name);

    PartitionFieldValue result;
    result.column_name = field_name;

    auto source_it = source_columns.find(source_id);
    if (source_it == source_columns.end())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "add_files: partition field '{}' refers to source id {} which is not in the current schema", field_name, source_id);
    const NameAndTypePair & source_column = source_it->second;

    if (transform == "void")
    {
        result.value = Null();
        result.type = source_column.type;
        return result;
    }

    /// Parse bucket[N] / truncate[N].
    auto bracket = transform.find('[');
    String transform_name = bracket == String::npos ? transform : transform.substr(0, bracket);
    Int64 transform_argument = 0;
    if (bracket != String::npos)
        transform_argument = std::stoll(transform.substr(bracket + 1, transform.size() - bracket - 2));

    if (transform_name == "bucket")
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "add_files: partition field '{}' uses the bucket transform, which cannot be validated from "
            "file statistics alone (two different values may share a bucket with a third outside it). "
            "Files for bucket-partitioned tables are not supported yet", field_name);

    auto bounds_it = file.bounds_by_field_id.find(static_cast<Int32>(source_id));
    if (bounds_it == file.bounds_by_field_id.end() || !bounds_it->second.has_bounds)
    {
        /// Column absent from the file's statistics (e.g. all values are null): the partition
        /// value is null. Note this trusts the file's null-only stats; all-null columns carry
        /// no min/max by definition.
        result.value = Null();
        result.type = transform_name == "identity" || transform_name == "truncate" ? source_column.type : DataTypePtr(std::make_shared<DataTypeInt32>());
        return result;
    }
    const ColumnBounds & bounds = bounds_it->second;

    auto iceberg_type_it = iceberg_type_by_field_id.find(source_id);
    const String iceberg_type = iceberg_type_it != iceberg_type_by_field_id.end() ? iceberg_type_it->second : "";
    bool is_timestamp = iceberg_type.starts_with("timestamp");
    bool is_date = iceberg_type == "date";

    if (transform_name == "identity")
    {
        bool equal = bounds.kind == SourceValueKind::Integer ? bounds.min_int == bounds.max_int : bounds.min_str == bounds.max_str;
        if (!equal)
            throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
                "add_files: file {} spans more than one partition: column '{}' has distinct min and max values",
                file.registration_path, source_column.name);
        if (bounds.kind == SourceValueKind::Integer)
        {
            if (is_date)
            {
                /// Mirrors the native writer: identity Date partition values are the raw day
                /// number promoted to UInt64 (ColumnVector<UInt16> -> Field).
                result.value = Field(UInt64(bounds.min_int));
            }
            else if (is_timestamp)
            {
                /// Mirrors the native writer: a DateTime64 partition column carries its raw
                /// decimal value (scale <= 6, i.e. microseconds for Iceberg timestamp types).
                auto scale = getDecimalScale(*removeNullable(source_column.type));
                result.value = Field(DecimalField<Decimal64>(Decimal64(bounds.min_int), scale));
            }
            else
            {
                result.value = Field(bounds.min_int);
            }
        }
        else
        {
            result.value = Field(bounds.min_str);
        }
        result.type = source_column.type;
        return result;
    }

    if (bounds.kind != SourceValueKind::Integer && transform_name != "truncate")
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "add_files: partition field '{}' applies transform '{}' to a non-numeric column '{}'",
            field_name, transform, source_column.name);

    auto days_of = [&](Int64 v) { return is_timestamp ? floorDiv(v, 86400000000LL) : v; };
    auto year_of = [&](Int64 v)
    {
        Int64 y;
        unsigned m, d;
        civilFromDays(days_of(v), y, m, d);
        return y - 1970;
    };
    auto month_of = [&](Int64 v)
    {
        Int64 y;
        unsigned m, d;
        civilFromDays(days_of(v), y, m, d);
        return (y - 1970) * 12 + static_cast<Int64>(m) - 1;
    };

    Int64 lo = bounds.min_int;
    Int64 hi = bounds.max_int;
    Int64 lo_transformed = 0;
    Int64 hi_transformed = 0;

    if (transform_name == "year")
    {
        lo_transformed = year_of(lo);
        hi_transformed = year_of(hi);
    }
    else if (transform_name == "month")
    {
        lo_transformed = month_of(lo);
        hi_transformed = month_of(hi);
    }
    else if (transform_name == "day" || transform_name == "days" || transform_name == "date" || transform_name == "dates")
    {
        lo_transformed = days_of(lo);
        hi_transformed = days_of(hi);
    }
    else if (transform_name == "hour" || transform_name == "hours")
    {
        lo_transformed = floorDiv(lo, 3600000000LL);
        hi_transformed = floorDiv(hi, 3600000000LL);
    }
    else if (transform_name == "truncate")
    {
        if (bounds.kind == SourceValueKind::String)
        {
            String lo_trunc = bounds.min_str.substr(0, static_cast<size_t>(transform_argument));
            String hi_trunc = bounds.max_str.substr(0, static_cast<size_t>(transform_argument));
            if (lo_trunc != hi_trunc)
                throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
                    "add_files: file {} spans more than one partition: truncated values of column '{}' differ",
                    file.registration_path, source_column.name);
            result.value = Field(lo_trunc);
            result.type = source_column.type;
            return result;
        }
        lo_transformed = lo - ((lo % transform_argument + transform_argument) % transform_argument);
        hi_transformed = hi - ((hi % transform_argument + transform_argument) % transform_argument);
    }
    else
    {
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "add_files: partition field '{}' uses unsupported transform '{}'", field_name, transform);
    }

    if (lo_transformed != hi_transformed)
        throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
            "add_files: file {} spans more than one partition: column '{}' maps to different '{}' buckets "
            "(min {} -> {}, max {} -> {})",
            file.registration_path, source_column.name, transform_name, lo, lo_transformed, hi, hi_transformed);

    result.value = Field(lo_transformed);
    result.type = transform_name == "truncate" ? source_column.type : DataTypePtr(std::make_shared<DataTypeInt32>());
    return result;
}

String partitionKeyToString(const std::vector<Field> & values)
{
    WriteBufferFromOwnString buf;
    for (const auto & value : values)
        writeString(applyVisitor(FieldVisitorToString(), value), buf);
    return buf.str();
}

/// Collect the paths of all data files live in the current snapshot, for duplicate detection.
std::unordered_set<String> listLiveDataFiles(
    Poco::JSON::Object::Ptr metadata,
    const IcebergPathResolver & resolver,
    const ObjectStoragePtr & object_storage,
    const FormatSettings & format_settings)
{
    std::unordered_set<String> live_paths;
    if (!metadata->has(Iceberg::f_current_snapshot_id) || metadata->isNull(Iceberg::f_current_snapshot_id))
        return live_paths;

    Int64 current_snapshot_id = metadata->getValue<Int64>(Iceberg::f_current_snapshot_id);
    if (current_snapshot_id < 0)
        return live_paths;
    String manifest_list;
    for (const auto & snapshot_var : *metadata->getArray(Iceberg::f_snapshots))
    {
        auto snapshot = snapshot_var.extract<Poco::JSON::Object::Ptr>();
        if (snapshot->getValue<Int64>(Iceberg::f_metadata_snapshot_id) == current_snapshot_id)
        {
            manifest_list = snapshot->getValue<String>(Iceberg::f_manifest_list);
            break;
        }
    }
    if (manifest_list.empty())
        return live_paths;

    auto read_avro = [&](const String & path)
    {
        auto buffer = object_storage->readObject(
            StoredObject(resolver.resolve(IcebergPathFromMetadata::deserialize(path))), ReadSettings{});
        return AvroForIcebergDeserializer(std::move(buffer), IcebergPathFromMetadata::deserialize(path), format_settings);
    };

    auto manifest_list_reader = read_avro(manifest_list);
    for (size_t i = 0; i < manifest_list_reader.rows(); ++i)
    {
        /// Only data manifests (content == 0) hold data files.
        if (manifest_list_reader.getValueFromRowByName(i, Iceberg::f_content, TypeIndex::Int32).safeGet<Int32>() != 0)
            continue;
        String manifest_path = manifest_list_reader.getValueFromRowByName(i, Iceberg::f_manifest_path).safeGet<String>();
        auto manifest_reader = read_avro(manifest_path);
        for (size_t j = 0; j < manifest_reader.rows(); ++j)
        {
            auto entry = manifest_reader.getParsedManifestFileEntry(j);
            if (entry->status != ManifestEntryStatus::DELETED)
                live_paths.insert(entry->file_path_key.serialize());
        }
    }
    return live_paths;
}

Pipe resultToPipe(const AddFilesResult & result)
{
    Block header{
        ColumnWithTypeAndName(std::make_shared<DataTypeString>(), "metric_name"),
        ColumnWithTypeAndName(std::make_shared<DataTypeInt64>(), "metric_value"),
    };

    MutableColumns columns = header.cloneEmptyColumns();
    auto add = [&](const char * name, Int64 value)
    {
        columns[0]->insert(String(name));
        columns[1]->insert(value);
    };

    add("files_registered", result.files_registered);
    add("rows_registered", result.rows_registered);
    add("bytes_registered", result.bytes_registered);
    add("partitions_touched", result.partitions_touched);
    add("manifests_written", result.manifests_written);

    const size_t rows = columns[0]->size();
    Chunk chunk(std::move(columns), rows);
    return Pipe(std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(std::move(header)), std::move(chunk)));
}

}

AddFilesResult addFiles(
    const std::vector<String> & raw_paths,
    bool check_duplicate_files,
    bool allow_missing_columns,
    ContextPtr context,
    ObjectStoragePtr object_storage,
    const DataLakeStorageSettings & data_lake_settings,
    const PersistentTableComponents & persistent_components,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const String & table_name,
    ExternalStorageCache & external_storages)
{
    if (Poco::toLower(write_format) != "parquet")
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "add_files currently supports only Parquet data files, the table's write format is '{}'", write_format);

    const auto & resolver = persistent_components.path_resolver;
    LoggerPtr log = getLogger("IcebergAddFiles");
    FormatSettings format_settings = getFormatSettings(context);

    FileNamesGenerator filename_generator(
        resolver.getTableLocation(),
        (catalog != nullptr && catalog->isTransactional()),
        persistent_components.metadata_compression_method,
        write_format);

    auto [last_version, metadata_path, compression_method] = getLatestOrExplicitMetadataFileAndVersion(
        object_storage,
        persistent_components.table_path,
        data_lake_settings,
        persistent_components.metadata_cache,
        context,
        getLogger("IcebergAddFiles").get(),
        persistent_components.table_uuid,
        persistent_components.metadata_compression_method,
        true);
    filename_generator.setVersion(last_version + 1);

    Poco::JSON::Object::Ptr metadata = getMetadataJSONObject(
        metadata_path,
        object_storage,
        persistent_components.metadata_cache,
        context,
        log,
        compression_method,
        persistent_components.table_uuid);

    const Int64 original_schema_id = metadata->getValue<Int64>(Iceberg::f_current_schema_id);
    const Int64 partition_spec_id = metadata->getValue<Int64>(Iceberg::f_default_spec_id);
    Poco::JSON::Object::Ptr partition_spec = lookupPartitionSpec(metadata, partition_spec_id);
    Poco::JSON::Object::Ptr current_schema = lookupSchema(metadata, original_schema_id);
    Poco::JSON::Array::Ptr schema_fields = current_schema->getArray(Iceberg::f_fields);

    /// Column path -> field id and back, over all (including nested) leaf fields.
    std::unordered_map<String, Int64> column_path_to_field_id = IcebergSchemaProcessor::traverseSchema(schema_fields);
    std::unordered_map<Int64, String> field_id_to_column_path;
    for (const auto & [path, id] : column_path_to_field_id)
        field_id_to_column_path.emplace(id, path);

    /// Iceberg primitive type per field id (for date/timestamp transform scaling).
    std::unordered_map<Int64, String> iceberg_type_by_field_id;
    {
        std::function<void(Poco::JSON::Object::Ptr, const String &)> collect_types;
        collect_types = [&](Poco::JSON::Object::Ptr field, const String & prefix)
        {
            String name = prefix.empty() ? field->getValue<String>(Iceberg::f_name) : prefix + "." + field->getValue<String>(Iceberg::f_name);
            Int64 id = field->getValue<Int64>(Iceberg::f_id);
            auto type_var = field->get(Iceberg::f_type);
            if (type_var.isString())
            {
                iceberg_type_by_field_id[id] = type_var.extract<String>();
            }
            else if (type_var.isStruct())
            {
                auto type_obj = type_var.extract<Poco::JSON::Object::Ptr>();
                String kind = type_obj->getValue<String>(Iceberg::f_type);
                if (kind == "struct")
                {
                    for (const auto & nested : *type_obj->getArray(Iceberg::f_fields))
                        collect_types(nested.extract<Poco::JSON::Object::Ptr>(), name);
                }
            }
        };
        for (const auto & field_var : *schema_fields)
            collect_types(field_var.extract<Poco::JSON::Object::Ptr>(), "");
    }

    /// Source columns for partition fields, from the ClickHouse schema (top-level fields only).
    /// The schema processor is populated lazily (e.g. by reads or native writes); on a freshly
    /// created table this add_files call may be the first operation, so register the current
    /// schema ourselves first.
    if (!persistent_components.schema_processor->hasClickHouseTableSchemaById(static_cast<Int32>(original_schema_id)))
        persistent_components.schema_processor->addIcebergTableSchema(current_schema, context);
    auto clickhouse_schema = persistent_components.schema_processor->getClickHouseTableSchemaById(static_cast<Int32>(original_schema_id));
    Block sample_block;
    for (const auto & name_and_type : *clickhouse_schema)
        sample_block.insert(ColumnWithTypeAndName(name_and_type.type, name_and_type.name));
    SharedHeader shared_sample_block = std::make_shared<const Block>(sample_block);

    std::unordered_map<Int64, NameAndTypePair> source_columns;
    Poco::JSON::Array::Ptr spec_fields = partition_spec->getArray(Iceberg::f_fields);
    for (const auto & spec_field_var : *spec_fields)
    {
        auto spec_field = spec_field_var.extract<Poco::JSON::Object::Ptr>();
        Int64 source_id = spec_field->getValue<Int64>(Iceberg::f_source_id);
        auto path_it = field_id_to_column_path.find(source_id);
        if (path_it == field_id_to_column_path.end())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "add_files: partition spec refers to source id {} which is not in the current schema", source_id);
        if (path_it->second.find('.') != String::npos)
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                "add_files: partition source column '{}' is nested; only top-level partition columns are supported", path_it->second);
        auto type_it = std::find_if(clickhouse_schema->begin(), clickhouse_schema->end(),
            [&](const NameAndTypePair & nt) { return nt.name == path_it->second; });
        if (type_it == clickhouse_schema->end())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Column {} not found in ClickHouse schema", path_it->second);
        source_columns.emplace(source_id, *type_it);
    }

    /// Expand trailing-slash paths. Registration paths are kept exactly as given (or produced
    /// by expansion): they must be in metadata form, i.e. prefixed by the table location, which
    /// is the same form native writes register in manifests.
    std::vector<String> paths = expandPaths(raw_paths, resolver, object_storage, log);
    for (const auto & path : paths)
    {
        if (!endsWithParquet(path))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "add_files: only .parquet files can be registered, got '{}'", path);
    }

    /// Read every file once: footer statistics + bounds.
    std::vector<AnalyzedFile> analyzed_files;
    analyzed_files.reserve(paths.size());
    for (const auto & path : paths)
        analyzed_files.push_back(analyzeFile(path, resolver, object_storage, column_path_to_field_id, context, format_settings));

    /// Validate the file's columns against the table schema.
    if (!allow_missing_columns)
    {
        for (const auto & file : analyzed_files)
        {
            for (const auto & [field_path, field_id] : column_path_to_field_id)
            {
                if (iceberg_type_by_field_id.contains(field_id) && !file.bounds_by_field_id.contains(static_cast<Int32>(field_id)))
                {
                    /// Column missing from the file entirely (no statistics either way).
                    throw Exception(ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION,
                        "add_files: file {} lacks column '{}' present in the table schema; "
                        "pass allow_missing_columns = 1 to register it anyway (values will read as the column default)",
                        file.registration_path, field_path);
                }
            }
        }
    }

    /// Derive (and validate) the partition tuple of every file, then group files by tuple.
    struct PartitionGroup
    {
        std::vector<String> partition_columns;
        std::vector<Field> partition_values;
        DataTypes partition_types;
        std::vector<size_t> file_indices;
    };
    std::map<String, PartitionGroup> groups;
    for (size_t i = 0; i < analyzed_files.size(); ++i)
    {
        std::vector<Field> tuple;
        std::vector<String> names;
        DataTypes types;
        for (const auto & spec_field_var : *spec_fields)
        {
            auto field_value = derivePartitionField(
                spec_field_var.extract<Poco::JSON::Object::Ptr>(), analyzed_files[i], source_columns, iceberg_type_by_field_id);
            names.push_back(field_value.column_name);
            tuple.push_back(field_value.value);
            types.push_back(field_value.type);
        }
        String key = partitionKeyToString(tuple);
        auto [it, inserted] = groups.try_emplace(key);
        if (inserted)
        {
            it->second.partition_columns = names;
            it->second.partition_values = tuple;
            it->second.partition_types = types;
        }
        it->second.file_indices.push_back(i);
    }

    /// Duplicate detection against the current snapshot and inside the input set.
    if (check_duplicate_files)
    {
        std::unordered_set<String> seen;
        for (const auto & file : analyzed_files)
        {
            if (!seen.insert(file.registration_path).second)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "add_files: file '{}' is listed twice in the input", file.registration_path);
        }
        auto live_files = listLiveDataFiles(metadata, resolver, object_storage, format_settings);
        for (const auto & file : analyzed_files)
        {
            if (live_files.contains(file.registration_path))
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "add_files: file '{}' is already registered in the current snapshot", file.registration_path);
        }
    }

    /// Commit: one manifest per partition tuple, one manifest list, one append snapshot.
    AddFilesResult result;
    result.partitions_touched = static_cast<Int64>(groups.size());

    size_t retry = 0;
    static constexpr size_t max_retries = 100;
    while (true)
    {
        auto metadata_info = filename_generator.generateMetadataPathWithInfo();
        const auto storage_metadata_name = resolver.resolve(metadata_info.path);

        Int64 parent_snapshot = -1;
        if (metadata->has(Iceberg::f_current_snapshot_id) && !metadata->isNull(Iceberg::f_current_snapshot_id))
            parent_snapshot = metadata->getValue<Int64>(Iceberg::f_current_snapshot_id);

        Int64 total_files = 0;
        Int64 total_rows = 0;
        Int64 total_bytes = 0;
        for (const auto & file : analyzed_files)
        {
            ++total_files;
            total_rows += file.stats.record_count;
            total_bytes += file.stats.file_size_in_bytes;
        }

        auto [new_snapshot, manifest_list_path] = MetadataGenerator(metadata).generateNextMetadata(
            filename_generator, metadata_info.path, parent_snapshot,
            total_files, total_rows, total_bytes, static_cast<Int64>(groups.size()),
            /* added_delete_files */ 0, /* num_deleted_rows */ 0);
        const auto storage_manifest_list_name = resolver.resolve(manifest_list_path);

        std::vector<IcebergPathFromMetadata> manifest_paths;
        std::vector<Int64> manifest_lengths;
        std::vector<Int64> manifest_row_counts;
        std::vector<std::vector<std::pair<Field, DataTypePtr>>> manifest_partition_summaries;
        bool published = false;

        auto cleanup = [&](bool retry_because_of_metadata_conflict)
        {
            for (size_t i = 0; i < manifest_paths.size(); ++i)
                object_storage->removeObjectIfExists(StoredObject(resolver.resolve(manifest_paths[i])));
            object_storage->removeObjectIfExists(StoredObject(storage_manifest_list_name));

            if (retry_because_of_metadata_conflict)
            {
                auto [new_version, new_metadata_path, new_compression] = getLatestOrExplicitMetadataFileAndVersion(
                    object_storage,
                    persistent_components.table_path,
                    data_lake_settings,
                    persistent_components.metadata_cache,
                    context,
                    getLogger("IcebergAddFiles").get(),
                    persistent_components.table_uuid,
                    persistent_components.metadata_compression_method,
                    true);
                filename_generator.setVersion(new_version + 1);
                metadata = getMetadataJSONObject(
                    new_metadata_path, object_storage, persistent_components.metadata_cache,
                    context, log, new_compression, persistent_components.table_uuid);

                if (metadata->getValue<Int64>(Iceberg::f_current_schema_id) != original_schema_id)
                    throw Exception(ErrorCodes::METADATA_MISMATCH,
                        "Table schema changed during add_files; restart the command");
                if (metadata->getValue<Int64>(Iceberg::f_default_spec_id) != partition_spec_id)
                    throw Exception(ErrorCodes::METADATA_MISMATCH,
                        "Table partition spec changed during add_files; restart the command");
            }
        };

        try
        {
            /// Write one manifest per partition tuple.
            for (const auto & [key, group] : groups)
            {
                auto manifest_path = filename_generator.generateManifestEntryName();
                auto storage_manifest_name = resolver.resolve(manifest_path);
                auto buffer = object_storage->writeObject(
                    StoredObject(storage_manifest_name), WriteMode::Rewrite, std::nullopt,
                    DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

                std::vector<IcebergPathFromMetadata> group_paths;
                std::vector<UInt64> group_row_counts;
                std::vector<UInt64> group_byte_counts;
                std::vector<IcebergSerializedFileStats> group_stats;
                Int64 group_rows = 0;
                for (size_t idx : group.file_indices)
                {
                    group_paths.push_back(IcebergPathFromMetadata::deserialize(analyzed_files[idx].registration_path));
                    group_row_counts.push_back(static_cast<UInt64>(analyzed_files[idx].stats.record_count));
                    group_byte_counts.push_back(static_cast<UInt64>(analyzed_files[idx].stats.file_size_in_bytes));
                    group_stats.push_back(analyzed_files[idx].stats);
                    group_rows += analyzed_files[idx].stats.record_count;
                }

                generateManifestFile(
                    metadata,
                    group.partition_columns,
                    group.partition_values,
                    group.partition_types,
                    group_paths,
                    group_row_counts,
                    group_byte_counts,
                    /* data_file_statistics */ std::nullopt,
                    shared_sample_block,
                    new_snapshot,
                    write_format,
                    partition_spec,
                    partition_spec_id,
                    *buffer,
                    FileContentType::DATA,
                    /* user_defined_sequence_number */ std::nullopt,
                    /* user_defined_snapshot_id */ std::nullopt,
                    /* data_file_formats */ {},
                    /* per_file_statistics */ {},
                    /* data_file_sort_order_ids */ {},
                    /* per_file_entry_lineage */ {},
                    /* schema_to_serialize */ nullptr,
                    /* per_file_fresh_statistics */ nullptr,
                    /* per_file_stats */ group_stats);
                buffer->finalize();

                manifest_paths.push_back(manifest_path);
                manifest_lengths.push_back(static_cast<Int64>(buffer->count()));
                manifest_row_counts.push_back(group_rows);

                std::vector<std::pair<Field, DataTypePtr>> summary;
                summary.reserve(group.partition_values.size());
                for (size_t i = 0; i < group.partition_values.size(); ++i)
                    summary.emplace_back(group.partition_values[i], group.partition_types[i]);
                manifest_partition_summaries.push_back(std::move(summary));
            }

            {
                auto buffer_manifest_list = object_storage->writeObject(
                    StoredObject(storage_manifest_list_name), WriteMode::Rewrite, std::nullopt,
                    DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());
                generateManifestList(
                    resolver, metadata, object_storage, external_storages, context,
                    manifest_paths, new_snapshot, manifest_lengths, *buffer_manifest_list,
                    FileContentType::DATA, /* use_previous_snapshots */ true,
                    /* per_entry_content_types */ {},
                    /* entry_counts */ {},
                    /* carry_forward_manifest_paths */ {},
                    /* entry_partition_spec_ids */ {},
                    /* entry_partition_summaries */ manifest_partition_summaries,
                    /* previous_manifest_actions */ {},
                    /* entry_row_counts */ manifest_row_counts);
                buffer_manifest_list->finalize();
            }

            {
                std::ostringstream oss; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
                Poco::JSON::Stringifier::stringify(metadata, oss, 4);
                std::string json_representation = removeEscapedSlashes(oss.str());

                auto hint_path = filename_generator.generateVersionHint();
                if (!writeMetadataFileAndVersionHint(
                        resolver, metadata_info, json_representation, hint_path, object_storage, context,
                        data_lake_settings[DataLakeStorageSetting::iceberg_use_version_hint]))
                {
                    cleanup(true);
                    if (++retry >= max_retries)
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "add_files: too many metadata conflicts, giving up");
                    continue;
                }

                if (catalog)
                {
                    auto catalog_filename = resolver.resolveForCatalog(metadata_info.path);
                    const auto & [namespace_name, table] = DataLake::parseTableName(table_name);
                    if (!catalog->updateMetadata(namespace_name, table, catalog_filename, new_snapshot, context->getForwardedAuthToken()))
                    {
                        cleanup(true);
                        if (++retry >= max_retries)
                            throw Exception(ErrorCodes::BAD_ARGUMENTS, "add_files: too many catalog conflicts, giving up");
                        continue;
                    }
                }
                published = true;
            }

            persistent_components.invalidateMetadataCache();
        }
        catch (...)
        {
            if (published)
            {
                tryLogCurrentException(log,
                    "Post-publish work failed after Iceberg snapshot was committed by add_files; "
                    "skipping manifest cleanup to preserve the published snapshot");
            }
            else
            {
                cleanup(false);
            }
            throw;
        }

        result.files_registered = total_files;
        result.rows_registered = total_rows;
        result.bytes_registered = total_bytes;
        result.manifests_written = static_cast<Int64>(manifest_paths.size());
        return result;
    }
}

Pipe executeAddFiles(
    const ASTPtr & args,
    ContextPtr context,
    ObjectStoragePtr object_storage,
    const DataLakeStorageSettings & data_lake_settings,
    const PersistentTableComponents & persistent_components,
    const String & write_format,
    std::shared_ptr<DataLake::ICatalog> catalog,
    const String & table_name,
    ExternalStorageCache & external_storages)
{
    auto parsed = makeSchema().parse(args);
    std::vector<String> paths = parsePathsArgument(parsed.get("paths"));
    bool check_duplicates = parsed.getAs<UInt64>("check_duplicate_files") != 0;
    bool allow_missing = parsed.getAs<UInt64>("allow_missing_columns") != 0;

    auto result = addFiles(
        paths, check_duplicates, allow_missing,
        context, object_storage, data_lake_settings, persistent_components,
        write_format, catalog, table_name, external_storages);

    return resultToPipe(result);
}

}
}

#endif
