#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// The writer ref-log / ref-table subsystem (spec §Writer Algorithms / §Table State / §Decomposition),
/// extracted from `Cas::Store` (Phase 3.4 source-layout). It owns the whole-table ref cache
/// (`ref_tables`), the append lane (flat-combining flush leader + wedge protocol), snapshot publication,
/// stale-precommit sweep, cache-budget eviction, and the remount/shutdown drain coordination -- together
/// with the two mutexes that guard them (`ref_queue_mutex` and the per-table `state_mutex`) and the
/// CAS retry controller the ref-log writer path uses.
///
/// PURE relocation from `Store` -- zero logic change. Environment is injected by reference/value (no
/// `Store &` back-reference): the backend, the layout, the `RefLedgerConfig` slice, the event sink, the
/// pool-level `CasRequestBudget`, plus callbacks for the mount/watermark state that STAYS on `Store`
/// (live writer epoch, the append/publish fence predicate, the boot clock, `mayMutate`, the unclean-
/// epoch-boundary high-water-mark, the impossible-interference anomaly reaction, the owner pin for the
/// detached publish task, and the in-flight-build cancellation on namespace removal). `Store` keeps thin
/// public delegates for every currently-public method so wiring/tests are unchanged.
class CasRefLedger
{
public:
    CasRefLedger(
        BackendPtr backend_ptr,
        const Layout & layout_,
        RefLedgerConfig config_,
        const CasEventSink & event_sink_,
        CasRequestBudget cas_request_budget_,
        /// The RAW mount `boot_ms_fn` (may be empty), forwarded to the retry controller's `now_ms` seam
        /// exactly as the pre-decomposition `Store` ctor did (behavior-identical clock).
        std::function<uint64_t()> controller_boot_ms_fn,
        /// Callbacks into mount/watermark state that stays on `Store` (bound at construction):
        std::function<uint64_t()> live_epoch_fn_,          /// Store::liveWriterEpoch
        std::function<bool()> fence_ok_fn_,                 /// Store::refAppendFenceOk
        std::function<uint64_t()> boot_ms_now_fn_,          /// Store::bootMsNow
        std::function<bool()> may_mutate_,                  /// Store::mayMutate
        std::function<uint64_t()> unclean_boundary_epoch_,  /// Store::unclean_epoch_boundary_seen_at (relaxed load)
        std::function<void(const String &, const String &, const std::optional<String> &)> on_impossible_interference_,
        std::function<std::shared_ptr<void>()> pin_owner_,  /// Store::shared_from_this (pins owner for the detached publish task)
        std::function<void(const RootNamespace &)> cancel_inflight_builds_);

