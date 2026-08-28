#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <base/defines.h>

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

String encodeGcState(const GcState & state)
{
    if (state.gc_shards < 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "encodeGcState: gc_shards must be >= 1 -- refusing to persist an unreadable gc/state");
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::GcState);
    bool first = true;
    writeKey(out, "round", first);                   writeU64StringValue(out, state.round);
    writeKey(out, "gc_shards", first);               writeIntText(state.gc_shards, out);
    writeKey(out, "snapshot_generation", first);     writeU64StringValue(out, state.snap_generation);
    writeKey(out, "snapshot_pruned_through", first); writeU64StringValue(out, state.snap_pruned_through);
    writeKey(out, "snapshot_attempt", first);        writeU64StringValue(out, state.snap_attempt);
    writeKey(out, "manifest_sweep_cursor", first);   writeStringValue(out, state.manifest_sweep_cursor);
    writeKey(out, "lease_owner", first);             writeHex128Value(out, state.lease.owner);
    writeKey(out, "lease_sequence", first);          writeU64StringValue(out, state.lease.seq);
    closeObject(out, first);
    writeChar('\n', out);
    return std::move(out).take();
}

GcState decodeGcState(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcState);
    const String body = readLine(in, traitsFor(FormatId::GcState).line_cap, "gc/state");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "gc/state");

    GcState state;
    bool saw_gcs = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "round") state.round = r.readU64String();
        else if (key == "gc_shards") { state.gc_shards = r.readU64Number(); saw_gcs = true; }
        else if (key == "snapshot_generation") state.snap_generation = r.readU64String();
        else if (key == "snapshot_pruned_through") state.snap_pruned_through = r.readU64String();
        else if (key == "snapshot_attempt") state.snap_attempt = r.readU64String();
        else if (key == "manifest_sweep_cursor") state.manifest_sweep_cursor = r.readString();
        else if (key == "lease_owner") state.lease.owner = r.readHex128();
        else if (key == "lease_sequence") state.lease.seq = r.readU64String();
        else r.skipUnknown(key);
    }
    /// Fail closed on an absent `gc_shards`: the writer always emits it, so a missing key means a corrupt object.
    /// Do NOT silently keep the struct default (1) — that would hide corruption (no-fallback principle).
    if (!saw_gcs)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: missing gc_shards");
    if (state.gc_shards == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: gc_shards must be >= 1");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: trailing bytes");
    return state;
}

String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::GcHeartbeat);
    bool first = true;
    writeKey(out, "owner", first);              writeHex128Value(out, hb.owner);
    writeKey(out, "heartbeat_sequence", first); writeU64StringValue(out, hb.hb_seq);
    closeObject(out, first);
    writeChar('\n', out);
    return std::move(out).take();
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcHeartbeat);
    const String body = readLine(in, traitsFor(FormatId::GcHeartbeat).line_cap, "gc heartbeat");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "gc heartbeat");

    GcHeartbeat hb;
    bool saw_by = false;
    bool saw_seq = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "owner")
        {
            hb.owner = r.readHex128();
            saw_by = true;
        }
        else if (key == "heartbeat_sequence")
        {
            hb.hb_seq = r.readU64String();
            saw_seq = true;
        }
        else r.skipUnknown(key);
    }
    if (!saw_by || !saw_seq)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: missing identity field");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: trailing bytes");
    return hb;
}

}
