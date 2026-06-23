#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

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
    : backend(std::move(backend_))
    , key(layout_.buildHeartbeatKey(u128ToHex(build_id_)))
    , server_id(server_id_)
    , log(getLogger("CasHeartbeatKeeper"))
{
}

HeartbeatKeeper::~HeartbeatKeeper()
{
    /// Stop the renewal thread only — deliberately NO discard. Destruction without discard is
    /// the crash path: the heartbeat object persists, its seq stops advancing, and full GC
    /// eventually applies the debris rules (spec §5).
    stopBackground();
}

void HeartbeatKeeper::start()
{
    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: start after discard on key '{}'", key);
    if (seq != 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: already started on key '{}'", key);

    created_at_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    const String body = encodeHeartbeat({.server_id = server_id, .heartbeat_seq = 1, .created_at_ms = created_at_ms});
    Token token;
    if (backend->putIfAbsent(key, body, &token) != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: key '{}' already exists — the globally-unique build id collided, the world is broken", key);

    seq = 1;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void HeartbeatKeeper::renewOnce()
{
    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renew after discard on key '{}'", key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renew before start on key '{}'", key);

    const String body = encodeHeartbeat({.server_id = server_id, .heartbeat_seq = seq + 1, .created_at_ms = created_at_ms});
    Token token;
    if (backend->putOverwrite(key, body, last_token, &token) != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: key '{}' was touched by a foreign writer — failing closed, never re-minting", key);

    ++seq;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void HeartbeatKeeper::discard()
{
    /// Join the renewal thread before taking the state lock, so no renewal races the delete.
    stopBackground();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: double discard on key '{}'", key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: discard before start on key '{}'", key);

    const DeleteOutcome outcome = backend->deleteExact(key, last_token);
    /// Dead regardless of the outcome below: we attempted the delete, the keeper must never
    /// renew this key again.
    dead = true;

    if (outcome.kind == DeleteOutcome::Kind::TokenMismatch)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: discard of key '{}' hit a foreign incarnation — the world is broken", key);
    if (outcome.kind == DeleteOutcome::Kind::NotFound)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS heartbeat: discard of key '{}' found nothing, but we believed we held it", key);
}

void HeartbeatKeeper::startBackground(std::chrono::milliseconds period)
{
    /// After a thread-side renewal failure the loop returns (see backgroundLoop) but the thread
    /// handle stays joinable, so a subsequent startBackground throws "already running" until
    /// stopBackground is called. This is intentional fail-closed: the publish gate observes the
    /// frozen lastRenewTime and refuses to proceed — the gate, not the thread, is the safety
    /// mechanism (spec §5). We never silently re-arm renewal after it has failed.
    std::lock_guard lock(background_mutex);
    if (thread.joinable())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: background renewal is already running for key '{}'", key);
    stop_requested = false;
    thread = ThreadFromGlobalPool([this, period] { backgroundLoop(period); });
}

void HeartbeatKeeper::stopBackground()
{
    ThreadFromGlobalPool to_join;
    {
        std::lock_guard lock(background_mutex);
        if (!thread.joinable())
            return;
        stop_requested = true;
        wakeup.notify_all();
        to_join = std::move(thread);
    }
    to_join.join();
}

std::chrono::steady_clock::time_point HeartbeatKeeper::lastRenewTime() const
{
    std::lock_guard lock(state_mutex);
    return last_renew_time;
}

void HeartbeatKeeper::backgroundLoop(std::chrono::milliseconds period)
{
    /// The GATE, not this thread, is the safety mechanism (spec §5): a failed renewal is logged
    /// and stops the loop, so lastRenewTime stops advancing and the publish gate observes it
    /// through its local sanity bound. No retry, no re-mint — a wedged heartbeat only delays
    /// debris cleanup, never correctness.
    std::unique_lock lock(background_mutex);
    while (!stop_requested)
    {
        if (wakeup.wait_for(lock, period, [this] { return stop_requested; }))
            break;

        lock.unlock();
        try
        {
            renewOnce();
        }
        catch (...)
        {
            tryLogCurrentException(log, "CAS heartbeat: background renewal failed, the heartbeat stops advancing");
            return;
        }
        lock.lock();
    }
}

}
