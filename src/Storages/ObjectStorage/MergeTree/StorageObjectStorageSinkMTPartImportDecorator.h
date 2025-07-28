#pragma once

#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/StorageObjectStorageSink.h>
#include "Core/Settings.h"
#include "Disks/ObjectStorages/IObjectStorage.h"
#include "Disks/ObjectStorages/StoredObject.h"
#include "Formats/FormatFactory.h"
#include "IO/CompressionMethod.h"
#include "Processors/Formats/IOutputFormat.h"
#include "Storages/MergeTree/IMergeTreeDataPart.h"

namespace DB
{

struct MergeTreePartImportStats
{
    ExecutionStatus status;
    std::size_t bytes_on_disk = 0;
    std::size_t read_rows = 0;
    std::size_t read_bytes = 0;
    std::string file_path = "";
    DataPartPtr part = nullptr;
};

class StorageObjectStorageSinkMTPartImportDecorator : public SinkToStorage
{
public:
    using ConfigurationPtr = StorageObjectStorage::ConfigurationPtr;

    StorageObjectStorageSinkMTPartImportDecorator(
        const DataPartPtr & part_,
        const std::string & path_,
        const ObjectStoragePtr & object_storage_,
        const ConfigurationPtr & configuration_,
        const std::optional<FormatSettings> & format_settings_,
        const Block & sample_block_,
        const std::function<void(MergeTreePartImportStats)> & part_log_,
        const ContextPtr & context_)
        : SinkToStorage(sample_block_)
        , object_storage(object_storage_)
        , configuration(configuration_)
        , format_settings(format_settings_)
        , sample_block(sample_block_)
        , context(context_)
        , part_log(part_log_)
    {
        stats.part = part_;
        stats.file_path = path_;
    }

    String getName() const override { return "StorageObjectStorageSinkMTPartImportDecorator"; }

    void consume(Chunk & chunk) override
    {
        if (!sink)
        {
            sink = std::make_shared<StorageObjectStorageSink>(
                stats.file_path,
                object_storage,
                configuration,
                format_settings,
                sample_block,
                context
            );
        }

        stats.read_bytes += chunk.bytes();
        stats.read_rows += chunk.getNumRows();

        sink->consume(chunk);
    }

    void onFinish() override
    {
        sink->onFinish();
        if (const auto object_metadata = object_storage->tryGetObjectMetadata(stats.file_path))
        {
            stats.bytes_on_disk = object_metadata->size_bytes;
        }
        part_log(stats);
    }

    void onException(std::exception_ptr exception) override
    {
        SinkToStorage::onException(exception);
        part_log(stats);
    }

private:
    std::shared_ptr<StorageObjectStorageSink> sink;
    ObjectStoragePtr object_storage;
    ConfigurationPtr configuration;
    std::optional<FormatSettings> format_settings;
    Block sample_block;
    ContextPtr context;
    std::function<void(MergeTreePartImportStats)> part_log;

    MergeTreePartImportStats stats;
};

}
