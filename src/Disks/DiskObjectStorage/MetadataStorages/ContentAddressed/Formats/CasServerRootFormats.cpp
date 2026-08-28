#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

/// Read exactly the one JSON body line allowed by a server-root control object. `readLine` rejects a
/// missing newline and a line over the format-specific cap; each decoder separately checks that no
/// bytes follow this line, so a concatenated object cannot be accepted accidentally.
String readBodyLine(ReadBuffer & in, FormatId id, std::string_view what)
{
    return readLine(in, traitsFor(id).line_cap, what);
}

}

String encodeOwner(const OwnerObject & o)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::Owner);
    bool first = true;
    writeKey(out, "server_uuid", first);
    writeHex128Value(out, o.server_uuid);
    if (o.retired_at_ms)
    {
        writeKey(out, "retired_at_ms", first);
        writeIntText(*o.retired_at_ms, out);
    }
    closeObject(out, first);
    writeChar('\n', out);
    return std::move(out).take();
}

OwnerObject decodeOwner(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::Owner);
    const String body = readBodyLine(in, FormatId::Owner, "owner");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "owner");

    OwnerObject o;
    bool saw = false;
    std::optional<uint64_t> rt;
    String key;
    while (r.nextKey(key))
    {
        if (key == "server_uuid")
        {
            o.server_uuid = r.readHex128();
            saw = true;
        }
        else if (key == "retired_at_ms")
            rt = r.readU64Number();
        else
            r.skipUnknown(key);
    }
    o.retired_at_ms = rt;
    if (!saw)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: missing su");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: trailing bytes");
    return o;
}

String encodeServerEpoch(const ServerEpoch & e)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::ServerEpoch);
    bool first = true;
    writeKey(out, "next_writer_epoch", first);
    writeU64StringValue(out, e.next_writer_epoch);
    closeObject(out, first);
    writeChar('\n', out);
    return std::move(out).take();
}

ServerEpoch decodeServerEpoch(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::ServerEpoch);
    const String body = readBodyLine(in, FormatId::ServerEpoch, "server-epoch");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "server-epoch");

    ServerEpoch e;
    bool saw = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "next_writer_epoch")
        {
            e.next_writer_epoch = r.readU64String();
            saw = true;
        }
        else
            r.skipUnknown(key);
    }
    if (!saw)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: missing nwe");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: trailing bytes");
    return e;
}

String encodeMountLease(const MountLease & m)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::MountLease);
    bool first = true;
    writeKey(out, "server_uuid", first);    writeHex128Value(out, m.server_uuid);
    writeKey(out, "writer_epoch", first);   writeU64StringValue(out, m.writer_epoch);
    writeKey(out, "hostname", first);       writeStringValue(out, m.hostname);
    writeKey(out, "process_id", first);     writeIntText(m.pid, out);
    writeKey(out, "started_at_ms", first);  writeIntText(m.started_at_ms, out);
    writeKey(out, "sequence", first);       writeU64StringValue(out, m.seq);
    writeKey(out, "expires_at_ms", first);  writeIntText(m.expires_at_ms, out);
    writeKey(out, "min_active", first);     writeU64StringValue(out, m.min_active);
    writeKey(out, "gc_fenced", first);      writeBoolValue(out, m.gc_fenced);
    writeKey(out, "write_attempt_id", first); writeHex128Value(out, m.write_attempt_id);
    closeObject(out, first);
    writeChar('\n', out);
    return std::move(out).take();
}

MountLease decodeMountLease(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::MountLease);
    const String body = readBodyLine(in, FormatId::MountLease, "mount-lease");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "mount-lease");

    MountLease m;
    bool saw_su = false;
    bool saw_we = false;
    bool saw_write_attempt_id = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "server_uuid")
        {
            m.server_uuid = r.readHex128();
            saw_su = true;
        }
        else if (key == "writer_epoch")
        {
            m.writer_epoch = r.readU64String();
            saw_we = true;
        }
        else if (key == "hostname") m.hostname = r.readString();
        else if (key == "process_id") m.pid = r.readU64Number();
        else if (key == "started_at_ms") m.started_at_ms = r.readU64Number();
        else if (key == "sequence") m.seq = r.readU64String();
        else if (key == "expires_at_ms") m.expires_at_ms = r.readU64Number();
        else if (key == "min_active") m.min_active = r.readU64String();
        else if (key == "gc_fenced") m.gc_fenced = r.readBool();
        else if (key == "write_attempt_id")
        {
            m.write_attempt_id = r.readHex128();
            saw_write_attempt_id = true;
        }
        else r.skipUnknown(key);
    }
    if (!saw_su || !saw_we || !saw_write_attempt_id || m.write_attempt_id == UInt128{})
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: missing or zero identity field");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: trailing bytes");
    return m;
}

}
