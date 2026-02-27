#pragma once

#include <Storages/IStorage.h>
#include <Storages/NamedCollectionsHelpers.h>
#include <Storages/ObjectStorage/Azure/Configuration.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLakeMetadata.h>
#include <Storages/ObjectStorage/DataLakes/HudiMetadata.h>
#include <Storages/ObjectStorage/DataLakes/IDataLakeMetadata.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/HDFS/Configuration.h>
#include <Storages/ObjectStorage/Local/Configuration.h>
#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ObjectStorage/StorageObjectStorageConfiguration.h>
#include <Storages/StorageFactory.h>
#include <Storages/ColumnsDescription.h>
#include <Formats/FormatFilterInfo.h>
#include <Formats/FormatParserSharedResources.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSetQuery.h>
#include <Disks/DiskType.h>

#include <memory>
#include <string>

#include <Common/ErrorCodes.h>
#include <Databases/DataLake/RestCatalog.h>
#include <Databases/DataLake/GlueCatalog.h>

#include <fmt/ranges.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int FORMAT_VERSION_TOO_OLD;
    extern const int LOGICAL_ERROR;
}

namespace DataLakeStorageSetting
{
    extern const DataLakeStorageSettingsBool allow_dynamic_metadata_for_data_lakes;
    extern const DataLakeStorageSettingsDatabaseDataLakeCatalogType storage_catalog_type;
    extern const DataLakeStorageSettingsString object_storage_endpoint;
    extern const DataLakeStorageSettingsString storage_aws_access_key_id;
    extern const DataLakeStorageSettingsString storage_aws_secret_access_key;
    extern const DataLakeStorageSettingsString storage_region;
    extern const DataLakeStorageSettingsString storage_catalog_url;
    extern const DataLakeStorageSettingsString storage_warehouse;
    extern const DataLakeStorageSettingsString storage_catalog_credential;
    extern DataLakeStorageSettingsString storage_aws_role_arn;
    extern DataLakeStorageSettingsString storage_aws_role_session_name;
    extern const DataLakeStorageSettingsString storage_auth_scope;
    extern const DataLakeStorageSettingsString storage_auth_header;
    extern const DataLakeStorageSettingsString storage_oauth_server_uri;
    extern const DataLakeStorageSettingsBool storage_oauth_server_use_request_body;
    extern const DataLakeStorageSettingsString iceberg_metadata_file_path;
}

template <typename T>
concept StorageConfiguration = std::derived_from<T, StorageObjectStorageConfiguration>;

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
class DataLakeConfiguration : public BaseStorageConfiguration, public std::enable_shared_from_this<StorageObjectStorageConfiguration>
{
public:
    explicit DataLakeConfiguration(DataLakeStorageSettingsPtr settings_) : settings(settings_) {}

    bool isDataLakeConfiguration() const override { return true; }

    const DataLakeStorageSettings & getDataLakeSettings() const override { return *settings; }

    std::string getEngineName() const override { return DataLakeMetadata::name + BaseStorageConfiguration::getEngineName(); }

    /// Returns true, if metadata is of the latest version, false if unknown.
    bool update(
        ObjectStoragePtr object_storage,
        ContextPtr local_context,
        bool if_not_updated_before,
        bool check_consistent_with_previous_metadata) override
    {
        const bool updated_before = current_metadata != nullptr;
        if (updated_before && if_not_updated_before)
            return false;

        BaseStorageConfiguration::update(
            object_storage, local_context, if_not_updated_before, check_consistent_with_previous_metadata);

        const bool changed = updateMetadataIfChanged(object_storage, local_context);
        if (!changed)
            return true;

        if (check_consistent_with_previous_metadata && hasExternalDynamicMetadata() && updated_before)
        {
            throw Exception(
                ErrorCodes::FORMAT_VERSION_TOO_OLD,
                "Metadata is not consistent with the one which was used to infer table schema. "
                "Please, retry the query.");
        }
        return true;
    }

