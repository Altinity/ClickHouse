#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
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
#include <set>

namespace DB::Cas
{

class PartWriteTxn;
using PartWriteTxnPtr = std::shared_ptr<PartWriteTxn>;

/// The pool-level lifecycle condition (rev.7 §1) a `Pool` moves through as its shared backing changes
/// underfoot. It is distinct from the storage-level `Constructing/Started/ShutDown` and from the
/// `MountState` the metadata storage tracks (both of which stay in force until Task 15). Ordering of the
/// enumerators is not significant; membership tests do the work.
///   - `Live`             — the steady state; the mount lease is (or was last) held.
///   - `TransientNotLive` — the lease was lost; access is uncertain and a self-remount retries. The §2
///                          `Present`+identity-match recovery rule fires only from here (or `Live`).
///   - `IdentityLost`     — the pool sentinels are gone while data remains (a live erase in flight):
///                          fail-loud, but NON-absorbing ([C1]) — a low-rate observer keeps probing and
///                          may promote one-way to `VanishedErased` (Task 6). Matching-sentinel
///                          reappearance does NOT auto-revive it ([D3]); only a restart recovers.
///   - `Vanished*`        — fully terminal truth: the data root was erased/replaced, or the disk was
///                          decommissioned by `FORGET`. Store-class access fails loud from here.
enum class PoolLifecycle : uint8_t
{
    Live,
    TransientNotLive,
    IdentityLost,
    VanishedErased,
    VanishedReplaced,
    VanishedForgotten,
};

/// Configuration owned by `CasMountRuntime`. `PoolConfig::mountConfig` projects the flat pool settings
/// into this value, keeping the pool's existing configuration interface unchanged while allowing this
/// lower-layer header to describe its own dependencies.
struct MountConfig
{
    std::chrono::milliseconds mount_lease_ttl_ms{30000};
    std::chrono::milliseconds mount_renew_period{10000};
    uint64_t materialization_grace_ms = 30000;
    /// When false, tests drive `renewWatermarkOnce` explicitly. In production this flag enables both
    /// the merged mount-lease/build-watermark heartbeat and self-remount recovery.
    bool background_watermark = false;
    std::function<uint64_t()> boot_ms_fn = {};
    std::function<void(uint64_t)> wait_sleep_fn = {};
};

/// Local, in-memory write fence. It is deliberately not checked by reading the object store for every
/// write: the `MountLeaseKeeper` is the sole lease reader/renewer. A successful renewal translates the
/// durable `expires_at_ms` into `deadline_boot_ms`; a foreign owner, newer `writer_epoch`, or failed
/// renewal latches `lost`. Mutable operations are allowed only while the latch is clear and the local
/// deadline has not passed. The `writer_epoch` is the durable fencing token.
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
/// the `MountLeaseKeeper`, local `MountFence`, build watermark and in-flight build registry,
/// `live_writer_epoch`, unclean-boundary marker, and self-remount thread. `Pool` retains the higher-level
/// claim/recovery sequence and its `remount_mutex`; in particular, the runtime does not acquire or own
/// the ref-ledger locks. The runtime receives its backend, layout, configuration, event sink, request
/// budget, and a callback that performs one pool-level remount attempt, so it has no `Pool` back-reference.
/// `Pool` delegates preserve the existing callers and test seams.
class CasMountRuntime
{
public:
    CasMountRuntime(
        BackendPtr backend_ptr_,
        const Layout & layout_,
        MountConfig config_,
        String server_root_id_,
        const CasEventSink & event_sink_,
        CasRequestBudget cas_request_budget_,
        /// One pool-level recovery attempt. The callback captures the owning `Pool` and is invoked only
        /// after construction, from the recovery thread.
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
    /// Renew the merged mount heartbeat once, including its build-watermark floor. A read-only runtime
    /// has no keeper and fails with a logical exception rather than fabricating a heartbeat.
    void renewWatermarkOnce();

    /// ---- local write fence ----
    /// Return whether a mutable operation may start under the locally observed lease state.
    bool mayMutate() const;
    /// Permanently latch the local fence as lost for this runtime incarnation.
    void tripMountLost();
    /// Publish the BOOTTIME deadline from a successful lease renewal.
    void setMountDeadline(uint64_t deadline_boot_ms);
    /// Arm a new lease incarnation and clear any loss latched for the prior incarnation.
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
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
    /// iteration, not just the first attempt. Throws the typed transient error (`INVALID_STATE`) when the
    /// fence is not currently held or the generation moved since admission; the caller's write must never
    /// reach the backend in either case.
    void checkFenceOrThrow(uint64_t admitted_generation) const;

    /// RAII marker for one durable-effect operation's admission-through-resolution lifetime.
    /// Construction increments `outstanding_durable_requests`; destruction -- including on an
    /// exception-unwind path -- decrements it. This is what the erasure-proof gate waits to observe at
    /// (and staying at) zero before treating the pool prefix as fully durable-lane-quiescent ([D1]): ref
    /// lanes settling alone is not enough, because an admitted plain-object/staging request can still be
    /// in flight.
    class DurableRequestGuard
    {
    public:
        explicit DurableRequestGuard(CasMountRuntime & runtime_) : runtime(&runtime_)
        {
            runtime->outstanding_durable_requests.fetch_add(1, std::memory_order_acq_rel);
        }

        ~DurableRequestGuard()
        {
            if (runtime)
                runtime->outstanding_durable_requests.fetch_sub(1, std::memory_order_acq_rel);
        }

        DurableRequestGuard(const DurableRequestGuard &) = delete;
        DurableRequestGuard & operator=(const DurableRequestGuard &) = delete;

        DurableRequestGuard(DurableRequestGuard && other) noexcept : runtime(other.runtime)
        {
            other.runtime = nullptr;
        }
        DurableRequestGuard & operator=(DurableRequestGuard &&) = delete;

    private:
        CasMountRuntime * runtime;
    };

    /// Count of durable-effect operations currently admitted (their `DurableRequestGuard` still alive).
    uint64_t outstandingDurableRequests() const { return outstanding_durable_requests.load(std::memory_order_acquire); }

    /// The `bootMsNow()` instant at which this runtime FIRST lost its lease (the `Live -> TransientNotLive`
    /// transition recorded in `noteLeaseLost`), or 0 if the lease was never lost. This is the anchor for
    /// the erasure-proof minimum-grace gate ([D1]): the proof window may not open until at least
    /// max(materialization grace, the backend's total per-operation request-timeout budget) has elapsed
    /// since this instant, so any durable-effect write admitted under the dying incarnation has provably
    /// drained or been dropped. Recorded once (on the first successful `Live -> TransientNotLive`
    /// compare-exchange) and never advanced, so it reflects the ACTUAL lease loss — not a later observer
    /// tick — in production (`tripMountLost -> noteLeaseLost` fires it before the observer even runs).
    uint64_t fenceTripBootMs() const { return fence_trip_boot_ms.load(std::memory_order_acquire); }

    /// ---- pool lifecycle condition (rev.7 §1, spec §§1-3); enum at namespace scope below ----
    /// Atomic read of the current lifecycle (acquire).
    PoolLifecycle lifecycle() const { return pool_lifecycle.load(std::memory_order_acquire); }
    /// Whether the pool has reached one of the three fully-terminal `Vanished` values.
    bool isVanished() const;

    /// Non-terminal lease-loss transition: `Live -> TransientNotLive`. Idempotent and lock-free; a
    /// compare-exchange FROM `Live` only, so it never downgrades a terminal state. `tripMountLost`
    /// calls this (the lease-loss primitive), and the remount loop's identity gate calls it as its
    /// first step so a direct/forced remount attempt has a valid non-terminal predecessor state.
    void noteLeaseLost();
    /// Non-terminal recovery transition: `TransientNotLive -> Live`. Called after a self-remount
    /// reclaimed a fresh incarnation. A compare-exchange FROM `TransientNotLive` only, so it NEVER
    /// revives `IdentityLost`/`Vanished` ([D3]).
    void noteRemounted();

    /// One-way terminal transition to `IdentityLost`, from `TransientNotLive` only (a compare-exchange
    /// FROM `TransientNotLive`, so it is idempotent and cannot fire from `Live`/`Vanished`). On the
    /// transition it emits ONE WARN and one `CasIdentityLost` ProfileEvent. `IdentityLost` is NON-absorbing
    /// per [C1]: it does NOT publish the terminal-intent latch, so the lifecycle observer keeps running
    /// and Task 6 may later promote it to `VanishedErased`. Must be called under the caller's remount
    /// serialization (Pool::remount_mutex).
    void enterIdentityLost();
    /// Test seam: force the lifecycle condition directly to `lc`, bypassing the natural transition
    /// preconditions (used by the operation-gate tests to pin each class × state cell without driving a
    /// full remount/erase sequence). For a `Vanished*` value it also latches `vanished_intent`, so the
    /// forced state is indistinguishable from a naturally-reached one. Never used in production.
    void setLifecycleForTest(PoolLifecycle lc);

    /// One-way transition to a fully-terminal `Vanished` value (spec §3). Publishes the terminal-intent
    /// latch FIRST (so the keeper stops scheduling remounts and the remount loop exits at its next step
    /// boundary), then the state, then ONE WARN + one `CasDataRootVanished` ProfileEvent. Idempotent: the
    /// first terminal transition wins. `which` MUST be one of the three `Vanished*` values. Threads exit
    /// their own loops; the joins happen in `~Pool` for a natural transition. Must be called under the
    /// caller's remount serialization (Pool::remount_mutex).
    void enterVanished(PoolLifecycle which, const String & reason);

    /// Extends `mayMutate` with a remaining-budget check. A ref-log attempt is refused unless the
    /// current lease has room for its configured timeout and safety margin, so work is not started when
    /// it cannot plausibly finish before the fence expires.
    bool refAppendFenceOk() const;

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

    /// ---- mount-lease keeper (owned) ----
    /// Construct the `MountLeaseKeeper` adopting (our_uuid, writer_epoch) and wire its fence callbacks
    /// (renew-ok refreshes the fence deadline; on-lost latches the fence + arms a self-remount) plus its
    /// build-watermark `minActive` reader, and event sink. `keeperStart` is separate so pool claim
    /// orchestration can catch `MountFencedException`, discard the keeper, allocate a fresh epoch, and
    /// retry the claim.
    void installKeeper(UInt128 our_uuid, uint64_t writer_epoch, const std::function<uint64_t()> & now_ms);
    /// Adopt the already-claimed mount slot; on return the adoption is durable.
    void keeperStart();
    /// Discard a keeper after a refused adoption so the caller can retry with a fresh epoch.
    void keeperReset();
    /// Start periodic lease and watermark renewal.
    void keeperStartBackground(std::chrono::milliseconds period);
    /// Stop periodic renewal; safe to call more than once.
    void keeperStopBackground();
    bool hasKeeper() const { return static_cast<bool>(mount_keeper); }

    /// Record the `writer_epoch` of a writable claim or self-remount that reclaimed an unclean
    /// predecessor. This relaxed per-epoch high-water mark is compared for exact equality by the ref-table
    /// recovery seal gate; zero means no such boundary has been observed.
    void setUncleanEpochBoundarySeenAt(uint64_t v);
    uint64_t uncleanEpochBoundarySeenAtRelaxed() const
    {
        return unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed);
    }

