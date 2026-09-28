#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

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
    UInt64 payload_offset;
    UInt64 payload_size;
};

struct ContentAddressedPlainFileCopySource
{
    StoredObject object;
};

using ContentAddressedFileCopySource = std::variant<
    ContentAddressedInlineFileCopySource,
    ContentAddressedBlobFileCopySource,
    ContentAddressedPlainFileCopySource>;

}
