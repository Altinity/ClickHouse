#include <Storages/System/StorageSystemContentAddressedMounts.h>
#include <DataTypes/DataTypesNumber.h>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnsDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeUUID.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/Pipe.h>
#include <Interpreters/Context.h>
#include <Common/assert_cast.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <chrono>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

StorageSystemContentAddressedMounts::StorageSystemContentAddressedMounts(const StorageID & table_id_)
    : StorageWithCommonVirtualColumns(table_id_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(ColumnsDescription(
    {
        {"disk", std::make_shared<DataTypeString>(), "Name of the content-addressed disk."},
        {"srid", std::make_shared<DataTypeString>(), "Server root id owning the mount slot."},
        {"server_uuid", std::make_shared<DataTypeUUID>(), "UUID of the server incarnation holding the lease."},
        {"hostname", std::make_shared<DataTypeString>(), "Hostname recorded in the lease body."},
        {"pid", std::make_shared<DataTypeUInt64>(), "Process id recorded in the lease body."},
        {"writer_epoch", std::make_shared<DataTypeUInt64>(), "Fenced writer epoch of the incarnation."},
        {"seq", std::make_shared<DataTypeUInt64>(), "Lease renewal sequence number."},
        {"started_at_ms", std::make_shared<DataTypeDateTime64>(3), "Lease start."},
        {"expires_at_ms", std::make_shared<DataTypeDateTime64>(3), "Lease expiry."},
        {"min_active", std::make_shared<DataTypeUInt64>(), "Oldest in-flight build sequence (UINT64_MAX = farewell)."},
        {"gc_fenced", std::make_shared<DataTypeUInt8>(), "1 if GC fenced this slot out (terminal)."},
        {"state", std::make_shared<DataTypeString>(), "live | expired | terminated | fenced | corrupt."},
        {"is_leader", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt8>()), "1 if THIS server's GC scheduler currently holds this disk's leadership lease (per-disk; supersedes the retired process-global CasGcIsLeader metric). NULL on rows describing OTHER servers' mounts; populated only on this server's own mount row."},
        {"pending_reclaim", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeInt64>()), "Cumulative two-phase deletion backlog observed by this process's GC on this disk (condemned entries minus executed exact-token deletes). NULL on rows describing OTHER servers' mounts; populated only on this server's own mount row."},
        {"last_success_age_seconds", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt64>()), "Seconds since this disk's GC last led a round (0 if it has never led or GC is not running here). NULL on rows describing OTHER servers' mounts; populated only on this server's own mount row."},
        {"wedged_namespace_count", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt64>()), "Ref-append lanes currently wedged on this disk (an uncertain PUT exhausted its retry budget). NULL on rows describing OTHER servers' mounts; populated only on this server's own mount row."},
    }));
    storage_metadata.setVirtuals(createVirtuals());
    setInMemoryMetadata(storage_metadata);
}

VirtualColumnsDescription StorageSystemContentAddressedMounts::createVirtuals()
{
    VirtualColumnsDescription desc;
    desc.addEphemeral("_table", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "", VirtualsMaterializationPlace::Plan);
    desc.addEphemeral("_database", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "", VirtualsMaterializationPlace::Plan);
    return desc;
}

