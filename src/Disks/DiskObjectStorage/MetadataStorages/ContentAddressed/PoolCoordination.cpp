#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ObjectIO.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteSettings.h>

#include <Common/Exception.h>
#include <Common/ErrnoException.h>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int S3_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int CANNOT_OPEN_FILE;
    extern const int CANNOT_WRITE_TO_FILE_DESCRIPTOR;
    extern const int CANNOT_CLOSE_FILE;
}

namespace ContentAddressed
{

namespace
{

/// Atomically create `path` on the local filesystem carrying `bytes`. The create is made atomic with
/// `O_CREAT | O_EXCL`: the open fails with `EEXIST` if the file already exists, which is the CAS-lost
/// signal (returns false). For `LocalObjectStorage` the object key IS the local path (see `readObject`
/// / `exists` in `LocalObjectStorage.cpp`, which use `StoredObject::remote_path` verbatim as the path),
/// so this is the exact seam the S3 `If-None-Match` path occupies on a real object store.
bool localCreateExcl(const std::string & path, const std::string & bytes)
{
    /// Unlike a real object store, a local create cannot materialize a missing parent prefix, so create
    /// the parent directory tree first (mirrors `LocalObjectStorage::writeObject`).
    fs::create_directories(fs::path(path).parent_path());

    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (fd < 0)
    {
        if (errno == EEXIST)
            return false; /// CAS lost: another writer created the object first.
        ErrnoException::throwFromPath(ErrorCodes::CANNOT_OPEN_FILE, path, "Cannot atomically create file {}", path);
    }

    /// We own the freshly created file: write the payload, then close. On any failure unlink the
    /// half-written file so a retry can re-take the CAS (we are the only writer that can see it).
    size_t written = 0;
    while (written < bytes.size())
    {
        ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            int saved_errno = errno;
            ::close(fd);
            ::unlink(path.c_str());
            errno = saved_errno;
            ErrnoException::throwFromPath(ErrorCodes::CANNOT_WRITE_TO_FILE_DESCRIPTOR, path, "Cannot write to file {}", path);
        }
        written += static_cast<size_t>(n);
    }

    if (0 != ::close(fd))
    {
        int saved_errno = errno;
        ::unlink(path.c_str());
        errno = saved_errno;
        ErrnoException::throwFromPath(ErrorCodes::CANNOT_CLOSE_FILE, path, "Cannot close file {}", path);
    }

    return true;
}

/// Issue a conditional PUT with `If-None-Match: *`. The object store rejects a write onto an existing
/// key with `PreconditionFailed` (HTTP 412), surfaced as an `S3Exception` (code `S3_ERROR`) whose
/// message carries `PreconditionFailed` — the same conflict signal the Iceberg metadata writer relies
/// on (`IcebergMetadata.cpp`). A conflict returns false; any other error is rethrown.
bool condCreateViaIfNoneMatch(IObjectStorage & object_storage, const std::string & key, const std::string & bytes)
{
    WriteSettings ws;
    ws.object_storage_write_if_none_match = "*";

    try
    {
        auto buf = object_storage.writeObject(
            StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, /*buf_size=*/DBMS_DEFAULT_BUFFER_SIZE,
            ContentAddressed::caControlWriteSettings(ws));
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        return true;
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::S3_ERROR && e.message().find("PreconditionFailed") != std::string::npos)
            return false; /// CAS lost: the key already existed.
        throw;
    }
}

/// Backends we already know honor `If-None-Match: *`: S3-like and Azure. `getName` returns these exact
/// strings (see the object-storage headers). `LocalObjectStorage` is handled separately (O_EXCL).
bool isKnownConditionalCreateBackend(const IObjectStorage & object_storage)
{
    const std::string name = object_storage.getName();
    return name == "S3" || name == "Azure";
}

