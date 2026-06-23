#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.h>
#include <base/extended_types.h>
#include <base/types.h>
#include <cstdint>
#include <functional>
#include <string_view>

namespace DB::Cas
{

/// Per-server build watermark (spec 2026-06-16-ca-build-watermark): one object per server under
/// roots/<server-hex>/_watermark (Phase 6), renewed async ~2s off the write path, anchored synchronously
/// before the first object PUT. Strict JSON, fail-closed decode (mirrors CasHeartbeat encoding split).
///
/// Non-hashed metadata object => STRICT JSON ("cas_server_watermark" v1):
///   {"format":"cas_server_watermark","version":1,"server_id":"<32 lowercase hex>",
///    "epoch":7,"min_active":5,"seq":3}
/// min_active is a JSON number for live values (stays below 2^53, the JSON-number interop bound,
/// like every other CAS counter) OR the JSON string "retired" for the retired sentinel
/// (UINT64_MAX) written by farewell — the sentinel cannot ride as a number because the codec
/// parses integers as Int64.
/// Fail-closed decode (wrong format / unknown key / missing key / wrong type / bad hex /
/// malformed document => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).
struct ServerWatermark
{
    UInt128 server_id{};
    uint64_t epoch = 0;         /// random per process start; GC checks equality, never ordering
    uint64_t min_active = 0;    /// oldest in-flight build_seq; UINT64_MAX when retired (farewell)
    uint64_t seq = 0;           /// liveness counter, bumped each renewal (frozen-seq crash detection)
};

String encodeServerWatermark(const ServerWatermark & w);
ServerWatermark decodeServerWatermark(std::string_view data);

/// W-ANCHOR (spec 2026-06-16-ca-build-watermark): the per-server watermark is durable BEFORE the
/// first object PUT and renewed async in background. The slot is keyed by server_id ALONE, so it
/// can already exist from a prior process incarnation — start CLAIMS it (HEAD → putIfAbsent if
/// absent, else putOverwrite against the observed token) for the new epoch. There is a single
/// writer per server_id, so any precondition failure during renewal means a foreign touch — fail
/// closed with an exception, never re-mint. A `SingleWriterSlot` (shared with `HeartbeatKeeper`),
/// but it anchors a per-server slot instead of a per-build one, and farewell retires the epoch
/// (min_active = UINT64_MAX) rather than deleting the object.
class WatermarkKeeper final : public SingleWriterSlot
{
public:
    WatermarkKeeper(BackendPtr backend_, const Layout & layout_, UInt128 server_id_, uint64_t epoch_,
                    std::function<uint64_t()> min_active_fn_);

    /// Claims the per-server slot for our epoch with seq=1 — durable when start returns (W-ANCHOR).
    /// HEAD the key: if absent putIfAbsent, else putOverwrite against the observed current token (a
    /// fresh process legitimately overwrites its own server slot — single writer per server_id).
    void start() { doStart(); }

    /// Retires the epoch: putOverwrite against the last token with min_active = UINT64_MAX and
    /// seq+1, same epoch. Stops the background thread first. The keeper is done afterwards.
    /// NOTE: has no production caller (the `Store` dtor calls `stopBackground`, never `farewell`);
    /// preserved as an explicit policy method.
    void farewell() { doTerminate(); }

protected:
    RenewPayload prepareRenew() const override;
    String encodeBody(uint64_t seq_, const RenewPayload & payload) const override;
    Token claim(const String & body) override;
    void terminate() override;

private:
    UInt128 server_id;
    uint64_t epoch;
    std::function<uint64_t()> min_active_fn;
};

}
