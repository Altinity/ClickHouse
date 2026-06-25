#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_root_shard.pb.h>
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

String encodeServerWatermark(const ServerWatermark & w)
{
    Cas::Proto::WatermarkProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Watermark));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_id(u128ToBytesBE(w.server_id));
    msg.set_epoch(w.epoch);
    msg.set_min_active(w.min_active);   // UINT64_MAX encodes the retired sentinel directly
    msg.set_seq(w.seq);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: protobuf serialization failed");
    return out;
}

ServerWatermark decodeServerWatermark(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS watermark: empty object");

    /// Parse the whole message directly (pure protobuf, no binary prefix).
    Cas::Proto::WatermarkProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS watermark: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::Watermark))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS watermark: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::Watermark));
    checkCompatibility(msg.header().compatibility_version(), "watermark");

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
