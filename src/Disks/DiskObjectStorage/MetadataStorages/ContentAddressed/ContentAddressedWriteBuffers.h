#pragma once
#include <Core/Defines.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>
#include <functional>
#include <memory>
#include <string>

namespace DB::ContentAddressed
{

/// Write buffer for a CONTENT part file (M-W T3). The blob key is the content hash, only known
/// once all bytes are written, so the buffer spills to a unique local temp file while hashing
/// (the streaming HashingWriteBuffer convention — the pool-wide file-hash function the wiring
/// defines; the core never re-hashes payloads). On finalize it hands (hash_hex, size, temp_path)
/// to the owning transaction; B188: the transaction owns the temp file post-finalize and uploads it
/// post-precommit, so finalizeImpl no longer removes it. cancelImpl and the destructor (on error
/// paths) still remove it.
class CaContentWriteBuffer : public WriteBufferFromFileBase
{
public:
    using OnFinalized = std::function<void(const std::string & hash_hex, size_t size, const std::string & temp_path)>;

    /// Buffer sizing mirrors the plain object-storage backends: with adaptive sizing on, the
    /// working buffer STARTS small and grows (what min_columns_to_activate_adaptive_write_buffer
    /// toggles — a wide part keeps its per-INSERT footprint small).
    CaContentWriteBuffer(
        std::string temp_dir,
        size_t buf_size,
        bool use_adaptive_buffer_size,
        size_t adaptive_buffer_initial_size,
        OnFinalized on_finalized_);
    ~CaContentWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    void nextImpl() override;
    void finalizeImpl() override;
    void cancelImpl() noexcept override;
    void removeTempFile() noexcept;

    OnFinalized on_finalized;
    std::string temp_path;
    std::unique_ptr<WriteBufferFromFile> temp_file;
    std::unique_ptr<HashingWriteBuffer> hashing;
    bool temp_ownership_transferred = false;   /// B188: set after on_finalized; the dtor skips removeTempFile
};

/// Write buffer for bytes that live INSIDE pool metadata (a mutable per-part file staged into
/// RefPayload.mutable_files, or a verbatim namespace file PUT on finalize). Accumulates in memory
/// (the bytes are tiny) and hands them to the callback at finalize; the callback decides where
/// they go and whether they are durable immediately (verbatim) or at commit (mutable staging).
class CaInlineWriteBuffer : public WriteBufferFromFileBase
{
public:
    using OnInlined = std::function<void(std::string bytes)>;

    explicit CaInlineWriteBuffer(OnInlined on_inlined_);
    ~CaInlineWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    void nextImpl() override;
    void finalizeImpl() override;

    OnInlined on_inlined;
    std::string accumulated;
};

}
