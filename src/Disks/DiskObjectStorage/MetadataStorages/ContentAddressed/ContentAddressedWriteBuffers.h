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
///
/// S3-native staging (Task 4, plan `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md`):
/// a SECOND constructor streams directly to an already-opened object-store sink (an S3 staging
/// object) while hashing, instead of spilling to a local temp file — see its own doc comment below.
/// The local-temp-file constructor above is UNCHANGED byte-for-byte; this is an independent mode
/// selected only by which constructor the caller uses.
class CaContentWriteBuffer : public WriteBufferFromFileBase
{
public:
    using OnFinalized = std::function<void(const std::string & hash_hex, size_t size, const std::string & temp_path)>;

    /// Local-staging mode (today's default; BYTE-FOR-BYTE unchanged behavior). Buffer sizing mirrors
    /// the plain object-storage backends: with adaptive sizing on, the working buffer STARTS small
    /// and grows (what min_columns_to_activate_adaptive_write_buffer toggles — a wide part keeps its
    /// per-INSERT footprint small).
    CaContentWriteBuffer(
        std::string temp_dir,
        size_t buf_size,
        bool use_adaptive_buffer_size,
        size_t adaptive_buffer_initial_size,
        OnFinalized on_finalized_);

    /// S3-native staging mode: `object_store_sink` is an ALREADY-OPENED write buffer over the staging
    /// object at `object_key` (e.g. `object_storage->writeObject(StoredObject(object_key), ...)`).
    /// Bytes are hashed while streaming into `object_store_sink`; on finalize `on_finalized` receives
    /// `object_key` as its third argument (in place of a local temp path) and `getFileName()` returns
    /// it too. `cancelImpl` only cancels `object_store_sink` — it never attempts to delete the
    /// (possibly partially-written) staging object; reclaiming an orphaned staging object after a
    /// cancelled write is the mount-lease sweeper's job (a later task), not this buffer's.
    CaContentWriteBuffer(
        std::unique_ptr<WriteBufferFromFileBase> object_store_sink,
        std::string object_key,
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
    /// Local mode: the local temp file path (removed by removeTempFile). S3 mode: the staging
    /// object's key (never fs::remove'd — see is_s3_staging below).
    std::string temp_path;
    /// Selects the S3-staging semantics in cancelImpl/the destructor (skip local-file cleanup,
    /// since `temp_path` is a remote key, not a path on this filesystem). false (the default,
    /// local-temp-file constructor) is the pre-existing, byte-for-byte-unchanged behavior.
    bool is_s3_staging = false;
    /// The spill sink: a local WriteBufferFromFile (Local mode) or the caller-supplied object-store
    /// sink (S3 mode). Either way it is a SECOND per-stream buffer wrapped by `hashing` below.
    std::unique_ptr<WriteBufferFromFileBase> sink;
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
