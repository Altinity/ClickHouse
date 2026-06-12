#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{
constexpr uint64_t ROOTS_REGISTRY_VERSION = 1;
}

String encodeRootsRegistry(const RootsRegistry & registry)
{
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_roots_registry", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(ROOTS_REGISTRY_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "registry_version");
    writeIntText(registry.registry_version, out);
    writeChar(',', out);
    writeJsonKey(out, "fence_round");
    writeIntText(registry.fence_round, out);
    writeChar(',', out);
    writeJsonKey(out, "namespaces");
    writeChar('[', out);
    bool first = true;
    for (const String & ns : registry.namespaces)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeJsonString(ns, out);
    }
    writeChar(']', out);
    writeChar('}', out);
    return std::move(out.str());
}

RootsRegistry decodeRootsRegistry(std::string_view data)
{
    return decodeJsonGuarded("roots registry", [&]
    {
        auto obj = parseJsonDocument(data, "cas_roots_registry", ROOTS_REGISTRY_VERSION, "roots registry");
        checkNoUnknownKeys(*obj,
            {"format", "version", "registry_version", "fence_round", "namespaces"}, "roots registry");

        RootsRegistry registry;
        registry.registry_version = requireU64(*obj, "registry_version", "roots registry");
        registry.fence_round = requireU64(*obj, "fence_round", "roots registry");

        const auto namespaces = requireArray(*obj, "namespaces", "roots registry");
        for (size_t i = 0; i < namespaces->size(); ++i)
        {
            const auto var = namespaces->get(static_cast<unsigned int>(i));
            if (var.type() != typeid(String))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS roots registry: namespaces[{}] must be a string", i);
            String ns = var.extract<String>();
            if (ns.empty())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS roots registry: namespaces[{}] is empty", i);
            if (!registry.namespaces.insert(std::move(ns)).second)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS roots registry: duplicate namespace entry at index {}", i);
        }
        return registry;
    });
}

}
