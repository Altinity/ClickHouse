#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_root_shard.pb.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{
constexpr std::string_view ROOTS_REGISTRY_MAGIC = "CARR";
}

String encodeRootsRegistry(const RootsRegistry & registry)
{
    Cas::Proto::RootsRegistryProto msg;
    msg.set_registry_version(registry.registry_version);
    msg.set_fence_round(registry.fence_round);
    /// `registry.namespaces` is a std::set<String> => already sorted ascending.
    for (const String & ns : registry.namespaces)
        msg.add_namespaces(ns);

    std::string body;
    if (!msg.SerializeToString(&body))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS roots registry: protobuf serialization failed");

    WriteBufferFromOwnString out;
    Cas::writeFramingHeader(out, ROOTS_REGISTRY_MAGIC, Cas::currentWriterVersion(Cas::FormatId::RootsRegistry));
    writeString(body, out);
    return std::move(out.str());
}

RootsRegistry decodeRootsRegistry(std::string_view data)
{
    return decodeGuarded("roots registry", [&]
    {
        if (data.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS roots registry: empty object");

        ReadBufferFromMemory in(data.data(), data.size());
        Cas::readFramingHeader(in, ROOTS_REGISTRY_MAGIC, "roots registry");
        const std::string_view body = data.substr(Cas::FRAMING_HEADER_SIZE);

        Cas::Proto::RootsRegistryProto msg;
        if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS roots registry: protobuf parse failed");

        RootsRegistry registry;
        registry.registry_version = msg.registry_version();
        registry.fence_round = msg.fence_round();

        /// Post-parse invariants (preserved from JSON era): each namespace is non-empty and unique.
        for (int i = 0; i < msg.namespaces_size(); ++i)
        {
            const String & ns = msg.namespaces(i);
            if (ns.empty())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS roots registry: namespaces[{}] is empty", i);
            if (!registry.namespaces.insert(ns).second)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS roots registry: duplicate namespace entry at index {}", i);
        }
        return registry;
    });
}

}
