#include <Storages/ObjectStorage/DataLakes/DataLakeConfiguration.h>
#include <Storages/NamedCollectionsHelpers.h>
#include <Storages/StorageFactory.h>
#include <Common/logger_useful.h>
#include <Storages/ColumnsDescription.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSetQuery.h>
#include <Disks/DiskType.h>

#include <memory>
#include <string>

#include <Common/ErrorCodes.h>

#include <fmt/ranges.h>

namespace DB
{

namespace ErrorCodes
{
extern const int FORMAT_VERSION_TOO_OLD;
extern const int LOGICAL_ERROR;
}

namespace StorageObjectStorageSetting
{
extern const StorageObjectStorageSettingsBool allow_dynamic_metadata_for_data_lakes;
extern const StorageObjectStorageSettingsString iceberg_metadata_file_path;
}


template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
void DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::update(ObjectStoragePtr object_storage, ContextPtr local_context)
{
    BaseStorageConfiguration::update(object_storage, local_context);

    bool existed = current_metadata != nullptr;

    if (updateMetadataObjectIfNeeded(object_storage, local_context))
    {
        if (hasExternalDynamicMetadata() && existed)
        {
            throw Exception(
                ErrorCodes::FORMAT_VERSION_TOO_OLD,
                "Metadata is not consinsent with the one which was used to infer table schema. Please, retry the query.");
        }
    }
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
std::optional<ColumnsDescription> DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::tryGetTableStructureFromMetadata() const
{
    if (!current_metadata)
        return std::nullopt;
    auto schema_from_metadata = current_metadata->getTableSchema();
    if (!schema_from_metadata.empty())
    {
        return ColumnsDescription(std::move(schema_from_metadata));
    }
    return std::nullopt;
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
std::optional<String> DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::tryGetSamplePathFromMetadata() const
{
    if (!current_metadata)
        return std::nullopt;
    auto data_files = current_metadata->getDataFiles();
    if (!data_files.empty())
        return data_files[0];
    return std::nullopt;
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
std::optional<size_t> DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::totalRows()
{
    if (!current_metadata)
        return {};

    return current_metadata->totalRows();
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
std::shared_ptr<NamesAndTypesList> DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::getInitialSchemaByPath(const String & data_path) const
{
    if (!current_metadata)
        return {};
    return current_metadata->getInitialSchemaByPath(data_path);
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
std::shared_ptr<const ActionsDAG> DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::getSchemaTransformer(const String & data_path) const
{
    if (!current_metadata)
        return {};
    return current_metadata->getSchemaTransformer(data_path);
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
bool DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::hasExternalDynamicMetadata()
{
    return BaseStorageConfiguration::getSettingsRef()[StorageObjectStorageSetting::allow_dynamic_metadata_for_data_lakes]
        && current_metadata
        && current_metadata->supportsExternalMetadataChange();
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
ColumnsDescription DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::updateAndGetCurrentSchema(
    ObjectStoragePtr object_storage,
    ContextPtr context)
{
    BaseStorageConfiguration::update(object_storage, context);
    updateMetadataObjectIfNeeded(object_storage, context);
    return ColumnsDescription{current_metadata->getTableSchema()};
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
ReadFromFormatInfo DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::prepareReadingFromFormat(
    ObjectStoragePtr object_storage,
    const Strings & requested_columns,
    const StorageSnapshotPtr & storage_snapshot,
    bool supports_subset_of_columns,
    ContextPtr local_context)
{
    auto info = DB::prepareReadingFromFormat(requested_columns, storage_snapshot, local_context, supports_subset_of_columns);
    if (!current_metadata)
    {
        current_metadata = DataLakeMetadata::create(
            object_storage,
            weak_from_this(),
            local_context);
    }
    auto read_schema = current_metadata->getReadSchema();
    if (!read_schema.empty())
    {
        /// There is a difference between "table schema" and "read schema".
        /// "table schema" is a schema from data lake table metadata,
        /// while "read schema" is a schema from data files.
        /// In most cases they would be the same.
        /// TODO: Try to hide this logic inside IDataLakeMetadata.

        const auto read_schema_names = read_schema.getNames();
        const auto table_schema_names = current_metadata->getTableSchema().getNames();
        chassert(read_schema_names.size() == table_schema_names.size());

        if (read_schema_names != table_schema_names)
        {
            LOG_TEST(log, "Read schema: {}, table schema: {}, requested columns: {}",
                        fmt::join(read_schema_names, ", "),
                        fmt::join(table_schema_names, ", "),
                        fmt::join(info.requested_columns.getNames(), ", "));

            auto column_name_mapping = [&]()
            {
                std::map<std::string, std::string> result;
                for (size_t i = 0; i < read_schema_names.size(); ++i)
                    result[table_schema_names[i]] = read_schema_names[i];
                return result;
            }();

            /// Go through requested columns and change column name
            /// from table schema to column name from read schema.

            std::vector<NameAndTypePair> read_columns;
            for (const auto & column_name : info.requested_columns)
            {
                const auto pos = info.format_header.getPositionByName(column_name.name);
                auto column = info.format_header.getByPosition(pos);
                column.name = column_name_mapping.at(column_name.name);
                info.format_header.setColumn(pos, column);

                read_columns.emplace_back(column.name, column.type);
            }
            info.requested_columns = NamesAndTypesList(read_columns.begin(), read_columns.end());
        }
    }

    return info;
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
bool DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::updateMetadataObjectIfNeeded(
    ObjectStoragePtr object_storage,
    ContextPtr context)
{
    if (!current_metadata)
    {
        current_metadata = DataLakeMetadata::create(
            object_storage,
            weak_from_this(),
            context);
        return true;
    }

    if (current_metadata->supportsUpdate())
    {
        return current_metadata->update(context);
    }

    auto new_metadata = DataLakeMetadata::create(
        object_storage,
        weak_from_this(),
        context);

    if (*current_metadata != *new_metadata)
    {
        current_metadata = std::move(new_metadata);
        return true;
    }
    else
    {
        return false;
    }
}

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
ASTPtr DataLakeConfiguration<BaseStorageConfiguration, DataLakeMetadata>::createArgsWithAccessData() const
{
    auto res = BaseStorageConfiguration::createArgsWithAccessData();

    auto iceberg_metadata_file_path = BaseStorageConfiguration::getSettingsRef()[StorageObjectStorageSetting::iceberg_metadata_file_path];

    if (iceberg_metadata_file_path.changed)
    {
        auto * arguments = res->template as<ASTExpressionList>();
        if (!arguments)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Arguments are not an expression list");

        bool has_settings = false;
        
        for (auto & arg : arguments->children)
        {
            if (auto * settings = arg->template as<ASTSetQuery>())
            {
                has_settings = true;
                settings->changes.setSetting("iceberg_metadata_file_path", iceberg_metadata_file_path.value);
                break;
            }
        }

        if (!has_settings)
        {
            std::shared_ptr<ASTSetQuery> settings_ast = std::make_shared<ASTSetQuery>();
            settings_ast->is_standalone = false;
            settings_ast->changes.setSetting("iceberg_metadata_file_path", iceberg_metadata_file_path.value);
            arguments->children.push_back(settings_ast);
        }
    }

    return res;
}

#if USE_AVRO
#    if USE_AWS_S3
template class DataLakeConfiguration<StorageS3Configuration, IcebergMetadata>;
#    endif
#    if USE_AZURE_BLOB_STORAGE
template class DataLakeConfiguration<StorageAzureConfiguration, IcebergMetadata>;
#    endif
#    if USE_HDFS
template class DataLakeConfiguration<StorageHDFSConfiguration, IcebergMetadata>;
#    endif
template class DataLakeConfiguration<StorageLocalConfiguration, IcebergMetadata>;
#endif
#if USE_PARQUET && USE_AWS_S3
template class DataLakeConfiguration<StorageS3Configuration, DeltaLakeMetadata>;
#endif
#if USE_PARQUET
template class DataLakeConfiguration<StorageLocalConfiguration, DeltaLakeMetadata>;
#endif
#if USE_AWS_S3
template class DataLakeConfiguration<StorageS3Configuration, HudiMetadata>;
#endif

ObjectStorageType StorageIcebergConfiguration::extractDynamicStorageType(
    ASTs & args, ContextPtr context, ASTPtr * type_arg) const
{
    static const auto storage_type_name = "storage_type";

    if (auto named_collection = tryGetNamedCollectionWithOverrides(args, context))
    {
        if (named_collection->has(storage_type_name))
        {
            return objectStorageTypeFromString(named_collection->get<String>(storage_type_name));
        }
    }

    auto type_it = args.end();

    /// S3 by default for backward compatibility
    /// Iceberg without storage_type == IcebergS3
    ObjectStorageType type = ObjectStorageType::S3;

    for (auto arg_it = args.begin(); arg_it != args.end(); ++arg_it)
    {
        const auto * type_ast_function = (*arg_it)->as<ASTFunction>();

        if (type_ast_function && type_ast_function->name == "equals"
            && type_ast_function->arguments && type_ast_function->arguments->children.size() == 2)
        {
            auto name = type_ast_function->arguments->children[0]->as<ASTIdentifier>();

            if (name && name->name() == storage_type_name)
            {
                if (type_it != args.end())
                {
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "DataLake can have only one key-value argument: storage_type='type'.");
                }

                auto value = type_ast_function->arguments->children[1]->as<ASTLiteral>();

                if (!value)
                {
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "DataLake parameter 'storage_type' has wrong type, string literal expected.");
                }

                if (value->value.getType() != Field::Types::String)
                {
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "DataLake parameter 'storage_type' has wrong value type, string expected.");
                }

                type = objectStorageTypeFromString(value->value.safeGet<String>());

                type_it = arg_it;
            }
        }
    }

    if (type_it != args.end())
    {
        if (type_arg)
            *type_arg = *type_it;
        args.erase(type_it);
    }

    return type;
}

void StorageIcebergConfiguration::createDynamicStorage(ObjectStorageType type)
{
    if (impl)
    {
        if (impl->getType() == type)
            return;

        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't change datalake engine storage");
    }

    switch (type)
    {
#    if USE_AWS_S3
        case ObjectStorageType::S3:
            impl = std::make_unique<StorageS3IcebergConfiguration>();
            break;
#    endif
#    if USE_AZURE_BLOB_STORAGE
        case ObjectStorageType::Azure:
            impl = std::make_unique<StorageAzureIcebergConfiguration>();
            break;
#    endif
#    if USE_HDFS
        case ObjectStorageType::HDFS:
            impl = std::make_unique<StorageHDFSIcebergConfiguration>();
            break;
#    endif
        case ObjectStorageType::Local:
            impl = std::make_unique<StorageLocalIcebergConfiguration>();
            break;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unsuported DataLake storage {}", type);
    }
}

StorageObjectStorage::Configuration & StorageIcebergConfiguration::getImpl() const
{
    if (!impl)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Dynamic DataLake storage not initialized");

    return *impl;
}

ASTPtr StorageIcebergConfiguration::createArgsWithAccessData() const
{
    return getImpl().createArgsWithAccessData();
}

}
