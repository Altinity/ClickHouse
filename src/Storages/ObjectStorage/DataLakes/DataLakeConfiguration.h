#pragma once

#include <Storages/IStorage.h>
#include <Storages/ObjectStorage/Azure/Configuration.h>
#include <Storages/ObjectStorage/DataLakes/DeltaLakeMetadata.h>
#include <Storages/ObjectStorage/DataLakes/HudiMetadata.h>
#include <Storages/ObjectStorage/DataLakes/IDataLakeMetadata.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/HDFS/Configuration.h>
#include <Storages/ObjectStorage/Local/Configuration.h>
#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSettings.h>


namespace DB
{

template <typename T>
concept StorageConfiguration = std::derived_from<T, StorageObjectStorage::Configuration>;

template <StorageConfiguration BaseStorageConfiguration, typename DataLakeMetadata>
class DataLakeConfiguration : public BaseStorageConfiguration, public std::enable_shared_from_this<StorageObjectStorage::Configuration>
{
public:
    using Configuration = StorageObjectStorage::Configuration;

    bool isDataLakeConfiguration() const override { return true; }

    std::string getEngineName() const override { return DataLakeMetadata::name + BaseStorageConfiguration::getEngineName(); }

    void update(ObjectStoragePtr object_storage, ContextPtr local_context) override;

    std::optional<ColumnsDescription> tryGetTableStructureFromMetadata() const override;

    std::optional<String> tryGetSamplePathFromMetadata() const override;

    std::optional<size_t> totalRows() override;

    std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(const String & data_path) const override;

    std::shared_ptr<const ActionsDAG> getSchemaTransformer(const String & data_path) const override;

    bool hasExternalDynamicMetadata() override;

    ColumnsDescription updateAndGetCurrentSchema(
        ObjectStoragePtr object_storage,
        ContextPtr context) override;

    bool supportsFileIterator() const override { return true; }

    ObjectIterator iterate(
        const ActionsDAG * filter_dag,
        IDataLakeMetadata::FileProgressCallback callback,
        size_t list_batch_size) override
    {
        chassert(current_metadata);
        return current_metadata->iterate(filter_dag, callback, list_batch_size);
    }

    /// This is an awful temporary crutch,
    /// which will be removed once DeltaKernel is used by default for DeltaLake.
    /// By release 25.3.
    /// (Because it does not make sense to support it in a nice way
    /// because the code will be removed ASAP anyway)
#if USE_PARQUET && USE_AWS_S3
    DeltaLakePartitionColumns getDeltaLakePartitionColumns() const
    {
        const auto * delta_lake_metadata = dynamic_cast<const DeltaLakeMetadata *>(current_metadata.get());
        if (delta_lake_metadata)
            return delta_lake_metadata->getPartitionColumns();
        return {};
    }
#endif

    ASTPtr createArgsWithAccessData() const override;

private:
    DataLakeMetadataPtr current_metadata;
    LoggerPtr log = getLogger("DataLakeConfiguration");

    ReadFromFormatInfo prepareReadingFromFormat(
        ObjectStoragePtr object_storage,
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        bool supports_subset_of_columns,
        ContextPtr local_context) override;

    bool updateMetadataObjectIfNeeded(
        ObjectStoragePtr object_storage,
        ContextPtr context);
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
class StorageIcebergConfiguration : public StorageObjectStorage::Configuration, public std::enable_shared_from_this<StorageObjectStorage::Configuration>
{
    friend class StorageObjectStorage::Configuration;

public:
    ObjectStorageType getType() const override { return getImpl().getType(); }

    std::string getTypeName() const override { return getImpl().getTypeName(); }
    std::string getEngineName() const override { return getImpl().getEngineName(); }
    std::string getNamespaceType() const override { return getImpl().getNamespaceType(); }

    Path getFullPath() const override { return getImpl().getFullPath(); }
    Path getPath() const override { return getImpl().getPath(); }
    void setPath(const Path & path) override { getImpl().setPath(path); }

    const Paths & getPaths() const override { return getImpl().getPaths(); }
    void setPaths(const Paths & paths) override { getImpl().setPaths(paths); }

    String getDataSourceDescription() const override { return getImpl().getDataSourceDescription(); }
    String getNamespace() const override { return getImpl().getNamespace(); }

    StorageObjectStorage::QuerySettings getQuerySettings(const ContextPtr & context) const override
        { return getImpl().getQuerySettings(context); }

    void addStructureAndFormatToArgsIfNeeded(
        ASTs & args, const String & structure_, const String & format_, ContextPtr context, bool with_structure) override
        { getImpl().addStructureAndFormatToArgsIfNeeded(args, structure_, format_, context, with_structure); }

    bool withPartitionWildcard() const override { return getImpl().withPartitionWildcard(); }
    bool withGlobsIgnorePartitionWildcard() const override { return getImpl().withGlobsIgnorePartitionWildcard(); }
    bool isPathWithGlobs() const override { return getImpl().isPathWithGlobs(); }
    bool isNamespaceWithGlobs() const override { return getImpl().isNamespaceWithGlobs(); }
    std::string getPathWithoutGlobs() const override { return getImpl().getPathWithoutGlobs(); }

