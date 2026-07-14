#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.h>
#include <Common/Exception.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int ABORTED;
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
/// carrying a CasHeader (magic + checkCompatibility, UInt128 in big-endian 16-byte form). Decode
/// fails closed with CORRUPTED_DATA on bad bytes.

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
    /// Merged heartbeat field: the per-server build-watermark floor rides the SAME object as the lease,
    /// so one renewal PUT stamps both.
    uint64_t min_active = 0;          /// oldest in-flight build_seq; UINT64_MAX = retired (farewell)
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

/// Which certificate of death justified a same-uuid, different-epoch mount reclaim (rev.6 design
/// §token-stability observation). `None` when no reclaim of that kind happened (a fresh claim, a
/// same-epoch refresh, `LiveDoubleStart`, `ForeignOwner`, `FencedSelf`).
enum class MountPriorState
{
    None,
    Clean,             /// the predecessor's own graceful farewell (`min_active == UINT64_MAX`)
    Fenced,            /// the GC leader's own (already threshold-gated) fence-out (`gc_fenced`)
    UncleanObserved,   /// OUR observation watched the write-token hold stable for the full threshold
};

/// Startup decision for the mount lease (`gc/server-roots/<srid>/mount`), run AFTER the owner gate
/// (so `our_uuid` is the established owner). The lease is LIVENESS, not identity — the owner object
/// already settled who may write; the lease settles whether a live incarnation currently holds the
/// slot. Decision over `get(mountKey)`:
///   - absent → write our body via `putIfAbsent` → `Claimed`;
///   - same `server_uuid` AND same `writer_epoch` as (our_uuid, our_epoch) → it is OUR OWN claim
///     (a replay / the keeper adopting it):
///       - `gc_fenced` → terminal for THIS (uuid, epoch) — a fence costs an epoch, so refreshing it
///         in place would resurrect a fenced incarnation → `FencedSelf` (no write);
///       - otherwise → refresh (`putOverwrite` to bump seq + fresh `expires_at_ms`) → `Claimed`;
///   - same `server_uuid`, DIFFERENT `writer_epoch` → reclaimed ONLY on a certificate of death that
///     needs no fresh wall-clock trust (rev.6 design §token-stability observation — see
///     `claimMountAwaitingExpiry` below for how a plain "looks expired" reading is turned into one):
///       - `gc_fenced` (the GC leader already, itself, threshold-gated this incarnation dead; a fence
///         costs an epoch, so its keeper can never renew again) → reclaim, `prior = Fenced`;
///       - the clean marker (`min_active == UINT64_MAX`, the predecessor's own graceful farewell) →
///         reclaim, `prior = Clean`;
///       - `proven_dead_token` matches the CURRENTLY OBSERVED token (the caller itself watched this
///         exact token hold stable for the full observation threshold) → reclaim, `prior =
///         UncleanObserved`;
///       - none of the above → `LiveDoubleStart` (do NOT write). In particular `expires_at_ms <=
///         now_ms` ALONE is never sufficient — comparing a predecessor's stamp against OUR wall clock
///         is exactly the cross-node trust rev.6 removes (a clock-skewed or merely late-observing
///         caller must never conclude death from a bare timestamp read);
///   - different `server_uuid` → `ForeignOwner` (do NOT write, regardless of expiry or prior state).
struct MountClaimResult
{
    /// Plain (unscoped) enum: callers compare with `MountClaimResult::Claimed` directly.
    enum Kind
    {
        Claimed,
        LiveDoubleStart,
        ForeignOwner,
        /// Same (uuid, epoch) as ours, but the body is `gc_fenced`: terminal for THIS epoch — a fence
        /// costs an epoch. The caller must re-open with a fresh `writer_epoch` (see Task 4's open
        /// retry); refreshing/adopting a fenced body in place is never correct.
        FencedSelf,
    };
    Kind kind = ForeignOwner;
    MountLease body;
    /// Which certificate of death justified a same-uuid, different-epoch `Claimed` reclaim (`None` for
    /// every other `Kind`, and for the absent-slot / same-epoch-refresh `Claimed` cases).
    MountPriorState prior = MountPriorState::None;
};

