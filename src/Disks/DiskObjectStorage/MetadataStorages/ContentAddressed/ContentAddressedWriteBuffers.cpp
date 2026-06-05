#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/DiskType.h>
#include <Disks/WriteMode.h>

#include <Common/Exception.h>
#include <Common/getRandomASCIIString.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/copyData.h>

#include <base/hex.h>

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace ContentAddressed
{

ContentAddressedWriteBuffer::ContentAddressedWriteBuffer(
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    std::string temp_dir_,
    size_t buf_size_,
    bool use_adaptive_buffer_size_,
    size_t adaptive_buffer_initial_size_,
    std::shared_ptr<std::mutex> gc_lock_,
    std::shared_ptr<std::set<std::string>> in_flight_pinned_blobs_,
    OnFinalized on_finalized_,
    OnPinBlob on_pin_blob_)
    /// Honour the caller's requested working-buffer size, and the adaptive-sizing flag the plain
    /// object-storage backends (S3/Azure) honour: when adaptive sizing is on we pre-allocate only the
    /// small adaptive-initial buffer instead of the full `buf_size_`. Hard-coding DBMS_DEFAULT_BUFFER_SIZE
    /// here made every content-addressed column stream allocate a full 1 MiB working buffer regardless of
    /// `min_columns_to_activate_adaptive_write_buffer`, so a wide part with hundreds of columns blew the
    /// per-INSERT memory budget. Our own base buffer is fixed (it just stages caller bytes before they
    /// are hashed and spilled), so a small fixed buffer is correct — the costly IO is the temp-file
    /// spill below, which grows adaptively.
    : WriteBufferFromFileBase(use_adaptive_buffer_size_ ? adaptive_buffer_initial_size_ : buf_size_, nullptr, 0)
    , object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , gc_lock(std::move(gc_lock_))
    , in_flight_pinned_blobs(std::move(in_flight_pinned_blobs_))
    , on_finalized(std::move(on_finalized_))
    , on_pin_blob(std::move(on_pin_blob_))
{
    fs::create_directories(temp_dir_);
    temp_path = temp_dir_ + "/" + getRandomASCIIString(32) + ".tmp";

    /// The local-scratch spill buffer is a SECOND per-stream buffer (the bytes hashed out of our own
    /// working buffer land here before being uploaded). Thread the adaptive-sizing flag into it too so
    /// it STARTS small and grows on demand (its native support), keeping the per-stream footprint small
    /// for wide parts. It writes to a local temp file, not the remote stream.
    temp_file = std::make_unique<WriteBufferFromFile>(
        temp_path,
        /*buf_size=*/buf_size_,
        /*flags=*/-1,
        /*throttler=*/nullptr,
        /*mode=*/0666,
        /*existing_memory=*/nullptr,
        /*alignment=*/0,
        use_adaptive_buffer_size_,
        adaptive_buffer_initial_size_);
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

void ContentAddressedWriteBuffer::uploadBlobAtomically(const std::string & key)
{
    /// Publish the blob so a concurrent reader/writer NEVER observes a partially-written object at the
    /// final content-hash key (B41). LocalObjectStorage writes objects IN PLACE (no temp+rename), so a
    /// plain writeObject(key) followed by a streaming copy is visible at `key` while it is still being
    /// filled — a second writer racing the same content hash then sees a size-0 (or short) object and
    /// the size-guard above fires `CORRUPTED_DATA` on a perfectly valid concurrent insert. So for the
    /// local backend we upload to a UNIQUE temp object key in the SAME directory (same filesystem, so
    /// rename is a cheap metadata op) and then atomically rename it onto the final key — the final key
    /// only ever appears fully written. For an object storage whose single PUT is already atomic (S3,
    /// Azure: an object is not visible until the PUT completes) the plain one-shot upload is sufficient.
    if (object_storage->getType() == ObjectStorageType::Local)
    {
        /// For LocalObjectStorage the object "key" IS the on-disk path, so a temp key in the same
        /// parent directory shares the filesystem and std::filesystem::rename is atomic.
        const std::string temp_key = key + ".tmp." + getRandomASCIIString(16);
        {
            ReadBufferFromFile in(temp_path);
            auto out = object_storage->writeObject(StoredObject(temp_key), WriteMode::Rewrite);
            copyData(in, *out);
            out->finalize();
        }

        std::error_code ec;
        fs::rename(temp_key, key, ec);
        if (ec)
        {
            /// Clean up the temp object on failure; the final key is untouched (never partial).
            object_storage->removeObjectIfExists(StoredObject(temp_key));
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "Failed to atomically publish content-addressed blob {} (rename from {} failed: {})",
                key, temp_key, ec.message());
        }
        return;
    }

    ReadBufferFromFile in(temp_path);
    auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
    copyData(in, *out);
    out->finalize();
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

    /// B52: PIN the blob key for the lifetime of this transaction BEFORE deciding whether to skip the
    /// upload. CA GC S4 (G1): the `gc_lock` here is now only the NARROW container guard — it makes the pin
    /// insert + the existence check a single atomic step against a concurrent sweep's pin-set SNAPSHOT (no
    /// data race on the `std::set`), but it no longer excludes the whole sweep. The CROSS-process protection
    /// for a dedup-reused blob in the existence-check -> ref-publish window is the durable session pin
    /// (`on_pin_blob` below, raised before the blob even exists) read by the sweep's §6.2 re-check, NOT this
    /// in-process pin. The two orderings against a same-process sweep are still: (a) we pin first -> the
    /// sweep's snapshot sees the pin and keeps the blob; (b) the sweep deleted the blob just before our
    /// existence check -> we see it absent and RE-UPLOAD it (we still hold the local temp file). Either way
    /// the blob is alive when the ref is published. The pin is released by the owning transaction once the
    /// ref is published (commit) or when an uncommitted transaction is destroyed. Without the background GC
    /// wiring (unit/legacy construction) gc_lock is null and we fall back to a plain existence check.
    auto decide_and_pin = [&]() -> std::optional<ObjectMetadata>
    {
        if (in_flight_pinned_blobs)
            in_flight_pinned_blobs->insert(key);
        /// M8: also publish the CROSS-mounter pin (the WriteSession object) BEFORE the upload. The
        /// in-process pin above only protects this server's own sweep; the durable session object is the
        /// §7 handshake flag `A` — it protects a sweep running on ANY mounter (including this one, now that
        /// the in-process lock no longer excludes the sweep — CA GC S4 G1) that lists the bucket and would
        /// otherwise see this just-uploaded-but-not-yet-referenced blob as unreachable (data loss). It must
        /// be durable before the blob exists, so it is taken here (pin-before-upload), not in the
        /// post-upload on_finalized callback.
        if (on_pin_blob)
            on_pin_blob(BlobHash(blob_hash));
        return object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
    };

    std::optional<ObjectMetadata> existing;
    if (gc_lock)
    {
        std::lock_guard<std::mutex> gc_guard(*gc_lock);
        existing = decide_and_pin();
    }
    else
    {
        existing = decide_and_pin();
    }

    /// Skip re-uploading when the blob already exists (content dedup). The key IS the content hash, so
    /// a racing writer to the same key has identical bytes; the worst case is a redundant upload, never
    /// wrong content. We DO guard one thing: if an object already exists at the key with a DIFFERENT
    /// size, that is either a 128-bit hash collision or a genuinely corrupt blob — fail closed.
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
        uploadBlobAtomically(key);
    }

    removeTempFile();

    /// The hash is known and the blob is durable; let the owning transaction record it (typed).
    if (on_finalized)
        on_finalized(BlobHash(blob_hash), size);
}

