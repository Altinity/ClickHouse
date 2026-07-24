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

/// Controls whether `resolveRef` emits its `RefResolve` audit event. `Emit` (the default) preserves
/// today's behavior for every existing caller (`listRefs`, `dropRef`, GC, and ordinary reads).
/// `Deferred` is for a caller that itself decides, after inspecting the resolve outcome, whether the
/// access as a whole did real resolve work worth auditing — see `CachedPartFolderAccess::getView`,
/// which re-emits the identical event on every path except a warm view-cache hit that served without
/// re-validating anything.
enum class ResolveAudit : uint8_t { Emit, Deferred };

/// Coordinates the writer-side ref-log and ref-table protocol for all namespaces in one mounted pool.
/// It owns the recovered whole-table cache, the flat-combining append lane and its unresolved-`PUT`
/// wedge, snapshot publication, stale-precommit cleanup, cache-budget eviction, and remount/shutdown
/// draining. `ref_queue_mutex` protects cache membership and queue leadership; each table's
/// `state_mutex` protects its decoded state and per-table lifecycle. Network I/O is deliberately performed
/// without holding `state_mutex`, so readers and other maintenance operations are not blocked by retries.
///
/// The ledger receives storage, configuration, event delivery, and retry-budget dependencies directly.
/// Mount state remains owned by `Pool` and is exposed through callbacks: the live writer epoch, append
/// fence, clocks, mutation gate, unclean-boundary observation, anomaly reaction, owner lifetime pin, and
/// cancellation of in-flight builds. The detached snapshot publisher uses the lifetime pin; no
/// `Pool &` back-reference is retained. `Pool` forwards its existing public operations to this component.
class CasRefLedger
{
public:
    CasRefLedger(
        BackendPtr backend_ptr,
        const Layout & layout_,
        RefLedgerConfig config_,
        const CasEventSink & event_sink_,
        CasRequestBudget cas_request_budget_,
        /// Monotonic mount clock used by the retry controller; it may be empty when the controller's
        /// default clock is appropriate.
        std::function<uint64_t()> controller_boot_ms_fn,
        /// Callbacks into mount and watermark state owned by `Pool`, bound for this ledger's lifetime:
        std::function<uint64_t()> live_epoch_fn_,
        std::function<bool()> fence_ok_fn_,
        std::function<uint64_t()> boot_ms_now_fn_,
        std::function<bool()> may_mutate_,
        std::function<uint64_t()> unclean_boundary_epoch_,
        std::function<void(const String &, const String &, const std::optional<String> &)> on_impossible_interference_,
        std::function<std::shared_ptr<void>()> pin_owner_,
        std::function<void(const RootNamespace &)> cancel_inflight_builds_);

