#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.h>
#include <Common/Exception.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// Validate a `server_root_id` — the explicit, configured identity of the content-addressed layout
/// subtree a server owns (spec §mount-safety). It is a clean relative path: it composes into the
/// object-key tree (`gc/server-roots/<srid>/...`, `roots/<srid>/...`), so the same hygiene the layout
/// applies to a namespace applies here (mirrors `CasLayout.h::checkNamespace`):
///   - non-empty;
///   - no leading/trailing '/', no empty segment ("//");
///   - no '.' or '..' segment;
///   - total length <= 255;
///   - no segment equal to the reserved "_files" / "_manifests".
/// Throws `ErrorCodes::BAD_ARGUMENTS` on any violation. Fail closed — there is no sanitizing fallback.
inline void validateServerRootId(const String & id)
{
    if (id.empty())
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "server_root_id must be non-empty");

    if (id.size() > 255)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "server_root_id '{}' is too long ({} > 255 bytes)", id, id.size());

    size_t start = 0;
    while (true)
    {
        size_t end = id.find('/', start);
        const String segment = id.substr(start, end == String::npos ? String::npos : end - start);
        if (segment.empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' has an empty segment (leading/trailing or doubled '/')", id);
        if (segment == "." || segment == "..")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses a relative segment ('.' or '..')", id);
        if (segment == "_files")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses the reserved segment '_files'", id);
        if (segment == "_manifests")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses the reserved segment '_manifests'", id);
        if (end == String::npos)
            break;
        start = end + 1;
    }
}

/// Phase 0 (mount safety) per-server-root control objects. Each is a pure-protobuf mutable object
/// carrying a CasHeader; the codecs mirror `CasWatermark.cpp` exactly (magic + checkCompatibility,
/// UInt128 in big-endian 16-byte form). Decode fails closed with CORRUPTED_DATA on bad bytes.

/// Owner anchor (`gc/server-roots/<srid>/owner`): binds the configured `server_root_id` to the
/// server UUID that owns the subtree.
struct OwnerObject
{
    UInt128 server_uuid{};
};

/// Writer-epoch fence (`gc/server-roots/<srid>/epoch`): the monotone next writer epoch to hand out.
struct ServerEpoch
{
    uint64_t next_writer_epoch = 0;
};

/// Mount lease (`gc/server-roots/<srid>/mount`): the current live mount holder of the subtree.
struct MountLease
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    String hostname;
    uint64_t pid = 0;
    uint64_t started_at_ms = 0;
    uint64_t seq = 0;
    uint64_t expires_at_ms = 0;
    /// Merged heartbeat fields (ack-floor redesign): the per-server build-watermark floor and the
    /// GC-round acknowledgement ride the SAME object as the lease, so one beat renews all three.
    uint64_t min_active = 0;          /// oldest in-flight build_seq; UINT64_MAX = retired (farewell)
    uint64_t observed_gc_round = 0;   /// newest gc round whose retired list this server has loaded
    bool gc_fenced = false;           /// set ONLY by GC fence-out of an expired lease; terminal
};

String encodeOwner(const OwnerObject & o);
OwnerObject decodeOwner(std::string_view data);

String encodeServerEpoch(const ServerEpoch & e);
ServerEpoch decodeServerEpoch(std::string_view data);

String encodeMountLease(const MountLease & m);
MountLease decodeMountLease(std::string_view data);

class Backend;
class Layout;

/// Mount-safety claim logic (Phase 0). These are the identity + epoch-allocation steps a server
/// runs at startup over its `server_root_id` subtree, BEFORE any ordinary data write. They fail
/// closed (`ErrorCodes::CORRUPTED_DATA`); there is no re-mint or silent-recreate fallback.