/// One-time capability probe for an UNKNOWN remote backend (fail closed). Cond-create a throwaway key
/// twice via the `If-None-Match` path: a backend that truly supports conditional create MUST report the
/// second create as a conflict (false). If it reports success twice, it silently overwrote and does NOT
/// support CAS — using it would race, so we refuse. The result is memoized per backend type so the
/// probe runs at most once for each unknown backend name in a process.
void ensureConditionalCreateSupported(IObjectStorage & object_storage)
{
    if (isKnownConditionalCreateBackend(object_storage))
        return;

    static std::mutex probed_mutex;
    static std::set<std::string> probed_ok;

    const std::string name = object_storage.getName();
    {
        std::lock_guard lock(probed_mutex);
        if (probed_ok.contains(name))
            return;
    }

    const std::string probe_key = ".cas_capability_probe";

    /// Best-effort cleanup so the probe is repeatable; ignore failures.
    try
    {
        object_storage.removeObjectIfExists(StoredObject(probe_key));
    }
    catch (...) // NOLINT
    {
    }

    const bool first = condCreateViaIfNoneMatch(object_storage, probe_key, "1");
    const bool second = condCreateViaIfNoneMatch(object_storage, probe_key, "2");

    try
    {
        object_storage.removeObjectIfExists(StoredObject(probe_key));
    }
    catch (...) // NOLINT
    {
    }

    if (!(first && !second))
        throw Exception(
            ErrorCodes::NOT_IMPLEMENTED,
            "ContentAddressed: object storage backend '{}' does not support a conditional create-if-absent "
            "(compare-and-set); refusing to coordinate a content-addressed pool on it (a read-then-write "
            "fallback would race). Probe results: first create={}, second create={} (expected true/false).",
            name, first, second);

    std::lock_guard lock(probed_mutex);
    probed_ok.insert(name);
}

}

bool condCreateIfAbsent(IObjectStorage & object_storage, const std::string & key, const std::string & bytes)
{
    /// `LocalObjectStorage` does NOT honor `If-None-Match`, so use an atomic `O_EXCL` create at the
    /// resolved local path. For this backend the object key is the local filesystem path verbatim.
    if (dynamic_cast<LocalObjectStorage *>(&object_storage) != nullptr)
        return localCreateExcl(key, bytes);

    /// Any other backend: it must honor `If-None-Match: *`. Probe unknown backends once (fail closed).
    ensureConditionalCreateSupported(object_storage);
    return condCreateViaIfNoneMatch(object_storage, key, bytes);
}

namespace
{

/// Read the bytes at `key`, or nullopt if it does not exist. Used by the lock acquire/renew/release
/// read-checks (the fence is the authority, but the lock object is read to make the liveness/steal
/// decision and to avoid clobbering a successor's lock).
std::optional<std::string> readIfExists(IObjectStorage & object_storage, const std::string & key)
{
    StoredObject object(key);
    if (!object_storage.exists(object))
        return std::nullopt;
    auto buf = object_storage.readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    std::string content;
    readStringUntilEOF(content, *buf);
    return content;
}

/// Rewrite (unconditionally overwrite) `key` with `bytes`. Used only on the steal / renew paths, where
/// the fence token — not the lock object — is the safety authority, so a non-atomic overwrite is safe.
void rewriteObject(IObjectStorage & object_storage, const std::string & key, const std::string & bytes)
{
    auto buf = object_storage.writeObject(StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, ContentAddressed::caControlWriteSettings());
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

}

uint64_t allocateFenceToken(IObjectStorage & object_storage, const std::string & key_prefix, uint64_t start_hint)
{
    /// Scan n upward from the hint (never below 1) and take the FIRST n we can create. Only one caller
    /// can win `condCreateIfAbsent(fence/<n>)` for a given n, so the returned token is unique; a token
    /// already taken just makes us advance, so a later allocation can only land strictly higher than any
    /// earlier-completed one. The hint is an optimization (skip known-taken tokens), never a correctness
    /// requirement: even a stale hint converges by scanning. The fence object's bytes are irrelevant
    /// (only the key's existence matters), so we store the token itself for debuggability.
    uint64_t n = start_hint < 1 ? 1 : start_hint;
    while (true)
    {
        if (condCreateIfAbsent(object_storage, fenceKey(key_prefix, n), std::to_string(n)))
            return n;
        ++n;
    }
}

std::string GcLock::serialize() const
{
    /// MAGIC(4) + encoding version(1) + body, on the shared codec. The body carries the owning server id
    /// (length-prefixed string), then the lease deadline and fence token (fixed-width little-endian u64).
    /// All explicitly little-endian so the object is byte-identical regardless of the writer's arch.
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, ENCODING_VERSION}.write(buf);
    DB::writeStringBinary(server_id, buf);
    DB::writeBinaryLittleEndian(lease_deadline_unix, buf);
    DB::writeBinaryLittleEndian(fence_token, buf);
    buf.finalize();
    return out;
}