    bool isArchive() const override { return getImpl().isArchive(); }
    bool isPathInArchiveWithGlobs() const override { return getImpl().isPathInArchiveWithGlobs(); }
    std::string getPathInArchive() const override { return getImpl().getPathInArchive(); }

    void check(ContextPtr context) const override { getImpl().check(context); }
    void validateNamespace(const String & name) const override { getImpl().validateNamespace(name); }

    ObjectStoragePtr createObjectStorage(ContextPtr context, bool is_readonly) override
        { return getImpl().createObjectStorage(context, is_readonly); }
    StorageObjectStorage::ConfigurationPtr clone() override { return getImpl().clone(); }
    bool isStaticConfiguration() const override { return getImpl().isStaticConfiguration(); }

    bool isDataLakeConfiguration() const override { return getImpl().isDataLakeConfiguration(); }

    bool hasExternalDynamicMetadata() override { return getImpl().hasExternalDynamicMetadata(); }

    std::shared_ptr<NamesAndTypesList> getInitialSchemaByPath(const String & path) const override
        { return getImpl().getInitialSchemaByPath(path); }

    std::shared_ptr<const ActionsDAG> getSchemaTransformer(const String & data_path) const override
        { return getImpl().getSchemaTransformer(data_path); }

    ColumnsDescription updateAndGetCurrentSchema(ObjectStoragePtr object_storage, ContextPtr context) override
        { return getImpl().updateAndGetCurrentSchema(object_storage, context); }

    ReadFromFormatInfo prepareReadingFromFormat(
        ObjectStoragePtr object_storage,
        const Strings & requested_columns,
        const StorageSnapshotPtr & storage_snapshot,
        bool supports_subset_of_columns,
        ContextPtr local_context) override
    {
        return getImpl().prepareReadingFromFormat(
            object_storage,
            requested_columns,
            storage_snapshot,
            supports_subset_of_columns,
            local_context);
    }

    std::optional<ColumnsDescription> tryGetTableStructureFromMetadata() const override
        { return getImpl().tryGetTableStructureFromMetadata(); }

    bool supportsFileIterator() const override { return getImpl().supportsFileIterator(); }
    ObjectIterator iterate(
        const ActionsDAG * filter_dag,
        std::function<void(FileProgress)> callback,
        size_t list_batch_size) override
    {
        return getImpl().iterate(filter_dag, callback, list_batch_size);
    }

    void update(ObjectStoragePtr object_storage, ContextPtr local_context) override
        { return getImpl().update(object_storage, local_context); }
    void updateIfRequired(ObjectStoragePtr object_storage, ContextPtr local_context) override
        { return getImpl().updateIfRequired(object_storage, local_context); }

    void initialize(
        ASTs & engine_args,
        ContextPtr local_context,
        bool with_table_structure,
        std::shared_ptr<StorageObjectStorageSettings> settings) override
    {
        createDynamicConfiguration(engine_args, local_context);
        getImpl().initialize(engine_args, local_context, with_table_structure, settings);
    }

    ASTPtr createArgsWithAccessData() const override;

    const String & getFormat() const override { return getImpl().getFormat(); }
    const String & getCompressionMethod() const override { return getImpl().getCompressionMethod(); }
    const String & getStructure() const override { return getImpl().getStructure(); }

    void setFormat(const String & format_) override { getImpl().setFormat(format_); }
    void setCompressionMethod(const String & compression_method_) override { getImpl().setCompressionMethod(compression_method_); }
    void setStructure(const String & structure_) override { getImpl().setStructure(structure_); }

    void fromNamedCollection(const NamedCollection & collection, ContextPtr context) override
        { return getImpl().fromNamedCollection(collection, context); }
    void fromAST(ASTs & args, ContextPtr context, bool with_structure) override
        { return getImpl().fromAST(args, context, with_structure); }

    /// Find storage_type argument and remove it from args if exists.
    /// Return storage type.
    ObjectStorageType extractDynamicStorageType(ASTs & args, ContextPtr context, ASTPtr * type_arg = nullptr) const override;

    void createDynamicConfiguration(ASTs & args, ContextPtr context)
    {
        ObjectStorageType type = extractDynamicStorageType(args, context);
        createDynamicStorage(type);
    }

    std::optional<String> tryGetSamplePathFromMetadata() const override
    {
        return getImpl().tryGetSamplePathFromMetadata();
    }

    virtual void assertInitialized() const override { return getImpl().assertInitialized(); }

private:
    StorageObjectStorage::Configuration & getImpl() const;

    void createDynamicStorage(ObjectStorageType type);

    std::shared_ptr<StorageObjectStorage::Configuration> impl;
};
#endif

#if USE_PARQUET && USE_AWS_S3
using StorageS3DeltaLakeConfiguration = DataLakeConfiguration<StorageS3Configuration, DeltaLakeMetadata>;
#endif

#if USE_PARQUET
using StorageLocalDeltaLakeConfiguration = DataLakeConfiguration<StorageLocalConfiguration, DeltaLakeMetadata>;
#endif

#if USE_AWS_S3
using StorageS3HudiConfiguration = DataLakeConfiguration<StorageS3Configuration, HudiMetadata>;
#endif
}
