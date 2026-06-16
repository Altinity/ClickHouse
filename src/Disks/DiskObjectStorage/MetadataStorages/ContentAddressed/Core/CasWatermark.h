#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/extended_types.h>
#include <base/types.h>
#include <string_view>

namespace DB::Cas
{

/// Per-server build watermark (spec 2026-06-16-ca-build-watermark): one object per server under
/// servers/<server_id>, renewed async ~2s off the write path, anchored synchronously before the first
/// object PUT. Strict JSON, fail-closed decode (mirrors CasHeartbeat encoding split).
///
/// Non-hashed metadata object => STRICT JSON ("cas_server_watermark" v1):
///   {"format":"cas_server_watermark","version":1,"server_id":"<32 lowercase hex>",
///    "epoch":7,"min_active":5,"seq":3}
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

}