    void create(
        ObjectStoragePtr object_storage,
        ContextPtr local_context,
        const std::optional<ColumnsDescription> & columns,
        ASTPtr partition_by,
        bool if_not_exists,
        std::shared_ptr<DataLake::ICatalog> catalog,
        const StorageID & table_id_) override
    {
        BaseStorageConfiguration::create(
            object_storage, local_context, columns, partition_by, if_not_exists, catalog, table_id_);

        DataLakeMetadata::createInitial(
            object_storage,
            weak_from_this(),
            local_context,
            columns,
            partition_by,
            if_not_exists,
            catalog,
            table_id_
        );
    }

    bool supportsDelete() const override
    {
        assertInitializedDL();
        return current_metadata->supportsDelete();
    }

    void mutate(const MutationCommands & commands,
        ContextPtr context,
        const StorageID & storage_id,
        StorageMetadataPtr metadata_snapshot,
        std::shared_ptr<DataLake::ICatalog> catalog,
        const std::optional<FormatSettings> & format_settings) override
    {
        assertInitializedDL();
        current_metadata->mutate(commands, context, storage_id, metadata_snapshot, catalog, format_settings);
    }

    void checkMutationIsPossible(const MutationCommands & commands) override
    {
        assertInitializedDL();
        current_metadata->checkMutationIsPossible(commands);
    }

    void checkAlterIsPossible(const AlterCommands & commands) override
    {
        assertInitializedDL();
        current_metadata->checkAlterIsPossible(commands);
    }

    void alter(const AlterCommands & params, ContextPtr context) override
    {
        assertInitializedDL();
        current_metadata->alter(params, context);

    }

    std::optional<ColumnsDescription> tryGetTableStructureFromMetadata() const override
    {
        assertInitializedDL();
        if (auto schema = current_metadata->getTableSchema(); !schema.empty())
            return ColumnsDescription(std::move(schema));
        return std::nullopt;
    }

    std::optional<size_t> totalRows(ContextPtr local_context) override
    {
        assertInitializedDL();
        return current_metadata->totalRows(local_context);
    }

    std::optional<size_t> totalBytes(ContextPtr local_context) override
    {
        assertInitializedDL();
        return current_metadata->totalBytes(local_context);
    }

    std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(ContextPtr local_context, ObjectInfoPtr object_info) const override
    {
        assertInitializedDL();
        return current_metadata->getInitialSchemaByPath(local_context, object_info);
    }

    std::shared_ptr<const ActionsDAG> getSchemaTransformer(ContextPtr local_context, ObjectInfoPtr object_info) const override
    {
        assertInitializedDL();
        return current_metadata->getSchemaTransformer(local_context, object_info);
    }

    bool hasExternalDynamicMetadata() override
    {
        assertInitializedDL();
        return (*settings)[DataLakeStorageSetting::allow_dynamic_metadata_for_data_lakes]
            && current_metadata->supportsSchemaEvolution();
    }

    IDataLakeMetadata * getExternalMetadata() override
    {
        assertInitializedDL();
        return current_metadata.get();
    }

    bool supportsFileIterator() const override { return true; }

    bool supportsWrites() const override
    {
        assertInitializedDL();
        return current_metadata->supportsWrites();
    }

    ObjectIterator iterate(
        const ActionsDAG * filter_dag,
        IDataLakeMetadata::FileProgressCallback callback,
        size_t list_batch_size,
        ContextPtr context) override
    {
        assertInitializedDL();
        return current_metadata->iterate(filter_dag, callback, list_batch_size, context);
    }

    /// This is an awful temporary crutch,
    /// which will be removed once DeltaKernel is used by default for DeltaLake.
    /// By release 25.3.
    /// (Because it does not make sense to support it in a nice way
    /// because the code will be removed ASAP anyway)
#if USE_PARQUET && USE_AWS_S3
    DeltaLakePartitionColumns getDeltaLakePartitionColumns() const
    {
        assertInitializedDL();
        const auto * delta_lake_metadata = dynamic_cast<const DeltaLakeMetadata *>(current_metadata.get());
        if (delta_lake_metadata)
            return delta_lake_metadata->getPartitionColumns();
        return {};
    }
#endif

