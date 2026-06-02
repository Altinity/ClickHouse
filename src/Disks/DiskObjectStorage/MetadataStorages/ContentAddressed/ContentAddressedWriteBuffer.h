#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>

#include <memory>
#include <string>

namespace DB::ContentAddressed
{

/// Write buffer for the content-addressed disk.
///
/// The normal object-storage write path picks the remote object key up front and streams
/// straight to remote. Content addressing cannot do that: the key is the content hash, which
/// is only known once all bytes have been written, and object storage has no rename.
///
/// So this buffer spills incoming bytes to a unique local temp file while accumulating the
/// same `cityHash128` ClickHouse uses for `checksums.txt` file hashes. On finalize it derives
/// the key `blobs/<hash>` and uploads the temp file there exactly once, using put-if-absent:
/// if an object with that key already exists the upload is skipped and the existing object is
/// reused (identical content deduplicates to the same blob).
class ContentAddressedWriteBuffer : public WriteBufferFromFileBase
{
public:
    ContentAddressedWriteBuffer(ObjectStoragePtr object_storage_, std::string temp_dir_);
    ~ContentAddressedWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

    /// Valid after finalize: lowercase hex of the cityHash128 of the written content.
    const std::string & getBlobHash() const { return blob_hash; }
    /// Valid after finalize: number of bytes written.
    size_t getSize() const { return size; }

private:
    void nextImpl() override;
    void finalizeImpl() override;
    void removeTempFile() noexcept;

    ObjectStoragePtr object_storage;
    std::string temp_path;

    std::unique_ptr<WriteBufferFromFile> temp_file;
    std::unique_ptr<HashingWriteBuffer> hashing;

    std::string blob_hash;
    size_t size = 0;
};

}
