#include <Storages/ObjectStorage/MergeTree/StorageObjectStorageMergeTreePartImporterSink.h>

namespace DB
{

StorageObjectStorageMergeTreePartImporterSink::StorageObjectStorageMergeTreePartImporterSink(
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
    sink = std::make_shared<StorageObjectStorageSink>(
        stats.file_path,
        object_storage,
        configuration,
        format_settings,
        sample_block,
        context);
}

String StorageObjectStorageMergeTreePartImporterSink::getName() const
{
    return "StorageObjectStorageMergeTreePartImporterSink";
}

void StorageObjectStorageMergeTreePartImporterSink::consume(Chunk & chunk)
{
    sink->consume(chunk);

    stats.read_bytes += chunk.bytes();
    stats.read_rows += chunk.getNumRows();
}

void StorageObjectStorageMergeTreePartImporterSink::onFinish()
{
    sink->onFinish();
    if (const auto object_metadata = object_storage->tryGetObjectMetadata(stats.file_path))
    {
        stats.bytes_on_disk = object_metadata->size_bytes;
    }
    part_log(stats);
}

void StorageObjectStorageMergeTreePartImporterSink::onException(std::exception_ptr exception)
{
    sink->onException(exception);
    stats.status = ExecutionStatus(-1, "Error importing part");
    part_log(stats);
}

}