    /// Return whether any writable claim or self-remount in this runtime's lifetime crossed an unclean
    /// predecessor boundary. This sticky diagnostic is intentionally coarser than the per-epoch value
    /// used by the seal-gating decision.
    bool uncleanEpochBoundarySeenForTest() const
    {
        return unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed) != 0;
    }

    /// Return whether `ns` belongs to this server root and the current live epoch is exactly the epoch
    /// that reclaimed an unclean predecessor.
    bool ownsAndSawUncleanBoundaryFor(const RootNamespace & ns) const
    {
        const String & prefix = server_root_id;
        const String & full = ns.string();
        return full.size() > prefix.size() && full.starts_with(prefix)
            && full[prefix.size()] == '/'
            && unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed) == liveWriterEpoch();
    }

    /// ---- self-remount recovery ----
    /// On a lost lease, arm a recovery thread when background operation is enabled. It retries the
    /// pool-level remount callback with exponential backoff until success or teardown.
    void scheduleRemount();
    /// Test seam: drive the arm/refuse path directly. Returns true iff a recovery thread is armed after.
    bool scheduleRemountForTest();
    /// Test seam: latch the shutdown gate without joining or otherwise tearing down the runtime.
    void beginShutdownForTest();
    /// Return how many times `scheduleRemount` was entered, including calls refused by the background
    /// setting. This is useful for testing the keeper's loss callback without starting a real recovery.
    uint64_t scheduleRemountCallCountForTest() const
    {
        return schedule_remount_calls_for_test.load(std::memory_order_relaxed);
    }

    /// ---- teardown ----
    /// Stop and join the self-remount thread before retiring the keeper; otherwise it could recreate the
    /// keeper while teardown is in progress.
    void stopRemountThread();
    /// Retire the merged heartbeat. When `drained` is true, publish the clean farewell; otherwise stop
    /// background renewal without writing a terminal marker, because unresolved writes must not be
    /// certified as clean. Finish with a second recovery-thread join to close the final callback window.
    void finishTeardown(bool drained);

    /// Sleep through the injected test hook when present; otherwise use the production thread sleep.
    /// `Pool` claim observation and materialization grace waits share this seam so tests control both.
    void waitSleep(uint64_t ms) const;

    /// Forward keeper events to the injected sink. The sink is held by reference so it observes the
    /// owning pool's current event routing for the runtime's entire lifetime.
    void emitEvent(CasEvent && e) const { if (event_sink) event_sink(std::move(e)); }