    void modifyFormatSettings(FormatSettings & settings_, const Context & local_context) const override
    {
        assertInitializedDL();
        current_metadata->modifyFormatSettings(settings_, local_context);
    }

    ColumnMapperPtr getColumnMapperForObject(ObjectInfoPtr object_info) const override
    {
        assertInitializedDL();
        return current_metadata->getColumnMapperForObject(object_info);
    }
    ColumnMapperPtr getColumnMapperForCurrentSchema() const override
    {
        assertInitializedDL();
        return current_metadata->getColumnMapperForCurrentSchema();
    }

    SinkToStoragePtr write(
        SharedHeader sample_block,
        const StorageID & table_id,
        ObjectStoragePtr object_storage,
        const std::optional<FormatSettings> & format_settings,
        ContextPtr context,
        std::shared_ptr<DataLake::ICatalog> catalog) override
    {
        return current_metadata->write(
            sample_block,
            table_id,
            object_storage,
            shared_from_this(),
            format_settings.has_value() ? *format_settings : FormatSettings{},
            context,
            catalog);
    }

    std::shared_ptr<DataLake::ICatalog> getCatalog([[maybe_unused]] ContextPtr context, [[maybe_unused]] bool is_attach) const override
    {
#if USE_AWS_S3 && USE_AVRO
        if ((*settings)[DataLakeStorageSetting::storage_catalog_type].value == DatabaseDataLakeCatalogType::GLUE)
        {
            auto catalog_parameters = DataLake::CatalogSettings{
                .storage_endpoint = (*settings)[DataLakeStorageSetting::object_storage_endpoint].value,
                .aws_access_key_id = (*settings)[DataLakeStorageSetting::storage_aws_access_key_id].value,
                .aws_secret_access_key = (*settings)[DataLakeStorageSetting::storage_aws_secret_access_key].value,
                .region = (*settings)[DataLakeStorageSetting::storage_region].value,
                .aws_role_arn = (*settings)[DataLakeStorageSetting::storage_aws_role_arn].value,
                .aws_role_session_name = (*settings)[DataLakeStorageSetting::storage_aws_role_session_name].value
            };

            return std::make_shared<DataLake::GlueCatalog>(
                (*settings)[DataLakeStorageSetting::storage_catalog_url].value,
                context,
                catalog_parameters,
                /* table_engine_definition */nullptr
            );
        }
        /// Attach condition is provided for compatibility.
        if ((*settings)[DataLakeStorageSetting::storage_catalog_type].value == DatabaseDataLakeCatalogType::ICEBERG_REST ||
            (is_attach && (*settings)[DataLakeStorageSetting::storage_catalog_type].value == DatabaseDataLakeCatalogType::NONE && !(*settings)[DataLakeStorageSetting::storage_catalog_url].value.empty()))
        {
            return std::make_shared<DataLake::RestCatalog>(
                (*settings)[DataLakeStorageSetting::storage_warehouse].value,
                (*settings)[DataLakeStorageSetting::storage_catalog_url].value,
                (*settings)[DataLakeStorageSetting::storage_catalog_credential].value,
                (*settings)[DataLakeStorageSetting::storage_auth_scope].value,
                (*settings)[DataLakeStorageSetting::storage_auth_header],
                (*settings)[DataLakeStorageSetting::storage_oauth_server_uri].value,
                (*settings)[DataLakeStorageSetting::storage_oauth_server_use_request_body].value,
                context);
        }

#endif
        return nullptr;
    }

    bool optimize(const StorageMetadataPtr & metadata_snapshot, ContextPtr context, const std::optional<FormatSettings> & format_settings) override
    {
        assertInitializedDL();
        return current_metadata->optimize(metadata_snapshot, context, format_settings);
    }

    void addDeleteTransformers(ObjectInfoPtr object_info, QueryPipelineBuilder & builder, const std::optional<FormatSettings> & format_settings, ContextPtr local_context) const override
    {
        current_metadata->addDeleteTransformers(object_info, builder, format_settings, local_context);
    }

