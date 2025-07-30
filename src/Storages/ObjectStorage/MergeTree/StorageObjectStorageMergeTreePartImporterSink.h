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

/*
 * Wrapper around `StorageObjectsStorageSink` that takes care of accounting & metrics for partition export
 */
class StorageObjectStorageMergeTreePartImporterSink : public SinkToStorage
{
public:
    using ConfigurationPtr = StorageObjectStorage::ConfigurationPtr;

    StorageObjectStorageMergeTreePartImporterSink(
        const DataPartPtr & part_,
        const std::string & path_,
        const ObjectStoragePtr & object_storage_,
        const ConfigurationPtr & configuration_,
        const std::optional<FormatSettings> & format_settings_,
        const Block & sample_block_,
        const std::function<void(MergeTreePartImportStats)> & part_log_,
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
    std::function<void(MergeTreePartImportStats)> part_log;

    MergeTreePartImportStats stats;
};

}
