#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <IO/AsynchronousReader.h>
#include <IO/ReadBufferFromFile.h>

namespace Poco { class Logger; }

namespace DB
{
class FilesystemCacheLog;

/**
 * Remote disk might need to split one clickhouse file into multiple files in remote fs.
 * This class works like a proxy to allow transition from one file into multiple.
 */
class ReadBufferFromRemoteFSGather final : public ReadBufferFromFileBase
{
friend class ReadIndirectBufferFromRemoteFS;

public:
    using ReadBufferCreator = std::function<std::unique_ptr<ReadBufferFromFileBase>(bool restricted_seek, const StoredObject & object)>;

    /// `object_payload_offset_` is a fixed prefix in EVERY object that is not part of the logical
    /// file -- a content-addressed blob's envelope header. `StoredObject::bytes_size` keeps meaning
    /// the object's LOGICAL contribution (the payload length), so all of this class's file-offset
    /// arithmetic is unchanged; the offset is applied only where a position is handed to, or read
    /// back from, a per-object buffer. The creator is given the PHYSICAL size, so a cache layer
    /// wrapped around it still sees the real object length. Zero -- the default, and the case for
    /// every caller other than a chunked content-addressed read -- costs nothing.
    ReadBufferFromRemoteFSGather(
        ReadBufferCreator && read_buffer_creator_,
        const StoredObjects & blobs_to_read_,
        size_t min_bytes_for_seek_,
        bool use_external_buffer_,
        size_t buffer_size,
        size_t object_payload_offset_ = 0);

    String getFileName() const override { return current_object.remote_path; }

    String getInfoForLog() override { return current_buf ? current_buf->getInfoForLog() : ""; }

    void setReadUntilPosition(size_t position) override;

    void setReadUntilEnd() override { setReadUntilPosition(getFileSize()); }

    std::optional<size_t> tryGetFileSize() override { return getTotalSize(blobs_to_read); }

    size_t getFileOffsetOfBufferEnd() const override { return file_offset_of_buffer_end; }

    off_t seek(off_t offset, int whence) override;

    off_t getPosition() override { return file_offset_of_buffer_end - available(); }

    bool isSeekCheap() override;

    bool isContentCached(size_t offset, size_t size) override;

private:
    SeekableReadBufferPtr createImplementationBuffer(const StoredObject & object, size_t start_offset);

    bool nextImpl() override;

    void initialize();

    bool readImpl();

    bool moveToNextBuffer();

    void reset();

    const size_t min_bytes_for_seek;
    const StoredObjects blobs_to_read;
    const ReadBufferCreator read_buffer_creator;
    const String query_id;
    const bool use_external_buffer;
    /// Bytes at the front of every object that are not logical file content (see the constructor).
    const size_t object_payload_offset = 0;

    size_t read_until_position = 0;
    size_t file_offset_of_buffer_end = 0;

    StoredObject current_object;
    size_t current_buf_idx = 0;
    SeekableReadBufferPtr current_buf;

    LoggerPtr log;
};
}
