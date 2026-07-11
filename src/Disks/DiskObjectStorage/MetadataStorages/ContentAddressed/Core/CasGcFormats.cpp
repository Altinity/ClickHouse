#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <algorithm>

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

namespace Proto = ::clickhouse::cas::format;

String encodeGcState(const GcState & state)
{
    chassert(state.gc_shards >= 1);   /// catch a zeroed GC constant at the write site

    Cas::Proto::GcStateProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::GcState));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_round(state.round);
    msg.set_fence_seq(state.fence_seq);
    msg.set_snap_shards(state.gc_shards);
    msg.set_snap_generation(state.snap_generation);
    msg.set_snap_pruned_through(state.snap_pruned_through);
    msg.set_snap_attempt(state.snap_attempt);
    msg.set_manifest_sweep_cursor(state.manifest_sweep_cursor);

    auto * lease = msg.mutable_lease();
    lease->set_owner(u128ToBytesBE(state.lease.owner));
    lease->set_seq(state.lease.seq);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS gc/state: protobuf serialization failed");
    return out;
}

GcState decodeGcState(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: empty object");

    /// Parse the whole message directly (pure protobuf, no binary prefix).
    Cas::Proto::GcStateProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::GcState))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc/state: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::GcState));
    checkCompatibility(msg.header().compatibility_version(), "gc/state");

    GcState state;
    state.round = msg.round();
    state.fence_seq = msg.fence_seq();
    state.gc_shards = msg.snap_shards();
    if (state.gc_shards == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: gc_shards must be >= 1");
    state.snap_generation = msg.snap_generation();
    state.snap_pruned_through = msg.snap_pruned_through();
    state.snap_attempt = msg.snap_attempt();
    state.manifest_sweep_cursor = msg.manifest_sweep_cursor();

    state.lease.owner = u128FromBytesBE(msg.lease().owner(), "gc/state lease owner");
    state.lease.seq = msg.lease().seq();

    return state;
}

String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    String out(24, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(hb.owner >> (8 * (15 - i))));
    for (int i = 0; i < 8; ++i)
        out[16 + i] = static_cast<char>(static_cast<UInt8>(hb.hb_seq >> (8 * (7 - i))));
    return out;
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    if (data.size() != 24)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: expected 24 bytes, got {}", data.size());
    GcHeartbeat hb;
    for (int i = 0; i < 16; ++i)
        hb.owner = (hb.owner << 8) | static_cast<UInt8>(data[i]);
    for (int i = 0; i < 8; ++i)
        hb.hb_seq = (hb.hb_seq << 8) | static_cast<UInt8>(static_cast<unsigned char>(data[16 + i]));
    return hb;
}

}
