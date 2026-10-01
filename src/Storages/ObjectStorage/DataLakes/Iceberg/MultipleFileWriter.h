#pragma once

#include <Formats/FormatFilterInfo.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/FileNamesGenerator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergPath.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/DataFileStatistics.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataFileEntry.h>

namespace DB
{

namespace Iceberg
{

/// Iceberg `unknown` maps to `Nullable(Nothing)` and can be nested inside structs, lists and maps.
/// No serialisation format (Parquet, ORC, Avro) can represent `Nothing`, so only those leaves are
/// removed before writing; the reader fills a missing struct field with NULL.
/// Returns `type` itself if it has no `Nothing` leaf, the type without its `Nothing` leaves otherwise,
/// and `nullptr` if nothing serialisable remains (`unknown` itself, a struct of only `unknown` fields,
/// or a list or map whose element, key or value strips to nothing).
DataTypePtr stripNothing(const DataTypePtr & type);

/// Removes from `column` of type `original_type` the parts that `stripNothing` removes from the type.
/// `stripped_type` must be the non-null result of `stripNothing(original_type)`.
ColumnPtr stripNothingColumn(const ColumnPtr & column, const DataTypePtr & original_type, const DataTypePtr & stripped_type);

}

#if USE_AVRO

class MultipleFileWriter
{
public:
    explicit MultipleFileWriter(
        UInt64 max_data_file_num_rows_,
        UInt64 max_data_file_num_bytes_,
        Poco::JSON::Array::Ptr schema,
        FileNamesGenerator & filename_generator_,
        const Iceberg::IcebergPathResolver & path_resolver_,
        ObjectStoragePtr object_storage_,
        ContextPtr context_,
        const std::optional<FormatSettings> & format_settings_,
        const String & write_format_,
        SharedHeader sample_block_,
        std::function<void(const std::string &)> new_file_path_callback_ = {});

    void consume(const Chunk & chunk);
    void startNewFile();
    void finalize();
    void release();
    void cancel();
    void clearAllDataFiles() const;

    UInt64 getResultBytes() const;

    const std::vector<Iceberg::IcebergPathFromMetadata> & getDataFiles() const
    {
        return data_file_names;
    }

    const std::vector<UInt64> & getDataFileRowCounts() const
    {
        return data_file_row_counts;
    }

    const std::vector<UInt64> & getDataFileByteCounts() const
    {
        return data_file_byte_counts;
    }

    const DataFileStatistics & getResultStatistics() const
    {
        return aggregate_stats;
    }

    const std::vector<DataFileStatisticsPtr> & getPerFileStatistics() const
    {
        return completed_file_stats;
    }

    /// Returns one entry per written data file, with the accurate row count, byte size,
    /// and per-file column statistics collected during finalization.
    /// Must be called only after finalize().
    std::vector<IcebergDataFileEntry> getDataFileEntries() const;

private:
    /// Strips `Nothing` leaves from `columns` and drops fully `Nothing` columns, throwing if a dropped one holds a non-default row.
    Columns filterColumns(const Columns & columns) const;

    UInt64 max_data_file_num_rows;
    UInt64 max_data_file_num_bytes;
    Poco::JSON::Array::Ptr schema;
    DataFileStatistics aggregate_stats;   /// accumulates across all files
    DataFileStatisticsPtr current_file_stats; /// accumulates for the current file only
    std::vector<DataFileStatisticsPtr> completed_file_stats;
    /// Pre-built ColumnMapper for `startNewFile`. Traversing the Iceberg schema is invariant
    /// for the lifetime of the writer, so we compute the mapping once and reuse it across
    /// every rolled-over data file instead of recomputing it on each rollover.
    ColumnMapperPtr column_mapper;
    std::optional<size_t> current_file_num_rows = std::nullopt;
    std::optional<size_t> current_file_num_bytes = std::nullopt;
    std::vector<Iceberg::IcebergPathFromMetadata> data_file_names;
    std::vector<UInt64> data_file_row_counts;
    std::vector<UInt64> data_file_byte_counts;
    std::unique_ptr<WriteBufferFromFileBase> buffer;
    OutputFormatPtr output_format;
    FileNamesGenerator & filename_generator;
    const Iceberg::IcebergPathResolver & path_resolver;
    ObjectStoragePtr object_storage;
    ContextPtr context;
    std::optional<FormatSettings> format_settings;
    const String& write_format;
    SharedHeader sample_block;
    /// Per `sample_block` column: the type passed to the format writer, i.e. the result of
    /// `Iceberg::stripNothing`. It is the original type when the column has no Iceberg `unknown`
    /// leaf, and nullptr when nothing serialisable remains: such a column is left out of the data
    /// file and may only hold default values, because anything else would be lost.
    DataTypes written_column_types;
    bool has_nothing_leaves = false;
    /// `sample_block` with `Nothing` leaves stripped and fully `Nothing` columns removed; used by the format writer.
    SharedHeader filtered_sample_block;
    UInt64 total_bytes = 0;
    std::function<void(const std::string &)> new_file_path_callback;
};

#endif

}