void ContentAddressedWriteBuffer::cancelImpl() noexcept
{
    /// The insert-cancel path (~MergeTreeSink -> Finalizer::cancel) destroys this buffer without
    /// finalizing. Propagate the cancel to the inner buffers (hashing wraps temp_file) so they are not
    /// destroyed "neither finalized nor canceled". No blob is uploaded or recorded on cancel (only
    /// finalizeImpl uploads); the destructor's removeTempFile reclaims the scratch file.
    WriteBufferFromFileBase::cancelImpl();
    if (hashing)
        hashing->cancel();
    if (temp_file)
        temp_file->cancel();
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

ContentAddressedInlineWriteBuffer::ContentAddressedInlineWriteBuffer(OnInlined on_inlined_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , on_inlined(std::move(on_inlined_))
{
}

ContentAddressedInlineWriteBuffer::~ContentAddressedInlineWriteBuffer()
{
    /// Best-effort cleanup if finalize() was never called (e.g. an exception unwound the stack).
    cancel();
}

void ContentAddressedInlineWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    accumulated.append(working_buffer.begin(), offset());
}

void ContentAddressedInlineWriteBuffer::finalizeImpl()
{
    /// Flush our own buffered data into the accumulator first.
    next();
    if (on_inlined)
        on_inlined(std::move(accumulated));
}

void ContentAddressedInlineWriteBuffer::sync()
{
    next();
}

std::string ContentAddressedInlineWriteBuffer::getFileName() const
{
    return "<content-addressed-inline>";
}

}

}
