#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasMigratePool.h>
#include <Common/Exception.h>
#include <ICommand.h>
#include <Utils.h>

#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

/// The one-time offline conversion of a content-addressed pool to blob deletion intents, generation 4.
class CommandCaMigratePool final : public ICommand
{
public:
    CommandCaMigratePool() : ICommand("CommandCaMigratePool")
    {
        command_name = "cas-migrate-pool";
        exits_nonzero_on_error = true;
        description = "Convert a content-addressed pool to generation 4 (blob deletion intents). Stop every server and tool of "
                      "the pool first; once the tool prints that servers may start, the rest (removing the legacy markers) "
                      "needs no downtime. Run it alone in a non-interactive query: clickhouse-disks --config-file <config> "
                      "--disk <disk> --query 'cas-migrate-pool'.";
        options_description.add_options()(
            "grace-seconds", po::value<UInt64>(), "how long no mount slot and no GC lease may move before the conversion (default 120)")(
            "trust-clean-markers", "give an intent only to a body a marker or a GC row condemns, not to every body; only for a "
                                   "pool known never to have run a GC of 26.6 or a rebuild of gc/state; ignored below generation 3")(
            "keep-markers", "stop once the pool is migrated and leave the legacy markers; a rerun, or `cas-fsck --fix "
                            "legacy-markers`, removes them later")(
            "concurrency", po::value<UInt64>(), "requests in flight while markers are read, intents created and markers removed "
                                                "(default: the disk's `cas_gc_io_concurrency`)")(
            "dry-run", "read what the conversion would read, check the read rights on blob-intents/, and write nothing");
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        auto disk = client.getCurrentDiskWithPath().getDisk();
        auto * ca = &contentAddressedStorageOf(*disk, "cas-migrate-pool");
        /// The deferred open keeps the disk from mounting the pool, so the disk need not be read-only: a read-only
        /// local object storage would refuse the tool's own writes.
        if (!deferPoolOpenForMigration())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "cas-migrate-pool: run it as `clickhouse-disks --query 'cas-migrate-pool ...'`, so the disk does not open the pool "
                "first");

        Cas::MigratePoolOptions migrate;
        migrate.grace_ms = Cas::migrateGraceMs(getValueFromCommandLineOptionsWithDefault<UInt64>(options, "grace-seconds", 120));
        migrate.trust_clean_markers = options.contains("trust-clean-markers");
        migrate.keep_markers = options.contains("keep-markers");
        migrate.concurrency = getValueFromCommandLineOptionsWithDefault<UInt64>(options, "concurrency", 0);
        migrate.dry_run = options.contains("dry-run");

        const Cas::PoolPtr pool = ca->openPoolForMigration();
        const Cas::MigratePoolReport r = Cas::migratePool(*pool, migrate, [](std::string_view line) { std::cout << line << std::endl; });

        std::cout << "floor_read=" << r.floor_read << " probes=" << r.probes_run << " slots=" << r.slots_read
                  << " condemned_rows=" << r.condemned_rows << " markers=" << r.markers_read << " bodies=" << r.bodies_listed
                  << " intents=" << r.intents_confirmed << " control_writes=" << r.control_objects_written
                  << " slots_reread=" << r.slots_reread << " floor_raised=" << r.floor_raised
                  << " markers_removed=" << r.markers_removed << " markers_changed=" << r.markers_changed << "\n";
        if (r.completed)
            std::cout << "migration complete" << std::endl;
        else if (r.migrated)
            std::cout << "the pool is migrated; markers are left for a rerun or `cas-fsck --fix legacy-markers`" << std::endl;
    }
};

CommandPtr makeCommandCaMigratePool()
{
    return std::make_shared<DB::CommandCaMigratePool>();
}

}
