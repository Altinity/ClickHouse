#include <Storages/ObjectStorage/DataLakes/Iceberg/MultipleFileWriter.h>

#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatFilterInfo.h>
#include <Processors/Formats/IOutputFormat.h>
#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>


namespace DB
{

#if USE_AVRO

MultipleFileWriter::MultipleFileWriter(
    UInt64 max_data_file_num_rows_,
    UInt64 max_data_file_num_bytes_,
    Poco::JSON::Array::Ptr schema_,
    FileNamesGenerator & filename_generator_,
    const Iceberg::IcebergPathResolver & path_resolver_,
    ObjectStoragePtr object_storage_,
    ContextPtr context_,
    const std::optional<FormatSettings> & format_settings_,
    const String & write_format_,
    SharedHeader sample_block_,
    std::function<void(const std::string &)> new_file_path_callback_)
    : max_data_file_num_rows(max_data_file_num_rows_)
    , max_data_file_num_bytes(max_data_file_num_bytes_)
    , schema(schema_)
    , aggregate_stats(schema_)
    , column_mapper(std::make_shared<ColumnMapper>())
    , filename_generator(filename_generator_)
    , path_resolver(path_resolver_)
    , object_storage(object_storage_)
    , context(context_)
    , format_settings(format_settings_)
    , write_format(std::move(write_format_))
    , sample_block(sample_block_)
    , new_file_path_callback(std::move(new_file_path_callback_))
{
    column_mapper->setStorageColumnEncoding(Iceberg::IcebergSchemaProcessor::traverseSchema(schema_));

    /// Iceberg `unknown` type maps to Nullable(Nothing) in ClickHouse.  No
    /// serialisation format (Parquet, ORC, Avro) can represent the Nothing type,
    /// and the column is guaranteed to contain only NULLs, so we strip it from the
    /// block that is passed to the format writer.  The column still appears in the
    /// Iceberg schema metadata and is read back as NULLs on the read path.
    for (size_t i = 0; i < sample_block->columns(); ++i)
    {
        auto inner_type = removeNullable(sample_block->getByPosition(i).type);
        if (isNothing(inner_type))
            nothing_column_indices.push_back(i);
    }

    if (nothing_column_indices.empty())
    {
        filtered_sample_block = sample_block;
    }
    else
    {
        Block filtered;
        for (size_t i = 0; i < sample_block->columns(); ++i)
        {
            if (std::find(nothing_column_indices.begin(), nothing_column_indices.end(), i) == nothing_column_indices.end())
                filtered.insert(sample_block->getByPosition(i));
        }
        filtered_sample_block = std::make_shared<const Block>(std::move(filtered));
    }
}

void MultipleFileWriter::startNewFile()
{
    if (buffer)
    {
        finalize();
    }

    current_file_stats = std::make_shared<DataFileStatistics>(schema);
    current_file_num_rows = 0;
    current_file_num_bytes = 0;
    auto metadata_path = filename_generator.generateDataFileName();
    auto storage_path = path_resolver.resolve(metadata_path);

    data_file_names.push_back(metadata_path);
    if (new_file_path_callback)
        new_file_path_callback(storage_path);

    buffer = object_storage->writeObject(
        StoredObject(storage_path), WriteMode::Rewrite, std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

    if (format_settings)
    {
        format_settings->parquet.write_page_index = true;
        format_settings->parquet.bloom_filter_push_down = true;
        format_settings->parquet.filter_push_down = true;
    }
    FormatFilterInfoPtr format_filter_info = std::make_shared<FormatFilterInfo>(nullptr, context, column_mapper, nullptr, nullptr);
    output_format = FormatFactory::instance().getOutputFormatParallelIfPossible(
        write_format, *buffer, *filtered_sample_block, context, format_settings, format_filter_info);
}

void MultipleFileWriter::consume(const Chunk & chunk)
{
    if (!current_file_num_rows || *current_file_num_rows >= max_data_file_num_rows || *current_file_num_bytes >= max_data_file_num_bytes)
    {
        startNewFile();
    }

    if (nothing_column_indices.empty())
    {
        output_format->write(sample_block->cloneWithColumns(chunk.getColumns()));
    }
    else
    {
        /// Strip Nullable(Nothing) columns before passing to the format writer.
        auto columns = chunk.getColumns();
        Columns filtered_columns;
        filtered_columns.reserve(columns.size() - nothing_column_indices.size());
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (std::find(nothing_column_indices.begin(), nothing_column_indices.end(), i) == nothing_column_indices.end())
                filtered_columns.push_back(columns[i]);
        }
        output_format->write(filtered_sample_block->cloneWithColumns(std::move(filtered_columns)));
    }

    output_format->flush();
    *current_file_num_rows += chunk.getNumRows();
    *current_file_num_bytes += chunk.bytes();
    aggregate_stats.update(chunk);
    current_file_stats->update(chunk);
}

void MultipleFileWriter::finalize()
{
    output_format->flush();
    output_format->finalize();
    buffer->finalize();
    auto buffer_bytes = buffer->count();
    UInt64 file_bytes = 0;
    if (buffer_bytes > 0)
    {
        file_bytes = buffer_bytes;
        total_bytes += file_bytes;
    }
    else if (!data_file_names.empty())
    {
        /// Some storage backends (e.g. Azure) don't track bytes in the write buffer.
        /// Fall back to querying the actual object size.
        auto obj_metadata = object_storage->getObjectMetadata(path_resolver.resolve(data_file_names.back()), /*with_tags=*/false);
        file_bytes = obj_metadata.size_bytes;
        total_bytes += file_bytes;
    }

    if (current_file_stats)
        completed_file_stats.push_back(std::move(current_file_stats));
    data_file_byte_counts.push_back(file_bytes);
    data_file_row_counts.push_back(current_file_num_rows.value_or(0));
}

std::vector<IcebergDataFileEntry> MultipleFileWriter::getDataFileEntries() const
{
    chassert(data_file_names.size() == data_file_row_counts.size());
    chassert(data_file_names.size() == data_file_byte_counts.size());
    chassert(data_file_names.size() == completed_file_stats.size());

    std::vector<IcebergDataFileEntry> entries;
    entries.reserve(data_file_names.size());

    for (size_t i = 0; i < data_file_names.size(); ++i)
    {
        std::optional<DataFileStatistics> statistics;
        if (completed_file_stats[i])
            statistics = *completed_file_stats[i];

        entries.emplace_back(
            path_resolver.resolve(data_file_names[i]),
            static_cast<Int64>(data_file_row_counts[i]),
            static_cast<Int64>(data_file_byte_counts[i]),
            std::move(statistics));
    }

    return entries;
}

void MultipleFileWriter::release()
{
    output_format.reset();
    buffer.reset();
}

void MultipleFileWriter::cancel()
{
    if (output_format)
        output_format->cancel();
    if (buffer)
        buffer->cancel();
}

void MultipleFileWriter::clearAllDataFiles() const
{
    for (const auto & metadata_path : data_file_names)
        object_storage->removeObjectIfExists(StoredObject(path_resolver.resolve(metadata_path)));
}

UInt64 MultipleFileWriter::getResultBytes() const
{
    return total_bytes;
}

#endif

}
