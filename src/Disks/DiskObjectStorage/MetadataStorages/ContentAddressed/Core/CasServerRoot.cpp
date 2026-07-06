#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Common/logger_useful.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <base/getFQDNOrHostName.h>
#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <string_view>
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
    msg.set_min_active(m.min_active);
    msg.set_observed_gc_round(m.observed_gc_round);
    msg.set_gc_fenced(m.gc_fenced);

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
    m.min_active = msg.min_active();
    m.observed_gc_round = msg.observed_gc_round();
    m.gc_fenced = msg.gc_fenced();
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
        /// Mirror mountDoubleStartMessage's operator guidance: the by-far most common cause is a
        /// REGENERATED local ClickHouse uuid file (wiped /var/lib/clickhouse, a pod rescheduled
        /// without a persistent volume) while the pool kept the old identity — name it and the
        /// recovery options instead of a bare refusal.
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS server-root '{}' is owned by a different server (owner server_uuid={}, ours={}) — refusing to claim. "
            "This usually means THIS server's local uuid file was regenerated (e.g. /var/lib/clickhouse was wiped, "
            "or the container/pod was recreated without a persistent volume) while the pool kept the old identity. "
            "Recover by restoring the old local uuid file; or configure a fresh <server_root_id> for this disk; "
            "or — only after verifying that NO server uses this root — manually delete the owner object '{}' and restart.",
            srid, u128ToHex(owner.server_uuid), u128ToHex(our_uuid), key);
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

    /// Same uuid, DIFFERENT epoch, GC-FENCED → reclaim IMMEDIATELY, expiry regardless. The fence-out
    /// is terminal for that incarnation by construction (its keeper's every renewal fails the token
    /// guard forever, so it can never write again) — there is no liveness left to wait for. This is
    /// what makes self-remount (and a fast restart after a fence-out) instant instead of a TTL wait.
    /// Same uuid, DIFFERENT epoch, NOT fenced, lease still LIVE → a second incarnation is genuinely
    /// up → double-start guard.
    if (!existing.gc_fenced && existing.expires_at_ms > now_ms)
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

String mountDoubleStartMessage(const String & srid, const MountLease & existing)
{
    return fmt::format(
        "Content-addressed disk cannot start: server_root_id '{}' is actively mounted by another LIVE server.\n"
        "  Existing mount: server_uuid={} hostname={} pid={} last_seq={} expires_at_ms={}\n"
        "This server already waited for the mount lease to lapse, but it kept being renewed — a second\n"
        "server is holding the same CAS namespace. This prevents two ClickHouse servers from writing it.\n"
        " - If the other server is running intentionally, configure a unique <server_root_id> for this disk.\n"
        " - If the other server is a stale/zombie process, stop it; this server will then reclaim the mount on restart.\n"
        " - CLOCK SKEW CAVEAT: liveness is judged by comparing the lease's wall-clock expires_at_ms against\n"
        "   THIS server's clock, so a large clock skew between the two servers can misjudge it (a healthy holder\n"
        "   may look mounted here, or a dead one may look live). Verify both servers' clocks are in sync (NTP).\n"
        " - If the local ClickHouse uuid file was regenerated, restore the old uuid file, or remove the stale\n"
        "   owner object gc/server-roots/{}/owner only after verifying no server uses this root.\n"
        " - As a LAST RESORT, after verifying that NO server is writing this root, manually delete the mount\n"
        "   object gc/server-roots/{}/mount and restart; this server will then re-claim it.",
        srid, u128ToHex(existing.server_uuid), existing.hostname, existing.pid,
        existing.seq, existing.expires_at_ms, srid, srid);
}

MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    const std::function<uint64_t()> & now_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms, uint64_t margin_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn,
    const std::function<void(const MountLease &, uint64_t)> & on_wait_start)
{
    /// A zero poll interval would spin; a single-ms floor keeps the loop a real (bounded) wait.
    const uint64_t poll = poll_interval_ms == 0 ? 1 : poll_interval_ms;

    MountClaimResult r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms);
    if (r.kind != MountClaimResult::LiveDoubleStart)
        return r;

    /// A same-uuid, different-epoch, still-live lease from a prior incarnation of THIS server. It is
    /// either our own crashed process (its keeper died without releasing the lease) or a genuinely live
    /// twin. Wait for the lease to lapse — a live twin keeps renewing and never lapses, so we time out
    /// and report it; a dead predecessor lapses within its TTL and we reclaim (token-guarded).
    const uint64_t start_ms = now_ms_fn();
    uint64_t wait_deadline = r.body.expires_at_ms + margin_ms;
    const uint64_t cap = start_ms + ttl_ms + margin_ms;
    if (wait_deadline > cap)
        wait_deadline = cap;

    if (on_wait_start)
        on_wait_start(r.body, wait_deadline);

    while (now_ms_fn() < wait_deadline)
    {
        sleep_ms_fn(poll);
        r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms);
        if (r.kind != MountClaimResult::LiveDoubleStart)
            return r;
    }

    /// Timed out still LiveDoubleStart → a genuinely live second server holds the mount.
    return r;
}

HeartbeatFloor computeHeartbeatFloor(Backend & b, const Layout & l, uint64_t now_ms,
                                     uint64_t skew_margin_ms)
{
    HeartbeatFloor floor;

    const String prefix = l.serverRootsPrefix();
    String cursor;
    while (true)
    {
        const ListPage page = b.list(prefix, cursor, /*limit*/ 1000);
        for (const auto & listed : page.keys)
        {
            /// `/owner` and `/epoch` objects share the subtree — only mount bodies gate the floor.
            static constexpr std::string_view mount_suffix = "/mount";
            if (listed.key.size() < mount_suffix.size()
                || listed.key.compare(listed.key.size() - mount_suffix.size(), mount_suffix.size(), mount_suffix) != 0)
                continue;

            const String & key = listed.key;

            /// The srid is the path segment between `serverRootsPrefix()` and the `/mount` suffix
            /// (`<prefix>/gc/server-roots/<srid>/mount`). Used only for observability (lagging / fenced).
            const String srid = key.substr(prefix.size(),
                key.size() - prefix.size() - mount_suffix.size());

            /// Fence-out on PreconditionFailed re-GETs and reclassifies from the top; bound the retries
            /// so a pathologically contended holder cannot spin forever. On exhaustion the entry is
            /// counted as live with its current ack (conservative — never excluded without a landed
            /// fence-out).
            constexpr int max_reclassify = 4;
            for (int attempt = 0; ; ++attempt)
            {
                const auto got = b.get(key);
                if (!got)
                    break;   /// Raced away (deleted) — nothing to classify.

                const MountLease m = decodeMountLease(got->bytes);
                floor.max_ack = std::max(floor.max_ack, m.observed_gc_round);

                if (m.gc_fenced)
                {
                    ++floor.already_fenced;
                    break;
                }
                if (m.min_active == std::numeric_limits<uint64_t>::max())
                {
                    ++floor.terminated;
                    break;
                }

                const bool live = now_ms <= m.expires_at_ms + skew_margin_ms;
                const bool exhausted = attempt >= max_reclassify;
                if (live || exhausted)
                {
                    floor.min_ack = std::min(floor.min_ack, m.observed_gc_round);
                    ++floor.live;
                    floor.lagging.emplace_back(srid, m.observed_gc_round);
                    break;
                }

                /// Expired, not terminated, not yet fenced → token-guarded fence-out preserving the
                /// whole body (gc_fenced = true, seq + 1).
                MountLease fenced = m;
                fenced.gc_fenced = true;
                fenced.seq = m.seq + 1;
                const PutResult res = b.putOverwrite(key, encodeMountLease(fenced), got->token);
                if (res.outcome == PutOutcome::Done)
                {
                    ++floor.fenced_now;
                    floor.fenced_srids.push_back(srid);
                    break;
                }
                /// PreconditionFailed: the holder renewed between our GET and PUT — re-GET and
                /// reclassify (it is now live).
            }
        }

        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }

    return floor;
}

