#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/MergeTree/StorageObjectStorageMergeTreePartImportSink.h>
#include "Core/Settings.h"
#include "Disks/ObjectStorages/IObjectStorage.h"
#include "Disks/ObjectStorages/StoredObject.h"
#include "Formats/FormatFactory.h"
#include "IO/CompressionMethod.h"
#include "Processors/Formats/IOutputFormat.h"

namespace DB
{

namespace Setting
{
extern const SettingsUInt64 output_format_compression_level;
extern const SettingsUInt64 output_format_compression_zstd_window_log;
}


StorageObjectStorageMergeTreePartSink::StorageObjectStorageMergeTreePartSink(
    const DataPartPtr & part,
    ObjectStoragePtr object_storage,
    const std::string & format,
    const std::string & compression_method,
    const std::optional<FormatSettings> & format_settings_,
    const Block & sample_block_,
    ContextPtr context)
: SinkToStorage(sample_block_)
{
    const auto & settings = context->getSettingsRef();
    const auto chosen_compression_method = chooseCompressionMethod(path, compression_method);

    auto buffer = object_storage->writeObject(
        StoredObject(path), WriteMode::Rewrite, std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, context->getWriteSettings());

    write_buf = wrapWriteBufferWithCompressionMethod(
        std::move(buffer),
        chosen_compression_method,
        static_cast<int>(settings[Setting::output_format_compression_level]),
        static_cast<int>(settings[Setting::output_format_compression_zstd_window_log]));

    writer = FormatFactory::instance().getOutputFormatParallelIfPossible(format, *write_buf, sample_block, context, format_settings_);
}

void StorageObjectStorageMergeTreePartSink::consume(Chunk & chunk)
{
    if (isCancelled())
        return;
    writer->write(getHeader().cloneWithColumns(chunk.getColumns()));
}

void StorageObjectStorageMergeTreePartSink::onFinish()
{
    if (isCancelled())
        return;

    finalizeBuffers();
    releaseBuffers();
}

void StorageObjectStorageMergeTreePartSink::finalizeBuffers()
{
    if (!writer)
        return;

    try
    {
        writer->flush();
        writer->finalize();
    }
    catch (...)
    {
        /// Stop ParallelFormattingOutputFormat correctly.
        cancelBuffers();
        releaseBuffers();
        throw;
    }

    write_buf->finalize();
}

void StorageObjectStorageMergeTreePartSink::releaseBuffers()
{
    writer.reset();
    write_buf.reset();
}

void StorageObjectStorageMergeTreePartSink::cancelBuffers()
{
    if (writer)
        writer->cancel();
    if (write_buf)
        write_buf->cancel();
}

}
