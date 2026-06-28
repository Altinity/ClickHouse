#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_format.pb.h>
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

namespace Proto = ::clickhouse::cas::format;

String encodeOwner(const OwnerObject & o)
{
    Cas::Proto::OwnerProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Owner));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_uuid(u128ToBytesBE(o.server_uuid));

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS owner: protobuf serialization failed");
    return out;
}

OwnerObject decodeOwner(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: empty object");

    Cas::Proto::OwnerProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::Owner))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS owner: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::Owner));
    checkCompatibility(msg.header().compatibility_version(), "owner");

    OwnerObject o;
    o.server_uuid = u128FromBytesBE(msg.server_uuid(), "owner server_uuid");
    return o;
}

String encodeServerEpoch(const ServerEpoch & e)
{
    Cas::Proto::ServerEpochProto msg;

    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::ServerEpoch));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_next_writer_epoch(e.next_writer_epoch);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS server-epoch: protobuf serialization failed");
    return out;
}

ServerEpoch decodeServerEpoch(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: empty object");

    Cas::Proto::ServerEpochProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: protobuf parse failed");

    if (msg.header().magic() != magicFor(FormatId::ServerEpoch))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-epoch: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::ServerEpoch));
    checkCompatibility(msg.header().compatibility_version(), "server-epoch");

    ServerEpoch e;
    e.next_writer_epoch = msg.next_writer_epoch();
    return e;
}

String encodeMountLease(const MountLease & m)
{
    Cas::Proto::MountLeaseProto msg;

    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::MountLease));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_uuid(u128ToBytesBE(m.server_uuid));
    msg.set_writer_epoch(m.writer_epoch);
    msg.set_hostname(m.hostname);
    msg.set_pid(m.pid);
    msg.set_started_at_ms(m.started_at_ms);
    msg.set_seq(m.seq);
    msg.set_expires_at_ms(m.expires_at_ms);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount-lease: protobuf serialization failed");
    return out;
}

MountLease decodeMountLease(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: empty object");

    Cas::Proto::MountLeaseProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: protobuf parse failed");

    if (msg.header().magic() != magicFor(FormatId::MountLease))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS mount-lease: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::MountLease));
    checkCompatibility(msg.header().compatibility_version(), "mount-lease");

    MountLease m;
    m.server_uuid = u128FromBytesBE(msg.server_uuid(), "mount-lease server_uuid");
    m.writer_epoch = msg.writer_epoch();
    m.hostname = msg.hostname();
    m.pid = msg.pid();
    m.started_at_ms = msg.started_at_ms();
    m.seq = msg.seq();
    m.expires_at_ms = msg.expires_at_ms();
    return m;
}

}
