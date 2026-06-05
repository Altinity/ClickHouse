#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ObjectIO.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

namespace DB::ContentAddressed
{

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