/// TRUE iff the whole `server_root_id` subtree is provably empty — `list(prefix, "", 1)` (limit 1)
/// over EACH of the three subtrees that can hold this server root's data
/// (`cas/refs/<srid>/`, `cas/manifests/<srid>/`, `roots/<srid>/`) returns no keys. The `cas/refs`
/// and `cas/manifests` prefixes carry no data until Phase 1 relocates into them, but all three are
/// listed so the precondition stays correct once Phase 1 lands.
bool serverRootSubtreeEmpty(Backend & b, const Layout & l, const String & srid);

/// Claim (or validate) the sticky owner anchor that binds `srid` to a server UUID (identity).
///   - owner present, equal `our_uuid` → ok (return);
///   - owner present, different → throw `CORRUPTED_DATA` (foreign owner — fail closed);
///   - owner absent AND the subtree is provably empty → `putIfAbsent` the owner (claim);
///   - owner absent BUT the subtree is non-empty → throw `CORRUPTED_DATA` (identity lost over
///     existing data — never silently re-claim).
/// The owner object is never deleted and never reassigned to a different UUID.
void claimOwnerOrThrow(Backend & b, const Layout & l, const String & srid, UInt128 our_uuid);

/// Allocate the next durable-monotone `writer_epoch` by CAS-bumping the sticky `epoch` object
/// (`ServerEpoch{next_writer_epoch}`), returning the value the caller adopts as its writer_epoch.
///   - `epoch` absent AND the subtree is non-empty → throw `CORRUPTED_DATA` (missing epoch over
///     data is a reset hazard);
///   - `epoch` absent AND the subtree is empty → start at 0;
///   - otherwise read `next = current.next_writer_epoch`, `casPut` `{next + 1}` against the
///     observed token, retry on `Conflict` (bounded), and return `next`.
uint64_t allocateWriterEpoch(Backend & b, const Layout & l, const String & srid);

/// Startup decision for the mount lease (`gc/server-roots/<srid>/mount`), run AFTER the owner gate
/// (so `our_uuid` is the established owner). The lease is LIVENESS, not identity — the owner object
/// already settled who may write; the lease settles whether a live incarnation currently holds the
/// slot. Decision over `get(mountKey)`:
///   - absent → write our body via `putIfAbsent` → `Claimed`;
///   - same `server_uuid` AND same `writer_epoch` as (our_uuid, our_epoch) → it is OUR OWN claim
///     (a replay / the keeper adopting it) → refresh (`putOverwrite` to bump seq + fresh
///     `expires_at_ms`) → `Claimed`;
///   - same `server_uuid`, DIFFERENT `writer_epoch`, lease EXPIRED (`expires_at_ms <= now_ms`) →
///     reclaim (overwrite with our body, seq = prev + 1) → `Claimed`;
///   - same `server_uuid`, DIFFERENT `writer_epoch`, lease LIVE (`expires_at_ms > now_ms`) →
///     `LiveDoubleStart` (do NOT write — a second incarnation of the same server is already up);
///   - different `server_uuid` → `ForeignOwner` (do NOT write, regardless of expiry).
struct MountClaimResult
{
    /// Plain (unscoped) enum: callers compare with `MountClaimResult::Claimed` directly.
    enum Kind
    {
        Claimed,
        LiveDoubleStart,
        ForeignOwner,
    };
    Kind kind = ForeignOwner;
    MountLease body;
};

MountClaimResult claimMount(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    uint64_t now_ms, uint64_t ttl_ms);

/// Format the operator-actionable startup error shown when the mount lease is held by a genuinely
/// live second server (the same `server_root_id` is mounted twice). Produced only AFTER this server
/// has already waited for the lease to lapse (see `claimMountAwaitingExpiry`) and it did not — so the
/// remediation is about a live twin, not about waiting.
String mountDoubleStartMessage(const String & srid, const MountLease & existing);

