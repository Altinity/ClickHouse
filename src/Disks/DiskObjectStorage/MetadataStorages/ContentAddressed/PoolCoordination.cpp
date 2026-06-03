#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

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
            StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, /*buf_size=*/DBMS_DEFAULT_BUFFER_SIZE, ws);
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

}

}
