#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <Core/Defines.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromFileBase.h>

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace DB
{

namespace ContentAddressed
{

/// Write buffer for the content-addressed disk (a write-path impl detail used only by
/// ContentAddressedTransaction, so it lives here).
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
    /// Invoked from finalizeImpl once the content hash is known and the blob has been uploaded
    /// (or found already present). Lets the owning transaction record (logical_file -> blob). The
    /// hash is handed over as a typed BlobHash so the transaction cannot confuse it with an object key.
    using OnFinalized = std::function<void(const BlobHash & blob_hash, size_t size)>;

    /// Invoked from finalizeImpl at the SAME point the in-process pin (B52) is taken — under the GC
    /// lock, BEFORE the dedup existence-check / upload. Lets the owning transaction record the blob in
    /// its cross-mounter WriteSession and persist that session object, so a GC sweep on another mounter
    /// treats the hash as reachable before it is ever uploaded (M8 cross-mounter pin). The hash is typed
    /// so it cannot be confused with an object key. May be empty (no session wiring).
    using OnPinBlob = std::function<void(const BlobHash & blob_hash)>;

    /// key_prefix_ is the object-storage common key prefix to prepend to the blob key; an empty
    /// prefix yields the bare blobs/<hash> key. It is threaded from the owning transaction so the
    /// blob is uploaded exactly where the read side resolves it.
    /// `gc_lock_` + `in_flight_pinned_blobs_` are the per-pool GC lock and in-flight pin set (B52). On
    /// finalize, the buffer takes the lock, pins the blob key, then makes its dedup existence-check
    /// decision under that lock so the pin is visible to a concurrent sweep before the skip — closing
    /// the window where a reused (skip-uploaded) blob could be reclaimed before the ref is published.
    /// Both may be null (legacy / unit construction with no background GC): then no pin is taken.
    /// `buf_size_` is the working-buffer size requested by the caller (the MergeTree writer threads it
    /// down). `use_adaptive_buffer_size_` / `adaptive_buffer_initial_size_` mirror the same flags the
    /// plain object-storage backends honour (`S3ObjectStorage`, `AzureObjectStorage`): when adaptive
    /// sizing is on the per-stream buffer STARTS at `adaptive_buffer_initial_size_` (a few KiB) and
    /// grows on demand, instead of pre-allocating the full `buf_size_`. This is what
    /// `min_columns_to_activate_adaptive_write_buffer` toggles, so a wide part with hundreds of columns
    /// keeps its per-INSERT write-buffer footprint small. These size ONLY this buffer's own working
    /// buffer — the caller writes into it before the bytes are hashed and spilled — so the footprint
    /// tracks the plain path. The local-scratch spill file keeps a small fixed buffer of its own (its
    /// IO is to a local temp file, not the costly remote stream).
    ContentAddressedWriteBuffer(
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        std::string temp_dir_,
        size_t buf_size_ = DBMS_DEFAULT_BUFFER_SIZE,
        bool use_adaptive_buffer_size_ = false,
        size_t adaptive_buffer_initial_size_ = DBMS_DEFAULT_INITIAL_ADAPTIVE_BUFFER_SIZE,
        std::shared_ptr<std::mutex> gc_lock_ = nullptr,
        std::shared_ptr<std::set<std::string>> in_flight_pinned_blobs_ = nullptr,
        OnFinalized on_finalized_ = {},
        OnPinBlob on_pin_blob_ = {});
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
    void cancelImpl() noexcept override;
    void removeTempFile() noexcept;

    /// Publish the spilled temp file to the final content-hash blob key so a concurrent reader/writer
    /// never observes a partially-written object (B41). For the local backend (in-place writes) this
    /// uploads to a unique temp object key in the same directory and atomically renames it onto the
    /// final key; for an atomic-PUT backend (S3/Azure) it uploads the final key in one shot.
    void uploadBlobAtomically(const std::string & key);

    ObjectStoragePtr object_storage;
    std::string key_prefix;
    std::string temp_path;
    /// Per-pool GC lock + in-flight blob pin set (B52); see the ctor doc. May be null.
    std::shared_ptr<std::mutex> gc_lock;
    std::shared_ptr<std::set<std::string>> in_flight_pinned_blobs;
    OnFinalized on_finalized;
    /// Invoked under gc_lock, before the dedup-skip/upload decision, to record the blob in the owning
    /// transaction's cross-mounter WriteSession (M8). May be empty.
    OnPinBlob on_pin_blob{};

    std::unique_ptr<WriteBufferFromFile> temp_file;
    std::unique_ptr<HashingWriteBuffer> hashing;

    std::string blob_hash;
    size_t size = 0;
};

/// Write buffer for a MUTABLE per-part file (uuid.txt / txn_version.txt / metadata_version.txt).
///
/// Unlike a content file, a mutable file must NOT be content-addressed: two parts with identical
/// content share one manifest, but each keeps its own mutable bytes. So this buffer does NOT upload a
/// blob — it accumulates the bytes in memory and, on finalize, hands them to the owning transaction
/// to store inline in that part's per-ref sidecar (no orphan blob is ever created). The bytes are
/// tiny (a uuid / a small integer), so in-memory accumulation is appropriate.
class ContentAddressedInlineWriteBuffer : public WriteBufferFromFileBase
{
public:
    /// Invoked from finalizeImpl with the fully-accumulated file bytes.
    using OnInlined = std::function<void(std::string bytes)>;

    explicit ContentAddressedInlineWriteBuffer(OnInlined on_inlined_);
    ~ContentAddressedInlineWriteBuffer() override;

    void sync() override;
    std::string getFileName() const override;

private:
    void nextImpl() override;
    void finalizeImpl() override;

    OnInlined on_inlined;
    std::string accumulated;
};

}

}
