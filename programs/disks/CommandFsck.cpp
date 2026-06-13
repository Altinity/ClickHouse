#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Common/Exception.h>
#include <ICommand.h>

#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class CommandFsck final : public ICommand
{
public:
    CommandFsck() : ICommand("CommandFsck")
    {
        command_name = "fsck";
        description = "Independently verify content-addressed pool reachability (read-only). "
                      "Exits nonzero if any reachable object is missing (dangling).";
        options_description.add_options()("detail", "list per-object rows (class, key, size, reachable_from)");
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const bool detail = options.contains("detail");
        auto disk = client.getCurrentDiskWithPath().getDisk();

        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        if (!dos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: '{}' is not an object-storage disk", disk->getName());

        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: disk '{}' is not content-addressed", disk->getName());

        if (!ca->isReadOnly())
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "fsck: open the CA disk read-only (<readonly>true</readonly>) so inspection never probes/schedules a live pool");

        const Cas::FsckReport report = Cas::runFsck(*ca->store(), detail);

        std::cout << "reachable=" << report.reachable << " dangling=" << report.dangling << " unreachable=" << report.unreachable
                  << " physical_bytes=" << report.physical_bytes << " referenced_logical_bytes=" << report.referenced_logical_bytes
                  << " distinct_blobs=" << report.distinct_blobs << " total_blob_refs=" << report.total_blob_refs
                  << " dedup_ratio=" << report.dedupRatio() << "\n";

        if (detail)
        {
            for (const auto & o : report.objects)
            {
                const char * c = o.cls == Cas::FsckClass::Reachable
                    ? "reachable"
                    : (o.cls == Cas::FsckClass::Dangling ? "dangling" : "unreachable");
                std::cout << c << "\t" << o.key << "\t" << o.size;
                for (const auto & r : o.reachable_from)
                    std::cout << "\t" << r;
                std::cout << "\n";
            }
        }

        if (report.dangling > 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS, "fsck: {} reachable object(s) MISSING (INV-NO-LOSS violation)", report.dangling);
    }
};

CommandPtr makeCommandFsck()
{
    return std::make_shared<DB::CommandFsck>();
}

}
