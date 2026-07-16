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

class Build;
using BuildPtr = std::shared_ptr<Build>;

/// Per-owner config slice for the mount-runtime subsystem (spec §PoolConfig Slices). A PROJECTION of the
/// flat `PoolConfig` fields, built on demand by `PoolConfig::mountConfig` and passed BY VALUE to
/// `CasMountRuntime` (Phase 3.5). Lives here (not in `CasStore.h`) so `CasMountRuntime.h` -- a lower-layer
/// header `CasStore.h` includes for the `mount_runtime` member -- carries its own config type; the flat
/// `PoolConfig` fields stay put so every external caller (wiring, tests) that sets them is unchanged.
struct MountConfig
{
    std::chrono::milliseconds mount_lease_ttl_ms{30000};
    std::chrono::milliseconds mount_renew_period{10000};
    uint64_t materialization_grace_ms = 30000;
    /// tests drive renewOnce explicitly; gates the merged heartbeat's background thread AND the
    /// self-remount recovery thread (`scheduleRemount`).
    bool background_watermark = false;
    std::function<uint64_t()> boot_ms_fn = {};
    std::function<void(uint64_t)> wait_sleep_fn = {};
};

/// Local write fence (spec §write-fence, Phase 0 Task 6). A PURELY LOCAL, in-memory check — never a
/// per-write S3 read. The renewer (the MountLeaseKeeper, Task 7) is the only thing that touches S3 for
/// the lease; on each successful renew it refreshes `deadline_boot_ms` (an S3 `expires_at_ms`
/// translated to a CLOCK_BOOTTIME instant, so a wall-clock change cannot extend it), and on any
/// supersession (a foreign uuid / a newer writer_epoch / an unrenewable expired lease) it latches
/// `lost`. A mutable op proceeds only while `!lost` AND the deadline has not passed. `writer_epoch` is
/// the fencing token.
///
/// WHY BOOTTIME, NOT MONOTONIC: `CLOCK_MONOTONIC` does not advance while a VM is suspended, so a
/// resumed sleeper would compute the same "not yet expired" verdict it had before the nap even though
/// wall time (and the GC leader's fence-out) moved far ahead — it could mutate the shared state under
/// a live writer. `CLOCK_BOOTTIME` includes suspend time, so a resumed sleeper sees its fence expired.
/// Container pause is already safe under either clock (the process is frozen, so no local check runs).
struct MountFence
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    /// Permissive default (set in the Store ctor): until something arms a real lease deadline, the
    /// fence allows mutations — so existing tests and pre-Task-7 behavior are unchanged. UINT64_MAX =
    /// unarmed (never expires); otherwise a CLOCK_BOOTTIME-milliseconds instant.
    std::atomic<uint64_t> deadline_boot_ms{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> lost{false};
};