/// Thrown when a mount operation observes that OUR OWN (uuid, epoch) slot was `gc_fenced` by the GC
/// after our lease expired — a RECOVERABLE state ("a fence costs an epoch"): the caller re-opens with
/// a fresh `writer_epoch`. A CAS-local typed exception rather than a new `ErrorCodes` number: a fork
/// carries these edits indefinitely and the numbered `ErrorCodes` list conflicts with upstream on
/// every rebase. Catch sites match BY TYPE (`catch (const MountFencedException &)`), never by code;
/// the base code is `ABORTED` so an uncaught one still surfaces as a clean startup abort.
class MountFencedException : public DB::Exception
{
public:
    explicit MountFencedException(const String & msg)
        : DB::Exception(msg, DB::ErrorCodes::ABORTED) {}
};

/// `proven_dead_token`: the write-token of a same-uuid, different-epoch lease that the CALLER already
/// proved dead by observation (see `claimMountAwaitingExpiry`) — matching it against the CURRENTLY
/// observed token is the ONLY way (besides `gc_fenced` / the clean marker) a same-uuid different-epoch
/// lease is ever reclaimed. Absent (`{}`, the default) for a bare claim attempt with no such proof.
MountClaimResult claimMount(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    uint64_t now_ms, uint64_t ttl_ms, const std::optional<Token> & proven_dead_token = {},
    const CasEventSink & sink = {});

/// Format the operator-actionable startup error shown when the mount lease is held by a genuinely
/// live second server (the same `server_root_id` is mounted twice). Produced only AFTER this server
/// has already waited for the lease to lapse (see `claimMountAwaitingExpiry`) and it did not — so the
/// remediation is about a live twin, not about waiting.
String mountDoubleStartMessage(const String & srid, const MountLease & existing);

/// Observation-based mount claim (S13 crash-recovery; rev.6 design §token-stability observation).
/// Wraps `claimMount` in a loop:
///   - first attempt decided immediately for `Claimed` (fresh / refreshed / reclaimed via `Fenced` or
///     `Clean`), `ForeignOwner`, or `FencedSelf`;
///   - a `LiveDoubleStart` from OUR OWN uuid (a stale lease from a prior incarnation of this server,
///     OR a genuinely live twin — the two are indistinguishable from a bare read) is resolved by
///     WATCHING the lease's write-token on OUR OWN clock (`mono_ms_fn`), NEVER by comparing the
///     lease's stamped `expires_at_ms` against any clock: once the observed token has held stable for
///     the full rate-bound threshold (`ttl_ms + ttl_ms / 20 + poll_interval_ms` — the lease TTL, a 5%
///     clock-drift allowance, and one poll interval of discreteness; mirrors the TLA+-verified
///     `TTL + Drift` threshold in `docs/superpowers/models/CaCasMountCore.tla`'s `ObservedReclaim`),
///     that token is handed to `claimMount` as `proven_dead_token`, which then reclaims token-guarded
///     (`prior = UncleanObserved`). If the token changes DURING the wait (the holder renewed, or a
///     genuine twin is alive) the observation RESTARTS from the new token; bounded to a handful of
///     restarts before giving up and returning the last `LiveDoubleStart` (a holder whose token keeps
///     changing across that many restarts is alive, not dead).
/// `now_ms_fn` is WALL clock, used only for stamping the body we (may) write / diagnostics — it never
/// participates in the reclaim decision. `mono_ms_fn` is the OBSERVATION clock: monotonic on this
/// process, never compared against any other node's clock, and the ONLY clock the threshold is
/// measured against. `sleep_ms_fn` paces the poll. All three, plus `on_wait_start`, are injected so
/// tests drive fake clocks with no real sleeping. `on_wait_start` (default no-op) fires once per
/// observation-window start (including restarts), with the currently-observed lease and the
/// threshold, for an operator-visible startup log.
MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    const std::function<uint64_t()> & now_ms_fn,
    const std::function<uint64_t()> & mono_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn,
    const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {},
    const CasEventSink & sink = {});

