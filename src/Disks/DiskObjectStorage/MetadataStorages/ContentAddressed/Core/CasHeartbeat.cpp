#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_root_shard.pb.h>
#include <Common/Exception.h>

#include <chrono>

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

String encodeHeartbeat(const Heartbeat & heartbeat)
{
    Cas::Proto::HeartbeatProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Heartbeat));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_id(u128ToBytesBE(heartbeat.server_id));
    msg.set_heartbeat_seq(heartbeat.heartbeat_seq);
    msg.set_created_at_ms(heartbeat.created_at_ms);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: protobuf serialization failed");
    return out;
}

Heartbeat decodeHeartbeat(std::string_view data)
{
    return decodeGuarded("heartbeat", [&]
    {
        if (data.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS heartbeat: empty object");

        /// Parse the whole message directly (pure protobuf, no binary prefix).
        Cas::Proto::HeartbeatProto msg;
        if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS heartbeat: protobuf parse failed");

        /// Check magic then compatibility_version BEFORE reading any other fields.
        if (msg.header().magic() != magicFor(FormatId::Heartbeat))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS heartbeat: bad magic (got 0x{:08x}, expected 0x{:08x})",
                msg.header().magic(), magicFor(FormatId::Heartbeat));
        checkCompatibility(msg.header().compatibility_version(), "heartbeat");

        Heartbeat heartbeat;
        heartbeat.server_id = u128FromBytesBE(msg.server_id(), "heartbeat server_id");
        heartbeat.heartbeat_seq = msg.heartbeat_seq();
        heartbeat.created_at_ms = msg.created_at_ms();
        return heartbeat;
    });
}

HeartbeatKeeper::HeartbeatKeeper(BackendPtr backend_, const Layout & layout_, UInt128 build_id_, UInt128 server_id_)
    : SingleWriterSlot(
        std::move(backend_), layout_.buildHeartbeatKey(u128ToHex(build_id_)), "heartbeat", "discard",
        "CasHeartbeatKeeper")
    , server_id(server_id_)
{
}

SingleWriterSlot::RenewPayload HeartbeatKeeper::prepareRenew() const
{
    /// The heartbeat carries no dynamic per-call value (created_at_ms is minted once in encodeBody
    /// and reused).
    return {};
}

String HeartbeatKeeper::encodeBody(uint64_t seq_, const RenewPayload & /*payload*/) const
{
    /// created_at_ms is minted under the state lock on the first encode (at start) and reused in
    /// renewals, so the object always records build creation time. DIAGNOSTIC ONLY (spec §3.1).
    if (created_at_ms == 0)
        created_at_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    return encodeHeartbeat({.server_id = server_id, .heartbeat_seq = seq_, .created_at_ms = created_at_ms});
}

SingleWriterSlot::Token HeartbeatKeeper::claim(const String & body)
{
    /// putIfAbsent only — the key already existing means the globally-unique build id collided.
    const PutResult res = backend->putIfAbsent(key, body);
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: key '{}' already exists — the globally-unique build id collided, the world is broken", key);
    return res.token;
}

void HeartbeatKeeper::terminate()
{
    const DeleteOutcome outcome = backend->deleteExact(key, last_token);

    if (outcome.kind == DeleteOutcome::Kind::TokenMismatch)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: discard of key '{}' hit a foreign incarnation — the world is broken", key);
    if (outcome.kind == DeleteOutcome::Kind::NotFound)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: discard of key '{}' found nothing, but we believed we held it", key);
}

}
