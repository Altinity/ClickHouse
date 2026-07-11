#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Common/getRandomASCIIString.h>
#include <base/hex.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace DB::ContentAddressed
{

CaContentWriteBuffer::CaContentWriteBuffer(
    std::string temp_dir,
    size_t buf_size,
    bool use_adaptive_buffer_size,
    size_t adaptive_buffer_initial_size,
    OnFinalized on_finalized_)
    : WriteBufferFromFileBase(use_adaptive_buffer_size ? adaptive_buffer_initial_size : buf_size, nullptr, 0)
    , on_finalized(std::move(on_finalized_))
{
    fs::create_directories(temp_dir);
    temp_path = temp_dir + "/" + getRandomASCIIString(32) + ".tmp";

    /// The spill buffer is a SECOND per-stream buffer; thread the adaptive flag into it too so a
    /// wide part keeps its footprint small. Its IO is a local temp file, not the remote stream.
    sink = std::make_unique<WriteBufferFromFile>(
        temp_path,
        buf_size,
        /*flags=*/-1,
        /*throttler=*/nullptr,
        /*mode=*/0666,
        /*existing_memory=*/nullptr,
        /*alignment=*/0,
        use_adaptive_buffer_size,
        adaptive_buffer_initial_size);
    hashing = std::make_unique<HashingWriteBuffer>(*sink);
}

CaContentWriteBuffer::CaContentWriteBuffer(
    std::unique_ptr<WriteBufferFromFileBase> object_store_sink,
    std::string object_key,
    size_t buf_size,
    bool use_adaptive_buffer_size,
    size_t adaptive_buffer_initial_size,
    OnFinalized on_finalized_)
    : WriteBufferFromFileBase(use_adaptive_buffer_size ? adaptive_buffer_initial_size : buf_size, nullptr, 0)
    , on_finalized(std::move(on_finalized_))
    , temp_path(std::move(object_key))
    , is_s3_staging(true)
    , sink(std::move(object_store_sink))
{
    /// The sink is ALREADY opened against the staging object by the caller (writeFile) — this
    /// constructor only wraps it in the hashing chain, exactly like the local-temp-file mode. The
    /// adaptive-sizing params only affect THIS outer buffer (mirroring the Local ctor above); the
    /// sink's own buffering was decided by the caller when it opened the object-store write.
    hashing = std::make_unique<HashingWriteBuffer>(*sink);
}

CaContentWriteBuffer::~CaContentWriteBuffer()
{
    /// Best-effort cleanup if finalize was never reached (exception unwind / cancel).
    cancel();
    /// B188: if on_finalized ran successfully the transaction (Local mode) or a later task's promote
    /// path (S3 mode) owns the staged bytes and cleans them up. Do not remove them here. S3-mode
    /// staging objects are never removed by this class at all (see cancelImpl / removeTempFile).
    if (!temp_ownership_transferred && !is_s3_staging)
        removeTempFile();
}

void CaContentWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    hashing->write(working_buffer.begin(), offset());
}

void CaContentWriteBuffer::finalizeImpl()
{
    next();
    const size_t size = count();

    /// getHash flushes the chain and returns the streaming cityHash128 of everything written.
    const auto hash = hashing->getHash();
    const std::string hash_hex = getHexUIntLowercase(hash);

    hashing->finalize();
    sink->finalize();

    /// B188: on successful finalize, ownership of temp_path (Local: the local temp path; S3: the
    /// staging object key) transfers to the caller (the transaction uploads/promotes it and cleans
    /// up). cancel() still removes/cancels it.
    if (on_finalized)
    {
        on_finalized(hash_hex, size, temp_path);
        temp_ownership_transferred = true;
    }
}

void CaContentWriteBuffer::cancelImpl() noexcept
{
    if (hashing)
        hashing->cancel();
    if (sink)
        sink->cancel();
    /// S3 mode: `temp_path` is a remote object key, not a path on this filesystem — do NOT attempt
    /// to delete the (possibly partially-written) staging object here. Cancelling `sink` above is
    /// enough to make sure no partial finalize happens; reclaiming an orphaned staging object is the
    /// mount-lease sweeper's job (a later task).
    if (!is_s3_staging)
        removeTempFile();
}

void CaContentWriteBuffer::removeTempFile() noexcept
{
    std::error_code ec;
    fs::remove(temp_path, ec);
}

void CaContentWriteBuffer::sync()
{
    next();
    hashing->next();
    sink->sync();
}

std::string CaContentWriteBuffer::getFileName() const
{
    return temp_path;
}

CaInlineWriteBuffer::CaInlineWriteBuffer(OnInlined on_inlined_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , on_inlined(std::move(on_inlined_))
{
}

CaInlineWriteBuffer::~CaInlineWriteBuffer()
{
    cancel();
}

void CaInlineWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    accumulated.append(working_buffer.begin(), offset());
}

void CaInlineWriteBuffer::finalizeImpl()
{
    next();
    if (on_inlined)
        on_inlined(std::move(accumulated));
}

void CaInlineWriteBuffer::sync()
{
    next();
}

std::string CaInlineWriteBuffer::getFileName() const
{
    return "ca_inline";
}

}
