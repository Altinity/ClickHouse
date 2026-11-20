#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Common/Exception.h>
#include <ICommand.h>
#include <Utils.h>

#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
}

class CommandCaGcDryRun final : public ICommand
{
public:
    CommandCaGcDryRun() : ICommand("CommandCaGcDryRun")
    {
        command_name = "cas-gc-dryrun";
        exits_nonzero_on_error = true;
        description = "Preview the next GC round's deletes for a content-addressed pool (read-only, no deletes).";
    }

    void executeImpl(const CommandLineOptions &, DisksClient & client) override
    {
        auto disk = client.getCurrentDiskWithPath().getDisk();
        auto * ca = &contentAddressedStorageOf(*disk, "cas-gc-dryrun");

        if (!ca->isReadOnly())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "cas-gc-dryrun: open the CA disk read-only");

        /// A non-leader, read-only Gc handle: previewDeletes never acquires the lease or writes.
        Cas::Gc gc(ca->store(), UInt128(1));
        const auto preview = gc.previewDeletes();
        if (!preview.unread.empty())
            throw Exception(ErrorCodes::ABORTED, "cas-gc-dryrun: {}; run it again", preview.unread);

        std::cout << "preview_deletes=" << preview.deletes.size() << "\n";
        for (const auto & p : preview.deletes)
            std::cout << p.reason << "\t" << p.key << "\t" << p.size << "\n";
        if (preview.refused_shards.empty())
            return;
        std::cout << "refused_shards=" << preview.refused_shards.size() << "\n";
        for (const String & refusal : preview.refused_shards)
            std::cout << "refused\t" << refusal << "\n";
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "cas-gc-dryrun: a GC round refuses the runs of {} GC shard(s) as corrupt; their rows are not previewed",
            preview.refused_shards.size());
    }
};

CommandPtr makeCommandCaGcDryRun()
{
    return std::make_shared<DB::CommandCaGcDryRun>();
}

}
