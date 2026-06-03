#pragma once
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <string>

namespace DB::ContentAddressed
{

/// The foundational create-if-absent compare-and-set (CAS) primitive for content-addressed pool
/// coordination. The object-store bucket is the single source of truth and this conditional create is
/// the ONLY consistency primitive we assume; everything in the safe-shared-pool work (write-session
/// pins, the GC-leader lock, fencing, the `_pool_meta` claim) is built on it.
///
/// Atomically create `key` carrying `bytes` iff it does not already exist. Returns true if THIS call
/// created the object; false if it already existed (the CAS was lost — another writer won). Throws on
/// any other error.
///
/// Backends:
///   - S3-like / Azure: the write is issued with `If-None-Match: *`, so the object store rejects a PUT
///     onto an existing key with `PreconditionFailed` (HTTP 412), surfaced as a conflict (returns
///     false). This field is honored ONLY by S3-like / Azure object storages.
///   - `LocalObjectStorage`: there is no conditional-PUT, so the create is made atomic with an
///     `O_CREAT | O_EXCL` open at the resolved local path (`EEXIST` -> conflict). This keeps the
///     primitive unit-testable without MinIO.
///
/// Fail closed: never silently fall back to a read-then-write (that races). If the backend cannot be
/// shown to support conditional create, `condCreateIfAbsent` throws `NOT_IMPLEMENTED` rather than
/// performing an unsafe non-atomic create.
bool condCreateIfAbsent(IObjectStorage & object_storage, const std::string & key, const std::string & bytes);

}
