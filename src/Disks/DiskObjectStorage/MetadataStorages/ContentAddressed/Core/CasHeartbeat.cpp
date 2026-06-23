#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

#include <chrono>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint64_t HEARTBEAT_VERSION = 1;

}

String encodeHeartbeat(const Heartbeat & heartbeat)
{
    WriteBufferFromOwnString out;
    JsonObjectWriter writer(out);
    writer.field("format", "cas_heartbeat");
    writer.field("version", HEARTBEAT_VERSION);
    writer.field("server_id", u128ToHex(heartbeat.server_id));
    writer.field("heartbeat_seq", heartbeat.heartbeat_seq);
    writer.field("created_at_ms", heartbeat.created_at_ms);
    writer.finalize();
    return std::move(out.str());
}

Heartbeat decodeHeartbeat(std::string_view data)
{
    return decodeJsonGuarded("heartbeat", [&]
    {
        auto obj = parseJsonDocument(data, "cas_heartbeat", HEARTBEAT_VERSION, "heartbeat");
        checkNoUnknownKeys(*obj, {"format", "version", "server_id", "heartbeat_seq", "created_at_ms"}, "heartbeat");

        Heartbeat heartbeat;
        heartbeat.server_id = requireHash(*obj, "server_id", "heartbeat");
        heartbeat.heartbeat_seq = requireU64(*obj, "heartbeat_seq", "heartbeat");
        heartbeat.created_at_ms = requireU64(*obj, "created_at_ms", "heartbeat");
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
