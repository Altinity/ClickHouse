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

/// Per-table marker for the ref lane's three post-durable install regions (spec §A2).
///
/// §A1 made every one of those regions allocation-free, so "the ref-log object is durable but the
/// runtime does not record it" is unreachable by construction. This marker exists so that if it ever
/// happens ANYWAY it is VISIBLE instead of silent, and so the relink confirm has a cheap predicate to
/// read (its rule 4). The states:
///   `Clean`        -- no ref-log object of this table's is known to be, or may be, durable without
///                     having been applied to the cached state;
///   `ApplyPending` -- armed immediately BEFORE a durable `PUT` and cleared once that transaction is
///                     installed (or once it is PROVEN that nothing became durable). It is also the
///                     steady state of a wedged lane, which is exactly "may be durable, not applied";
///   `Poisoned`     -- an install failed although its object may already be durable, i.e. the cached
///                     state can be MISSING a durable transaction. TERMINAL for the runtime: no later
///                     flush clears it, only a fresh recovery (which means a REPLACED runtime -- a
///                     runtime is never re-recovered in place). Enforced by construction: every
///                     transition out of it would have to be a `Clean`/`ApplyPending` CAS whose
///                     expected value is not `Poisoned`, and there is no plain store anywhere.
///
/// It is deliberately an ASSERT LAYER, NOT an operational fence: nothing refuses work because a table
/// is `Poisoned`. Safety rests on §A1. Turning this into a fence (the spec's "ordering caveat") would
/// require rejecting later `appendRefOps`, stale-precommit sweeps and snapshot admissions, exposing
/// reads/confirm as unknown, and ACTIVELY driving runtime replacement -- none of which this does, and
/// none of which should be inferred from the marker's existence.
enum class RefApplyState : uint8_t { Clean, ApplyPending, Poisoned };