Pipe StorageSystemContentAddressedMounts::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr context,
    QueryProcessingStage::Enum /*processed_stage*/,
    const size_t /*max_block_size*/,
    const size_t /*num_streams*/)
{
    storage_snapshot->check(column_names);

    MutableColumnPtr col_disk = ColumnString::create();
    MutableColumnPtr col_srid = ColumnString::create();
    MutableColumnPtr col_uuid = ColumnUUID::create();
    MutableColumnPtr col_host = ColumnString::create();
    MutableColumnPtr col_pid = ColumnUInt64::create();
    MutableColumnPtr col_epoch = ColumnUInt64::create();
    MutableColumnPtr col_seq = ColumnUInt64::create();
    MutableColumnPtr col_started = ColumnDateTime64::create(0, 3);
    MutableColumnPtr col_expires = ColumnDateTime64::create(0, 3);
    MutableColumnPtr col_min_active = ColumnUInt64::create();
    MutableColumnPtr col_fenced = ColumnUInt8::create();
    MutableColumnPtr col_state = ColumnString::create();
    MutableColumnPtr col_is_leader = ColumnNullable::create(ColumnUInt8::create(), ColumnUInt8::create());
    MutableColumnPtr col_pending = ColumnNullable::create(ColumnInt64::create(), ColumnUInt8::create());
    MutableColumnPtr col_last_success = ColumnNullable::create(ColumnUInt64::create(), ColumnUInt8::create());
    MutableColumnPtr col_wedged = ColumnNullable::create(ColumnUInt64::create(), ColumnUInt8::create());

    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    /// A disk's metadata storage is content-addressed iff getMetadataStorage() yields a CA storage.
    /// Plain (non-object-storage) disks throw NOT_IMPLEMENTED, which simply means "not
    /// content-addressed" here (mirrors InterpreterSystemQuery::runContentAddressedGarbageCollection).
    for (const auto & [disk_name, disk] : context->getDisksMap())
    {
        MetadataStoragePtr md;
        try
        {
            md = disk->getMetadataStorage();
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::NOT_IMPLEMENTED)
                continue;
            throw;
        }
        if (!md || !md->isContentAddressed())
            continue;
        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(md.get());
        if (!ca)
            continue;

        Cas::PoolPtr store;
        try
        {
            store = ca->store();
        }
        catch (...)
        {
            continue;   /// disk not started yet — no rows, not an error
        }

        const uint64_t skew_margin_ms = static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count()) / 2;
        std::vector<Cas::MountInfo> mounts;
        try
        {
            mounts = Cas::listMounts(store->backend(), store->layout(), now_ms, skew_margin_ms);
        }
        catch (...)
        {
            /// This table exists for incident-time diagnosis: one disk's transient backend error
            /// must not blind the operator to the other disks' rows. Skip the disk, keep the rest.
            tryLogCurrentException(getLogger("StorageSystemContentAddressedMounts"),
                                   "listing mounts for disk '" + disk_name + "'");
            continue;
        }
        const auto health = ca->gcHealth();
        const String & local_srid = store->poolConfig().server_root_id;
        for (const auto & m : mounts)
        {
            col_disk->insert(disk_name);
            col_srid->insert(m.srid);
            assert_cast<ColumnUUID &>(*col_uuid).insertValue(UUID(m.lease.server_uuid));
            col_host->insert(m.lease.hostname);
            col_pid->insert(m.lease.pid);
            col_epoch->insert(m.lease.writer_epoch);
            col_seq->insert(m.lease.seq);
            assert_cast<ColumnDateTime64 &>(*col_started).insertValue(static_cast<Decimal64>(m.lease.started_at_ms));
            assert_cast<ColumnDateTime64 &>(*col_expires).insertValue(static_cast<Decimal64>(m.lease.expires_at_ms));
            col_min_active->insert(m.lease.min_active);
            col_fenced->insert(static_cast<UInt8>(m.lease.gc_fenced));
            col_state->insert(m.state);

            /// GC health is a process-local fact about THIS server's scheduler. Stamping it onto
            /// peer rows misreads as "peer B is GC leader" during incidents — NULL there instead.
            const bool is_local_row = (m.srid == local_srid);
            if (is_local_row && health)
            {
                col_is_leader->insert(static_cast<UInt8>(health->is_leader));
                col_pending->insert(health->pending_reclaim);
                col_last_success->insert(health->last_success_age_seconds);
                col_wedged->insert(health->wedged_namespace_count);
            }
            else
            {
                col_is_leader->insertDefault();
                col_pending->insertDefault();
                col_last_success->insertDefault();
                col_wedged->insertDefault();
            }
        }
    }

    Columns res_columns;
    res_columns.emplace_back(std::move(col_disk));
    res_columns.emplace_back(std::move(col_srid));
    res_columns.emplace_back(std::move(col_uuid));
    res_columns.emplace_back(std::move(col_host));
    res_columns.emplace_back(std::move(col_pid));
    res_columns.emplace_back(std::move(col_epoch));
    res_columns.emplace_back(std::move(col_seq));
    res_columns.emplace_back(std::move(col_started));
    res_columns.emplace_back(std::move(col_expires));
    res_columns.emplace_back(std::move(col_min_active));
    res_columns.emplace_back(std::move(col_fenced));
    res_columns.emplace_back(std::move(col_state));
    res_columns.emplace_back(std::move(col_is_leader));
    res_columns.emplace_back(std::move(col_pending));
    res_columns.emplace_back(std::move(col_last_success));
    res_columns.emplace_back(std::move(col_wedged));

    UInt64 num_rows = res_columns.at(0)->size();
    Chunk chunk(std::move(res_columns), num_rows);

    return Pipe(std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(storage_snapshot->metadata->getSampleBlock()), std::move(chunk)));
}

}
