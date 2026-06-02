#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Common/getRandomASCIIString.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/copyData.h>

#include <base/hex.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace DB::ContentAddressed
{

ContentAddressedWriteBuffer::ContentAddressedWriteBuffer(ObjectStoragePtr object_storage_, std::string temp_dir_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , object_storage(std::move(object_storage_))
{
    fs::create_directories(temp_dir_);
    temp_path = temp_dir_ + "/" + getRandomASCIIString(32) + ".tmp";

    temp_file = std::make_unique<WriteBufferFromFile>(temp_path);
    hashing = std::make_unique<HashingWriteBuffer>(*temp_file);
}

ContentAddressedWriteBuffer::~ContentAddressedWriteBuffer()
{
    /// Best-effort cleanup if finalize() was never called (e.g. an exception unwound the stack).
    cancel();
    removeTempFile();
}

void ContentAddressedWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    hashing->write(working_buffer.begin(), offset());
}

void ContentAddressedWriteBuffer::finalizeImpl()
{
    /// Flush our own buffered data into the hashing buffer first.
    next();

    size = count();

    /// getHash() flushes the hashing buffer (and thus the temp file buffer) and returns the
    /// cityHash128 of everything written.
    const auto hash = hashing->getHash();
    blob_hash = getHexUIntLowercase(hash);

    hashing->finalize();
    temp_file->finalize();

    const std::string key = blobKey(blob_hash);

    /// Put-if-absent: identical content deduplicates to the same blob, so skip the upload when
    /// the object already exists.
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false).has_value())
    {
        ReadBufferFromFile in(temp_path);
        auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
        copyData(in, *out);
        out->finalize();
    }

    removeTempFile();
}

void ContentAddressedWriteBuffer::sync()
{
    next();
    if (hashing)
        hashing->sync();
}

std::string ContentAddressedWriteBuffer::getFileName() const
{
    return temp_path;
}

void ContentAddressedWriteBuffer::removeTempFile() noexcept
{
    if (temp_path.empty())
        return;
    std::error_code ec;
    fs::remove(temp_path, ec);
}

}
