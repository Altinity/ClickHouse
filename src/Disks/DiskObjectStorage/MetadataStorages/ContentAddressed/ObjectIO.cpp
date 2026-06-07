#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ObjectIO.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

namespace DB::ContentAddressed
{

WriteSettings caControlWriteSettings(WriteSettings base)
{
    /// Pass-through. This used to force `s3_allow_parallel_part_upload = false` to dodge the B90 crash
    /// (a use-after-free where a detached S3 upload task walked a freed borrowed-`ThreadGroup` parent
    /// tracker). That was a band-aid; the root cause is now fixed generically — a borrowed child
    /// `ThreadGroup` retains a `shared_ptr` to its parent (see `ThreadGroup::parent_thread_group`), so the
    /// parent's trackers cannot be freed while a task is attached. Parallel part upload is therefore safe
    /// for CA-internal writes again. The seam is kept in case CA-specific write tuning is wanted later.
    return base;
}

std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key)
{
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

std::optional<std::string> readSmallObjectIfExists(IObjectStorage & object_storage, const std::string & key)
{
    if (!object_storage.tryGetObjectMetadata(key, /*with_tags=*/false))
        return std::nullopt;

    StoredObject object(key);
    auto buf = object_storage.readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

}