private:
    /// ---- injected environment (no `Pool` back-reference); initialized first, in this order ----
    BackendPtr backend_ptr;
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
    /// is published by the merged `mount_keeper`
    /// beat (there is no standalone watermark object anymore). ATOMIC because a self-remount re-stamps it
    /// (kept equal to `live_writer_epoch`) off the background remount thread while `epoch`/`writerEpoch`
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

    /// Mount-lease heartbeat. Constructed and started on a writable
    /// open AFTER the owner/epoch/mount startup protocol; renews the mount lease async off the write
    /// path and drives the local write fence (deadline on each successful renew, `tripMountLost` on a
    /// superseded/foreign touch). Teardown stops it, whose `terminate` retires the lease (so a
    /// same-server reopen can immediately reclaim). Null on a read-only open.
    std::unique_ptr<MountLeaseKeeper> mount_keeper;

    std::atomic<uint64_t> live_writer_epoch{0};
    std::mutex remount_thread_mutex;       /// guards the thread handle below
    std::atomic<bool> remount_running{false};
    std::atomic<bool> remount_stop{false};
    std::atomic<bool> remount_shutting_down{false};   /// latched at teardown top; scheduleRemount refuses to re-arm during teardown
    std::condition_variable remount_cv;
    std::mutex remount_cv_mutex;
    ThreadFromGlobalPool remount_thread;
    /// Counted entries into `scheduleRemount`; retained as a test-only observability seam.
    std::atomic<uint64_t> schedule_remount_calls_for_test{0};

    /// Local write fence. The unarmed default (`deadline_boot_ms = UINT64_MAX`, `lost = false`) permits
    /// mutation until a keeper supplies a real lease deadline or reports that the lease was lost. This
    /// is the gate at the ref-append mutation chokepoint.
    MountFence mount_fence;

    /// Fence-generation token (rev.7 [C2]): bumped by `tripMountLost` and `armMountFence`. See
    /// `fenceGeneration`/`checkFenceOrThrow`.
    std::atomic<uint64_t> fence_generation{0};
    /// Count of currently-admitted durable-effect operations (rev.7 [D1]). See `DurableRequestGuard`.
    std::atomic<uint64_t> outstanding_durable_requests{0};
    /// `bootMsNow()` at the first `Live -> TransientNotLive` transition (rev.7 [D1]); 0 until then. The
    /// erasure-proof grace gate measures elapsed-since-fence-trip from this. See `fenceTripBootMs`.
    std::atomic<uint64_t> fence_trip_boot_ms{0};

    /// The pool lifecycle condition (rev.7 §1). Starts `Live`. Non-terminal transitions
    /// (`noteLeaseLost`/`noteRemounted`) are lock-free compare-exchanges guarded by their exact
    /// predecessor state; the terminal transitions (`enterIdentityLost`/`enterVanished`) are serialized
    /// by the caller's `Pool::remount_mutex` and made race-safe against the keeper thread's concurrent
    /// `noteLeaseLost` by the compare-exchange/latch discipline in the .cpp.
    std::atomic<PoolLifecycle> pool_lifecycle{PoolLifecycle::Live};
    /// Terminal-intent latch (spec §3), published FIRST by `enterVanished` before the state store. Only
    /// the fully-terminal `Vanished` transition sets it — `IdentityLost` is NON-absorbing ([C1]) and
    /// deliberately does NOT, so its observer keeps running. Checked by the keeper callback before
    /// scheduling a remount and by the remount loop at every step boundary, so a vanished pool never
    /// claims, allocates, or writes again.
    std::atomic<bool> vanished_intent{false};

    /// The `writer_epoch` that most recently reclaimed an unclean predecessor, or zero if none did. This
    /// is a per-epoch high-water mark rather than a sticky boolean: ref-table recovery seals only when
    /// its own epoch exactly matches this value, so a table is sealed only for the specific transition
    /// whose dead region it recovered. `uncleanEpochBoundarySeenForTest` exposes the coarser lifetime
    /// diagnostic.
    std::atomic<uint64_t> unclean_epoch_boundary_seen_at{0};
};

}
