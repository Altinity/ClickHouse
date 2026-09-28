#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <optional>
#include <variant>

namespace DB
{

struct ContentAddressedInlineFileCopySource
{
    String data;
};

struct ContentAddressedBlobFileCopySource
{
    StoredObject object;
    UInt64 payload_offset = 0;
    UInt64 payload_size = 0;
};

struct ContentAddressedPlainFileCopySource
{
    StoredObject object;
};

using ContentAddressedFileCopySource = std::variant<
    ContentAddressedInlineFileCopySource,
    ContentAddressedBlobFileCopySource,
    ContentAddressedPlainFileCopySource>;

struct ContentAddressedObjectWindow
{
    StoredObject object;
    UInt64 offset = 0;
};

std::optional<ContentAddressedObjectWindow> getContentAddressedObjectWindow(
    const ContentAddressedFileCopySource & source, const String & path);

}
