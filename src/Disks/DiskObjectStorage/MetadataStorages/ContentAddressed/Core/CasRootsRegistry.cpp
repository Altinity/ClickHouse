#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_root_shard.pb.h>
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

String encodeRootsRegistry(const RootsRegistry & registry)
{
    Cas::Proto::RootsRegistryProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::RootsRegistry));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_registry_version(registry.registry_version);
    msg.set_fence_round(registry.fence_round);
    /// `registry.namespaces` is a std::set<String> => already sorted ascending.
    for (const String & ns : registry.namespaces)
        msg.add_namespaces(ns);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS roots registry: protobuf serialization failed");
    return out;
}

RootsRegistry decodeRootsRegistry(std::string_view data)
{
    return decodeGuarded("roots registry", [&]
    {
        if (data.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS roots registry: empty object");

        /// Parse the whole message directly (pure protobuf, no binary prefix).
        Cas::Proto::RootsRegistryProto msg;
        if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS roots registry: protobuf parse failed");

        /// Check magic then compatibility_version BEFORE reading any other fields.
        if (msg.header().magic() != magicFor(FormatId::RootsRegistry))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS roots registry: bad magic (got 0x{:08x}, expected 0x{:08x})",
                msg.header().magic(), magicFor(FormatId::RootsRegistry));
        checkCompatibility(msg.header().compatibility_version(), "roots registry");

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