/// The mount / write-fence / build-watermark / self-remount runtime (spec §Decomposition), extracted
/// from `Cas::Store` (Phase 3.5 source-layout). It owns the `MountLeaseKeeper` (the mount-lease
/// heartbeat), the local `MountFence`, the per-server build watermark (`process_epoch` +
/// `builds_mutex`/`next_build_seq`/`active_build_seqs`/`inflight_builds`), the live-incarnation
/// `live_writer_epoch`, the unclean-epoch-boundary high-water-mark, and the self-remount recovery thread
/// (with its own thread-lifecycle locks). `Store` keeps the CLAIM/RECOVERY orchestration
/// (`open`/`mountWritable`/`openForDecommission`/`tryRemountOnce`) and drives these owned PRIMITIVES;
/// `remount_mutex` (which serializes `tryRemountOnce`'s orchestration) therefore STAYS on `Store`.
///
/// PURE relocation from `Store` -- zero logic, flow, or lock change. Environment is injected by
/// reference/value (no `Store &` back-reference): the backend, the layout, the `MountConfig` slice, the
/// `server_root_id`, the event sink, the pool `CasRequestBudget`, and the `remount_attempt` callback
/// (== `Store::tryRemountOnce`, the body the recovery thread loops on). `Store` keeps thin public
/// delegates for every currently-public method so the wiring, `Build`, `Gc`, the `ref_ledger` callbacks,
/// and every test call site are unchanged.
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
        /// The body the self-remount recovery thread loops on (== `Store::tryRemountOnce`). Bound at
        /// construction; captures the owning `Store` (invoked only at runtime, post-construction).
        std::function<bool()> remount_attempt_);

    /// ---- per-server watermark surface (spec 2026-06-16-ca-build-watermark) ----
    /// process_epoch: random nonzero per Store (process). GC checks epoch EQUALITY, never ordering.
    uint64_t epoch() const { return process_epoch.load(std::memory_order_acquire); }
    uint64_t writerEpoch() const { return process_epoch.load(std::memory_order_acquire); }
    /// The GC floor: the oldest in-flight build_seq, or next_build_seq when no build is active (so a
    /// quiescent server's watermark floor advances to the next-to-be-allocated seq). Locks builds_mutex.
    uint64_t minActive();
    /// Test/assertion accessor for the next-to-allocate build_seq under the lock.
    uint64_t peekNextBuildSeq();
    /// Renew the merged heartbeat once (bump seq, refresh min_active from the live callback, stamp a
    /// fresh expires_at_ms). The build-watermark floor rides this beat.
    void renewWatermarkOnce();

    /// ---- local write fence (spec §write-fence, Phase 0 Task 6) ----
    bool mayMutate() const;
    void tripMountLost();
    void setMountDeadline(uint64_t deadline_boot_ms);
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
    /// The fence clock: CLOCK_BOOTTIME in milliseconds (includes VM-suspend time, unlike
    /// CLOCK_MONOTONIC — see `MountFence`). Consults the injected `config.boot_ms_fn` if set (tests),
    /// otherwise `bootMs`.
    uint64_t bootMsNow() const;
    /// The real boot clock: CLOCK_BOOTTIME in milliseconds. Static so tests can compose it.
    static uint64_t bootMs();

    /// RFC pre-attempt fence check: extends `mayMutate` with the REMAINING budget check -- an attempt
    /// is not even started unless there is enough of the mount lease left for one more attempt_timeout
    /// plus the lease safety margin. Passed as `fence_ok` to every `CasRequestController` call the
    /// ref-log writer path makes (via the `ref_ledger`'s `fence_ok_fn`, which `Store` binds to this).
    bool refAppendFenceOk() const;

    /// The writer_epoch of the LIVE mount incarnation. Bumped by `tryRemountOnce` (self-remount after a
    /// GC fence-out) — a `Build` minted under an older epoch fails closed on its next step.
    uint64_t liveWriterEpoch() const { return live_writer_epoch.load(std::memory_order_acquire); }

    /// ---- build registry (populated by Store::startBuild, retired by the Build) ----
    /// Allocate a strictly-increasing build_seq and add it to the active set.
    uint64_t allocateBuildSeq();
    /// Register the in-flight build so `dropNamespace`'s post-durable cancellation can reach it (weak_ptr).
    void registerInflightBuild(uint64_t seq, const BuildPtr & build);
    /// Remove a build_seq from the active set + inflight map; idempotent (safe from publish/abandon/dtor).
    void retireBuildSeq(uint64_t seq);
    /// Cancel every in-flight build targeting `ns` once its removal transaction is durable (spec
    /// §Namespace Removal: "cancels local builds"). Collects the live shared_ptrs under `builds_mutex`
    /// and cancels OUTSIDE the lock. Injected into `ref_ledger` (via a Store delegate) as the
    /// `cancel_inflight_builds` callback.
    void cancelInflightBuildsForNamespace(const RootNamespace & ns);

    /// ---- process epoch (identity) ----
    /// Mint the random NONZERO `process_epoch` (`Store::open`'s read-only prologue). GC checks it for
    /// equality only (a different epoch == a dead incarnation).
    void mintRandomProcessEpoch();
    /// Set `process_epoch` to the durable `writer_epoch` (the identity bridge). `order` is caller-chosen
    /// so the two pre-move call sites keep their EXACT memory ordering: `mountWritable` uses relaxed, the
    /// `tryRemountOnce` epoch bump uses release.
    void setProcessEpoch(uint64_t v, std::memory_order order);
    /// Set the live-incarnation `live_writer_epoch` (release, matching both pre-move call sites).
    void setLiveWriterEpoch(uint64_t v);

    /// ---- mount-lease keeper (owned) ----
    /// Construct the `MountLeaseKeeper` adopting (our_uuid, writer_epoch) and wire its fence callbacks
    /// (renew-ok refreshes the fence deadline; on-lost latches the fence + arms a self-remount) plus its
    /// build-watermark `minActive` reader and the event sink -- all captured on THIS runtime. Encapsulates
    /// the pre-move keeper-construction block verbatim; `keeperStart` is separate so `Store`'s claim
    /// orchestration can catch `MountFencedException` and retry (`keeperReset` + a fresh epoch).
    void installKeeper(UInt128 our_uuid, uint64_t writer_epoch, const std::function<uint64_t()> & now_ms);
    void keeperStart();                                       /// adopt the slot (durable when it returns)
    void keeperReset();                                       /// drop the keeper (fence-recovery retry)
    void keeperStartBackground(std::chrono::milliseconds period);
    void keeperStopBackground();                              /// idempotent
    bool hasKeeper() const { return static_cast<bool>(mount_keeper); }

    /// The `writer_epoch` a writable open/self-remount JUST reclaimed over an unclean predecessor -- a
    /// per-epoch high-water-mark (relaxed), 0 = never. Set by `Store`'s claim orchestration.
    void setUncleanEpochBoundarySeenAt(uint64_t v);
    uint64_t uncleanEpochBoundarySeenAtRelaxed() const
    {
        return unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed);
    }

    /// rev.6 Task 6 / fix-round F2: true once ANY writable open/self-remount of this incarnation's
    /// lifetime reclaimed a mount over an unclean predecessor -- a lifetime diagnostic signal, sticky by
    /// design (the test/observability seam; the seal-gating decision uses the per-epoch high-water-mark).
    bool uncleanEpochBoundarySeenForTest() const
    {
        return unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed) != 0;
    }

    /// rev.6 fix-round F1: TRUE iff `ns` is scoped under this runtime's own `server_root_id` AND this
    /// incarnation's live epoch's predecessor died uncleanly (F2's per-epoch high-water-mark).
    bool ownsAndSawUncleanBoundaryFor(const RootNamespace & ns) const
    {
        const String & prefix = server_root_id;
        const String & full = ns.string();
        return full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0
            && full[prefix.size()] == '/'
            && unclean_epoch_boundary_seen_at.load(std::memory_order_relaxed) == liveWriterEpoch();
    }

    /// ---- self-remount recovery (liveness counterpart of the fence-out safety rule) ----
    /// `scheduleRemount` (called from the keeper's renew-failure path in production; gated on
    /// `background_watermark` like every background thread) runs the `remount_attempt` callback
    /// (`Store::tryRemountOnce`) with exponential backoff until it succeeds or teardown begins.
    void scheduleRemount();
    /// Test seam: drive the arm/refuse path directly. Returns true iff a recovery thread is armed after.
    bool scheduleRemountForTest();
    /// Test seam: latch `remount_shutting_down` exactly as teardown does, WITHOUT tearing down.
    void beginShutdownForTest();
    /// Task 11 review follow-up: how many times `scheduleRemount` has been ENTERED (counted
    /// unconditionally as its first statement, before the `background_watermark` early-return).
    uint64_t scheduleRemountCallCountForTest() const
    {
        return schedule_remount_calls_for_test.load(std::memory_order_relaxed);
    }

    /// ---- teardown (driven by ~Store, in this order) ----
    /// Stop + join the self-remount recovery thread FIRST (it may otherwise re-create the keeper).
    void stopRemountThread();
    /// Retire the merged heartbeat: `drained` (from the ref-ledger drain) selects the clean farewell
    /// (`stop`) vs the fail-closed no-terminal-op (`stopBackground`). Ends with a belt-and-suspenders
    /// remount-thread re-join.
    void finishTeardown(bool drained);

    /// rev.6 Task 6 / S13 observation loop: the ONE seam both `Store::open`'s mount-claim observation
    /// poll and the `materialization_grace_ms` wait go through. Routes through `config.wait_sleep_fn`
    /// when a test injected one, else a real `std::this_thread::sleep_for` (production, unchanged).
    void waitSleep(uint64_t ms) const;

    /// The keeper's event callback routes here (mirrors `Store::emitEvent`): the injected sink reference
    /// observes the late `setEventSink` assignment exactly as the pre-move `s->emitEvent` capture did.
    void emitEvent(CasEvent && e) const { if (event_sink) event_sink(std::move(e)); }

