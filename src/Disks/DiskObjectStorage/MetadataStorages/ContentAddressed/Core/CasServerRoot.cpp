#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <base/getFQDNOrHostName.h>

#include <unistd.h>

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
            /// Fresh empty root: reserve 0 as a sentinel (0 means "no epoch", UINT64_MAX is the
            /// retired sentinel), so the first epoch handed out is 1 — matching the random
            /// `process_epoch` draw (CasStore.cpp re-draws on 0) and the TLA+ model (first
            /// AllocEpoch makes epoch=1).
            current.next_writer_epoch = 1;
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

namespace
{
/// Build a fresh mount-lease body for (uuid, epoch) with the given seq, stamped from `now_ms`.
MountLease makeMountBody(UInt128 uuid, uint64_t epoch, uint64_t seq, uint64_t now_ms, uint64_t ttl_ms)
{
    return MountLease{
        .server_uuid = uuid,
        .writer_epoch = epoch,
        .hostname = getFQDNOrHostName(),
        .pid = static_cast<uint64_t>(::getpid()),
        .started_at_ms = now_ms,
        .seq = seq,
        .expires_at_ms = now_ms + ttl_ms,
    };
}
}

MountClaimResult claimMount(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    uint64_t now_ms, uint64_t ttl_ms)
{
    const String key = l.mountKey(srid);
    const auto got = b.get(key);

    /// Absent → fresh claim.
    if (!got)
    {
        const MountLease body = makeMountBody(our_uuid, our_epoch, /*seq=*/ 1, now_ms, ttl_ms);
        const PutResult put = b.putIfAbsent(key, encodeMountLease(body));
        if (put.outcome != PutOutcome::Done)
            /// Raced with a concurrent writer between get and putIfAbsent. Treat as a live double
            /// start — fail closed; never overwrite a slot that appeared under us.
            return {.kind = MountClaimResult::LiveDoubleStart, .body = body};
        return {.kind = MountClaimResult::Claimed, .body = body};
    }

    const MountLease existing = decodeMountLease(got->bytes);

    /// Foreign owner → fail closed regardless of expiry. (This runs after the owner gate, so a foreign
    /// mount should not normally exist, but the lease must never be taken across UUIDs.)
    if (existing.server_uuid != our_uuid)
        return {.kind = MountClaimResult::ForeignOwner, .body = existing};

    /// Same uuid + same epoch → it is OUR OWN claim (replay / adopt). Refresh seq + expiry.
    if (existing.writer_epoch == our_epoch)
    {
        const MountLease body = makeMountBody(our_uuid, our_epoch, existing.seq + 1, now_ms, ttl_ms);
        const PutResult put = b.putOverwrite(key, encodeMountLease(body), got->token);
        if (put.outcome != PutOutcome::Done)
            return {.kind = MountClaimResult::LiveDoubleStart, .body = body};
        return {.kind = MountClaimResult::Claimed, .body = body};
    }

    /// Same uuid, DIFFERENT epoch, lease still LIVE → a second incarnation is up → double-start guard.
    if (existing.expires_at_ms > now_ms)
        return {.kind = MountClaimResult::LiveDoubleStart, .body = existing};

    /// Same uuid, DIFFERENT epoch, lease EXPIRED → reclaim with a fresh body (seq continues).
    const MountLease body = makeMountBody(our_uuid, our_epoch, existing.seq + 1, now_ms, ttl_ms);
    const PutResult put = b.putOverwrite(key, encodeMountLease(body), got->token);
    if (put.outcome != PutOutcome::Done)
        /// The mount changed under us between get and putOverwrite — someone else is racing the
        /// reclaim. Fail closed.
        return {.kind = MountClaimResult::LiveDoubleStart, .body = body};
    return {.kind = MountClaimResult::Claimed, .body = body};
}

MountLeaseKeeper::MountLeaseKeeper(
    BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
    uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_)
    : SingleWriterSlot(std::move(backend_), layout_.mountKey(srid_), "mount-lease", "release", "CasMountLeaseKeeper")
    , srid(srid_)
    , server_uuid(server_uuid_)
    , writer_epoch(writer_epoch_)
    , ttl(ttl_)
    , now_ms_fn(std::move(now_ms_fn_))
{
}