/// Bounded wait-for-expiry mount claim (S13 crash-recovery). Wraps `claimMount`:
///   - first attempt decided immediately for `Claimed` (reclaimed / adopted) or `ForeignOwner`;
///   - a `LiveDoubleStart` from OUR OWN uuid (a stale lease from a prior incarnation of this server)
///     is waited out: poll every `poll_interval_ms` (advancing wall-clock via `now_ms_fn`, sleeping via
///     `sleep_ms_fn`) until the lease lapses and we reclaim it (`Claimed`), or the wait bound elapses.
/// The wait bound is latched ONCE from the first observed `expires_at_ms + margin_ms`, capped so we never
/// block longer than `now + ttl_ms + margin_ms` (bounds a forward-clock-skewed expiry). On timeout the
/// last `LiveDoubleStart` is returned (a genuinely live second server). The reclaim inside `claimMount`
/// is token-guarded, so a holder that renews after our read can never be stolen from — correctness does
/// not depend on the poll interval. `now_ms_fn` / `sleep_ms_fn` are injected so tests drive a fake clock
/// with no real sleeping. `on_wait_start` (default no-op) is invoked once, with the observed lease and
/// the latched wait deadline, when the function decides to wait — for an operator-visible startup log.
MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    const std::function<uint64_t()> & now_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms, uint64_t margin_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn,
    const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {});

/// Mount-lease heartbeat (liveness), the sibling of `WatermarkKeeper` over the per-server-root mount
/// object. Reuses `SingleWriterSlot`: anchors the slot synchronously on `start`, renews it async off
/// the write path, and fails closed on any foreign touch (`renewOnce` throws on a precondition miss).
///
/// ADOPT RULE (critical): the steady-state flow is `claimMount(...)` writes the live mount under
/// (our_uuid, our_epoch), THEN `keeper.start()`. So `start`'s `claim` hook must ADOPT a live mount
/// that is ALREADY ours — same `server_uuid` AND same `writer_epoch` — instead of self-tripping the
/// live-double-start guard. The discriminator is the (uuid, epoch) pair:
///   - same uuid + same epoch  → our own just-written claim (or a replay) → adopt: `putOverwrite`
///     against the observed token to refresh seq/expiry (no fail);
///   - same uuid + DIFFERENT live epoch → a newer incarnation superseded us → fail closed;
///   - foreign uuid → fail closed;
///   - absent → `putIfAbsent`; expired-our-uuid (any epoch) → `putOverwrite` reclaim.
/// After `start`, `renewOnce` (base) keeps the slot alive and already fails closed on a foreign touch.
class MountLeaseKeeper final : public SingleWriterSlot
{
public:
    MountLeaseKeeper(
        BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
        uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_);

    /// Claims (adopts) the mount slot for (server_uuid, writer_epoch) with seq following the observed
    /// one — durable when `start` returns.
    void start() { doStart(); }

    /// Releases the mount: the terminal op. Stops the background thread first.
    void stop() { doTerminate(); }

    /// Local write-fence coupling (set once by `Store::open` before `startBackground`): the keeper is
    /// the ONLY thing that touches S3 for the lease, so the fence must reflect the live lease without a
    /// per-write S3 read. On each SUCCESSFUL background renew the keeper calls `on_renew_ok` (the Store
    /// refreshes its monotonic fence deadline); when background renewal FAILS (a foreign/superseded
    /// touch makes `renewOnce` throw and the loop stop) the keeper calls `on_lost` (the Store latches
    /// its fence to lost). Both default to no-op so a keeper used without a Store is inert.
    void setFenceCallbacks(std::function<void()> on_renew_ok_, std::function<void()> on_lost_)
    {
        on_renew_ok = std::move(on_renew_ok_);
        on_lost = std::move(on_lost_);
    }

protected:
    RenewPayload prepareRenew() const override;
    String encodeBody(uint64_t seq_, const RenewPayload & payload) const override;
    Token claim(const String & body) override;
    void terminate() override;
    void onRenewSucceeded() override;
    void onRenewFailed() override;

private:
    String srid;
    UInt128 server_uuid;
    uint64_t writer_epoch;
    std::chrono::milliseconds ttl;
    std::function<uint64_t()> now_ms_fn;
    std::function<void()> on_renew_ok;
    std::function<void()> on_lost;
};

}