    /// ---- read side (spec §6) ----
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false);
    std::map<String, Resolved> listRefs(const RootNamespace & ns);

    /// ---- ref lifecycle (Task 10, spec §Writer Algorithms) ----
    void dropRef(const RootNamespace & ns, const String & ref_name);
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefPayloadUpdate &)> mutator);
    DropNamespaceStats dropNamespace(const RootNamespace & ns);
    bool namespaceIsRemoved(const RootNamespace & ns);

    /// ==== writer ref-log append lane (Task 10, spec §Writer Algorithms) ==== (see the Store delegate's
    /// doc comment for the full contract; body is verbatim here).
    RefTxnId appendRefOps(const RootNamespace & ns, MutationScope scope,
                         std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                         RootMutationOrigin origin, RootMutationKind kind,
                         bool skip_stale_precommit_sweep = false);

    bool observedNamespaceCleanupMarker(const RootNamespace & ns, const RefTxnId & remove_txn_id);
    bool trySnapshotPublishOnce(const RootNamespace & ns);
    size_t wedgedRefLaneCount();

    /// ---- remount / shutdown coordination (called by the owning Store) ----
    void quiesceRefTablesForRemount();
    bool refLanesSettledForRemount();
    bool drainRefLanesForShutdown(uint64_t wait_budget_ms);

    /// Staging PUT wrappers for `Build` (design 2026-07-16 source-layout §3.4): encapsulate BOTH the
    /// retry-controller call AND the ref-lane fence predicate (`fence_ok_fn`), so `Build` no longer
    /// reaches the controller or `refAppendFenceOk` directly (the `friend class Build` is gone).
    /// Behavior-identical to the previously-inlined controller+fence at `CasBuild.cpp` stageManifest /
    /// uploadFromSource.
    CasWriteOutcome stagingPutIfAbsent(std::string_view key, std::string_view bytes, Token * out_token);
    CasCreateResult stagingConditionalCreate(std::string_view key, const std::function<PutResult()> & attempt);

    /// `EventEmitter` concept hooks (Primitives/CasEvent.h): the ledger emits `CasEvent`s through the
    /// same `EventEmitter{*this}` helper the Store used, and forwards a couple of direct emissions -- all
    /// onto the injected `event_sink`. Kept as members (not bare `event_sink(...)` calls) so
    /// `EventEmitter<CasRefLedger>` compiles unchanged and the emission idiom matches the pre-move code.
    bool hasEventSink() const noexcept { return static_cast<bool>(event_sink); }
    void emitEvent(CasEvent && e) const { if (event_sink) event_sink(std::move(e)); }

    void setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn);

    /// ---- test seams (Task 10/11/13) ----
    uint64_t refRecoveryRestartsForTest(const RootNamespace & ns);
    bool refLaneWedgedForTest(const RootNamespace & ns);
    String wedgedKeyForTest(const RootNamespace & ns);
    void forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                           const String & key, const String & bytes);
    bool needsStalePrecommitSweepForTest(const RootNamespace & ns);
    void waitForSnapshotPublishSettleForTest(const RootNamespace & ns);
    int pendingSnapshotPublishesForTest(const RootNamespace & ns);
    std::optional<RefTxnId> newestPublishedSnapshotIdForTest(const RootNamespace & ns);
    size_t tailSinceSnapshotCountForTest(const RootNamespace & ns);
    void setRefPreCarveHookForTest(std::function<void()> hook) { ref_pre_carve_hook_for_test = std::move(hook); }

    size_t refQueuePendingForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it == ref_tables.end() ? 0 : it->second->pending.size();
    }

    uint64_t refRecoveryWaitersForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        std::lock_guard<std::mutex> g(rt->state_mutex);
        return rt->recovery_waiters_for_test;
    }

    size_t refTablesCachedCountForTest()
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        return ref_tables.size();
    }
    bool refTableCachedForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it != ref_tables.end() && it->second->recovered;
    }

