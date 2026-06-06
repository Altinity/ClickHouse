#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <IO/WriteSettings.h>

#include <optional>
#include <string>

namespace DB::ContentAddressed
{

/// WriteSettings for CA-INTERNAL object-storage writes (control objects, blobs, refs, manifests, ...).
///
/// Forces `s3_allow_parallel_part_upload = false` so `S3ObjectStorage::writeObject` runs the upload
/// INLINE on the calling thread (`TaskTracker` uses `syncRunner` when its scheduler is empty) instead of
/// dispatching it to a pooled async-upload worker.
///
/// Rationale (B88): `ThreadGroupSwitcher` no-ops when the submitting thread has no `ThreadGroup`, so a
/// pooled async-upload worker can keep a stale `current_performance_counters` pointer from a prior job and
/// dereference it on the next `ProfileEvents::increment` (a use-after-free SIGSEGV). CA floods S3 with tiny
/// control-object PUTs from background/GC/commit contexts that often lack a `ThreadGroup`, which exposes
/// the bug. Running the upload inline on the calling thread keeps a valid ProfileEvents context. For CA's
/// tiny control objects this has zero throughput cost; for large blob uploads it loses parallel multipart
/// throughput, which is an accepted tradeoff until the generic `ThreadGroupSwitcher` fix lands (B88).
///
/// Pass an existing `base` to preserve other fields (e.g. `object_storage_write_if_none_match` for the
/// conditional create-if-absent path); the only field this overrides is `s3_allow_parallel_part_upload`.
WriteSettings caControlWriteSettings(WriteSettings base = {});

/// Read the entire content of a small object at `key` into a string.
/// Throws if the object does not exist (propagates the `readObject` exception).
std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key);

/// Read the entire content of a small object at `key` into a string.
/// Returns `std::nullopt` if the object does not exist (probed via `tryGetObjectMetadata`).
/// Only the missing-object check is swallowed; any other read error propagates.
std::optional<std::string> readSmallObjectIfExists(IObjectStorage & object_storage, const std::string & key);

}