GcLock GcLock::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, ENCODING_VERSION, "gc.lock");

    GcLock lock;
    DB::readStringBinary(lock.server_id, buf);
    DB::readBinaryLittleEndian(lock.lease_deadline_unix, buf);
    DB::readBinaryLittleEndian(lock.fence_token, buf);
    return lock;
}

std::optional<GcLock> tryAcquireGcLock(
    IObjectStorage & object_storage,
    const std::string & key_prefix,
    const std::string & server_id,
    uint64_t lease_seconds,
    uint64_t now_unix)
{
    const std::string key = gcLockKey(key_prefix);

    /// Absent -> cond-create with a freshly-allocated fence (start_hint = 1). The CAS makes the create
    /// atomic: only one of two truly-concurrent first-takers can create the lock object. If WE created
    /// it, we are the leader. (We allocate the fence BEFORE the cond-create; a fence consumed by a lost
    /// create is simply skipped by the next allocation — fence tokens are cheap and monotonic.)
    {
        GcLock fresh;
        fresh.server_id = server_id;
        fresh.fence_token = allocateFenceToken(object_storage, key_prefix, /*start_hint=*/1);
        fresh.lease_deadline_unix = now_unix + lease_seconds;
        if (condCreateIfAbsent(object_storage, key, fresh.serialize()))
            return fresh;
    }

    /// The lock already existed: read it and decide live-vs-steal.
    auto existing_bytes = readIfExists(object_storage, key);
    if (!existing_bytes)
        /// Raced with a release between the cond-create and the read; the caller can simply retry.
        return std::nullopt;

    const GcLock existing = GcLock::deserialize(*existing_bytes);

    /// Still live: someone holds leadership. We do not take it.
    if (existing.lease_deadline_unix >= now_unix)
        return std::nullopt;

    /// Expired -> STEAL. Allocate a fence strictly higher than the dead holder's, then rewrite the lock.
    /// A two-stealer race here is acceptable: each stealer allocates a DISTINCT (and both strictly
    /// higher) fence token, and the fence — not this lock object — is the safety authority a later task
    /// re-checks before deleting. The rewrite only records the most-recent steal for the liveness hint.
    GcLock stolen;
    stolen.server_id = server_id;
    stolen.fence_token = allocateFenceToken(object_storage, key_prefix, /*start_hint=*/existing.fence_token + 1);
    stolen.lease_deadline_unix = now_unix + lease_seconds;
    rewriteObject(object_storage, key, stolen.serialize());
    return stolen;
}

bool renewGcLock(
    IObjectStorage & object_storage,
    const std::string & key_prefix,
    GcLock & held,
    uint64_t lease_seconds,
    uint64_t now_unix)
{
    const std::string key = gcLockKey(key_prefix);

    /// Read-check: we may only renew while the on-disk lock still carries OUR fence token. If it is gone
    /// or a higher fence took over, a successor stole leadership and we lost it — never rewrite then.
    auto on_disk_bytes = readIfExists(object_storage, key);
    if (!on_disk_bytes)
        return false;
    if (GcLock::deserialize(*on_disk_bytes).fence_token != held.fence_token)
        return false;

    held.lease_deadline_unix = now_unix + lease_seconds;
    rewriteObject(object_storage, key, held.serialize());
    return true;
}

void releaseGcLock(IObjectStorage & object_storage, const std::string & key_prefix, const GcLock & held)
{
    const std::string key = gcLockKey(key_prefix);

    /// Read-check before delete: only remove the lock while it still carries OUR fence token, so we
    /// never delete a successor's lock (which would let a third party take leadership while the
    /// successor still believes it holds it). A missing or superseded lock is a no-op.
    auto on_disk_bytes = readIfExists(object_storage, key);
    if (!on_disk_bytes)
        return;
    if (GcLock::deserialize(*on_disk_bytes).fence_token != held.fence_token)
        return;

    object_storage.removeObjectIfExists(StoredObject(key));
}

}

}
