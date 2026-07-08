#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/Exception.h>
#include <ICommand.h>

#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

/// Read-only "decode any object" command: takes the RAW object-storage key (e.g. as printed by
/// `ca-gc-dryrun` or `fsck`) rather than a ClickHouse-relative path, GETs its bytes straight from
/// the pool's backend, and dispatches to `Cas::caInspectToJson` (the same free function the unit
/// tests exercise directly against encoder output). Never writes; safe to run against a live pool.
class CommandCaInspect final : public ICommand
{
public:
    CommandCaInspect() : ICommand("CommandCaInspect")
    {
        command_name = "ca-inspect";
        description = "Decode a content-addressed pool object (by its raw object-storage key) to JSON (read-only).";
        options_description.add_options()("key", po::value<String>(), "the raw object-storage key to decode (mandatory, positional)");
        positional_options_description.add("key", 1);
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const String key = getValueFromCommandLineOptionsThrow<String>(options, "key");

        auto disk = client.getCurrentDiskWithPath().getDisk();

        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        if (!dos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: '{}' is not an object-storage disk", disk->getName());

        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: disk '{}' is not content-addressed", disk->getName());

        if (!ca->isReadOnly())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: open the CA disk read-only");

        const auto got = ca->store()->backend().get(key);
        if (!got)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: key '{}' does not exist", key);

        std::cout << Cas::caInspectToJson(ca->store()->layout(), key, got->bytes) << "\n";
    }
};

CommandPtr makeCommandCaInspect()
{
    return std::make_shared<DB::CommandCaInspect>();
}

}
