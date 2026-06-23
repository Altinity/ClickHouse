#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

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
    : SingleWriterSlot(
        std::move(backend_), layout_.serverWatermarkKey(u128ToHex(server_id_)), "watermark", "farewell",
        "CasWatermarkKeeper")
    , server_id(server_id_)
    , epoch(epoch_)
    , min_active_fn(std::move(min_active_fn_))
{
}

SingleWriterSlot::RenewPayload WatermarkKeeper::prepareRenew() const
{
    /// Compute min_active off the state lock (the callback reaches into the Store's own lock).
    return {.value = min_active_fn()};
}

String WatermarkKeeper::encodeBody(uint64_t seq_, const RenewPayload & payload) const
{
    return encodeServerWatermark({.server_id = server_id, .epoch = epoch, .min_active = payload.value, .seq = seq_});
}

SingleWriterSlot::Token WatermarkKeeper::claim(const String & body)
{
    /// The per-server slot may already exist from a prior process incarnation — there is a single
    /// writer per server_id, so a fresh process legitimately claims its own slot. HEAD → putIfAbsent
    /// if absent, else putOverwrite against the observed current token. W-ANCHOR: durable on return.
    const HeadResult head = backend->head(key);
    if (!head.exists)
    {
        const PutResult res = backend->putIfAbsent(key, body);
        if (res.outcome != PutOutcome::Done)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS watermark: key '{}' appeared between head and putIfAbsent — concurrent writer on our own server slot", key);
        return res.token;
    }

    const PutResult res = backend->putOverwrite(key, body, head.token);
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS watermark: key '{}' was touched while claiming our own server slot — failing closed", key);
    return res.token;
}

void WatermarkKeeper::terminate()
{
    const String body = encodeServerWatermark(
        {.server_id = server_id, .epoch = epoch, .min_active = std::numeric_limits<uint64_t>::max(), .seq = seq + 1});
    const PutResult res = backend->putOverwrite(key, body, last_token);

    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS watermark: farewell of key '{}' hit a foreign incarnation — the world is broken", key);

    recordWrite(seq + 1, res.token);
}

}
