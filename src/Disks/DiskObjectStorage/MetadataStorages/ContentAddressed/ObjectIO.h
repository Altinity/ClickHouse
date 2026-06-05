#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>

#include <optional>
#include <string>

namespace DB::ContentAddressed
{

/// Read the entire content of a small object at `key` into a string.
/// Throws if the object does not exist (propagates the `readObject` exception).
std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key);

/// Read the entire content of a small object at `key` into a string.
/// Returns `std::nullopt` if the object does not exist (probed via `tryGetObjectMetadata`).
/// Only the missing-object check is swallowed; any other read error propagates.
std::optional<std::string> readSmallObjectIfExists(IObjectStorage & object_storage, const std::string & key);

}