std::vector<MountInfo> listMounts(Backend & backend, const Layout & layout, uint64_t now_ms, uint64_t skew_margin_ms)
{
    std::vector<MountInfo> out;
    String cursor;
    while (true)
    {
        const ListPage page = backend.list(layout.serverRootsPrefix(), cursor, 1000);
        for (const auto & k : page.keys)
        {
            static constexpr std::string_view suffix = "/mount";
            if (!k.key.ends_with(suffix))
                continue;
            const auto got = backend.get(k.key);
            if (!got)
                continue;   /// raced a delete — read-only view, skip the row
            MountInfo info;
            const size_t end = k.key.size() - suffix.size();
            const size_t start = k.key.rfind('/', end - 1);
            info.srid = k.key.substr(start + 1, end - start - 1);
            try
            {
                info.lease = decodeMountLease(got->bytes);
            }
            catch (...)
            {
                info.state = "corrupt";
                out.push_back(std::move(info));
                continue;
            }
            if (info.lease.gc_fenced)
                info.state = "fenced";
            else if (info.lease.min_active == std::numeric_limits<uint64_t>::max())
                info.state = "terminated";
            else if (now_ms <= info.lease.expires_at_ms + skew_margin_ms)
                info.state = "live";
            else
                info.state = "expired";
            out.push_back(std::move(info));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return out;
}

MountLeaseKeeper::MountLeaseKeeper(
    BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
    uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
    std::function<uint64_t()> min_active_fn_, std::function<uint64_t()> observed_round_fn_)
    : SingleWriterSlot(std::move(backend_), layout_.mountKey(srid_), "mount-lease", "release", "CasMountLeaseKeeper")
    , srid(srid_)
    , server_uuid(server_uuid_)
    , writer_epoch(writer_epoch_)
    , ttl(ttl_)
    , now_ms_fn(std::move(now_ms_fn_))
    , min_active_fn(std::move(min_active_fn_))
    , observed_round_fn(std::move(observed_round_fn_))
{
}

SingleWriterSlot::RenewPayload MountLeaseKeeper::prepareRenew() const
{
    /// Carry the three dynamic fields (all read OFF the state lock — the merged floor/round callbacks
    /// reach into the Store's own locks): `value` = wall-clock `now_ms` (so `encodeBody` stamps a
    /// fresh `expires_at_ms = now_ms + ttl`), `value2` = `min_active` (the build-watermark floor),
    /// `value3` = `observed_gc_round` (the acked GC round).
    return {.value = now_ms_fn(), .value2 = min_active_fn(), .value3 = observed_round_fn()};
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
        .min_active = payload.value2,
        .observed_gc_round = payload.value3,
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
    /// seq+1, against the token we hold. This makes a same-uuid reopen immediately reclaimable. The
    /// merged watermark farewell folds in HERE: `min_active = UINT64_MAX` is the retired sentinel the
    /// GC floor treats as "every build_seq of this server is retired" — one release retires both the
    /// mount lease and the build watermark.
    const uint64_t now_ms = now_ms_fn();
    const String body = encodeMountLease(MountLease{
        .server_uuid = server_uuid,
        .writer_epoch = writer_epoch,
        .hostname = getFQDNOrHostName(),
        .pid = static_cast<uint64_t>(::getpid()),
        .started_at_ms = now_ms,
        .seq = seq + 1,
        .expires_at_ms = now_ms,
        .min_active = std::numeric_limits<uint64_t>::max(),
    });
    const PutResult res = backend->putOverwrite(key, body, last_token);
    if (res.outcome != PutOutcome::Done)
    {
        /// A foreign incarnation on OUR release path has exactly one expected cause: GC fenced this
        /// mount out after its lease expired (the `gc_fenced` stamp). That is a clean outcome — the
        /// slot is already released-by-fence and there is nothing left to retire. Anything else on
        /// this path is a genuine single-writer violation and stays loud.
        if (const auto got = backend->get(key))
        {
            const MountLease current = decodeMountLease(got->bytes);
            if (current.gc_fenced)
            {
                LOG_INFO(getLogger("CasMountLeaseKeeper"),
                    "CAS mount-lease: '{}' was fenced out by GC (expired lease); release is a no-op", key);
                return;
            }
        }
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: release of key '{}' hit a foreign incarnation — the world is broken", key);
    }
    recordWrite(seq + 1, res.token);
}

}
