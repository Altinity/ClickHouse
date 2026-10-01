#include <Storages/ObjectStorage/DataLakes/Iceberg/MultipleFileWriter.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnSparse.h>
#include <Columns/ColumnTuple.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatFilterInfo.h>
#include <Processors/Formats/IOutputFormat.h>
#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace Iceberg
{

DataTypePtr stripNothing(const DataTypePtr & type)
{
    if (isNothing(type))
        return nullptr;

    if (const auto * nullable_type = typeid_cast<const DataTypeNullable *>(type.get()))
    {
        const auto & nested = nullable_type->getNestedType();
        auto stripped_nested = stripNothing(nested);
        if (!stripped_nested)
            return nullptr;
        return stripped_nested == nested ? type : makeNullable(stripped_nested);
    }

    if (const auto * tuple_type = typeid_cast<const DataTypeTuple *>(type.get()))
    {
        const auto & elements = tuple_type->getElements();
        DataTypes kept_elements;
        Strings kept_names;
        bool changed = false;
        for (size_t i = 0; i < elements.size(); ++i)
        {
            auto stripped_element = stripNothing(elements[i]);
            if (!stripped_element)
            {
                changed = true;
                continue;
            }
            changed |= stripped_element != elements[i];
            kept_elements.push_back(stripped_element);
            kept_names.push_back(tuple_type->getNameByPosition(i + 1));
        }
        if (!changed)
            return type;
        if (kept_elements.empty())
            return nullptr;
        if (tuple_type->hasExplicitNames())
            return std::make_shared<DataTypeTuple>(kept_elements, kept_names);
        return std::make_shared<DataTypeTuple>(kept_elements);
    }

    if (const auto * array_type = typeid_cast<const DataTypeArray *>(type.get()))
    {
        const auto & nested = array_type->getNestedType();
        auto stripped_nested = stripNothing(nested);
        if (!stripped_nested)
            return nullptr;
        return stripped_nested == nested ? type : std::make_shared<DataTypeArray>(stripped_nested);
    }

    if (const auto * map_type = typeid_cast<const DataTypeMap *>(type.get()))
    {
        const auto & key = map_type->getKeyType();
        const auto & value = map_type->getValueType();
        auto stripped_key = stripNothing(key);
        auto stripped_value = stripNothing(value);
        if (!stripped_key || !stripped_value)
            return nullptr;
        if (stripped_key == key && stripped_value == value)
            return type;
        return std::make_shared<DataTypeMap>(stripped_key, stripped_value);
    }

    return type;
}

namespace
{

ColumnPtr stripNothingColumnImpl(const ColumnPtr & column, const DataTypePtr & type)
{
    auto stripped_type = stripNothing(type);
    if (!stripped_type)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Column of type {} has nothing left after stripping Nothing", type->getName());
    if (stripped_type == type)
        return column;

    auto full_column = removeSpecialRepresentations(column->convertToFullColumnIfConst());

    if (const auto * nullable_type = typeid_cast<const DataTypeNullable *>(type.get()))
    {
        const auto & nullable_column = assert_cast<const ColumnNullable &>(*full_column);
        return ColumnNullable::create(
            stripNothingColumnImpl(nullable_column.getNestedColumnPtr(), nullable_type->getNestedType()),
            nullable_column.getNullMapColumnPtr());
    }

    if (const auto * tuple_type = typeid_cast<const DataTypeTuple *>(type.get()))
    {
        const auto & tuple_column = assert_cast<const ColumnTuple &>(*full_column);
        const auto & elements = tuple_type->getElements();
        Columns kept_columns;
        for (size_t i = 0; i < elements.size(); ++i)
        {
            if (stripNothing(elements[i]))
                kept_columns.push_back(stripNothingColumnImpl(tuple_column.getColumnPtr(i), elements[i]));
        }
        return ColumnTuple::create(kept_columns);
    }

    if (const auto * array_type = typeid_cast<const DataTypeArray *>(type.get()))
    {
        const auto & array_column = assert_cast<const ColumnArray &>(*full_column);
        return ColumnArray::create(
            stripNothingColumnImpl(array_column.getDataPtr(), array_type->getNestedType()),
            array_column.getOffsetsPtr());
    }

    if (const auto * map_type = typeid_cast<const DataTypeMap *>(type.get()))
    {
        const auto & map_column = assert_cast<const ColumnMap &>(*full_column);
        const auto & key_value = map_column.getNestedData();
        return ColumnMap::create(
            stripNothingColumnImpl(key_value.getColumnPtr(0), map_type->getKeyType()),
            stripNothingColumnImpl(key_value.getColumnPtr(1), map_type->getValueType()),
            map_column.getNestedColumn().getOffsetsPtr());
    }

    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type {} while stripping Nothing from a column", type->getName());
}

}

ColumnPtr stripNothingColumn(const ColumnPtr & column, const DataTypePtr & original_type, const DataTypePtr & stripped_type)
{
    auto result = stripNothingColumnImpl(column, original_type);
    chassert(stripped_type && stripped_type->equals(*stripNothing(original_type)));
    return result;
}

}

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

    written_column_types.reserve(sample_block->columns());
    for (const auto & column : *sample_block)
    {
        written_column_types.push_back(Iceberg::stripNothing(column.type));
        has_nothing_leaves |= written_column_types.back() != column.type;
    }

    if (!has_nothing_leaves)
    {
        filtered_sample_block = sample_block;
    }
    else
    {
        Block filtered;
        for (size_t i = 0; i < sample_block->columns(); ++i)
        {
            if (!written_column_types[i])
                continue;
            const auto & column = sample_block->getByPosition(i);
            filtered.insert({written_column_types[i]->createColumn(), written_column_types[i], column.name});
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

Columns MultipleFileWriter::filterColumns(const Columns & columns) const
{
    Columns filtered_columns;
    filtered_columns.reserve(filtered_sample_block->columns());
    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto & original_type = sample_block->getByPosition(i).type;
        const auto & written_type = written_column_types[i];
        if (written_type == original_type)
        {
            filtered_columns.push_back(columns[i]);
        }
        else if (written_type)
        {
            filtered_columns.push_back(Iceberg::stripNothingColumn(columns[i], original_type, written_type));
        }
        else
        {
            for (size_t row = 0; row < columns[i]->size(); ++row)
            {
                if (!columns[i]->isDefaultAt(row))
                    throw Exception(
                        ErrorCodes::NOT_IMPLEMENTED,
                        "Cannot write column '{}' of type {} into an Iceberg data file: row {} holds a non-default value, "
                        "but the column contains an Iceberg `unknown` element that no data file format can store, so the "
                        "value (for example, list elements or map keys) would be lost. Only NULL, empty lists and empty "
                        "maps can be inserted into such a column",
                        sample_block->getByPosition(i).name, original_type->getName(), row);
            }
        }
    }
    return filtered_columns;
}

void MultipleFileWriter::consume(const Chunk & chunk)
{
    /// Validate before starting a file, so a rejected chunk leaves no empty data file behind.
    std::optional<Columns> filtered_columns;
    if (has_nothing_leaves)
        filtered_columns = filterColumns(chunk.getColumns());

    if (!current_file_num_rows || *current_file_num_rows >= max_data_file_num_rows || *current_file_num_bytes >= max_data_file_num_bytes)
    {
        startNewFile();
    }

    if (filtered_columns)
        output_format->write(filtered_sample_block->cloneWithColumns(std::move(*filtered_columns)));
    else
        output_format->write(sample_block->cloneWithColumns(chunk.getColumns()));

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
