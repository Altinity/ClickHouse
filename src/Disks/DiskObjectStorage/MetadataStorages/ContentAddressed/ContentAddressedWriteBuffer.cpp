#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Common/Exception.h>
#include <Common/getRandomASCIIString.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/copyData.h>

#include <base/hex.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::ContentAddressed
{

ContentAddressedWriteBuffer::ContentAddressedWriteBuffer(ObjectStoragePtr object_storage_, std::string key_prefix_, std::string temp_dir_, OnFinalized on_finalized_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , on_finalized(std::move(on_finalized_))
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

    const std::string key = blobKey(key_prefix, BlobHash(blob_hash)).string();

    /// Skip re-uploading when the blob already exists (content dedup). This is a check-then-write,
    /// NOT an atomic put-if-absent — safe here because the key IS the content hash: a racing writer
    /// to the same key writes identical bytes, so the worst case is a redundant upload, never wrong
    /// content. In single-writer M1 a blob is never read until its part's ref is published at commit
    /// (after this write completes), so there is no read-during-write. An atomic conditional PUT
    /// (If-None-Match) and safe multi-writer are deferred (B7/B11). One thing we DO guard now: if an
    /// object already exists at the key but with a different size, that is either a 128-bit hash
    /// collision or a partially-written blob from a crashed writer (LocalObjectStorage writes in
    /// place, no temp+rename) — fail closed rather than silently trust it.
    auto existing = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
    if (existing.has_value())
    {
        if (existing->size_bytes != size)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "Content-addressed blob {} already exists with size {} but new content has size {} "
                "(hash collision or partially-written blob)",
                key, existing->size_bytes, size);
    }
    else
    {
        ReadBufferFromFile in(temp_path);
        auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
        copyData(in, *out);
        out->finalize();
    }

    removeTempFile();

    /// The hash is known and the blob is durable; let the owning transaction record it (typed).
    if (on_finalized)
        on_finalized(BlobHash(blob_hash), size);
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
