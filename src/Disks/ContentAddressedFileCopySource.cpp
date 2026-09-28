#include <Disks/ContentAddressedFileCopySource.h>

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

std::optional<ContentAddressedObjectWindow> getContentAddressedObjectWindow(
    const ContentAddressedFileCopySource & source, const String & path)
{
    if (std::holds_alternative<ContentAddressedInlineFileCopySource>(source))
        return std::nullopt;

    ContentAddressedObjectWindow window;
    if (const auto * blob = std::get_if<ContentAddressedBlobFileCopySource>(&source))
    {
        if (blob->payload_offset == 0 || blob->payload_size != blob->object.bytes_size)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Invalid CAS blob copy source for {}: payload offset {}, payload size {}, object size {}",
                path,
                blob->payload_offset,
                blob->payload_size,
                blob->object.bytes_size);

        window = {blob->object, blob->payload_offset};
    }
    else if (const auto * plain = std::get_if<ContentAddressedPlainFileCopySource>(&source))
    {
        window = {plain->object, 0};
    }
    else
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unhandled CAS copy source for {}", path);
    }

    if (window.object.remote_path.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS copy source for {} has an empty object key", path);

    return window;
}

}