/// GC heartbeat gate (GC round protocol step 1). Run by the GC leader at the top of a round: LIST
/// `gc/server-roots/` (O(servers), single-digit counts), GET each mount body, and classify + fence out
/// expired mounts (liveness only — graduation itself paces on GC rounds, not on heartbeat acks).
/// Classification per body:
///   - `gc_fenced` already set → excluded (`already_fenced`); a fenced mount is terminal, no PUT;
///   - terminated (`min_active == UINT64_MAX`, the farewell sentinel stamped by
///     `MountLeaseKeeper::terminate`) → excluded (`terminated`). `expires_at_ms` alone cannot
///     distinguish a graceful farewell from a crash, so the sentinel — not the timestamps — is the
///     terminated marker;
///   - live (`now_ms <= expires_at_ms + skew_margin_ms`) → counted in `live`;
///   - else expired (past the skew-padded deadline, not terminated, not yet fenced) → FENCE-OUT: one
///     token-guarded `putOverwrite` preserving the WHOLE body, setting `gc_fenced = true` and `seq +
///     1`. On `Done` → excluded (`fenced_now`); on `PreconditionFailed` (the holder renewed
///     concurrently) → re-GET and reclassify from the top (bounded retries; if still contended, count
///     it as live — conservative, never exclude a heartbeat without a landed fence-out).
///
/// The fence-out is BOTH safety and liveness. Safety: a sleeper's later renewal permanently fails
/// (its `putOverwrite` now mismatches the fenced token → `tripMountLost`), so it can never re-arm
/// without a fresh `open`. Liveness: a dead server's stale mount slot must not linger forever.
/// Preserving the body keeps S13 recovery intact: a same-uuid reopen reads the current body and
/// reclaims through the normal expired-our-uuid branch.
struct HeartbeatFloor
{
    size_t live = 0;
    size_t terminated = 0;
    size_t fenced_now = 0;
    size_t already_fenced = 0;
    /// The srids of every mount fenced-out THIS call (one GcFenceOut audit event each).
    std::vector<String> fenced_srids;
};

HeartbeatFloor computeHeartbeatFloor(Backend & b, const Layout & l, uint64_t now_ms,
                                     uint64_t skew_margin_ms);

/// A read-only snapshot of one server's mount slot, for introspection (`system.content_addressed_mounts`).
/// state: `live` (lease within TTL+skew), `expired` (lease ran out; the next GC round's heartbeat floor
/// will fence it), `terminated` (clean farewell: `min_active == UINT64_MAX`), `fenced` (`gc_fenced`),
/// `corrupt` (body failed to decode — surfaced as a row, never an exception).
struct MountInfo
{
    String srid;
    MountLease lease;
    String state;
};

/// Enumerate every mount slot under `gc/server-roots/`, decoded and classified — the read-only sibling
/// of `computeHeartbeatFloor`: ZERO writes (no fence-out), per-row fail-open. One LIST + one GET per slot.
std::vector<MountInfo> listMounts(Backend & backend, const Layout & layout, uint64_t now_ms, uint64_t skew_margin_ms);

/// Per-server MERGED heartbeat: one `SingleWriterSlot` over the per-server-root mount object carries
/// the mount lease (liveness) AND the build-watermark floor (`min_active`). One renewal PUT stamps the
/// clock and the build-watermark floor together. Anchors the slot synchronously on `start`, renews it
/// async off the write path, and fails closed on any foreign touch (`renewOnce` throws on a
/// precondition miss). `graceful stop` folds the watermark farewell (`min_active = UINT64_MAX`) into
/// the terminal already-expired mount body.
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
    /// `min_active_fn_` is read OFF the state lock on each beat (via `prepareRenew`) and stamped into
    /// the mount body — the merged watermark floor. It reaches into the Store's own lock, so it must
    /// never run under `state_mutex`.
    MountLeaseKeeper(
        BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
        uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
        std::function<uint64_t()> min_active_fn_,
        CasEventSink event_sink_ = {});

    /// Claims (adopts) the mount slot for (server_uuid, writer_epoch) with seq following the observed
    /// one — durable when `start` returns.
    void start() { doStart(); }

    /// Releases the mount: the terminal op. Stops the background thread first.
    void stop() { doTerminate(); }

    /// Join the renewal thread BEFORE this object's own `std::function` members (`on_renew_ok` /
    /// `on_lost`, which reach back into the Store) are destroyed. The base `~SingleWriterSlot` also
    /// calls `stopBackground`, but it runs AFTER the derived members are gone — a renewal firing
    /// `on_lost` in that window would call a destroyed `std::function`. Stopping here closes that window.
    ~MountLeaseKeeper() override { stopBackground(); }

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
    void onRenewMismatch(const String & mismatched_key) override;

private:
    String srid;
    UInt128 server_uuid;
    uint64_t writer_epoch;
    std::chrono::milliseconds ttl;
    std::function<uint64_t()> now_ms_fn;
    std::function<uint64_t()> min_active_fn;
    std::function<void()> on_renew_ok;
    std::function<void()> on_lost;
    CasEventSink event_sink;
};

}