/// The answer of the relink confirm's gate 1 (`CasRefLedger::confirmExactRef`).
///
/// `Yes` is the only answer that AUTHORIZES anything, so it is the only one that must be earned: it is
/// returned exclusively when every rule of the lane snapshot holds. `Unknown` is the catch-all for
/// every ambiguity, and it is the answer this primitive is biased towards: a cold, evicted, recovering,
/// busy, wedged or poisoned table answers `Unknown` rather than doing any work to find out.
///
/// `No` means "this runtime's committed row for that ref is not the manifest you asked about" -- and
/// nothing more. It is NOT a proof of the negative about the durable table, because the mount fence is
/// evaluated LAST (rule 6, deliberately): a mount that has already lost its fence, and whose view may
/// therefore be behind another writer's repoint, still answers `No` rather than `Unknown`. That is
/// sound only because `No` and `Unknown` are the SAME outcome for the caller -- both are
/// `SourceProofFailed` (spec §failure-taxonomy) -- so nothing is authorized by either. Do not build a
/// consumer that treats `No` as knowledge; only `Yes` is gated on the fence.
enum class ConfirmAnswer : uint8_t { Yes, No, Unknown };

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
        /// The two fence-GENERATION primitives (`CasMountRuntime::fenceGeneration`/`checkFenceOrThrow`),
        /// injected exactly as `CasPlainObjects` takes them. `fence_ok_fn` above answers "may this mount
        /// write AT ALL, right now"; these two answer the different question an append lane must ask
        /// across an I/O window: "is this still the SAME mount incarnation that admitted the transaction
        /// I am about to act on?" A wedge captures the generation at admission and presents it back on
        /// every later retry and before every install, so a result that returns after a fence loss or a
        /// re-arm is inert for the superseded runtime instead of installing a stale view (spec §3,
        /// "the mount-fence generation is captured at admission and required on every slot-occupy and
        /// install").
        std::function<uint64_t()> fence_generation_fn_,
        std::function<void(uint64_t)> check_fence_or_throw_,
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

    /// Gate 1 of the relink confirm (spec §confirm-primitive): does `ref_name` in `ns` still name
    /// EXACTLY `manifest_ref` in this writer's committed view, read under a lane snapshot that cannot
    /// observe a stale cache?
    ///
    /// Performs ZERO object-store I/O: a cold, evicted or recovering table answers `Unknown` rather
    /// than recovering from storage, and no runtime is created as a side effect of asking. That is a
    /// contract, not an optimization -- the confirm is a read-only interserver query that a remote
    /// receiver drives, so it must never be able to make this writer do work.
    ///
    /// The rules are evaluated as ONE snapshot spanning both lane mutexes, in this order: table warm
    /// and resident; lane quiescent; apply-state `Clean`; exact committed-row equality; mount fence
    /// live LAST. Every ambiguity answers `Unknown` -- see `ConfirmAnswer`, and the .cpp for why the
    /// order and the two-mutex hold are what make a `Yes` a linearization point rather than a guess.
    ConfirmAnswer confirmExactRef(const RootNamespace & ns, const String & ref_name,
                                  const ManifestRef & manifest_ref) const;

    /// Appends the transaction that removes one ref and waits for its durable result. A failed append
    /// propagates its exception and does not apply the removal to the in-memory table.
    void dropRef(const RootNamespace & ns, const String & ref_name);

    /// Builds and appends a published_at_ms update for one ref. The mutator is invoked while
    /// constructing the transaction, and its changes become visible only after the append is durable.
    void updateRefPublishedAt(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefPublishedAtUpdate &)> mutator);

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
    /// Returns the fence generation retained with the unresolved append of `ns` (0 when not wedged).
    uint64_t wedgedAdmittedGenerationForTest(const RootNamespace & ns);
    /// Installs a synthetic unresolved append for `ns` so callers can exercise resolution and blocking.
    /// `admitted_generation` defaults to the CURRENT fence generation, which is what a real wedge born
    /// now would carry; pass an explicit value to model a wedge admitted under an older incarnation.
    void forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                           const String & key, const String & bytes,
                           std::optional<uint64_t> admitted_generation = std::nullopt);
    /// Returns the seal that closed `ns`'s previous writer epoch -- the `prev_epoch_seal` its next
    /// sequence-1 append will carry (`nullopt` at genesis). See `RefTableRuntime::last_epoch_seal`.
    std::optional<RefTxnId> lastEpochSealForTest(const RootNamespace & ns);
    /// Installs `seal` as `ns`'s last epoch seal, standing in for the recovery CAS-walk that produces it
    /// in production (Task 6). Lets a writer-side test drive the ordinary post-transition append without
    /// a whole recovery.
    void setLastEpochSealForTest(const RootNamespace & ns, const std::optional<RefTxnId> & seal);
    /// Returns the post-durable install marker of `ns` (spec §A2; see `RefApplyState`). Reads the
    /// runtime's atomic directly -- no lock, and no recovery is forced beyond the runtime materialization
    /// every other seam here already performs.
    RefApplyState applyStateForTest(const RootNamespace & ns);
    /// Reports whether recovery or a prior incomplete sweep requires stale-precommit cleanup.
    bool needsStalePrecommitSweepForTest(const RootNamespace & ns);
    /// Waits until all background snapshot publications for `ns` have completed.
    void waitForSnapshotPublishSettleForTest(const RootNamespace & ns);
    /// Returns the number of background snapshot publications currently in flight for `ns`.
    int pendingSnapshotPublishesForTest(const RootNamespace & ns);
    /// Returns the newest snapshot id adopted by the cached runtime, if any.
    std::optional<RefTxnId> newestPublishedSnapshotIdForTest(const RootNamespace & ns);
    /// Returns the `sealed_from` installed alongside `newestPublishedSnapshotIdForTest`: the seal's
    /// observed-region upper bound when the newest snapshot is a recovery seal, else `nullopt`.
    std::optional<RefTxnId> sealedFromForTest(const RootNamespace & ns);
    /// Returns the number of applied transactions newer than the adopted snapshot.
    size_t tailSinceSnapshotCountForTest(const RootNamespace & ns);
    /// Returns the number of committed entries in the mutable overlay, when the COW representation has one.
    size_t committedOverlayEntriesForTest(const RootNamespace & ns);
    /// Returns this table's LIVE precommit view: the exact `{ref_name, manifest}` owner bindings that
    /// `precommitAdd` creates and that `promote` (move to committed) or `abandon` (exact precommit
    /// removal) take away again. A leaked binding here is the same-epoch precommit leak the stale sweep
    /// -- prior-epoch-scoped -- can never reclaim, so it is what an abandon-path test must assert on.
    std::set<std::pair<String, ManifestRef>> livePrecommitsForTest(const RootNamespace & ns);
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
    /// itself, must leave the committed chunk's callers with their success). `PostDurableInstall` fires
    /// inside `commitRefChunk` after that chunk's `PUT` returned `Committed` and BEFORE the prepared
    /// candidate is installed into the live state -- the seam a test uses to prove that the region
    /// between "durable" and "recorded" can no longer strand a transaction (a throw injected there is
    /// the only way left to simulate the OLD post-durable apply failure, since the install itself is now
    /// allocation-free and cannot throw). Null in production.
    enum class CarvePhaseForTest { PlanSeenRefs, PlanBatchGrow, PlanReserveOwned, PublishPop, ValidateFinalOps, ChunkReseed, PostDurableInstall };
    void setCarveHookForTest(std::function<void(CarvePhaseForTest)> hook) { carve_hook_for_test = std::move(hook); }

    /// Installs the negative control for the post-durable install region (see
    /// `install_region_probe_for_test`).
    void setInstallRegionProbeForTest(std::function<void()> probe) { install_region_probe_for_test = std::move(probe); }

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
    /// Recovery-publication inventory accessors: the seeded per-table admission budgets, the recovered
    /// base snapshot's encoded body size, the tail-since-snapshot byte sum, and the `_cleanup` markers
    /// observed at recovery. Together with `newestPublishedSnapshotIdForTest`,
    /// `tailSinceSnapshotCountForTest`, `needsStalePrecommitSweepForTest` and the resolved state, they
    /// let a test assert EVERY `RecoveryResult` field the install seeds.
    uint64_t refSnapshotBudgetForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        std::lock_guard<std::mutex> g(rt->state_mutex);
        return rt->snapshot_budget;
    }
    uint64_t refRemovalBudgetForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        std::lock_guard<std::mutex> g(rt->state_mutex);
        return rt->removal_budget;
    }
    uint64_t refBaseSnapshotBytesForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        return rt->base_snapshot_bytes.load(std::memory_order_relaxed);
    }
    uint64_t refTailBytesSinceSnapshotForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        return rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
    }
    std::set<RefTxnId> refCleanupMarkersForTest(const RootNamespace & ns)
    {
        const auto rt = getRefTableRuntime(ns);
        std::lock_guard<std::mutex> g(rt->state_mutex);
        return rt->cleanup_markers;
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
    std::function<uint64_t()> fence_generation_fn;
    std::function<void(uint64_t)> check_fence_or_throw;
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
    ///
    /// The four fields together are the wedge's IDENTITY, and all four are compared before any later
    /// result is acted on (`resolveWedgeOnce`'s post-I/O recheck). Comparing the id alone -- or the
    /// admission generation alone -- is the aliasing bug the phase-0 model found: two attempts of the
    /// same table can carry the same id under the same generation and describe DIFFERENT bytes, and
    /// installing one attempt's candidate because the other's key resolved is precisely the
    /// acked-then-lost class this every-attempt rule exists to close.
    struct RefAppendWedge
    {
        RefTxnId txn_id;
        String key;
        String bytes;
        /// `CasMountRuntime::fenceGeneration()` as read at this transaction's ADMISSION -- the same
        /// critical section that snapshotted the state and derived the id, i.e. one atomic reading of
        /// "what this attempt was allowed to do". Every later `slotOccupy` retry is gated on THIS value
        /// (never the current one), and every install is preceded by presenting it back through
        /// `checkFenceOrThrow`: a retry admitted under a dead incarnation must send nothing, and a
        /// result that returns after a fence bump/re-arm must install nothing.
        uint64_t admitted_fence_generation = 0;
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
        /// The `EpochSeal` transaction that closed this namespace's PREVIOUS writer epoch -- exactly the
        /// `prev_epoch_seal` that this table's next sequence-1 transaction must carry (INV-2's grammar:
        /// required on sequence 1 of every epoch above the namespace's genesis, forbidden everywhere
        /// else). Guarded by `state_mutex`, and read in the SAME hold that derives the id, so the field
        /// and the sequence number it qualifies are one reading.
        ///
        /// `nullopt` means GENESIS, and it means it exactly: the namespace's recovered state contains no
        /// seal and its `greatest_applied.writer_epoch` is its own `life_epoch`, so its first
        /// transaction opens the stream rather than continuing one across a transition. A namespace born
        /// at global epoch 5 therefore appends `{5, 1}` with NO `prev_epoch_seal`; its first transition
        /// (5 -> 6) seals `{5, T+1}`, and the `{6, 1}` that follows carries that seal.
        ///
        /// Two producers set it: the wedge resolution, when a successor's seal is found occupying the
        /// wedged key (`resolveWedgeOnce`'s conclusive-rejection arm -- the seal it observed IS this
        /// namespace's epoch-closing record), and recovery's CAS-walk, which installs the last seal of
        /// the chain it walked (Task 6). Nothing else writes it: a seal is durable evidence, never a
        /// local guess.
        std::optional<RefTxnId> last_epoch_seal;
        /// The post-durable install marker (spec §A2; see `RefApplyState` for the state machine and for
        /// why it is an assert layer rather than a fence). Atomic and RELAXED because it is written
        /// inside the install regions, which must not allocate and must not take another lock -- and
        /// because it publishes no other state: every reader wants the marker itself, never a
        /// happens-before edge to the table state (which `state_mutex` provides).
        std::atomic<RefApplyState> apply_state{RefApplyState::Clean};
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
        /// The `sealed_from` of the snapshot at `newest_snapshot_id`, when that snapshot is a recovery
        /// seal: the upper bound of what that recovery's LIST actually observed (`nullopt` for an
        /// ordinary snapshot or a never-published table). Installed here so the recovered runtime carries
        /// the COMPLETE `RecoveryResult` inventory rather than dropping one field (the drift-proof
        /// publication invariant, Codex round 4 §5). The GC orphan sweep obtains `sealed_from`
        /// independently via its own `recoverRefTableDetailed`, so this copy has no hot-read consumer in
        /// the ledger today; it is kept for inventory completeness and introspection.
        std::optional<RefTxnId> sealed_from;
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

    /// `mutable` for `confirmExactRef`, the one CONST member function that needs the lane snapshot:
    /// taking a mutex to read consistently does not make the read a mutation.
    mutable std::mutex ref_queue_mutex;
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

    /// The id `rt`'s next transaction carries (INV-1): `RefTableState::nextTxnId` of the table's OWN
    /// state under the live writer epoch. There is no counter behind this -- the id is a pure function
    /// of the state the transaction will be applied to, which buys two properties a pool-wide counter
    /// could not:
    ///   - each namespace's ids are dense `1..T` within one epoch, so a reader holding a table's log
    ///     ids can tell a COMPLETE stream from a truncated one without consulting anything else;
    ///   - an attempt that provably sent nothing consumes nothing: the state it derived from is
    ///     unchanged, so the next caller derives the SAME id and no hole is left behind.
    ///
    /// "Provably sent nothing" is the premise of the second property, and one path breaks it: a
    /// post-durable install failure SENT the transaction and it IS durable -- only the install that
    /// would have recorded it threw (spec §A2's `Poisoned`). Deriving from `greatest_applied` alone
    /// would re-derive that id and collide with our own durable object. `RefTableState::durable_floor`
    /// is what keeps the derivation honest there, which is why this goes through `nextTxnId` rather
    /// than reading `getGreatestApplied` itself.
    ///
    /// The epoch component is the live mount incarnation's writer epoch, not the open-time
    /// `process_epoch`: a self-remount allocates a strictly-greater durable writer_epoch, so every ref
    /// transaction stamped after the remount sorts strictly ABOVE any (dead-incarnation or twin) log
    /// still durable under an older epoch. `RefTxnId` compares epoch first, so the epoch bump alone
    /// guarantees that a new log is never inserted at or below an already durable table log id.
    ///
    /// MUST be called with `rt.state_mutex` held, and the caller must apply the transaction to the SAME
    /// state it read here: an id derived from one snapshot of a table and applied to another is not that
    /// table's successor, and the apply-side density check would (correctly) reject it.
    RefTxnId allocateRefTxnId(const RefTableRuntime & rt) const
    {
        return rt.state.nextTxnId(live_epoch_fn());
    }

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

    /// Test-only probe fired INSIDE each of the THREE post-durable install regions, i.e. under their
    /// `DENY_ALLOCATIONS_IN_SCOPE`: `commitRefChunk`'s candidate install (`Committed`) and its wedge
    /// install (`Unresolved`), and the wedge-resolution install in `flushRefBatch`. It is the negative
    /// control for the guard: a probe that allocates must abort a debug build, which is what proves the
    /// region is armed and actually entered -- so a future edit that adds an allocating statement there
    /// cannot pass unnoticed. It is ALSO the only way left to reach the `Poisoned` transition (spec
    /// §A2), by throwing an exception BUILT OUTSIDE the region (building it inside would trip the guard
    /// instead of testing the poison). A test that installs a throwing probe must therefore disarm it
    /// after the region it targets, or every later install throws too. Null in production.
    std::function<void()> install_region_probe_for_test;

    /// Returns the cached runtime for `ns`, creating an empty unrecovered runtime when needed.
    std::shared_ptr<RefTableRuntime> getRefTableRuntime(const RootNamespace & ns);

    /// Lazily recovers `ns` by listing and replaying its snapshot/log objects. It does not expose the
    /// table as recovered until any required recovery seal is durable; concurrent callers serialize
    /// across the unlocked seal I/O through `recovery_in_progress`.
    void ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt);

    /// Publishes a completed streaming recovery into the runtime in one atomic step: copies EVERY field
    /// `RecoveryResult` carries into `rt` and sets `recovered` LAST, so a waiter woken after this returns
    /// never observes a partially-installed table. Struct-driven so a future added publication field
    /// cannot be silently dropped from a scattered assignment list (spec §5). MUST hold `rt.state_mutex`.
    void installRecoveryResult(RefTableRuntime & rt, RecoveryResult && result);

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
    /// and apply-after-commit ordering -- the LIVE state is still only ever advanced once the object is
    /// durable; `commitRefChunk`'s pre-`PUT` apply targets a private candidate that nothing else can
    /// observe. Every item it carves out of `pending` is appended to
    /// `owned_items` (the leader's responsibility set) at the moment it is carved. When a batch's total
    /// op count exceeds `ref_txn_max_ops`, the validation loop emits SEVERAL ref-log transactions in one
    /// tenure via `commitRefChunk` -- each a complete commit boundary.
    void flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                       std::vector<std::shared_ptr<RefMutationItem>> & owned_items);

    /// What one bounded wedge-resolution attempt decided. `Adopted` is the ONLY outcome that lets the
    /// flush proceed to carve a new batch; every other one fails the whole carve with
    /// `survivor_error` and returns, because the lane is either still uncertain or deliberately closed.
    enum class WedgeResolution : uint8_t
    {
        NoWedge,      /// nothing was wedged -- the ordinary flush
        Adopted,      /// the wedged transaction is durable; it is installed and the lane is clear
        Rejected,     /// a successor's `EpochSeal` occupies the key: the operation never landed and never will
        StillWedged,  /// unresolved, refused pre-attempt, or superseded -- the wedge is intact
        Corrupted,    /// a foreign non-seal object occupies the key -- impossible under mount exclusivity
    };

    struct WedgeResolutionResult
    {
        WedgeResolution kind = WedgeResolution::NoWedge;
        /// The error every queued caller of THIS flush receives, for every kind except `NoWedge` and
        /// `Adopted`. Built by `resolveWedgeOnce`, which is the only place that knows which of the
        /// several very different reasons applied.
        std::exception_ptr survivor_error;
    };

    /// ONE bounded resolution attempt for `rt`'s outstanding wedge (spec INV-1's every-attempt rule):
    /// at most one `slotOccupy(wedge.key, wedge.bytes, ...)` per calling flush, gated on the wedge's
    /// ORIGINAL `admitted_fence_generation` rather than the current one. There is deliberately NO
    /// background retry thread and no deadline-resetting loop: a permanently quiet wedged namespace
    /// waits for its next caller or for a remount, which is acceptable precisely because the wedged
    /// operation was never acknowledged.
    ///
    /// The conditional CREATE is what makes the rule "every attempt has its own conclusive rejection"
    /// affordable: the ref-log key is write-once, so a create either lands our exact bytes (the
    /// transaction is durable -- and it is the SAME transaction, byte for byte) or conflicts with
    /// whatever is there, which the follow-up read then names. A read alone could only ever report
    /// "absent", which is not a rejection: the earlier ambiguous attempt could still land afterwards.
    ///
    /// Post-I/O recheck: the outcome is adjudicated on an I/O result, so before ANY action follows from
    /// it (adopt, acknowledge, unwedge, fail the survivors) this re-acquires `state_mutex`, presents
    /// `admitted_fence_generation` back through `checkFenceOrThrow`, and compares the full wedge
    /// identity against what is still installed. A result that returns after a fence bump/re-arm, or
    /// after the wedge it belonged to was replaced, is INERT for this runtime.
    WedgeResolutionResult resolveWedgeOnce(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Commits ONE chunk of a `flushRefBatch` tenure as a complete ref-log transaction: allocates the
    /// real transaction id, applies `chunk_ops` to a CANDIDATE copy of `rt->state`, encodes and durably
    /// `PUT`s them, installs the candidate under `state_mutex` by a no-throw swap, advances the tail
    /// counters, records the per-transaction metrics, completes exactly `chunk_survivors` with the real
    /// id (waking their waiters), and schedules snapshot publication. The apply comes BEFORE the `PUT`
    /// so that nothing between "durable" and "recorded" can throw (spec §A1); a failure of that apply is
    /// an ordinary pre-durability rejection. For the same reason the `RefAppendWedge` is built COMPLETE
    /// before the `PUT` -- the request reads its key and body -- so the `Unresolved` arm only has to move
    /// it into the runtime: the OTHER thing that must be recorded once the object may be durable.
    /// Returns true when the chunk committed durably; false on any non-throwing failure (a rejected
    /// apply / DefiniteFailure / unresolved wedge / a conclusive PUT rejection / an encode failure),
    /// after having already failed `chunk_survivors` with the appropriate error. Past the durable `PUT`
    /// it does not throw at all.
    /// PRECONDITION: the caller has already released its scratch `working` copy so the post-commit
    /// overlay fold is in place (the E5 fast path).
    bool commitRefChunk(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                        const std::vector<RefOp> & chunk_ops,
                        const std::vector<std::shared_ptr<RefMutationItem>> & chunk_survivors);

    /// The three `RefApplyState` transitions (spec §A2), the ONLY writers of `RefTableRuntime::
    /// apply_state`. All of them are allocation-free at the point where it matters, so they can be
    /// called from inside a `DENY_ALLOCATIONS_IN_SCOPE` install region.
    ///
    /// `Clean -> ApplyPending`, armed immediately before a durable `PUT`. A CAS rather than a store, so
    /// arming can never resurrect a `Poisoned` table; a lane that is somehow already `ApplyPending`
    /// (impossible under the wedge hard contract, which forbids a new `PUT` while wedged) is left as it
    /// is, which is the conservative value anyway.
    static void armApplyPending(RefTableRuntime & rt) noexcept;
    /// `ApplyPending -> Clean`, on a completed install AND on every path that PROVES nothing became
    /// durable, so no apply is owed. Also a CAS: `Poisoned` is terminal, and a `Clean` table stays
    /// `Clean`.
    static void clearApplyPending(RefTableRuntime & rt) noexcept;
    /// `* -> Poisoned` with the `CasRefApplyPoisoned` event, emitted once per TRANSITION (a
    /// re-poisoned table is already accounted for). `noexcept`: it runs from a `catch` that is about to
    /// rethrow the original post-durable exception, and a failure to REPORT the poison must never
    /// replace it -- so the marker and the event are set first and only the logging is swallowed.
    static void poisonApplyState(RefTableRuntime & rt, const RootNamespace & ns, std::string_view region) noexcept;

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
