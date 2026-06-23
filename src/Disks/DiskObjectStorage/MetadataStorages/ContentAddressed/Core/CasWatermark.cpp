#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <limits>

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

constexpr uint64_t WATERMARK_VERSION = 1;

/// min_active = UINT64_MAX (written by farewell) is the retired sentinel. It cannot ride as a JSON
/// number — the codec parses integers as Int64 — so it is encoded as this string instead.
constexpr std::string_view RETIRED_SENTINEL = "retired";

}

String encodeServerWatermark(const ServerWatermark & w)
{
    WriteBufferFromOwnString out;
    JsonObjectWriter writer(out);
    writer.field("format", "cas_server_watermark");
    writer.field("version", WATERMARK_VERSION);
    writer.field("server_id", u128ToHex(w.server_id));
    writer.field("epoch", w.epoch);
    /// min_active is conditional: the retired sentinel rides as a string, a live value as a number.
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        writer.field("min_active", RETIRED_SENTINEL);
    else
        writer.field("min_active", w.min_active);
    writer.field("seq", w.seq);
    writer.finalize();
    return std::move(out.str());
}

ServerWatermark decodeServerWatermark(std::string_view data)
{
    return decodeJsonGuarded("watermark", [&]
    {
        auto obj = parseJsonDocument(data, "cas_server_watermark", WATERMARK_VERSION, "watermark");
        checkNoUnknownKeys(*obj, {"format", "version", "server_id", "epoch", "min_active", "seq"}, "watermark");

        ServerWatermark w;
        w.server_id = requireHash(*obj, "server_id", "watermark");
        w.epoch = requireU64(*obj, "epoch", "watermark");

        /// min_active is the retired sentinel (UINT64_MAX) when it is the "retired" string, else a
        /// live JSON number; any other string is corruption.
        const auto min_active_var = requireKey(*obj, "min_active", "watermark");
        if (min_active_var.type() == typeid(String))
        {
            if (min_active_var.extract<String>() != RETIRED_SENTINEL)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS watermark: key 'min_active' string must be '{}'", RETIRED_SENTINEL);
            w.min_active = std::numeric_limits<uint64_t>::max();
        }
        else
        {
            w.min_active = requireU64Var(min_active_var, "min_active", "watermark");
        }

        w.seq = requireU64(*obj, "seq", "watermark");
        return w;
    });
}

WatermarkKeeper::WatermarkKeeper(BackendPtr backend_, const Layout & layout_, UInt128 server_id_, uint64_t epoch_,
                                 std::function<uint64_t()> min_active_fn_)
    : backend(std::move(backend_))
    , key(layout_.serverWatermarkKey(u128ToHex(server_id_)))
    , server_id(server_id_)
    , epoch(epoch_)
    , min_active_fn(std::move(min_active_fn_))
    , log(getLogger("CasWatermarkKeeper"))
{
}

WatermarkKeeper::~WatermarkKeeper()
{
    /// Stop the renewal thread only — deliberately NO farewell. Destruction without farewell is the
    /// crash path: the watermark object persists, its seq stops advancing, and full GC observes the
    /// frozen seq (spec 2026-06-16-ca-build-watermark).
    stopBackground();
}

void WatermarkKeeper::start()
{
    /// Compute min_active BEFORE taking state_mutex: the callback reaches into the Store's own lock,
    /// so we never hold the keeper's state_mutex across it.
    const uint64_t min_active = min_active_fn();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: start after farewell on key '{}'", key);
    if (seq != 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: already started on key '{}'", key);

    const String body = encodeServerWatermark({.server_id = server_id, .epoch = epoch, .min_active = min_active, .seq = 1});

    /// The per-server slot may already exist from a prior process incarnation — there is a single
    /// writer per server_id, so a fresh process legitimately claims its own slot. HEAD → putIfAbsent
    /// if absent, else putOverwrite against the observed current token. W-ANCHOR: durable on return.
    const HeadResult head = backend->head(key);
    Token token;
    if (!head.exists)
    {
        if (backend->putIfAbsent(key, body, &token) != PutOutcome::Done)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS watermark: key '{}' appeared between head and putIfAbsent — concurrent writer on our own server slot", key);
    }
    else
    {
        if (backend->putOverwrite(key, body, head.token, &token) != PutOutcome::Done)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS watermark: key '{}' was touched while claiming our own server slot — failing closed", key);
    }

    seq = 1;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void WatermarkKeeper::renewOnce()
{
    /// Compute min_active BEFORE taking state_mutex (see start): never hold state_mutex across the
    /// Store callback.
    const uint64_t min_active = min_active_fn();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: renew after farewell on key '{}'", key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: renew before start on key '{}'", key);

    const String body = encodeServerWatermark({.server_id = server_id, .epoch = epoch, .min_active = min_active, .seq = seq + 1});
    Token token;
    if (backend->putOverwrite(key, body, last_token, &token) != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS watermark: key '{}' was touched by a foreign writer — failing closed, never re-minting", key);

    ++seq;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void WatermarkKeeper::farewell()
{
    /// Join the renewal thread before taking the state lock, so no renewal races the retirement.
    stopBackground();

    std::lock_guard lock(state_mutex);
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: double farewell on key '{}'", key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: farewell before start on key '{}'", key);

    const String body = encodeServerWatermark(
        {.server_id = server_id, .epoch = epoch, .min_active = std::numeric_limits<uint64_t>::max(), .seq = seq + 1});
    Token token;
    const PutOutcome outcome = backend->putOverwrite(key, body, last_token, &token);
    /// Dead regardless of the outcome below: we attempted the retirement, the keeper must never
    /// renew this key again.
    dead = true;

    if (outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS watermark: farewell of key '{}' hit a foreign incarnation — the world is broken", key);

    ++seq;
    last_token = token;
    last_renew_time = std::chrono::steady_clock::now();
}

void WatermarkKeeper::startBackground(std::chrono::milliseconds period)
{
    /// After a thread-side renewal failure the loop returns (see backgroundLoop) but the thread
    /// handle stays joinable, so a subsequent startBackground throws "already running" until
    /// stopBackground is called. Intentional fail-closed: we never silently re-arm renewal after it
    /// has failed.
    std::lock_guard lock(background_mutex);
    if (thread.joinable())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: background renewal is already running for key '{}'", key);
    stop_requested = false;
    thread = ThreadFromGlobalPool([this, period] { backgroundLoop(period); });
}

void WatermarkKeeper::stopBackground()
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

std::chrono::steady_clock::time_point WatermarkKeeper::lastRenewTime() const
{
    std::lock_guard lock(state_mutex);
    return last_renew_time;
}

void WatermarkKeeper::backgroundLoop(std::chrono::milliseconds period)
{
    /// A failed renewal is logged and stops the loop, so lastRenewTime (and the watermark seq) stop
    /// advancing and GC observes the frozen seq. No retry, no re-mint.
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
            tryLogCurrentException(log, "CAS watermark: background renewal failed, the watermark stops advancing");
            return;
        }
        lock.lock();
    }
}

}