private:
    /// ---- injected environment (no `Store` back-reference); initialized first, in this order ----
    Backend & backend;
    const Layout & layout;
    RefLedgerConfig config;
    const CasEventSink & event_sink;
    CasRequestBudget cas_request_budget;
    std::function<uint64_t()> live_epoch_fn;
    std::function<bool()> fence_ok_fn;
    std::function<uint64_t()> boot_ms_now_fn;
    std::function<bool()> may_mutate;
    std::function<uint64_t()> unclean_boundary_epoch;
    std::function<void(const String &, const String &, const std::optional<String> &)> on_impossible_interference;
    std::function<std::shared_ptr<void>()> pin_owner;
    std::function<void(const RootNamespace &)> cancel_inflight_builds;

    /// At most one outstanding uncertain `PUT` per table (spec §Writer-Side Linearization). Retained
    /// on the table's runtime until resolved durable (applied to cache first) or definitely rejected.
    struct RefAppendWedge
    {
        RefTxnId txn_id;
        String key;
        String bytes;
    };

    /// One queued `appendRefOps` caller. `build_ops` is invoked at most once, from inside the flush,
    /// and returns the ops it contributes rather than mutating storage directly.
    struct RefMutationItem
    {
        MutationScope scope;
        std::function<std::vector<RefOp>(const RefTableState &)> build_ops;
        RootMutationOrigin origin = RootMutationOrigin::Writer;
        RootMutationKind kind = RootMutationKind::Publish;
        bool done = false;                       /// guarded by ref_queue_mutex
        std::exception_ptr error;                /// guarded by ref_queue_mutex
        RefTxnId committed_id{};                  /// written by the leader before done = true
    };

    /// The whole-table runtime (spec §Startup And Recovery / §Table State): one coherent decoded
    /// `RefTableState` per namespace, evicted only as a whole (never populated here -- Phase 1 has no
    /// eviction trigger yet, only lazy recovery on first touch). `state_mutex` is SEPARATE from
    /// `ref_queue_mutex` (which only ever guards `pending`/`leader_active`) so a reader (resolveRef/
    /// listRefs) can observe `state` without contending with the flush leader's network round trip --
    /// the leader only holds `state_mutex` for the brief copy-out-before-validate and the
    /// apply-after-commit steps, never for the `putIfAbsentControlled` call itself.
    struct RefTableRuntime
    {
        std::mutex state_mutex;
        bool recovered = false;
        /// fix-round F3 (author-review: seal encode+PUT running under `state_mutex` for up to the
        /// ~90s retry envelope stalls every other touch of this table, plus `wedgedRefLaneCount`'s
        /// whole-store walk). `ensureRefTableRecovered` releases `state_mutex` around the seal's
        /// encode+PUT only (mirroring `trySnapshotPublishOnce`'s copy-under-lock/PUT-outside shape) --
        /// this flag is what keeps a concurrent second caller for the SAME namespace, arriving during
        /// that unlocked window, from redoing an independent LIST+replay+seal attempt that would race
        /// the SAME seal key (a losing racer's `putIfAbsentControlled` returns `Unresolved` --
        /// `CasWriteOutcome` has no distinguishable "someone else already created it" -- which the
        /// existing `!= Committed -> throw ABORTED` check would misreport as a real failure). Instead
        /// the second caller waits on `recovery_cv` and re-checks `recovered` once the first caller
        /// finishes, exactly the concurrent-second-caller contract `ensureRefTableRecovered`'s own top
        /// comment already promises -- just no longer implemented by holding the mutex itself for the
        /// I/O. Both guarded by `state_mutex`.
        bool recovery_in_progress = false;
        std::condition_variable recovery_cv;
        /// Test-only: how many callers are PARKED in the `recovery_cv` wait above, right now. A plain
        /// count (guarded by `state_mutex`, like everything else here) -- lets a test deterministically
        /// `yield()`-poll for "a second caller has actually reached the wait" instead of racing a
        /// blocked backend call against thread scheduling (mirrors `refQueuePendingForTest`'s idiom).
        uint64_t recovery_waiters_for_test = 0;
        RefTableState state;
        /// Retained `_cleanup/<remove-txn-id>` markers observed at recovery (Task 11's recreation
        /// gate consumes these via `observedNamespaceCleanupMarker`).
        std::set<RefTxnId> cleanup_markers;
        std::optional<RefAppendWedge> wedge;
        uint64_t recovery_restarts = 0;           /// diagnostic: LIST/GET restarts forced by a vanished object
        /// Per-table admission budgets (spec §Snapshot Format): the raw hard limits minus this table's
        /// own `4 + ns.size()` wire overhead and a fixed safety margin, computed once at recovery.
        uint64_t snapshot_budget = 0;
        uint64_t removal_budget = 0;

        /// rev.6 Task 10 (spec §publish-from-live): the count and encoded-byte sum of every applied txn
        /// strictly above `newest_snapshot_id` -- the threshold trigger's inputs, and the exact amounts
        /// a successful publish's adoption subtracts back out. No per-entry retention any more: the live
        /// `state` above IS the publish candidate body (`trySnapshotPublishOnce` copies it directly), so
        /// there is nothing left to replay a candidate forward from. Both ATOMIC (relaxed) for the same
        /// cross-lock `total`-loop read `enforceRefTableCacheBudget`'s pre-candidacy pass performs on
        /// EVERY cached table under `ref_queue_mutex` alone -- including hot tables whose append lane
        /// (holding only `state_mutex`) is concurrently mutating these fields. That cross-lock read is a
        /// data race on a plain `uint64_t` (formal UB, TSan-detectable); an atomic makes it well-defined.
        /// The gated candidate loop and every other reader/writer already hold `state_mutex`, so relaxed
        /// ordering is sufficient (these counters carry no happens-before for other state).
        std::atomic<uint64_t> tail_count_since_snapshot{0};
        std::atomic<uint64_t> tail_bytes_since_snapshot{0};
        std::optional<RefTxnId> newest_snapshot_id;
        /// Task 13 (spec §Byte, Memory, And CPU Budget): whole-table cache-weight bookkeeping for
        /// `enforceRefTableCacheBudget`. `base_snapshot_bytes` is the encoded body size of the snapshot
        /// at `newest_snapshot_id` (0 for a never-published table), captured for free from the
        /// recovered/published snapshot body -- refreshed only when that snapshot changes (recovery +
        /// each publish), never per mutation. The estimated resident weight is
        /// `base_snapshot_bytes + tail_bytes_since_snapshot`. `base_snapshot_bytes` is ATOMIC (relaxed)
        /// for the same cross-lock `total`-loop read as `tail_bytes_since_snapshot` above. `last_touch_tick`
        /// is the monotonic access stamp (`Store::ref_table_access_tick`) used to evict least-recently-
        /// touched tables first; it is read only in the `use_count()==1`-gated candidate loop (no
        /// concurrent writer there), so it stays a plain `uint64_t`.
        std::atomic<uint64_t> base_snapshot_bytes{0};
        uint64_t last_touch_tick = 0;
        /// Set true by recovery (spec §Clean Up Old Precommits); cleared when a sweep attempt is
        /// dispatched (so the sweep's own nested `appendRefOps` calls do not recurse) and PERMANENTLY
        /// only once an attempt completes VERIFIED CLEAN (a full pass over the live state found zero
        /// stale bindings). S13 fix: any failed/partial attempt re-arms it (with the
        /// `precommit_sweep_backoff_*` cooldown), so a later read/mutation trigger retries until clean --
        /// a single attempt burned in the post-restart error window must not leave a dead incarnation's
        /// precommit bindings protected from GC forever on a long-lived mount.
        bool needs_stale_precommit_sweep = false;
        /// S13 fix: per-table retry cooldown for the stale-precommit sweep (guarded by `state_mutex`),
        /// mirroring `publish_backoff_*` below: `until` is the boottime instant before which
        /// `maybeSweepStalePrecommits` refuses to re-attempt; `ms` is the current exponential interval
        /// (0 = no failure yet, or reset by the last verified-clean sweep).
        uint64_t precommit_sweep_backoff_until_ms = 0;
        uint64_t precommit_sweep_backoff_ms = 0;
        /// Test-observability + graceful settling for the background snapshot-publish dispatch (see
        /// `maybeScheduleSnapshotPublish`): the count of in-flight publish attempts for this table, and
        /// the condvar (guarded by `state_mutex`) a test waits on via `waitForSnapshotPublishSettleForTest`.
        std::atomic<int> pending_snapshot_publishes{0};
        std::condition_variable publish_settle_cv;
        /// C4 (spec §writer-snapshot-publication): per-table publish-dispatch backoff (guarded by
        /// `state_mutex`). `publish_backoff_until_ms` is the boottime instant before which
        /// `maybeScheduleSnapshotPublish` refuses to dispatch; `publish_backoff_ms` is the current
        /// exponential interval (0 = not backing off), doubled on each consecutive non-Committed publish
        /// outcome and reset to 0 on the next durable publish.
        uint64_t publish_backoff_until_ms = 0;
        uint64_t publish_backoff_ms = 0;

        std::deque<std::shared_ptr<RefMutationItem>> pending;    /// guarded by ref_queue_mutex
        bool leader_active = false;                               /// guarded by ref_queue_mutex
        std::condition_variable cv;

        /// Set true when a self-remount detaches this runtime from the cache (`quiesceRefTablesForRemount`):
        /// the fresh incarnation re-recovers each table under the new epoch on next touch, so any leader
        /// still holding THIS (now-orphaned) runtime must fail closed instead of allocating an id / applying
        /// against its stale cache once the re-armed fence re-opens the gate. Stored with release BEFORE the
        /// remount re-arms the fence, so a lane that observes `mayMutate` true also observes this flag
        /// (release/acquire through the fence) -- there is no interleaving where a stale runtime both passes
        /// the fence and reads this flag false.
        std::atomic<bool> superseded_by_remount{false};
    };
    static constexpr size_t kMaxRefBatch = 128;
    static constexpr size_t kRefRecoveryMaxRestarts = 3;          /// spec: "bounded (3) and counted"
    /// rev.6 Task 8 (spec §recovery-seal): a failed recovery-seal PUT (`putIfAbsentControlled` not
    /// `Committed`) is a SEPARATE fail-closed throw -- it is not one of the restarts counted here
    /// (those are only LIST/GET restart-on-vanish loops). It leaves `rt.recovered` false, so the table
    /// stays unrecovered/non-writable and the NEXT touch restarts recovery from a fresh attempt 0
    /// (re-LIST, re-replay, re-seal), never resuming this bounded loop mid-way.
    /// Fixed Phase-1 safety margin subtracted (alongside the per-table `4 + ns.size()` overhead) from
    /// the raw `ref_snapshot_max_bytes`/`ref_removal_max_bytes` hard limits before calling `admits`.
    static constexpr uint64_t kRefAdmissionSafetyMargin = 4096;

    std::mutex ref_queue_mutex;
    std::map<String, std::shared_ptr<RefTableRuntime>> ref_tables;
    /// Task 13 (spec §Byte, Memory, And CPU Budget): monotonic access stamp for whole-table cache LRU
    /// eviction; bumped on every table touch, recorded in `RefTableRuntime::last_touch_tick`.
    std::atomic<uint64_t> ref_table_access_tick{0};
    /// rev.6 Task 5 (spec §clean-release drain): latched by `drainRefLanesForShutdown` BEFORE it
    /// snapshots `ref_tables`/waits on each table's queue -- every ordinary ref mutation (`appendRefOps`)
    /// checks this under the SAME `ref_queue_mutex` critical section it uses to enqueue its item, so the
    /// check-and-enqueue is atomic with the drain's snapshot-and-wait: a caller either enqueues strictly
    /// before the drain observes this table (and the drain then waits for it), or observes this flag
    /// already true and never enqueues at all. No caller can land a NEW item after the drain has decided
    /// this table is idle.
    std::atomic<bool> shutting_down{false};

    /// Store-wide strictly-increasing counter (spec §Ordered Ref Transaction Identifier): shared by
    /// every table of this mounted writer; a fresh writer_epoch (a new Store) restarts it at one.
    std::atomic<uint64_t> next_ref_sequence{1};
    /// The epoch component is the LIVE mount incarnation's writer_epoch, not the open-time
    /// `process_epoch`: a self-remount allocates a strictly-greater durable writer_epoch, so every ref
    /// transaction stamped after the remount sorts strictly ABOVE any (dead-incarnation or twin) log
    /// still durable under an older epoch. `RefTxnId` compares epoch first, so the epoch bump alone
    /// guarantees the pagination premise "a new log is never inserted at or below an already durable
    /// table log id" (spec §Ordered Ref Transaction Identifier / §Step 1).
    RefTxnId allocateRefTxnId() { return RefTxnId{live_epoch_fn(), next_ref_sequence.fetch_add(1)}; }

    /// The CAS-owned retry controller (Task 5) this Store's ref-log writer path uses for every
    /// conditional log/snapshot `PUT` and uncertain-result resolution. Also shared (via the Build
    /// friendship) by `Build::stageManifest`'s part-manifest body `PUT` (chaos-tolerance-report
    /// §Task B) and by `Build::uploadFromSource`'s blob-body create — both the streaming
    /// `putIfAbsentStream` PUT and `promoteStaged`'s conditional server-side copy — via
    /// `conditionalCreateControlled` (availfix). The controller is stateless per call (immutable
    /// budget/clock/sleep — the sleep fn mutates only through the test-only seam, before traffic), so
    /// concurrent lanes and builds use the one instance safely.
    std::unique_ptr<CasRequestController> ref_request_controller;

    /// See `setRefPreCarveHookForTest`. Null in production (a no-op call site).
    std::function<void()> ref_pre_carve_hook_for_test;

    /// ---- moved private methods (bodies verbatim in CasRefLedger.cpp) ----
    std::shared_ptr<RefTableRuntime> getRefTableRuntime(const RootNamespace & ns);
    void ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt);
    void enforceRefTableCacheBudget(const RootNamespace & keep_ns);
    void runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                           const std::shared_ptr<RefMutationItem> & own);
    void flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    void maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    void advancePublishBackoff(RefTableRuntime & rt);
    void resetPublishBackoff(RefTableRuntime & rt);
    void maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    void sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    void sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    void advancePrecommitSweepBackoff(RefTableRuntime & rt);
    void resetPrecommitSweepBackoff(RefTableRuntime & rt);
    void publishRemovedSnapshotNow(const RootNamespace & ns);
};

}
