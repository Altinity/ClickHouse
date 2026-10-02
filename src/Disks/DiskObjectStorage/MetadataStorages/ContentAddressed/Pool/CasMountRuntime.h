#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Common/ThreadPool.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace DB::Cas
{

class PartWriteTxn;
using PartWriteTxnPtr = std::shared_ptr<PartWriteTxn>;

/// The pool-level lifecycle condition (rev.7 §1) a `Pool` moves through as its shared backing changes
/// underfoot. It is distinct from the storage-level `Constructing/Started/ShutDown` lifecycle (a null
/// published pool -- before `startup`/after `shutdown`) the metadata storage tracks. Ordering of the
/// enumerators is not significant; membership tests do the work.
///   - `Live`             — the steady state; the mount lease is (or was last) held.
///   - `TransientNotLive` — the lease was lost; access is uncertain and a self-remount retries. The §2
///                          `Present`+identity-match recovery rule fires only from here (or `Live`).
///   - `IdentityLost`     — the pool sentinels are authoritatively absent (both KeyAbsent):
///                          fail-loud and TERMINAL. The lease and GC threads exit;
///                          matching-sentinel reappearance does NOT auto-revive it ([D3]); recovery is a
///                          restart or `SYSTEM CAS FORGET`.
///   - `Vanished*`        — fully terminal truth: the data root was replaced by a foreign pool, or the
///                          disk was decommissioned by `FORGET`. Store-class access fails loud from here.
enum class PoolLifecycle : uint8_t
{
    Live,
    TransientNotLive,
    IdentityLost,
    VanishedReplaced,
    VanishedForgotten,
};

using RuntimeWorkerFactory = std::function<ThreadFromGlobalPool(std::function<void()>)>;

/// Configuration owned by `CasMountRuntime`. `PoolConfig::mountConfig` projects the flat pool settings
/// into this value, keeping the pool's existing configuration interface unchanged while allowing this
/// lower-layer header to describe its own dependencies.
struct MountConfig
{
    std::chrono::milliseconds mount_lease_ttl_ms{30000};
    std::chrono::milliseconds mount_renew_period{10000};
    /// When false, tests drive `renewWatermarkOnce` explicitly. In production this flag enables both
    /// the merged mount-lease/build-watermark heartbeat and self-remount recovery.
    bool background_watermark = false;
    std::function<uint64_t()> boot_ms_fn = {};
    std::function<void(uint64_t)> wait_sleep_fn = {};
    RuntimeWorkerFactory worker_factory = {};
    /// Deterministic test interposition at the top of each pass of the lease loop, before it takes
    /// `driver_mutex`.
    std::function<void()> renewal_before_driver_lock_hook_for_test = {};
    /// Deterministic test interposition after the loop decided to renew and released `driver_mutex`,
    /// before the renewal's I/O starts.
    std::function<void()> renewal_admitted_hook_for_test = {};
    /// Deterministic test interposition after a wait predicate of the lease loop sampled false,
    /// immediately before the wait atomically releases `driver_mutex`.
    /// Runs with `driver_mutex` held: it must issue no backend request and never wait on the pool's
    /// hot-key lane, whose holders sleep under that mutex, or the test deadlocks itself.
    std::function<void()> lease_wait_predicate_false_hook_for_test = {};
    /// Deterministic test interposition immediately before a terminal publisher attempts to acquire
    /// `driver_mutex`.
    std::function<void()> terminal_publication_waiting_for_driver_lock_hook_for_test = {};
    /// Deterministic test interposition after the terminal publisher has observed `driver_mutex`
    /// contention, but before it blocks acquiring the mutex.
    std::function<void()> terminal_publication_driver_lock_contended_hook_for_test = {};
    /// Deterministic test interposition immediately after a terminal publisher acquires `driver_mutex`.
    /// Runs with `driver_mutex` held: it must issue no backend request and never wait on the pool's
    /// hot-key lane, whose holders sleep under that mutex, or the test deadlocks itself.
    std::function<void()> terminal_publication_driver_lock_acquired_hook_for_test = {};
    /// Deterministic failure injection at the vanished-reason preparation boundary.
    std::function<void()> vanished_reason_prepare_hook_for_test = {};
    /// Test-only extra liveness condition, for exact pre/post-send gate interleavings. It is ANDed with
    /// the ordinary predicate, so a stop still ends the renewal. FALSE ends the renewal early.
    std::function<bool()> renewal_live_for_test = {};
};

/// Local, in-memory write fence. It is deliberately not checked by reading the object store for every
/// write: the `MountLeaseRenewer` is the sole lease reader/renewer. A successful renewal computes
/// `deadline_boot_ms` from its own confirmed request's pre-I/O `CLOCK_BOOTTIME` anchor plus the lease
/// TTL, never from the durable `expires_at_ms` stamp, which is a writer-stamped diagnostic only; a
/// foreign owner, newer `writer_epoch`, or failed renewal latches `lost`. Mutable operations are
/// allowed only while the latch is clear and the local deadline has not passed. The `writer_epoch` is
/// the durable fencing token.
///
/// The fence uses `CLOCK_BOOTTIME`, not `CLOCK_MONOTONIC`: monotonic time does not advance while a VM is
/// suspended, so a resumed sleeper would compute the same "not yet expired" verdict it had before the nap
/// even though wall time (and the GC leader's fence-out) moved far ahead — it could mutate the shared state
/// under a live writer.
/// `CLOCK_BOOTTIME` includes suspend time, so a resumed sleeper sees its fence expired.
/// Container pause is already safe under either clock (the process is frozen, so no local check runs).
struct MountFence
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    /// Until something arms a real lease deadline, the permissive default allows mutations. UINT64_MAX =
    /// unarmed (never expires); otherwise a CLOCK_BOOTTIME-milliseconds instant.
    std::atomic<uint64_t> deadline_boot_ms{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> lost{false};
};

/// Owns the live writer-incarnation mechanics shared by the pool's mount and recovery orchestration:
/// the `MountLeaseRenewer`, local `MountFence`, build watermark and in-flight build registry,
/// `live_writer_epoch`, unclean-boundary marker, and the lease thread. `Pool` retains the higher-level
/// claim/recovery sequence and its `remount_mutex`; in particular, the runtime does not acquire or own
/// the ref-ledger locks. The runtime receives its backend, layout, configuration, event sink, request
/// budget, and a callback that performs one pool-level remount attempt, so it has no `Pool` back-reference.
/// `Pool` delegates preserve the existing callers and test seams.
class CasMountRuntime
{
public:
    CasMountRuntime(
        BackendPtr backend_ptr_,
        /// The planes the `MountLeaseRenewer` runs on: a bounded renewal under the mount fence, the
        /// claim and the farewell on an open one, and the lease loop's renewal on `lease_requests_`,
        /// which has no lease budget and whose sleep a stop wakes. Owned by `Pool` and outliving this
        /// runtime.
        CasRequests & mount_requests_,
        CasRequests & farewell_requests_,
        CasRequests & lease_requests_,
        const Layout & layout_,
        MountConfig config_,
        String server_root_id_,
        const CasEventSink & event_sink_,
        CasRequestBudget cas_request_budget_,
        /// One pool-level recovery attempt. The callback captures the owning `Pool` and is invoked only
        /// after construction, from the lease thread.
        std::function<bool()> remount_attempt_);

    /// ---- per-server watermark and identity ----
    /// `process_epoch` is random and nonzero for this pool incarnation. GC compares it for equality,
    /// never ordering; a different value means that the previous writer incarnation is no longer live.
    uint64_t epoch() const { return process_epoch.load(std::memory_order_acquire); }
    uint64_t writerEpoch() const { return process_epoch.load(std::memory_order_acquire); }
    /// The GC floor: the oldest in-flight build_seq, or next_build_seq when no build is active (so a
    /// quiescent server's watermark floor advances to the next-to-be-allocated seq). Locks builds_mutex.
    uint64_t minActive();
    /// Test/assertion accessor for the next-to-allocate build_seq under the lock.
    uint64_t peekNextBuildSeq();
    /// Runs the lease thread's renewal step once on the calling thread and rethrows a terminal failure:
    /// the test seam. Refused when the runtime is configured for a lease thread, and on a read-only
    /// runtime, which has no renewer.
    void renewWatermarkOnce();

    /// ---- local write fence ----
    /// Return whether a mutable operation may start under the locally observed lease state.
    bool mayMutate() const;
    /// Latch the fence lost and count one lease loss. A trip that comes alone is re-armed by the next
    /// renewal that commits with room for a ref append, so every production caller pairs it with a
    /// remount request, a terminal lifecycle (a published FORGET intent included) or a stop, or trips
    /// where no renewal follows: the lease loop's error exit.
    void tripMountLost();
    /// Publish the BOOTTIME deadline from a successful lease renewal.
    void setMountDeadline(uint64_t deadline_boot_ms);
    /// Arm a new lease incarnation and clear any loss latched for the prior incarnation. Unconditional
    /// and reports no `Live`; a reclaim arms through `armIfAdmissible`.
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
    /// Step 0 of a reclaim. One step under `driver_mutex`: record the requested generation this attempt
    /// serves and latch the fence.
    void beginReclaim();
    /// End of a reclaim that claimed. One step under `driver_mutex`: acknowledge the generation
    /// `beginReclaim` recorded, publish the deadline, and arm the fence and report `Live` if the arming
    /// rule holds; otherwise latch the fence. Returns whether it armed.
    bool armIfAdmissible(uint64_t deadline_boot_ms);
    /// Blocks until the fence is armed. Gives up after `timeout_ms` on the fence clock, on a stop, on a
    /// terminal lifecycle, or when the lease thread was never started or was stopped. A lease thread that
    /// ended on its own does not end the wait early: it waits out `timeout_ms`. Returns whether the fence
    /// is armed.
    bool waitUntilArmed(uint64_t timeout_ms) const;
    /// Test-only interposition at the publication boundary between the re-armed generation and the
    /// live fence: after the new generation and, for a production arm, the `Live` report, before `lost`
    /// is cleared. A caller admitted from this hook must be refused: the old generation is already
    /// dead, while the new generation is not live until `lost` is cleared. Through `armIfAdmissible`
    /// it runs with `driver_mutex` held.
    void setArmMountFenceInterpositionHookForTest(std::function<void()> hook)
    {
        arm_mount_fence_interposition_hook_for_test = std::move(hook);
    }
    /// The fence clock: `CLOCK_BOOTTIME` in milliseconds (includes VM-suspend time, unlike
    /// CLOCK_MONOTONIC — see `MountFence`). Consults the injected `config.boot_ms_fn` if set (tests),
    /// otherwise `bootMs`.
    uint64_t bootMsNow() const;
    /// The real boot clock: `CLOCK_BOOTTIME` in milliseconds. Static so tests can compose it.
    static uint64_t bootMs();

    /// ---- fence-generation admission (rev.7 [C2]/[D1]) ----
    /// Bumped by EVERY `tripMountLost` (a fence loss) and EVERY `armMountFence` (a re-arm -- a fresh
    /// lease incarnation, e.g. after a self-remount). A durable-effect caller captures this value once
    /// at admission and compares it again immediately before its durable backend call: a DIFFERENT
    /// value means the lease incarnation moved from under it since admission -- even when the fence
    /// happens to be live again under a brand-new incarnation, the caller's write is stale and must not
    /// land. See `checkFenceOrThrow`.
    uint64_t fenceGeneration() const { return fence_generation.load(std::memory_order_acquire); }

    /// Fence-generation admission check for every durable CAS/PUT/DELETE (the plain-object surface,
    /// staging-buffer finalize): the caller captures `fenceGeneration()` once at admission and passes it
    /// back here immediately before its durable backend call -- and again before EVERY conditional-retry
    /// iteration, not just the first attempt. Throws the typed transient refusal
    /// (`throwCasTransientUnavailable`) when the fence is not currently held or the generation moved since
    /// admission; the caller's write must never reach the backend in either case.
    void checkFenceOrThrow(uint64_t admitted_generation) const;

    /// ---- pool lifecycle condition (rev.7 §1, spec §§1-3); enum at namespace scope below ----
    /// Atomic read of the current lifecycle (acquire).
    PoolLifecycle lifecycle() const { return pool_lifecycle.load(std::memory_order_acquire); }
    /// Whether the pool has reached one of the two fully-terminal `Vanished` values
    /// (`VanishedReplaced` / `VanishedForgotten`).
    bool isVanished() const;
    /// Whether the terminal-intent latch (`vanished_intent`) is published — set by a natural
    /// `enterVanished`, OR EARLY (spec §5 step 1) by FORGET's `publishVanishedIntent`, and NEVER by the
    /// non-absorbing `IdentityLost` ([C1]). This is the EARLIEST terminal signal: it can already be true
    /// while the state is still pre-terminal (mid-FORGET). Consulted alongside `isVanished()` wherever
    /// background work must stop the moment the pool is (being driven) terminal: `scheduleRemount`, the
    /// lease loop and the GC scheduler.
    bool vanishedIntentPublished() const { return vanished_intent.load(std::memory_order_acquire); }

    /// Non-terminal lease-loss transition: `Live -> TransientNotLive`. Idempotent and lock-free; a
    /// compare-exchange FROM `Live` only, so it never downgrades a terminal state. `tripMountLost`
    /// calls this (the lease-loss primitive).
    /// Returns true only to the compare-exchange winner, which owns the one-per-loss metric.
    bool noteLeaseLost();
    /// Non-terminal recovery transition: `TransientNotLive -> Live`. Called after a self-remount
    /// reclaimed a fresh incarnation. A compare-exchange FROM `TransientNotLive` only, so it NEVER
    /// revives `IdentityLost`/`Vanished` ([D3]).
    void noteRemounted();

    /// One-way terminal transition to `IdentityLost`, from `TransientNotLive` only (a compare-exchange
    /// FROM `TransientNotLive`, so it is idempotent and cannot fire from `Live`/`Vanished`). On the
    /// transition it emits ONE WARN and one `CASIdentityLost` ProfileEvent. rev.8: `IdentityLost` is a
    /// fail-loud TERMINAL state — `remountTerminal` reports it, so the lease thread exits
    /// (and the GC scheduler self-exits, through `Pool`) at its next boundary; there is no demoted observer.
    /// It deliberately does NOT publish the `vanished_intent` latch (which is reserved for the `Vanished*`
    /// idempotency/FORGET protocol); `remountTerminal` widens the worker-exit boundary to include it.
    /// Must be called under the caller's remount serialization (Pool::remount_mutex).
    void enterIdentityLost();
    /// Test seam: force the lifecycle condition directly to `lc`, bypassing the natural transition
    /// preconditions (used by the operation-gate tests to pin each class × state cell without driving a
    /// full remount/erase sequence). For a `Vanished*` value it also latches `vanished_intent`, so the
    /// forced state is indistinguishable from a naturally-reached one. Never used in production.
    void setLifecycleForTest(PoolLifecycle lc);

    /// Publish the terminal-intent latch (`vanished_intent`) WITHOUT settling the lifecycle state. This is
    /// the first step of `SYSTEM CAS FORGET`: publishing the latch FIRST makes the runtime stop latching
    /// remount generations and the lease loop exit at its next step boundary, so FORGET's join of the
    /// lease thread is bounded by one step and one backend timeout. The state store + WARN happen
    /// later, in `enterVanished`. Idempotent. Publication is serialized by `driver_mutex` and
    /// followed by a condition-variable notification, so the lease loop cannot miss the terminal edge
    /// between its predicate sample and wait. A natural
    /// terminal transition does NOT call this — its `enterVanished` publishes the latch itself.
    void publishVanishedIntent();

    /// One-way transition to a fully-terminal `Vanished` value (spec §3). Publishes the terminal-intent
    /// latch (so the runtime stops scheduling remount work and the lease loop exits at its next step
    /// boundary) if it is not already published, records `reason`, stores the state, then emits ONE WARN +
    /// one `CASDataRootVanished` ProfileEvent. Idempotent: the first terminal STATE transition wins (a
    /// dedicated latch keyed separately from `vanished_intent`, because FORGET publishes that intent latch
    /// early at step 1). `which` MUST be one of the two `Vanished*` values (`VanishedReplaced` or
    /// `VanishedForgotten`). `reason` is retained and
    /// surfaced verbatim in the `VanishedForgotten` [D5] error message (see `vanishedReason`). Threads exit
    /// their own loops; the joins happen in `~Pool` for a natural transition, or synchronously in
    /// `Pool::forgetDisk` for FORGET. Must be called under the caller's remount serialization
    /// (Pool::remount_mutex).
    void enterVanished(PoolLifecycle which, const String & reason);

    /// The reason string recorded by the winning `enterVanished`, or empty when none has run (a
    /// forced-for-test terminal state, or a non-terminal pool). `Pool::throwIfLifecycleTerminal` reads it
    /// to build the `VanishedForgotten` [D5] message (which carries the operator's decommission timestamp
    /// authored by `forgetDisk`). Safe to read only AFTER observing a terminal state via `lifecycle()`
    /// (acquire): the reason is written once, before the state's release-store, so a reader that
    /// acquire-observes the terminal state also observes the reason (release/acquire handoff).
    const String & vanishedReason() const { return vanished_reason; }

    /// Wall-clock second (`system_clock`, seconds since epoch) at which the pool ENTERED its current
    /// non-`Live` lifecycle state, or 0 while `Live`. This is the `since` the non-gated
    /// `system.cas_mounts` lifecycle snapshot (spec §7) reports. Written (release) at each
    /// lifecycle edge — `noteLeaseLost`/`enterIdentityLost`/`enterVanished` set it to now, `noteRemounted`
    /// clears it to 0 — and by `setLifecycleForTest`, so a forced state carries a `since` indistinguishable
    /// from a naturally-reached one.
    ///
    /// Ordering vs the `pool_lifecycle` transition it accompanies: the TERMINAL edges (`enterVanished`,
    /// `enterIdentityLost`) publish this store BEFORE the state store, so a reader that acquire-observes a
    /// terminal state is guaranteed (release/acquire handoff) to observe this timestamp. The lock-free
    /// lease-loss/remount edges (`noteLeaseLost`, `noteRemounted`) stamp it in the compare-exchange's
    /// SUCCESS branch — after the CAS — because they may run on an already-terminal pool (`noteLeaseLost` is
    /// called before the caller's `isVanished()` gate), where a pre-CAS stamp would clobber the terminal
    /// `since`; a reader may therefore momentarily observe a just-entered `not_live` with `since` not yet
    /// updated, a benign introspection artifact that converges within nanoseconds.
    time_t lifecycleSinceWallS() const
    {
        return static_cast<time_t>(lifecycle_since_wall_s.load(std::memory_order_acquire));
    }

    /// Extends `mayMutate` with a remaining-budget check. A ref-log attempt is refused unless the
    /// current lease has room for its configured timeout and safety margin, so work is not started when
    /// it cannot plausibly finish before the fence expires.
    bool refAppendFenceOk() const;

    /// ---- lease expiry ----
    /// The instant this server's confirmed lease expired, on the fence clock, while it stays expired:
    /// the lifecycle is `Live`, the fence is not lost and `bootMsNow` has reached the deadline. A
    /// renewal that commits with a start more than a TTL ago does not end the expiry, so the first expired
    /// deadline is kept until a renewal restores the lease. Empty otherwise.
    std::optional<uint64_t> leaseExpiredSinceBootMs() const;
    /// Text of the last failed renewal request; empty when no request failed since the
    /// last renewal that left the lease unexpired.
    String lastRenewFailure() const;
    /// The condition text for a request admitted under `admitted_generation` that is refused only
    /// because the lease expired; empty when the refusal has any other cause or there is none.
    std::optional<String> leaseExpiredRefusal(uint64_t admitted_generation) const;
    /// Counts each `PUT` of a renewal as it is sent and keeps the text of every failed `PUT` or resolve
    /// read, and wakes `waitUntilArmed` on each failure. Runs on the renewing thread, never under
    /// `driver_mutex`.
    void noteRenewRequest(const MountRenewRequestEvent & event) noexcept;

    /// Writes one `WARNING` per lease expiry, at the first request event after it. Lease thread only.
    void warnOnceIfLeaseExpired(const MountRenewRequestEvent & event) noexcept;

    /// TRUE once the pool has reached — or is being driven toward — a state on which the lease thread
    /// must stop: a published terminal `Vanished` intent (`vanished_intent` — set early by
    /// FORGET, or by a natural `enterVanished`, and already subsuming every settled `Vanished*` state since
    /// it is published before the state store) OR `IdentityLost` (a fail-loud TERMINAL state — no
    /// demoted observer; recovery is restart or FORGET). Consulted by `scheduleRemount`, by the arming rule
    /// and by the lease loop at every step boundary. (The GC scheduler applies the same three-way test through
    /// `Pool`.)
    bool remountTerminal() const
    {
        return vanished_intent.load(std::memory_order_acquire)
            || lifecycle() == PoolLifecycle::IdentityLost;
    }

    /// The inter-attempt sleep the mount plane runs on. A plain sleep would hold a stopping renewal
    /// for the whole capped backoff; this one wakes on the stop signal the lease thread watches.
    /// A remount request does not wake it: the wait runs out, at most one spacing draw for the loop's
    /// renewal. It shortens a stop, not a fence loss: the fence cannot see a stop request,
    /// so a woken operation still reissues unless its own liveness predicate refuses.
    void sleepInterruptibly(uint64_t ms);

    /// The mount fence's admission verdict, as `Fence::admit` expects it: may a request admitted under
    /// `admitted_generation`, still expected to be running `needed_ms` from now, proceed?
    /// `LostOrRearmed` when the fence is latched lost or a fresh lease incarnation replaced the one the
    /// caller was admitted under; `NoBudget` when the live lease has no room left for `needed_ms` plus
    /// the safety margin, so nothing is begun that could land after this node's fence may be gone.
    Fence::Admit admit(uint64_t admitted_generation, uint64_t needed_ms) const;

    /// The `writer_epoch` of the live mount incarnation. Bumped by `tryRemountOnce` (self-remount after a
    /// GC fence-out) — a `PartWriteTxn` minted under an older epoch fails closed on its next step.
    uint64_t liveWriterEpoch() const { return live_writer_epoch.load(std::memory_order_acquire); }

    /// ---- build registry ----
    /// Allocate a strictly-increasing `build_seq` and add it to the active set. A sequence is never
    /// reused or lowered, which lets the GC watermark advance monotonically.
    uint64_t allocateBuildSeq();
    /// Register the in-flight build so `dropNamespace`'s post-durable cancellation can reach it (weak_ptr).
    void registerInflightBuild(uint64_t seq, const PartWriteTxnPtr & build);
    /// Remove a build_seq from the active set + inflight map; idempotent (safe from publish/abandon/dtor).
    void retireBuildSeq(uint64_t seq);
    /// After the namespace-removal transaction is durable, cancel every in-flight build targeting `ns`.
    /// Live shared pointers are collected under `builds_mutex` and cancelled after releasing it, because
    /// cancellation may take a different path and must not run under the registry lock.
    void cancelInflightBuildsForNamespace(const RootNamespace & ns);

    /// ---- process epoch (identity) ----
    /// Mint the random nonzero process identity used by GC's equality check.
    void mintRandomProcessEpoch();
    /// Set `process_epoch` to the durable `writer_epoch`. The caller supplies the memory order because
    /// the initial writable claim and a later self-remount have different publication requirements.
    void setProcessEpoch(uint64_t v, std::memory_order order);
    /// Publish the live-incarnation `live_writer_epoch` with release ordering.
    void setLiveWriterEpoch(uint64_t v);

    /// ---- mount-lease renewer and the lease thread ----
    void installRenewer(UInt128 our_uuid, uint64_t writer_epoch, const std::function<uint64_t()> & now_ms);
    uint64_t startRenewer();
    void renewerReset();
    void startBackgroundWorkers(std::chrono::milliseconds period);
    void stopBackgroundWorkers();
    /// Latch a recovery generation for the lease thread. It never constructs a thread.
    void scheduleRemount();
    /// One interference report: trip the fence and request a remount in one `driver_mutex` step, so no
    /// reclaim can arm between the two. Raises no generation when a request that no reclaim has started
    /// serving is pending: the reclaim that serves it latches after this trip.
    void tripAndRequestRemount();
    bool scheduleRemountForTest();
    void beginShutdownForTest();
    /// Return how many remount requests were attempted, refused ones included: `scheduleRemount`,
    /// `tripAndRequestRemount` and terminal renewals. This is useful for testing the renewer's loss callback without starting a real recovery.
    uint64_t scheduleRemountCallCountForTest() const
    {
        return schedule_remount_calls_for_test.load(std::memory_order_relaxed);
    }

    bool workersRunningForTest() const;
    uint64_t remountRequestedGenerationForTest() const;

    /// Join the lease thread before an `Active` renewer may write its clean farewell.
    void finishTeardown(bool drained);

    /// Sleep through the injected test hook when present; otherwise use the production thread sleep.
    /// `Pool` claim observation and materialization grace waits share this seam so tests control both.
    void waitSleep(uint64_t ms) const;
    /// Swap the wait hook after construction -- a test that must change what a wait DOES partway
    /// through a scenario (e.g. driving a second incarnation's renewal from inside the observed
    /// incarnation's own poll) cannot express that through `PoolConfig::wait_sleep_fn` alone, since
    /// that value is fixed at open time. Unsynchronized against `waitSleep`'s `const` read of the same
    /// field: safe only called from the test's own thread before the lease thread starts (nothing else
    /// reads `config.wait_sleep_fn` concurrently with this write).
    void setWaitSleepForTest(std::function<void(uint64_t)> fn) { config.wait_sleep_fn = std::move(fn); }

    /// Forward renewer events to the injected sink. The sink is held by reference so it observes the
    /// owning pool's current event routing for the runtime's entire lifetime.
    void emitEvent(CasEvent && e) const { if (event_sink) event_sink(std::move(e)); }

private:
    /// A committed renewal that ended an expiry: how long the lease was expired, and the text of the
    /// last failed request.
    struct RestoredLease
    {
        uint64_t expired_ms = 0;
        String last_failure;
    };

    /// The renewal step of the lease thread: renew, consume the result under `driver_mutex`, then report
    /// it with no lock held.
    MountRenewResult renewOnce();
    MountRenewOperationEnvironment renewalEnvironment();
    std::optional<uint64_t> leaseExpiredAt(uint64_t now_boot_ms) const;
    /// Publishes a committed renewal's deadline. Requires `driver_mutex`. A deadline in the future ends
    /// the current run of trouble: it clears the failure text and, when the lease was expired, counts
    /// the restore and returns it for the caller to log after the unlock. Empty when the lease was not
    /// expired or is still expired.
    std::optional<RestoredLease> publishRenewedDeadline(uint64_t deadline_boot_ms);
    void consumeRenewResult(const MountRenewResult & result);
    void renewalLoop();
    ThreadFromGlobalPool makeWorker(std::function<void()> body);
    /// The renewal's liveness: no stop, no pending remount request, no terminal lifecycle (a published
    /// FORGET intent included); a lost fence alone does not end it. FALSE ends the renewal.
    bool renewalLive() const;
    /// Whether this node has already been asked to stop. Sampled ONCE, before the write, so a refusal
    /// caused by the stop cannot be mistaken for one that preceded it.
    bool renewalCancelled() const;
    void tripFenceWithoutOperationalLoss();
    /// Publish `deadline_boot_ms` as a fresh lease incarnation and open the fence. With `report_live`,
    /// `Live` is published before the fence opens, so a reader that sees the fence armed reads `Live`.
    void armFence(uint64_t deadline_boot_ms, bool report_live);
    /// The arming rule: no remount request pending, no stop requested, a lifecycle that is not
    /// terminal, and a deadline that admits a ref append now. Requires `driver_mutex`, the mutex that a
    /// stop, a request and a terminal publication take, so the check and the arm are one step.
    bool canArm(uint64_t deadline_boot_ms) const;
    /// `admit`'s budget verdict for a lease that ends at `deadline_boot_ms`, at `now_boot_ms`.
    Fence::Admit budgetAdmits(uint64_t deadline_boot_ms, uint64_t now_boot_ms, uint64_t needed_ms) const;
    /// What a ref append reserves: a write and the read that settles it, two attempt envelopes.
    uint64_t refAppendReservationMs() const;
    /// Whether a new loss needs a new remount generation. Requires `driver_mutex`. False only while a
    /// request that no reclaim has snapshotted is pending: the reclaim that serves it latches after the
    /// loss.
    bool lossNeedsNewRequest() const;
    /// Throws `LOGICAL_ERROR` when a lease thread runs and the caller is not it. Requires `driver_mutex`.
    void checkRenewerOwner() const;
    std::unique_lock<std::mutex> lockTerminalPublication();

    /// ---- injected environment (no `Pool` back-reference); initialized first, in this order ----
    BackendPtr backend_ptr;
    CasRequests & mount_requests;
    CasRequests & farewell_requests;
    CasRequests & lease_requests;
    const Layout & layout;
    MountConfig config;
    String server_root_id;
    const CasEventSink & event_sink;
    CasRequestBudget cas_request_budget;
    std::function<bool()> remount_attempt;

    /// Per-server build watermark. `process_epoch` is a random
    /// nonzero u64 minted once at open: GC checks it for EQUALITY (an object stamped with a different
    /// epoch is from a dead incarnation), never for ordering. next_build_seq is a strictly-increasing
    /// per-process counter (monotonicity is load-bearing — a seq is never reused or lowered);
    /// active_build_seqs holds the seqs of in-flight builds, so `minActive` yields the GC floor. The floor
    /// is published by the merged `mount_renewer`
    /// beat (there is no standalone watermark object anymore). ATOMIC because a self-remount re-stamps it
    /// (kept equal to `live_writer_epoch`) from the lease thread while `epoch`/`writerEpoch`
    /// may observe it; the ref-lane hot readers were moved to `liveWriterEpoch`, so this now backs only
    /// the identity accessors.
    std::atomic<uint64_t> process_epoch{0};
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;
    std::set<uint64_t> active_build_seqs;
    /// In-flight builds keyed by `build_seq`. `dropNamespace` upgrades these weak pointers only after its
    /// removal transaction is durable and cancels those targeting the removed namespace. The wiring owns
    /// the shared pointers, so an expired entry is simply skipped. Guarded by `builds_mutex`.
    std::map<uint64_t, std::weak_ptr<PartWriteTxn>> inflight_builds;

    /// Synchronous mount-lease protocol state. Constructed and started on a writable open after the
    /// owner/epoch/mount startup protocol; while the lease thread runs it is the renewer's only driver
    /// and publishes successful anchors or terminal loss into the local fence. After the lease thread
    /// joins, teardown releases an `Active` renewer so a same-server reopen can reclaim immediately.
    /// Null on a read-only open.
    std::unique_ptr<MountLeaseRenewer> mount_renewer;

    std::atomic<uint64_t> live_writer_epoch{0};

    /// One mutex/condition pair guards the lease thread's lifecycle, the cadence, the remount
    /// generations, and the terminal predicates paired with `driver_cv`. It is never held across a
    /// renewer or backend call, the reclaim, logging or a join.
    mutable std::mutex driver_mutex;
    mutable std::condition_variable driver_cv;
    bool workers_started = false;
    bool workers_stop_requested = false;
    /// The lease thread, from its first statement until `stopBackgroundWorkers` joins it.
    std::thread::id lease_thread_id;
    std::chrono::milliseconds renewal_period{0};
    uint64_t remount_requested_generation = 0;
    uint64_t remount_handled_generation = 0;
    /// The requested generation `beginReclaim` recorded; `armIfAdmissible` acknowledges it.
    uint64_t reclaim_generation = 0;
    ThreadFromGlobalPool renewal_worker;
    /// Counted entries into `scheduleRemount`; retained as a test-only observability seam.
    std::atomic<uint64_t> schedule_remount_calls_for_test{0};

    /// Local write fence. The unarmed default (`deadline_boot_ms = UINT64_MAX`, `lost = false`) permits
    /// mutation until a renewer supplies a real lease deadline or reports that the lease was lost. This
    /// is the gate at the ref-append mutation chokepoint.
    MountFence mount_fence;

    /// Fence-generation token (rev.7 [C2]): bumped by `tripMountLost` and `armMountFence`. See
    /// `fenceGeneration`/`checkFenceOrThrow`.
    std::atomic<uint64_t> fence_generation{0};
    std::function<void()> arm_mount_fence_interposition_hook_for_test;
    /// The first expired deadline of an expiry that a renewal committed past did not end. `UINT64_MAX`
    /// when there is none. Written by the renewal consumer and by `armMountFence`.
    std::atomic<uint64_t> lease_expired_at_boot_ms{std::numeric_limits<uint64_t>::max()};
    mutable std::mutex renew_failure_mutex;
    String last_renew_failure;
    /// The expiry instant `warnOnceIfLeaseExpired` last wrote a line for. A renewal that commits past
    /// its own deadline keeps the instant, so the same expiry is not reported twice.
    std::atomic<uint64_t> lease_expiry_warned_at_boot_ms{std::numeric_limits<uint64_t>::max()};

    /// The pool lifecycle condition (rev.7 §1). Starts `Live`. Non-terminal transitions
    /// (`noteLeaseLost`/`noteRemounted`) are lock-free compare-exchanges guarded by their exact
    /// predecessor state; terminal predicate publication is serialized with the lease loop's waits by
    /// `driver_mutex`, while the caller's `Pool::remount_mutex` serializes the higher-level remount flow.
    std::atomic<PoolLifecycle> pool_lifecycle{PoolLifecycle::Live};
    /// Terminal-intent latch (spec §3), published before the state store — by `enterVanished` for a
    /// natural transition, or EARLY (step 1) by `publishVanishedIntent` for FORGET. Only the fully-terminal
    /// `Vanished*` transition sets it — `IdentityLost` deliberately does NOT (rev.8 folds IdentityLost into
    /// the worker-exit boundary via `remountTerminal` instead). Consulted (with `IdentityLost`) by
    /// `remountTerminal`, so a terminal pool's runtime consumer never schedules a remount and the lease
    /// loop exits at its next step boundary — no claim/allocate/write after the pool is (being driven) terminal.
    std::atomic<bool> vanished_intent{false};
    /// Idempotency guard for the terminal STATE transition (`enterVanished`'s body). Distinct from
    /// `vanished_intent`: FORGET publishes that intent latch at step 1, so it can no longer serve as the
    /// "state transition already done" flag. Published last, after the no-throw reason move and terminal
    /// stores complete under `driver_mutex`; a preparation exception therefore leaves it clear so a later
    /// `enterVanished` can retry the whole transition.
    std::atomic<bool> terminal_state_published{false};
    /// The reason recorded by the winning `enterVanished` (see `vanishedReason`). Written once, BEFORE the
    /// `pool_lifecycle` release-store, and immutable thereafter — so a reader that acquire-observes a
    /// terminal state also observes this string. Empty when no terminal transition has run.
    String vanished_reason;

    /// Wall-clock second at which the current non-`Live` lifecycle state was entered; 0 while `Live` (see
    /// `lifecycleSinceWallS`). Set at every lifecycle edge with a release-store ordered before the
    /// `pool_lifecycle` transition it accompanies.
    std::atomic<int64_t> lifecycle_since_wall_s{0};
};

}