    /// Recovers `ns` on first access and resolves `ref_name` from the authoritative cached table.
    /// The optional staleness argument remains for API compatibility; this mounted writer has no
    /// alternate shard cache, so the recovered table is always the view used for the result.
    /// `audit` defaults to `Emit` so every existing caller keeps emitting `RefResolve` unchanged;
    /// `Deferred` suppresses the emit for a caller that re-emits it conditionally itself.
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false,
                                       ResolveAudit audit = ResolveAudit::Emit);

    /// Recovers `ns` on first access and returns every committed ref in canonical name order. Read-side
    /// maintenance may schedule snapshot publication and stale-precommit cleanup, but those actions do
    /// not change the returned committed view or make a read fail when maintenance has an uncertain `PUT`.
    std::map<String, Resolved> listRefs(const RootNamespace & ns);

    /// Recovers `ns` on first access and reports whether any committed ref name starts with `prefix`,
    /// without materializing the full ref map `listRefs` returns. An empty `prefix` means "any ref at
    /// all" and short-circuits on the first entry, so this is O(1) for that (dominant, emptiness-probe)
    /// case; a non-empty prefix still stays a no-allocation scan.
    bool hasAnyRefWithPrefix(const RootNamespace & ns, std::string_view prefix);

    /// Appends the transaction that removes one ref and waits for its durable result. A failed append
    /// propagates its exception and does not apply the removal to the in-memory table.
    void dropRef(const RootNamespace & ns, const String & ref_name);

    /// Builds and appends a payload update for one ref. The mutator is invoked while constructing the
    /// transaction, and its changes become visible only after the append is durable.
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefPayloadUpdate &)> mutator);

    /// Durably removes the complete namespace, including its current ref/precommit state, then performs
    /// the associated cleanup and cancellation work. Repeated removal observes the cached `Removed`
    /// state; a failed append leaves the namespace live and propagates the exception.
    DropNamespaceStats dropNamespace(const RootNamespace & ns);

    /// Reports whether recovery has established the namespace's durable lifecycle as `Removed`.
    bool namespaceIsRemoved(const RootNamespace & ns);

    /// Queues a mutation for flat-combining with compatible callers. `build_ops` runs at most once in
    /// the flush leader and must return operations without writing storage itself. The leader validates
    /// the complete batch, writes one ref-log object behind the append fence, and applies the batch to the
    /// cache only after a durable result; an unresolved conditional `PUT` wedges the table and blocks
    /// later appends until the same object is resolved or definitely rejected.
    RefTxnId appendRefOps(const RootNamespace & ns, MutationScope scope,
                         std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                         RootMutationOrigin origin, RootMutationKind kind,
                         bool skip_stale_precommit_sweep = false);

    /// Records that recovery observed the cleanup marker for `remove_txn_id`; returns whether the marker
    /// was present. The observation is retained with the recovered table for namespace recreation gates.
    bool observedNamespaceCleanupMarker(const RootNamespace & ns, const RefTxnId & remove_txn_id);

    /// Attempts one snapshot publication from a copy of the live state. The copy is made under
    /// `state_mutex`, the conditional `PUT` is performed without that mutex, and counters are adopted
    /// only when this attempt successfully publishes the captured snapshot.
    bool trySnapshotPublishOnce(const RootNamespace & ns);

    /// Counts tables with an unresolved append `PUT`; the walk takes each table lock briefly and never
    /// waits for the network operation that caused a wedge.
    size_t wedgedRefLaneCount();

    /// Marks cached runtimes obsolete before a self-remount reopens the append fence. Leaders holding an
    /// orphaned runtime therefore fail closed instead of mutating state from the previous epoch.
    void quiesceRefTablesForRemount();

    /// Returns whether every cached append lane has settled sufficiently for remount. It does not
    /// discard runtimes; `quiesceRefTablesForRemount` performs that state transition.
    bool refLanesSettledForRemount();

    /// Closes admission, snapshots the current table set, and waits up to `wait_budget_ms` for queued
    /// mutations and leaders to finish. The check and enqueue paths share `ref_queue_mutex`, so no new
    /// mutation can appear after shutdown has taken its snapshot.
    bool drainRefLanesForShutdown(uint64_t wait_budget_ms);

    /// Performs a staged conditional create through the ledger's retry controller and append-fence
    /// predicate. Callers do not access either dependency directly, so every attempt observes the same
    /// mount admission rule.
    CasWriteOutcome stagingPutIfAbsent(std::string_view key, std::string_view bytes, Token * out_token);

    /// Performs a conditional staged create using `attempt`, applying the same retry controller and
    /// append-fence policy as `stagingPutIfAbsent`.
    CasCreateResult stagingConditionalCreate(std::string_view key, const std::function<PutResult()> & attempt);

    /// Same retry/fence policy as `stagingPutIfAbsent`/`stagingConditionalCreate`, for a MUTABLE
    /// If-Match overwrite whose bytes are deterministic (safe for GET-based resolution).
    CasOverwriteResult stagingConditionalOverwrite(std::string_view key, std::string_view bytes, const Token & expected);

    /// Same retry/fence policy as `stagingPutIfAbsent`, for a MUTABLE marker where an existing
    /// DIFFERENT value at the key is a normal Conflict outcome, not corruption (see
    /// `CasRequestController::putIfAbsentControlledMutable`).
    CasOverwriteResult stagingPutIfAbsentMutable(std::string_view key, std::string_view bytes);

    /// Hooks required by `EventEmitter`: events are delivered to the injected sink when one is present.
    bool hasEventSink() const noexcept { return static_cast<bool>(event_sink); }
    void emitEvent(CasEvent && e) const { if (event_sink) event_sink(std::move(e)); }

    /// Replaces the retry controller's delay seam for deterministic tests; production callers leave it
    /// untouched.
    void setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn);

    /// Test-only observability and fault-injection seams for recovery, wedges, cleanup, and publication.
    /// The counters expose recovery and publication progress; wedge methods create and inspect the
    /// unresolved-`PUT` state; cleanup methods expose sweep eligibility; publication methods expose
    /// settling, snapshot identity, and tail accounting.
    /// Returns the number of LIST/GET recovery restarts recorded for `ns`.
    uint64_t refRecoveryRestartsForTest(const RootNamespace & ns);
    /// Reports whether `ns` currently has an unresolved append `PUT`.
    bool refLaneWedgedForTest(const RootNamespace & ns);
    /// Returns the object key retained for the unresolved append of `ns`.
    String wedgedKeyForTest(const RootNamespace & ns);
    /// Installs a synthetic unresolved append for `ns` so callers can exercise resolution and blocking.
    void forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                           const String & key, const String & bytes);
    /// Reports whether recovery or a prior incomplete sweep requires stale-precommit cleanup.
    bool needsStalePrecommitSweepForTest(const RootNamespace & ns);
    /// Waits until all background snapshot publications for `ns` have completed.
    void waitForSnapshotPublishSettleForTest(const RootNamespace & ns);
    /// Returns the number of background snapshot publications currently in flight for `ns`.
    int pendingSnapshotPublishesForTest(const RootNamespace & ns);
    /// Returns the newest snapshot id adopted by the cached runtime, if any.
    std::optional<RefTxnId> newestPublishedSnapshotIdForTest(const RootNamespace & ns);
    /// Returns the number of applied transactions newer than the adopted snapshot.
    size_t tailSinceSnapshotCountForTest(const RootNamespace & ns);
    /// Returns the number of committed entries in the mutable overlay, when the COW representation has one.
    size_t committedOverlayEntriesForTest(const RootNamespace & ns);
    /// Installs the test hook invoked immediately before the leader carves a compatible batch.
    void setRefPreCarveHookForTest(std::function<void()> hook) { ref_pre_carve_hook_for_test = std::move(hook); }

    /// Test-only fault seam for the two-phase carve/validation protocol (same `*ForTest` pattern as
    /// `setRefPreCarveHookForTest`). `flushRefBatch` fires the hook at each named point of the carve's
    /// plan/publish phases and of the per-item validation loop, so a test can inject `std::bad_alloc`
    /// and assert the append queue and the batch-validation `working` state stay intact. `PlanSeenRefs`,
    /// `PlanBatchGrow` and `PlanReserveOwned` fire in the non-mutating PLAN phase (nothing has been
    /// popped yet); `PublishPop` fires at the start of the no-throw PUBLISH phase; `ValidateFinalOps`
    /// fires once per admitted item, at the last throwing point before that item's effects are published
    /// into `working`/`final_ops`. `ChunkReseed` fires once at each chunk boundary of a chunked flush --
    /// immediately AFTER the just-full chunk committed durably (its survivors already completed) and
    /// BEFORE `working`/the trial-id high-water mark are reseeded from the now-live state; it is the
    /// injection point for the tenure-exception-containment contract (a throw here, or from the reseed
    /// itself, must leave the committed chunk's callers with their success). Null in production.
    enum class CarvePhaseForTest { PlanSeenRefs, PlanBatchGrow, PlanReserveOwned, PublishPop, ValidateFinalOps, ChunkReseed };
    void setCarveHookForTest(std::function<void(CarvePhaseForTest)> hook) { carve_hook_for_test = std::move(hook); }

    /// Installs the pre-tenure fault seam (see `ref_pre_tenure_hook_for_test`).
    void setRefPreTenureHookForTest(std::function<void()> hook) { ref_pre_tenure_hook_for_test = std::move(hook); }

    /// Returns the number of queued mutations for `ns` under the queue mutex.
    size_t refQueuePendingForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it == ref_tables.end() ? 0 : it->second->pending.size();
    }

    /// Reports whether `ns` currently has an active append-lane leader (the baton). Under the queue mutex.
    bool refLeaderActiveForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it != ref_tables.end() && it->second->leader_active;
    }

    /// Returns the number of callers currently waiting for `ns` recovery under its state mutex.
    uint64_t refRecoveryWaitersForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        std::lock_guard<std::mutex> g(rt->state_mutex);
        return rt->recovery_waiters_for_test;
    }

    /// Returns the number of namespace runtimes currently retained in the cache.
    size_t refTablesCachedCountForTest()
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        return ref_tables.size();
    }
    /// Reports whether `ns` has a cached runtime whose recovery completed.
    bool refTableCachedForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it != ref_tables.end() && it->second->recovered;
    }

