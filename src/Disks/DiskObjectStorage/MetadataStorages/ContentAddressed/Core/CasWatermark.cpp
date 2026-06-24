#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_root_shard.pb.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>

#include <limits>

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

namespace
{

constexpr std::string_view WATERMARK_MAGIC = "CAWM";

}

String encodeServerWatermark(const ServerWatermark & w)
{
    Cas::Proto::WatermarkProto msg;
    msg.set_server_id(u128ToBytesBE(w.server_id));
    msg.set_epoch(w.epoch);
    msg.set_min_active(w.min_active);   // UINT64_MAX encodes the retired sentinel directly
    msg.set_seq(w.seq);

    std::string body;
    if (!msg.SerializeToString(&body))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: protobuf serialization failed");

    WriteBufferFromOwnString out;
    Cas::writeFramingHeader(out, WATERMARK_MAGIC, Cas::currentWriterVersion(Cas::FormatId::Watermark));
    writeString(body, out);
    return std::move(out.str());
}

ServerWatermark decodeServerWatermark(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS watermark: empty object");

    ReadBufferFromMemory in(data.data(), data.size());
    Cas::readFramingHeader(in, WATERMARK_MAGIC, "watermark");
    const std::string_view body = data.substr(Cas::FRAMING_HEADER_SIZE);

    Cas::Proto::WatermarkProto msg;
    if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS watermark: protobuf parse failed");

    ServerWatermark w;
    w.server_id = u128FromBytesBE(msg.server_id(), "watermark server_id");
    w.epoch = msg.epoch();
    w.min_active = msg.min_active();   // UINT64_MAX is the retired sentinel
    w.seq = msg.seq();
    return w;
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