SingleWriterSlot::RenewPayload MountLeaseKeeper::prepareRenew() const
{
    /// Carry the wall-clock `now_ms` reading (off the state lock) so `encodeBody` can stamp a fresh
    /// `expires_at_ms = now_ms + ttl` for this renewal.
    return {.value = now_ms_fn()};
}

String MountLeaseKeeper::encodeBody(uint64_t seq_, const RenewPayload & payload) const
{
    const uint64_t now_ms = payload.value;
    const uint64_t ttl_ms = static_cast<uint64_t>(ttl.count());
    return encodeMountLease(MountLease{
        .server_uuid = server_uuid,
        .writer_epoch = writer_epoch,
        .hostname = getFQDNOrHostName(),
        .pid = static_cast<uint64_t>(::getpid()),
        .started_at_ms = now_ms,
        .seq = seq_,
        .expires_at_ms = now_ms + ttl_ms,
    });
}

SingleWriterSlot::Token MountLeaseKeeper::claim(const String & body)
{
    /// ADOPT-aware claim. The normal flow is `claimMount` wrote the live mount under
    /// (server_uuid, writer_epoch); `start` then adopts that very slot. We must NOT self-trip the
    /// live-double-start guard on our own (uuid, epoch).
    const HeadResult head = backend->head(key);
    if (!head.exists)
    {
        /// Absent → put it ourselves (a fresh start that ran without a prior claimMount, or a slot
        /// that lapsed and was swept). putIfAbsent fails closed if it appears under us.
        const PutResult res = backend->putIfAbsent(key, body);
        if (res.outcome != PutOutcome::Done)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS mount-lease: key '{}' appeared between head and putIfAbsent — concurrent writer on our mount slot", key);
        return res.token;
    }

    /// Read the observed slot to decide adopt vs fail-closed by the (uuid, epoch) discriminator.
    const auto got = backend->get(key);
    if (!got)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' vanished between head and get while claiming", key);
    const MountLease observed = decodeMountLease(got->bytes);

    /// Foreign uuid → fail closed (no cross-UUID takeover, ever).
    if (observed.server_uuid != server_uuid)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' is held by a foreign server — failing closed, never taking over", key);

    /// Same uuid but a DIFFERENT epoch → a newer incarnation superseded us (or a concurrent
    /// double-start). Fail closed.
    if (observed.writer_epoch != writer_epoch)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' is held by a different writer_epoch ({} != ours {}) — superseded, failing closed",
            key, observed.writer_epoch, writer_epoch);

    /// Same uuid AND same epoch → it is OUR OWN claim → ADOPT: overwrite against the observed token
    /// to refresh seq/expiry. (`body` is encoded for seq=1 by the base `doStart`; that is fine —
    /// renewals advance from there.)
    const PutResult res = backend->putOverwrite(key, body, got->token);
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' was touched while adopting our own mount slot — failing closed", key);
    return res.token;
}

void MountLeaseKeeper::onRenewSucceeded()
{
    /// A successful background renew extended the durable lease. Refresh the local write-fence
    /// deadline (the Store translates this to `steady_clock::now() + ttl`, monotonic). No S3 read.
    if (on_renew_ok)
        on_renew_ok();
}

void MountLeaseKeeper::onRenewFailed()
{
    /// Background renewal failed: `renewOnce` threw on a foreign/superseded touch and the loop is
    /// stopping. Latch the local write fence to lost so no further mutation proceeds — fail closed.
    if (on_lost)
        on_lost();
}

void MountLeaseKeeper::terminate()
{
    /// Terminal op: retire the lease by stamping it already-expired (expires_at_ms = started_at_ms),
    /// seq+1, against the token we hold. This makes a same-uuid reopen immediately reclaimable.
    const uint64_t now_ms = now_ms_fn();
    const String body = encodeMountLease(MountLease{
        .server_uuid = server_uuid,
        .writer_epoch = writer_epoch,
        .hostname = getFQDNOrHostName(),
        .pid = static_cast<uint64_t>(::getpid()),
        .started_at_ms = now_ms,
        .seq = seq + 1,
        .expires_at_ms = now_ms,
    });
    const PutResult res = backend->putOverwrite(key, body, last_token);
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: release of key '{}' hit a foreign incarnation — the world is broken", key);
    recordWrite(seq + 1, res.token);
}

}
