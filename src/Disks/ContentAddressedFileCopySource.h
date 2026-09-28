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
    UInt64 payload_offset = 0;
    UInt64 payload_size = 0;
};

using ContentAddressedFileCopySource = std::variant<
    ContentAddressedInlineFileCopySource,
    ContentAddressedBlobFileCopySource>;

}