private:
    /// ---- injected environment (no `Store` back-reference); initialized first, in this order ----
    BackendPtr backend_ptr;
    const Layout & layout;
    MountConfig config;
    String server_root_id;
    const CasEventSink & event_sink;
    CasRequestBudget cas_request_budget;
    std::function<bool()> remount_attempt;

    /// Per-server build watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random
    /// nonzero u64 minted once at open: GC checks it for EQUALITY (an object stamped with a different
    /// epoch is from a dead incarnation), never for ordering. next_build_seq is a strictly-increasing
    /// per-process counter (monotonicity is load-bearing — a seq is never reused or lowered);
    /// active_build_seqs holds the seqs of in-flight builds, so minActive yields the GC floor. After
    /// the ack-floor merge (spec 2026-07-02) the floor is published by the merged `mount_keeper`
    /// beat (there is no standalone watermark object anymore). ATOMIC because a self-remount re-stamps it
    /// (kept equal to `live_writer_epoch`) off the background remount thread while `epoch`/`writerEpoch`
    /// may observe it; the ref-lane hot readers were moved to `liveWriterEpoch`, so this now backs only
    /// the identity accessors.
    std::atomic<uint64_t> process_epoch{0};
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;
    std::set<uint64_t> active_build_seqs;
    /// In-flight builds keyed by build_seq (mirrors `active_build_seqs`' lifecycle: populated in
    /// `startBuild`, removed in `retireBuildSeq`). `dropNamespace` upgrades these weak_ptrs AFTER its
    /// removal transaction is durable and cancels those targeting the removed namespace (spec §Namespace
    /// Removal: "cancels local builds"). weak_ptr because the wiring owns the shared_ptr; an expired entry
    /// (a build already destroyed) is simply skipped. Guarded by builds_mutex.
    std::map<uint64_t, std::weak_ptr<Build>> inflight_builds;

    /// Mount-lease heartbeat (spec §mount-safety, Phase 0 Task 7). Constructed + started on a writable
    /// open AFTER the owner/epoch/mount startup protocol; renews the mount lease async off the write
    /// path and drives the local write fence (deadline on each successful renew, `tripMountLost` on a
    /// superseded/foreign touch). Teardown stops it, whose terminate() retires the lease (so a
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
    /// Task 11 review follow-up: see `scheduleRemountCallCountForTest`.
    std::atomic<uint64_t> schedule_remount_calls_for_test{0};

    /// Local write fence (spec §write-fence). Permissive by default (deadline = UINT64_MAX,
    /// lost = false), so mayMutate is true until Task 7 arms it with a real lease deadline and the
    /// renewer trips it. Gates the ref-append mutate chokepoint.
    MountFence mount_fence;

    /// rev.6 Task 6, fix-round F2: the `writer_epoch` a writable open/self-remount JUST reclaimed over
    /// an unclean predecessor (`MountPriorState::Fenced` or `UncleanObserved`) -- 0 = never. A per-epoch
    /// HIGH-WATER-MARK, not a sticky bool: `ensureRefTableRecovered`'s seal gate (Task 8) compares this
    /// against the table's OWN `my_epoch` for EXACT equality, so only a table recovered while THIS
    /// SPECIFIC transition's dead region is still current gets sealed. `uncleanEpochBoundarySeenForTest`
    /// keeps the coarse "ever" reading for observability.
    std::atomic<uint64_t> unclean_epoch_boundary_seen_at{0};
};

}