    ASTPtr createArgsWithAccessData() const override
    {
        auto res = BaseStorageConfiguration::createArgsWithAccessData();

        auto iceberg_metadata_file_path = (*settings)[DataLakeStorageSetting::iceberg_metadata_file_path];

        if (iceberg_metadata_file_path.changed)
        {
            auto * arguments = res->template as<ASTExpressionList>();
            if (!arguments)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Arguments are not an expression list");

            bool has_settings = false;

            for (auto & arg : arguments->children)
            {
                if (auto * settings_ast = arg->template as<ASTSetQuery>())
                {
                    has_settings = true;
                    settings_ast->changes.setSetting("iceberg_metadata_file_path", iceberg_metadata_file_path.value);
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

private:
    DataLakeMetadataPtr current_metadata;
    LoggerPtr log = getLogger("DataLakeConfiguration");
    const DataLakeStorageSettingsPtr settings;

    void assertInitializedDL() const
    {
        BaseStorageConfiguration::assertInitialized();
        if (!current_metadata)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Metadata is not initialized");
    }

    ReadFromFormatInfo prepareReadingFromFormat(
        ObjectStoragePtr object_storage,
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        bool supports_subset_of_columns,
        bool supports_tuple_elements,
        ContextPtr local_context,
        const PrepareReadingFromFormatHiveParams &) override
    {
        if (!current_metadata)
        {
            current_metadata = DataLakeMetadata::create(
                object_storage,
                weak_from_this(),
                local_context);
        }
        return current_metadata->prepareReadingFromFormat(
            requested_columns, storage_snapshot, local_context, supports_subset_of_columns, supports_tuple_elements);
    }

    bool updateMetadataIfChanged(
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

        if (*current_metadata == *new_metadata)
            return false;

        current_metadata = std::move(new_metadata);
        return true;
    }
};


#if USE_AVRO
#    if USE_AWS_S3
using StorageS3IcebergConfiguration = DataLakeConfiguration<StorageS3Configuration, IcebergMetadata>;
#endif

#    if USE_AZURE_BLOB_STORAGE
using StorageAzureIcebergConfiguration = DataLakeConfiguration<StorageAzureConfiguration, IcebergMetadata>;
#endif

#    if USE_HDFS
using StorageHDFSIcebergConfiguration = DataLakeConfiguration<StorageHDFSConfiguration, IcebergMetadata>;
#endif

using StorageLocalIcebergConfiguration = DataLakeConfiguration<StorageLocalConfiguration, IcebergMetadata>;

/// Class detects storage type by `storage_type` parameter if exists
/// and uses appropriate implementation - S3, Azure, HDFS or Local
class StorageIcebergConfiguration : public StorageObjectStorageConfiguration, public std::enable_shared_from_this<StorageObjectStorageConfiguration>
{
    friend class StorageObjectStorageConfiguration;

public:
    explicit StorageIcebergConfiguration(DataLakeStorageSettingsPtr settings_) : settings(settings_) {}

    ObjectStorageType getType() const override { return getImpl().getType(); }

    std::string getTypeName() const override { return getImpl().getTypeName(); }
    std::string getEngineName() const override { return getImpl().getEngineName(); }
    std::string getNamespaceType() const override { return getImpl().getNamespaceType(); }

    Path getRawPath() const override { return getImpl().getRawPath(); }
    const String & getRawURI() const override { return getImpl().getRawURI(); }
    const Path & getPathForRead() const override { return getImpl().getPathForRead(); }
    Path getPathForWrite(const std::string & partition_id) const override { return getImpl().getPathForWrite(partition_id); }

    void setPathForRead(const Path & path) override { getImpl().setPathForRead(path); }

    const Paths & getPaths() const override { return getImpl().getPaths(); }
    void setPaths(const Paths & paths) override { getImpl().setPaths(paths); }

    String getDataSourceDescription() const override { return getImpl().getDataSourceDescription(); }
    String getNamespace() const override { return getImpl().getNamespace(); }

    StorageObjectStorageQuerySettings getQuerySettings(const ContextPtr & context) const override
        { return getImpl().getQuerySettings(context); }

    void addStructureAndFormatToArgsIfNeeded(
        ASTs & args, const String & structure_, const String & format_, ContextPtr context, bool with_structure) override
        { getImpl().addStructureAndFormatToArgsIfNeeded(args, structure_, format_, context, with_structure); }

    bool isNamespaceWithGlobs() const override { return getImpl().isNamespaceWithGlobs(); }

    bool isArchive() const override { return getImpl().isArchive(); }
    bool isPathInArchiveWithGlobs() const override { return getImpl().isPathInArchiveWithGlobs(); }
    std::string getPathInArchive() const override { return getImpl().getPathInArchive(); }

    void check(ContextPtr context) override { getImpl().check(context); }
    void validateNamespace(const String & name) const override { getImpl().validateNamespace(name); }

    ObjectStoragePtr createObjectStorage(ContextPtr context, bool is_readonly) override
        { return getImpl().createObjectStorage(context, is_readonly); }
    bool isStaticConfiguration() const override { return getImpl().isStaticConfiguration(); }

    bool isDataLakeConfiguration() const override { return getImpl().isDataLakeConfiguration(); }

    std::optional<size_t> totalRows(ContextPtr context) override { return getImpl().totalRows(context); }
    std::optional<size_t> totalBytes(ContextPtr context) override { return getImpl().totalBytes(context); }

    bool hasExternalDynamicMetadata() override { return getImpl().hasExternalDynamicMetadata(); }

    IDataLakeMetadata * getExternalMetadata() override { return getImpl().getExternalMetadata(); }

    std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(ContextPtr context, ObjectInfoPtr object_info) const override
        { return getImpl().getInitialSchemaByPath(context, object_info); }

    std::shared_ptr<const ActionsDAG> getSchemaTransformer(ContextPtr context, ObjectInfoPtr object_info) const override
        { return getImpl().getSchemaTransformer(context, object_info); }

    void modifyFormatSettings(FormatSettings & settings_, const Context & local_context) const override { getImpl().modifyFormatSettings(settings_, local_context); }

    void addDeleteTransformers(
        ObjectInfoPtr object_info,
        QueryPipelineBuilder & builder,
        const std::optional<FormatSettings> & format_settings,
        ContextPtr local_context) const override { getImpl().addDeleteTransformers(object_info, builder, format_settings, local_context); }

    ReadFromFormatInfo prepareReadingFromFormat(
        ObjectStoragePtr object_storage,
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        bool supports_subset_of_columns,
        bool supports_tuple_elements,
        ContextPtr local_context,
        const PrepareReadingFromFormatHiveParams & hive_parameters) override
    {
        return getImpl().prepareReadingFromFormat(
            object_storage,
            requested_columns,
            storage_snapshot,
            supports_subset_of_columns,
            supports_tuple_elements,
            local_context,
            hive_parameters);
    }

    void initPartitionStrategy(ASTPtr partition_by, const ColumnsDescription & columns, ContextPtr context) override
        { getImpl().initPartitionStrategy(partition_by, columns, context); }

    std::optional<ColumnsDescription> tryGetTableStructureFromMetadata() const override
        { return getImpl().tryGetTableStructureFromMetadata(); }

    bool supportsFileIterator() const override { return getImpl().supportsFileIterator(); }
    bool supportsWrites() const override { return getImpl().supportsWrites(); }

    bool supportsPartialPathPrefix() const override { return getImpl().supportsPartialPathPrefix(); }

    ObjectIterator iterate(
        const ActionsDAG * filter_dag,
        std::function<void(FileProgress)> callback,
        size_t list_batch_size,
        ContextPtr context) override
    {
        return getImpl().iterate(filter_dag, callback, list_batch_size, context);
    }

    bool update(
        ObjectStoragePtr object_storage_ptr,
        ContextPtr context,
        bool if_not_updated_before,
        bool check_consistent_with_previous_metadata) override
    {
        return getImpl().update(object_storage_ptr, context, if_not_updated_before, check_consistent_with_previous_metadata);
    }

    void create(
        ObjectStoragePtr object_storage,
        ContextPtr local_context,
        const std::optional<ColumnsDescription> & columns,
        ASTPtr partition_by,
        bool if_not_exists,
        std::shared_ptr<DataLake::ICatalog> catalog,
        const StorageID & table_id_) override
    {
        getImpl().create(object_storage, local_context, columns, partition_by, if_not_exists, catalog, table_id_);
    }

    SinkToStoragePtr write(
        SharedHeader sample_block,
        const StorageID & table_id,
        ObjectStoragePtr object_storage,
        const std::optional<FormatSettings> & format_settings,
        ContextPtr context,
        std::shared_ptr<DataLake::ICatalog> catalog) override
    {
        return getImpl().write(sample_block, table_id, object_storage, format_settings, context, catalog);
    }

    bool supportsDelete() const override { return getImpl().supportsDelete(); }
    void mutate(const MutationCommands & commands,
        ContextPtr context,
        const StorageID & storage_id,
        StorageMetadataPtr metadata_snapshot,
        std::shared_ptr<DataLake::ICatalog> catalog,
        const std::optional<FormatSettings> & format_settings) override
    {
        getImpl().mutate(commands, context, storage_id, metadata_snapshot, catalog, format_settings);
    }

    void checkMutationIsPossible(const MutationCommands & commands) override { getImpl().checkMutationIsPossible(commands); }

    void checkAlterIsPossible(const AlterCommands & commands) override { getImpl().checkAlterIsPossible(commands); }

    void alter(const AlterCommands & params, ContextPtr context) override { getImpl().alter(params, context); }

    const DataLakeStorageSettings & getDataLakeSettings() const override { return getImpl().getDataLakeSettings(); }

    void initialize(
        ASTs & engine_args,
        ContextPtr local_context,
        bool with_table_structure) override
    {
        createDynamicConfiguration(engine_args, local_context);
        getImpl().initialize(engine_args, local_context, with_table_structure);
    }

    ASTPtr createArgsWithAccessData() const override
    {
        return getImpl().createArgsWithAccessData();
    }

    void fromNamedCollection(const NamedCollection & collection, ContextPtr context) override
        { getImpl().fromNamedCollection(collection, context); }
    void fromAST(ASTs & args, ContextPtr context, bool with_structure) override
        { getImpl().fromAST(args, context, with_structure); }

    const String & getFormat() const override { return getImpl().getFormat(); }
    const String & getCompressionMethod() const override { return getImpl().getCompressionMethod(); }
    const String & getStructure() const override { return getImpl().getStructure(); }

    PartitionStrategyFactory::StrategyType getPartitionStrategyType() const override { return getImpl().getPartitionStrategyType(); }
    bool getPartitionColumnsInDataFile() const override { return getImpl().getPartitionColumnsInDataFile(); }
    std::shared_ptr<IPartitionStrategy> getPartitionStrategy() const override { return getImpl().getPartitionStrategy(); }

    void setFormat(const String & format_) override { getImpl().setFormat(format_); }
    void setCompressionMethod(const String & compression_method_) override { getImpl().setCompressionMethod(compression_method_); }
    void setStructure(const String & structure_) override { getImpl().setStructure(structure_); }

    void setPartitionStrategyType(PartitionStrategyFactory::StrategyType partition_strategy_type_) override
    {
        getImpl().setPartitionStrategyType(partition_strategy_type_);
    }
    void setPartitionColumnsInDataFile(bool partition_columns_in_data_file_) override
    {
        getImpl().setPartitionColumnsInDataFile(partition_columns_in_data_file_);
    }
    void setPartitionStrategy(const std::shared_ptr<IPartitionStrategy> & partition_strategy_) override
    {
        getImpl().setPartitionStrategy(partition_strategy_);
    }

    ColumnMapperPtr getColumnMapperForObject(ObjectInfoPtr obj) const override { return getImpl().getColumnMapperForObject(obj); }

    ColumnMapperPtr getColumnMapperForCurrentSchema() const override { return getImpl().getColumnMapperForCurrentSchema(); }

    std::shared_ptr<DataLake::ICatalog> getCatalog(ContextPtr context, bool is_attach) const override
    {
        return getImpl().getCatalog(context, is_attach);
    }

    bool optimize(const StorageMetadataPtr & metadata_snapshot, ContextPtr context, const std::optional<FormatSettings> & format_settings) override
    {
        return getImpl().optimize(metadata_snapshot, context, format_settings);
    }

protected:
    /// Find storage_type argument and remove it from args if exists.
    /// Return storage type.
    ObjectStorageType extractDynamicStorageType(ASTs & args, ContextPtr context, ASTPtr * type_arg) const override
    {
        static const auto * const storage_type_name = "storage_type";

        if (auto named_collection = tryGetNamedCollectionWithOverrides(args, context))
        {
            if (named_collection->has(storage_type_name))
            {
                return objectStorageTypeFromString(named_collection->get<String>(storage_type_name));
            }
        }

        auto * type_it = args.end();

        /// S3 by default for backward compatibility
        /// Iceberg without storage_type == IcebergS3
        ObjectStorageType type = ObjectStorageType::S3;

        for (auto * arg_it = args.begin(); arg_it != args.end(); ++arg_it)
        {
            const auto * type_ast_function = (*arg_it)->as<ASTFunction>();

            if (type_ast_function && type_ast_function->name == "equals"
                && type_ast_function->arguments && type_ast_function->arguments->children.size() == 2)
            {
                auto * name = type_ast_function->arguments->children[0]->as<ASTIdentifier>();

                if (name && name->name() == storage_type_name)
                {
                    if (type_it != args.end())
                    {
                        throw Exception(
                            ErrorCodes::BAD_ARGUMENTS,
                            "DataLake can have only one key-value argument: storage_type='type'.");
                    }

                    auto * value = type_ast_function->arguments->children[1]->as<ASTLiteral>();

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

    void createDynamicConfiguration(ASTs & args, ContextPtr context)
    {
        ObjectStorageType type = extractDynamicStorageType(args, context, nullptr);
        createDynamicStorage(type);
    }

    void assertInitialized() const override { getImpl().assertInitialized(); }

private:
    inline StorageObjectStorageConfiguration & getImpl() const
    {
        if (!impl)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Dynamic DataLake storage not initialized");

        return *impl;
    }

    void createDynamicStorage(ObjectStorageType type)
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
                impl = std::make_unique<StorageS3IcebergConfiguration>(settings);
                break;
#    endif
#    if USE_AZURE_BLOB_STORAGE
            case ObjectStorageType::Azure:
                impl = std::make_unique<StorageAzureIcebergConfiguration>(settings);
                break;
#    endif
#    if USE_HDFS
            case ObjectStorageType::HDFS:
                impl = std::make_unique<StorageHDFSIcebergConfiguration>(settings);
                break;
#    endif
            case ObjectStorageType::Local:
                impl = std::make_unique<StorageLocalIcebergConfiguration>(settings);
                break;
            default:
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Unsuported DataLake storage {}", type);
        }
    }

    StorageObjectStorageConfigurationPtr impl;
    DataLakeStorageSettingsPtr settings;
};
#endif

#if USE_PARQUET
#if USE_AWS_S3
using StorageS3DeltaLakeConfiguration = DataLakeConfiguration<StorageS3Configuration, DeltaLakeMetadata>;
#endif

#if USE_AZURE_BLOB_STORAGE
using StorageAzureDeltaLakeConfiguration = DataLakeConfiguration<StorageAzureConfiguration, DeltaLakeMetadata>;
#endif

using StorageLocalDeltaLakeConfiguration = DataLakeConfiguration<StorageLocalConfiguration, DeltaLakeMetadata>;

#endif

#if USE_AWS_S3
using StorageS3HudiConfiguration = DataLakeConfiguration<StorageS3Configuration, HudiMetadata>;
#endif
}
