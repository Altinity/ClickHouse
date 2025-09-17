#pragma once

#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSink.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <Formats/FormatFactory.h>
#include <Processors/Formats/IOutputFormat.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/IStorage.h>

namespace DB
{

using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;

/*
 * Wrapper around `StorageObjectsStorageSink` that takes care of accounting & metrics for partition export
 */
class StorageObjectStorageImporterSink : public SinkToStorage
{
public:
    using ConfigurationPtr = StorageObjectStorage::ConfigurationPtr;

    StorageObjectStorageImporterSink(
        const std::string & path_,
        const ObjectStoragePtr & object_storage_,
        const ConfigurationPtr & configuration_,
        const std::optional<FormatSettings> & format_settings_,
        const Block & sample_block_,
        const std::function<void(IStorage::ImportStats)> & part_log_,
        const ContextPtr & context_);

    String getName() const override;

    void consume(Chunk & chunk) override;

    void onFinish() override;

    void onException(std::exception_ptr exception) override;

private:
    std::shared_ptr<StorageObjectStorageSink> sink;
    ObjectStoragePtr object_storage;
    ConfigurationPtr configuration;
    std::optional<FormatSettings> format_settings;
    Block sample_block;
    ContextPtr context;
    std::function<void(IStorage::ImportStats)> part_log;

    IStorage::ImportStats stats;
};

}
