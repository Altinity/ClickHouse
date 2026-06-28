#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>

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

String encodeOwner(const OwnerObject & o)
{
    Cas::Proto::OwnerProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Owner));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_uuid(u128ToBytesBE(o.server_uuid));

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS owner: protobuf serialization failed");
    return out;
}

OwnerObject decodeOwner(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: empty object");

    Cas::Proto::OwnerProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::Owner))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS owner: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::Owner));
    checkCompatibility(msg.header().compatibility_version(), "owner");

    OwnerObject o;
    o.server_uuid = u128FromBytesBE(msg.server_uuid(), "owner server_uuid");
    return o;
}

String encodeServerEpoch(const ServerEpoch & e)
{
    Cas::Proto::ServerEpochProto msg;

    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::ServerEpoch));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_next_writer_epoch(e.next_writer_epoch);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS server-epoch: protobuf serialization failed");
    return out;
}

ServerEpoch decodeServerEpoch(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: empty object");

    Cas::Proto::ServerEpochProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: protobuf parse failed");

    if (msg.header().magic() != magicFor(FormatId::ServerEpoch))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-epoch: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::ServerEpoch));
    checkCompatibility(msg.header().compatibility_version(), "server-epoch");

    ServerEpoch e;
    e.next_writer_epoch = msg.next_writer_epoch();
    return e;
}

String encodeMountLease(const MountLease & m)
{
    Cas::Proto::MountLeaseProto msg;

    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::MountLease));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_server_uuid(u128ToBytesBE(m.server_uuid));
    msg.set_writer_epoch(m.writer_epoch);
    msg.set_hostname(m.hostname);
    msg.set_pid(m.pid);
    msg.set_started_at_ms(m.started_at_ms);
    msg.set_seq(m.seq);
    msg.set_expires_at_ms(m.expires_at_ms);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount-lease: protobuf serialization failed");
    return out;
}

MountLease decodeMountLease(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: empty object");

    Cas::Proto::MountLeaseProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: protobuf parse failed");

    if (msg.header().magic() != magicFor(FormatId::MountLease))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS mount-lease: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::MountLease));
    checkCompatibility(msg.header().compatibility_version(), "mount-lease");

    MountLease m;
    m.server_uuid = u128FromBytesBE(msg.server_uuid(), "mount-lease server_uuid");
    m.writer_epoch = msg.writer_epoch();
    m.hostname = msg.hostname();
    m.pid = msg.pid();
    m.started_at_ms = msg.started_at_ms();
    m.seq = msg.seq();
    m.expires_at_ms = msg.expires_at_ms();
    return m;
}

namespace
{
/// TRUE iff a `list(prefix, "", 1)` over `prefix` returns at least one key.
bool prefixHasAnyKey(Backend & b, const String & prefix)
{
    return !b.list(prefix, /*cursor*/ "", /*limit*/ 1).keys.empty();
}
}

bool serverRootSubtreeEmpty(Backend & b, const Layout & l, const String & srid)
{
    /// All three subtrees that can ever hold this server root's data. Today only `roots/<srid>/`
    /// is populated; Phase 1 relocates ref/manifest data under `cas/refs`/`cas/manifests`. List all
    /// three so the precondition is correct once Phase 1 lands.
    if (prefixHasAnyKey(b, l.casRefsServerPrefix(srid)))
        return false;
    if (prefixHasAnyKey(b, l.casManifestsServerPrefix(srid)))
        return false;
    if (prefixHasAnyKey(b, l.serverRootDataPrefix(srid)))
        return false;
    return true;
}

void claimOwnerOrThrow(Backend & b, const Layout & l, const String & srid, UInt128 our_uuid)
{
    const String key = l.ownerKey(srid);

    /// Owner present → it is identity: equal UUID is ok, a different UUID fails closed regardless
    /// of any lease/clock state.
    if (const auto got = b.get(key))
    {
        const OwnerObject owner = decodeOwner(got->bytes);
        if (owner.server_uuid == our_uuid)
            return;
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-root '{}' is owned by a different server (foreign owner) — refusing to claim",
            srid);
    }

    /// Owner absent. Claiming is allowed ONLY over a provably-empty subtree; an absent owner over
    /// existing data means the identity was lost and must never be silently re-claimed.
    if (!serverRootSubtreeEmpty(b, l, srid))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-root '{}' has no owner anchor but its data subtree is non-empty "
            "(identity lost over existing data) — refusing to re-claim",
            srid);

    const PutResult put = b.putIfAbsent(key, encodeOwner(OwnerObject{.server_uuid = our_uuid}));
    if (put.outcome == PutOutcome::Done)
        return;

    /// Race: another process claimed between our get and our putIfAbsent. Re-read and compare.
    const auto reread = b.get(key);
    if (!reread)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-root '{}' owner anchor vanished during claim", srid);
    const OwnerObject owner = decodeOwner(reread->bytes);
    if (owner.server_uuid == our_uuid)
        return;
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS server-root '{}' was claimed by a different server during our claim (foreign owner) "
        "— refusing to proceed",
        srid);
}

uint64_t allocateWriterEpoch(Backend & b, const Layout & l, const String & srid)
{
    const String key = l.epochKey(srid);

    static constexpr int max_attempts = 100;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        const auto got = b.get(key);

        ServerEpoch current;
        std::optional<Token> expected;
        if (got)
        {
            current = decodeServerEpoch(got->bytes);
            expected = got->token;
        }
        else
        {
            /// A missing `epoch` over a non-empty subtree is a reset hazard (durable monotone
            /// counter cannot be reconstructed) — fail closed.
            if (!serverRootSubtreeEmpty(b, l, srid))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS server-root '{}' has no durable epoch object but its data subtree is "
                    "non-empty (writer_epoch reset hazard) — refusing to proceed",
                    srid);
            /// Fresh empty root: the first epoch handed out is 0.
        }

        const uint64_t next = current.next_writer_epoch;
        ServerEpoch new_state;
        new_state.next_writer_epoch = next + 1;

        const CasResult res = b.casPut(key, encodeServerEpoch(new_state), expected);
        if (res.outcome == CasOutcome::Committed)
            return next;
        /// Conflict: someone else allocated concurrently — retry against the fresh state.
    }

    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS server-root '{}' writer_epoch allocation did not converge after {} attempts",
        srid, max_attempts);
}

}
