#include <Storages/System/StorageSystemContentAddressedMounts.h>
#include <DataTypes/DataTypesNumber.h>

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeString.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/Pipe.h>
#include <Interpreters/Context.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <base/hex.h>

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
        {"server_uuid", std::make_shared<DataTypeString>(), "UUID of the server incarnation holding the lease (hex)."},
        {"hostname", std::make_shared<DataTypeString>(), "Hostname recorded in the lease body."},
        {"pid", std::make_shared<DataTypeUInt64>(), "Process id recorded in the lease body."},
        {"writer_epoch", std::make_shared<DataTypeUInt64>(), "Fenced writer epoch of the incarnation."},
        {"seq", std::make_shared<DataTypeUInt64>(), "Lease renewal sequence number."},
        {"started_at_ms", std::make_shared<DataTypeUInt64>(), "Lease start, unix ms."},
        {"expires_at_ms", std::make_shared<DataTypeUInt64>(), "Lease expiry, unix ms."},
        {"min_active", std::make_shared<DataTypeUInt64>(), "Oldest in-flight build sequence (UINT64_MAX = farewell)."},
        {"observed_gc_round", std::make_shared<DataTypeUInt64>(), "Newest GC round this server has acked."},
        {"gc_fenced", std::make_shared<DataTypeUInt8>(), "1 if GC fenced this slot out (terminal)."},
        {"state", std::make_shared<DataTypeString>(), "live | expired | terminated | fenced | corrupt."},
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
    MutableColumnPtr col_uuid = ColumnString::create();
    MutableColumnPtr col_host = ColumnString::create();
    MutableColumnPtr col_pid = ColumnUInt64::create();
    MutableColumnPtr col_epoch = ColumnUInt64::create();
    MutableColumnPtr col_seq = ColumnUInt64::create();
    MutableColumnPtr col_started = ColumnUInt64::create();
    MutableColumnPtr col_expires = ColumnUInt64::create();
    MutableColumnPtr col_min_active = ColumnUInt64::create();
    MutableColumnPtr col_round = ColumnUInt64::create();
    MutableColumnPtr col_fenced = ColumnUInt8::create();
    MutableColumnPtr col_state = ColumnString::create();

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

        Cas::StorePtr store;
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
        for (const auto & m : mounts)
        {
            col_disk->insert(disk_name);
            col_srid->insert(m.srid);
            col_uuid->insert(getHexUIntLowercase(m.lease.server_uuid));
            col_host->insert(m.lease.hostname);
            col_pid->insert(m.lease.pid);
            col_epoch->insert(m.lease.writer_epoch);
            col_seq->insert(m.lease.seq);
            col_started->insert(m.lease.started_at_ms);
            col_expires->insert(m.lease.expires_at_ms);
            col_min_active->insert(m.lease.min_active);
            col_round->insert(m.lease.observed_gc_round);
            col_fenced->insert(static_cast<UInt8>(m.lease.gc_fenced));
            col_state->insert(m.state);
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
    res_columns.emplace_back(std::move(col_round));
    res_columns.emplace_back(std::move(col_fenced));
    res_columns.emplace_back(std::move(col_state));

    UInt64 num_rows = res_columns.at(0)->size();
    Chunk chunk(std::move(res_columns), num_rows);

    return Pipe(std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(storage_snapshot->metadata->getSampleBlock()), std::move(chunk)));
}

}
