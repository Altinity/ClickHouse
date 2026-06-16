#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

namespace DB::Cas
{

namespace
{

constexpr uint64_t WATERMARK_VERSION = 1;

}

String encodeServerWatermark(const ServerWatermark & w)
{
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_server_watermark", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(WATERMARK_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "server_id");
    writeJsonString(u128ToHex(w.server_id), out);
    writeChar(',', out);
    writeJsonKey(out, "epoch");
    writeIntText(w.epoch, out);
    writeChar(',', out);
    writeJsonKey(out, "min_active");
    writeIntText(w.min_active, out);
    writeChar(',', out);
    writeJsonKey(out, "seq");
    writeIntText(w.seq, out);
    writeChar('}', out);
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
        w.min_active = requireU64(*obj, "min_active", "watermark");
        w.seq = requireU64(*obj, "seq", "watermark");
        return w;
    });
}

}