private:
    /// Injected storage and mount environment. The member order is part of construction/destruction
    /// behavior because the callbacks and references are used by the runtime owned below.
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

    /// Backoff sleep used by `ensureRefTableRecovered`'s transient-retry loop. Default is an
    /// interruptible slice-sleep (bails early if `fence_ok_fn` drops, e.g. on shutdown/lease loss);
    /// `setCasRetrySleepForTest` overrides it (a unit test injects a clock-advancing no-op).
    std::function<void(uint64_t)> recovery_retry_sleep_fn;

    /// Describes the one conditional `PUT` whose outcome is still uncertain for a table. It remains in
    /// the runtime until the object is confirmed durable and applied to the cache, or definitely rejected.
    struct RefAppendWedge
    {
        RefTxnId txn_id;
        String key;
        String bytes;
    };

    /// One queued append caller. `build_ops` is invoked at most once by the flush leader and returns the
    /// caller's operations rather than mutating storage directly. Completion fields are synchronized by
    /// `ref_queue_mutex`.
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

    /// One coherent decoded `RefTableState` and append runtime for a namespace. It is recovered lazily
    /// and evicted only as a whole. `state_mutex` is separate from
    /// `ref_queue_mutex` (which only ever guards `pending`/`leader_active`) so a reader (resolveRef/
    /// listRefs) can observe `state` without contending with the flush leader's network round trip --
    /// the leader only holds `state_mutex` for the brief copy-out-before-validate and the
    /// apply-after-commit steps, never for the `putIfAbsentControlled` call itself.
    struct RefTableRuntime
    {
        std::mutex state_mutex;
        bool recovered = false;
        /// Gates the recovery-seal I/O that runs outside `state_mutex`. A second caller waits on
        /// `recovery_cv` and rechecks `recovered` after the first caller finishes; otherwise it could
        /// perform a competing LIST/replay/seal and misclassify the losing conditional `PUT` as failure.
        /// Both this flag and the condition variable are guarded by `state_mutex`.
        bool recovery_in_progress = false;
        std::condition_variable recovery_cv;
        /// Test-only count of callers currently waiting for recovery; guarded by `state_mutex` so tests
        /// can observe that a concurrent caller reached the wait without depending on scheduling.
        uint64_t recovery_waiters_for_test = 0;
        RefTableState state;
        /// `_cleanup/<remove-txn-id>` markers observed at recovery, retained for namespace recreation
        /// checks.
        std::set<RefTxnId> cleanup_markers;
        std::optional<RefAppendWedge> wedge;
        uint64_t recovery_restarts = 0;           /// diagnostic: LIST/GET restarts forced by a vanished object
        /// Per-table admission budgets: raw configured limits minus the table's `4 + ns.size()` wire
        /// overhead and the fixed safety margin, computed once at recovery.
        uint64_t snapshot_budget = 0;
        uint64_t removal_budget = 0;

        /// Number and encoded-byte sum of applied transactions strictly newer than `newest_snapshot_id`.
        /// The live `state` is the next snapshot candidate, so no separate tail replay is retained.
        /// These counters are atomic because the cache-budget pass reads them while holding
        /// `ref_queue_mutex`, whereas append and publication paths hold `state_mutex`; relaxed ordering
        /// is sufficient because the counters do not publish any other state.
        std::atomic<uint64_t> tail_count_since_snapshot{0};
        std::atomic<uint64_t> tail_bytes_since_snapshot{0};
        std::optional<RefTxnId> newest_snapshot_id;
        /// Whole-table cache-weight bookkeeping for `enforceRefTableCacheBudget`.
        /// `base_snapshot_bytes` is the encoded body size of the snapshot
        /// at `newest_snapshot_id` (0 for a never-published table), captured for free from the
        /// recovered/published snapshot body -- refreshed only when that snapshot changes (recovery +
        /// each publish), never per mutation. The estimated resident weight is
        /// `base_snapshot_bytes + tail_bytes_since_snapshot`. `base_snapshot_bytes` is ATOMIC (relaxed)
        /// for the same cross-lock `total`-loop read as `tail_bytes_since_snapshot` above. `last_touch_tick`
        /// is the monotonic access stamp (`Pool::ref_table_access_tick`) used to evict least-recently-
        /// touched tables first; it is read only in the `use_count()==1`-gated candidate loop (no
        /// concurrent writer there), so it stays a plain `uint64_t`.
        std::atomic<uint64_t> base_snapshot_bytes{0};
        uint64_t last_touch_tick = 0;
        /// Set true by recovery; cleared when a sweep attempt is
        /// dispatched (so the sweep's own nested `appendRefOps` calls do not recurse) and PERMANENTLY
        /// only once an attempt completes VERIFIED CLEAN (a full pass over the live state found zero
        /// stale bindings). Any failed or partial attempt re-arms it (with the
        /// `precommit_sweep_backoff_*` cooldown), so a later read/mutation trigger retries until clean --
        /// a single attempt burned in the post-restart error window must not leave a dead incarnation's
        /// precommit bindings protected from GC forever on a long-lived mount.
        bool needs_stale_precommit_sweep = false;
        /// Per-table retry cooldown for the stale-precommit sweep (guarded by `state_mutex`),
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
        /// Per-table snapshot-publish dispatch backoff (guarded by
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
    static constexpr size_t kMaxRefBatch = 1000;
    /// Recovery retries at most this many times when an object selected by LIST vanishes before GET.
    /// A failed recovery-seal `PUT` is separate: it leaves `recovered` false and the next touch starts
    /// a fresh LIST/replay/seal attempt rather than resuming this bounded vanish-retry loop.
    static constexpr size_t kRefRecoveryMaxRestarts = 3;
    /// Fixed safety margin subtracted (alongside the per-table `4 + ns.size()` overhead) from
    /// the raw `ref_snapshot_max_bytes`/`ref_removal_max_bytes` hard limits before calling `admits`.
    static constexpr uint64_t kRefAdmissionSafetyMargin = 4096;

    std::mutex ref_queue_mutex;
    std::map<String, std::shared_ptr<RefTableRuntime>> ref_tables;
    /// Monotonic access stamp for whole-table cache LRU eviction, bumped on every table touch and
    /// recorded in `RefTableRuntime::last_touch_tick`.
    std::atomic<uint64_t> ref_table_access_tick{0};
    /// Latched by `drainRefLanesForShutdown` before it
    /// snapshots `ref_tables`/waits on each table's queue -- every ordinary ref mutation (`appendRefOps`)
    /// checks this under the SAME `ref_queue_mutex` critical section it uses to enqueue its item, so the
    /// check-and-enqueue is atomic with the drain's snapshot-and-wait: a caller either enqueues strictly
    /// before the drain observes this table (and the drain then waits for it), or observes this flag
    /// already true and never enqueues at all. No caller can land a NEW item after the drain has decided
    /// this table is idle.
    std::atomic<bool> shutting_down{false};

    /// Pool-wide strictly-increasing sequence shared by every table of this mounted writer. A new mount
    /// epoch starts the sequence at one.
    std::atomic<uint64_t> next_ref_sequence{1};
    /// The epoch component is the live mount incarnation's writer epoch, not the open-time
    /// `process_epoch`: a self-remount allocates a strictly-greater durable writer_epoch, so every ref
    /// transaction stamped after the remount sorts strictly ABOVE any (dead-incarnation or twin) log
    /// still durable under an older epoch. `RefTxnId` compares epoch first, so the epoch bump alone
    /// guarantees that a new log is never inserted at or below an already durable table log id.
    RefTxnId allocateRefTxnId() { return RefTxnId{live_epoch_fn(), next_ref_sequence.fetch_add(1)}; }

    /// The CAS-owned retry controller this Pool's ref-log writer path uses for every
    /// conditional log/snapshot `PUT` and uncertain-result resolution. Also shared (via the PartWriteTxn
    /// `PartWriteTxn::stageManifest`'s part-manifest body `PUT` and by `PartWriteTxn::uploadFromSource`'s
    /// blob-body create — both the streaming
    /// `putIfAbsentStream` PUT and `promoteStaged`'s conditional server-side copy — via
    /// `conditionalCreateControlled`. The controller is stateless per call (immutable
    /// budget/clock/sleep — the sleep fn mutates only through the test-only seam, before traffic), so
    /// concurrent lanes and builds use the one instance safely.
    std::unique_ptr<CasRequestController> ref_request_controller;

    /// Test-only hook called before a compatible append batch is carved; null in production.
    std::function<void()> ref_pre_carve_hook_for_test;

    /// Test-only fault seam fired on the CALLING thread at the instant a queue caller takes append-lane
    /// leadership -- i.e. at the FIRST allocation that builds the leader's responsibility set, the last
    /// throwing point before the baton (`leader_active`) is published. A throw here must leave the lane
    /// idle (baton un-taken, the caller's item un-enqueued), never a permanently non-idle namespace with
    /// no live leader. Null in production. See `appendRefOps`.
    std::function<void()> ref_pre_tenure_hook_for_test;

    /// Test-only hook fired at each carve/validation phase point (see `CarvePhaseForTest`); null in
    /// production.
    std::function<void(CarvePhaseForTest)> carve_hook_for_test;

    /// Returns the cached runtime for `ns`, creating an empty unrecovered runtime when needed.
    std::shared_ptr<RefTableRuntime> getRefTableRuntime(const RootNamespace & ns);

    /// Lazily recovers `ns` by listing and replaying its snapshot/log objects. It does not expose the
    /// table as recovered until any required recovery seal is durable; concurrent callers serialize
    /// across the unlocked seal I/O through `recovery_in_progress`.
    void ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt);

    /// Evicts least-recently-touched, idle whole-table runtimes until the configured cache budget is met,
    /// retaining `keep_ns` even when it is the next candidate.
    void enforceRefTableCacheBudget(const RootNamespace & keep_ns);

    /// Runs the append queue leader for `ns`, completing its own item and any compatible items carved
    /// into the same batch. Exceptions are stored for waiters and do not leave the leader flag latched.
    /// Every item this leader becomes responsible for -- its own `own` plus each item a flush removes
    /// from `pending` to form a batch -- is recorded into `owned_items`, so the caller's leadership guard
    /// can complete + de-pend any that a flush leaves unfinished on an exceptional exit.
    void runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                           const std::shared_ptr<RefMutationItem> & own,
                           std::vector<std::shared_ptr<RefMutationItem>> & owned_items);

    /// Validates, durably appends, and applies one compatible batch while preserving copy-before-commit
    /// and apply-after-commit ordering. Every item it carves out of `pending` is appended to
    /// `owned_items` (the leader's responsibility set) at the moment it is carved. When a batch's total
    /// op count exceeds `ref_txn_max_ops`, the validation loop emits SEVERAL ref-log transactions in one
    /// tenure via `commitRefChunk` -- each a complete commit boundary.
    void flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                       std::vector<std::shared_ptr<RefMutationItem>> & owned_items);

    /// Commits ONE chunk of a `flushRefBatch` tenure as a complete ref-log transaction: allocates the
    /// real transaction id, encodes and durably `PUT`s `chunk_ops`, applies to `rt->state` under
    /// `state_mutex`, advances the tail counters, records the per-transaction metrics, completes exactly
    /// `chunk_survivors` with the real id (waking their waiters), and schedules snapshot publication.
    /// Returns true when the chunk committed durably; false on any non-throwing failure
    /// (DefiniteFailure / unresolved wedge / a conclusive PUT rejection / an encode failure), after
    /// having already failed `chunk_survivors` with the appropriate error. Throws ONLY the
    /// provably-unreachable durable-apply failure (a bricked lane), having first failed the survivors.
    /// PRECONDITION: the caller has already released its scratch `working` copy so the post-commit
    /// overlay fold is in place (the E5 fast path).
    bool commitRefChunk(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                        const std::vector<RefOp> & chunk_ops,
                        const std::vector<std::shared_ptr<RefMutationItem>> & chunk_survivors);

    /// Leadership-exit guard for `appendRefOps`: under `ref_queue_mutex`, completes every still-unfinished
    /// item this leader owned (with `flush_exception` when unwinding, or a fail-closed `LOGICAL_ERROR`
    /// otherwise), removes each owned item from `pending` so no future leader can carve it, and releases
    /// leadership (`leader_active = false` + `cv.notify_all`). On the normal path every owned item is
    /// already `done`, so only the leadership release has effect. This is the single authority that
    /// resets `leader_active` on any exit from the leader loop.
    void completeOwnedItemsAndReleaseLeadership(
        const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
        const std::vector<std::shared_ptr<RefMutationItem>> & owned_items,
        std::exception_ptr flush_exception);

    /// Schedules best-effort background publication when tail thresholds and backoff permit. The
    /// dispatch is fenced and the detached task retains the owner pin until it finishes.
    void maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// The Live + single-in-flight-gate + backoff + tail-threshold admission decision, factored out so
    /// both the trigger (`maybeScheduleSnapshotPublish`) and the settlement re-evaluation share ONE
    /// authority. The caller MUST hold `rt.state_mutex`; on admission this increments
    /// `pending_snapshot_publishes` and returns true (the caller then dispatches). The fence check
    /// (`may_mutate`) is the caller's responsibility (it is not held under `state_mutex`).
    bool admitSnapshotPublishUnderStateLock(RefTableRuntime & rt);

    /// Launches one detached publish attempt. Assumes `pending_snapshot_publishes` was already
    /// incremented for this dispatch (by `admitSnapshotPublishUnderStateLock`). The task settles through
    /// `settleSnapshotPublish`; if the thread cannot even be constructed, the count is undone and the
    /// settle waiter notified so a leaked in-flight count never wedges shutdown/settle.
    void dispatchSnapshotPublisher(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Runs at the end of one detached publish attempt: drops this attempt's in-flight count and, under
    /// the SAME `state_mutex` hold, re-evaluates the accumulated tail so a trigger the single-flight gate
    /// discarded during this attempt (e.g. chunks 2..N of a chunked tenure whose chunk-1 publish was
    /// in flight) is re-fired instead of lost. Re-admitting under the same lock keeps the in-flight count
    /// from transiently reaching zero across the handoff, so a settle waiter never observes a false
    /// "settled". Notifies the settle condvar only when no follow-up publish is dispatched.
    void settleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Advances the exponential delay after a non-durable snapshot publication outcome.
    void advancePublishBackoff(RefTableRuntime & rt);
    /// Clears the snapshot-publication delay after durable progress.
    void resetPublishBackoff(RefTableRuntime & rt);

    /// Checks whether recovery or a mutation requested stale-precommit cleanup and dispatches it when
    /// its per-table cooldown permits.
    void maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Performs one fenced stale-precommit sweep. A partial or failed sweep re-arms the requirement and
    /// propagates its exception; only a verified-clean pass clears it.
    void sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Runs the read-triggered sweep without allowing an uncertain maintenance append to fail the read.
    void sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Advances the stale-precommit sweep's exponential cooldown after failure.
    void advancePrecommitSweepBackoff(RefTableRuntime & rt);
    /// Clears the stale-precommit sweep cooldown after a verified-clean pass.
    void resetPrecommitSweepBackoff(RefTableRuntime & rt);

    /// Publishes the terminal `Removed` snapshot after the namespace-removal transaction is durable;
    /// repeated cleanup is idempotent at the object-storage boundary.
    void publishRemovedSnapshotNow(const RootNamespace & ns);
};

}
