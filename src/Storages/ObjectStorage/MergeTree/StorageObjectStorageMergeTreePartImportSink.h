#pragma once

#include <Backups/BackupFactory.h>
#include <Processors/Sinks/SinkToStorage.h>
#include "Storages/MergeTree/RangesInDataPart.h"

namespace DB
{
class StorageObjectStorageMergeTreePartSink : public SinkToStorage
{
public:
    StorageObjectStorageMergeTreePartSink(
        const DataPartPtr & part,
        ObjectStoragePtr object_storage,
        const std::string & format,
        const std::string & compression_method,
        const std::optional<FormatSettings> & format_settings_,
        const Block & sample_block_,
        ContextPtr context);

    String getName() const override { return "StorageObjectStorageMergeTreePartSink"; }

    void consume(Chunk & chunk) override;

    void onFinish() override;

private:
    const String path;
    const Block sample_block;
    std::unique_ptr<WriteBuffer> write_buf;
    OutputFormatPtr writer;

    void finalizeBuffers();
    void releaseBuffers();
    void cancelBuffers();
};

}
