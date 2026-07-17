#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadPool.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}
}

namespace ProfileEvents
{
    extern const Event CasRefBatchFlushes;
    extern const Event CasRefBatchedMutations;
    extern const Event CasRefBatchScopeCuts;
    extern const Event CasRefQueueWaitMicroseconds;
    extern const Event CasRefRecoveryRestarts;
    extern const Event CasRefAppendWedged;
    extern const Event CasRefAppendUnwedged;
    extern const Event CasRefAppendDefiniteFailure;
    extern const Event CasRefSweepDeferred;
    extern const Event CasRefSweepRearmed;
    extern const Event CasRefStalePrecommitsReclaimed;
    extern const Event CasRefTableEvictions;
    extern const Event CasRefSnapshotPutBytes;
    extern const Event CasRefSnapshotTailLogs;
    extern const Event CasRefSnapshotPublishDispatched;
    extern const Event CasRefSnapshotPublishBackoff;
    extern const Event CasRefRecoverySealPublished;
}

namespace DB::Cas
{

CasRefLedger::CasRefLedger(
    BackendPtr backend_ptr,
    const Layout & layout_,
    RefLedgerConfig config_,
    const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_,
    std::function<uint64_t()> controller_boot_ms_fn,
    std::function<uint64_t()> live_epoch_fn_,
    std::function<bool()> fence_ok_fn_,
    std::function<uint64_t()> boot_ms_now_fn_,
    std::function<bool()> may_mutate_,
    std::function<uint64_t()> unclean_boundary_epoch_,
    std::function<void(const String &, const String &, const std::optional<String> &)> on_impossible_interference_,
    std::function<std::shared_ptr<void>()> pin_owner_,
    std::function<void(const RootNamespace &)> cancel_inflight_builds_)
    : backend(*backend_ptr)
    , layout(layout_)
    , config(std::move(config_))
    , event_sink(event_sink_)
    , cas_request_budget(cas_request_budget_)
    , live_epoch_fn(std::move(live_epoch_fn_))
    , fence_ok_fn(std::move(fence_ok_fn_))
    , boot_ms_now_fn(std::move(boot_ms_now_fn_))
    , may_mutate(std::move(may_mutate_))
    , unclean_boundary_epoch(std::move(unclean_boundary_epoch_))
    , on_impossible_interference(std::move(on_impossible_interference_))
    , pin_owner(std::move(pin_owner_))
    , cancel_inflight_builds(std::move(cancel_inflight_builds_))
{
    /// The ref-log writer path uses the same retry controller and clock seam as the mount's local
    /// write fence, so deadline-sensitive tests exercise both paths with one monotonic clock.
    /// The raw mount `boot_ms_fn` -- the SAME fake-clock seam the local write fence uses -- is reused
    /// here rather than adding a second clock knob; both are monotonic-ms clocks and tests that need
    /// deterministic deadline behavior already inject it.
    ref_request_controller = std::make_unique<CasRequestController>(backend_ptr, cas_request_budget, controller_boot_ms_fn);
}

CasWriteOutcome CasRefLedger::stagingPutIfAbsent(std::string_view key, std::string_view bytes, Token * out_token)
{
    /// The ref lane's mount predicate (`fence_ok_fn` == `Pool::refAppendFenceOk`, with no per-table
    /// runtime term) gates every attempt, matching the other staged writes.
    return ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok_fn, out_token);
}

CasCreateResult CasRefLedger::stagingConditionalCreate(std::string_view key, const std::function<PutResult()> & attempt)
{
    /// The supplied attempt is controlled by the same retry and mount-fence policy as other staged
    /// writes.
    return ref_request_controller->conditionalCreateControlled(key, attempt, fence_ok_fn);
}

void CasRefLedger::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    ref_request_controller->setSleepFnForTest(std::move(sleep_fn));
}


std::optional<Resolved> CasRefLedger::resolveRef(const RootNamespace & ns, const String & ref_name, bool /*allow_stale*/)
{
    /// The read side of the snapshot+log protocol has one authoritative cached table for this mounted
    /// writer. The `allow_stale` staleness-tolerance knob no longer selects anything: this mounted writer is the
    /// ONLY writer of `ns`'s ref state (no external CAS token to go stale against, unlike the old
    /// per-shard decode cache), so the recovered-and-cached `RefTableState` is always this process's
    /// authoritative view. Kept as a parameter so existing callers compile unchanged.
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    /// A table this mount only ever READS (never mutates) would otherwise
    /// never have its just-replayed tail/precommits checked -- `appendRefOps`'s own hoisted checks only
    /// fire for a table this mount WRITES to. Both are cheap (lock + comparison) on the warm path (the
    /// flag/threshold is already false after the table's first touch this mount); the sweep, if it DOES
    /// fire, runs synchronously here (safe: this call is not nested inside any queue leader's stack).
    /// Insulated (unlike appendRefOps's own hoisted call): a READ must not fail because a piggybacked
    /// maintenance action hit an uncertain PUT -- see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::lock_guard lock(rt->state_mutex);
    const auto it = rt->state.committed.find(ref_name);
    if (it == rt->state.committed.end())
        return std::nullopt;

    const RefCommittedRow & row = it->second;
    /// A resolved ref points to its manifest (the read-path entry point). `object_hash` is the manifest
    /// instance id the ref names; pairs with a later readManifest ReadMissing if that body is gone.
    if (hasEventSink())
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::RefResolve;
        _ev0.namespace_ = ns.string();
        _ev0.ref_name = ref_name;
        _ev0.object_kind = CasEventObjectKind::Manifest;
        _ev0.object_hash = manifestRefDebugString(row.manifest_ref);
        _ev0.outcome = "resolved";
        _ev0.reason = "read-side resolve of a ref to its part manifest";
        emitEvent(std::move(_ev0));
    }
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
        .manifest_size = 0,
        .published_at_ms = row.published_at_ms,
    };
}

std::map<String, Resolved> CasRefLedger::listRefs(const RootNamespace & ns)
{
    /// The whole ref set is a map iteration over this namespace's recovered-and-cached `RefTableState`:
    /// an empty or
    /// never-touched namespace still costs exactly one `LIST` (recovery) and zero further requests;
    /// a warm namespace costs nothing at all (replacing the old per-shard LIST-then-HEAD-present-shards
    /// dance, since there is no longer a shard fan-out to rediscover on every call).
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    /// Apply the same read-side maintenance policy as `resolveRef`; see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::map<String, Resolved> result;
    std::lock_guard lock(rt->state_mutex);
    for (const auto [ref_name, row] : rt->state.committed)
        result.emplace(ref_name, Resolved{
            .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
            .manifest_size = 0,
            .published_at_ms = row.published_at_ms,
        });
    return result;
}


std::shared_ptr<CasRefLedger::RefTableRuntime> CasRefLedger::getRefTableRuntime(const RootNamespace & ns)
{
    std::lock_guard lock(ref_queue_mutex);
    auto & slot = ref_tables[ns.string()];
    if (!slot)
        slot = std::make_shared<RefTableRuntime>();
    return slot;
}

void CasRefLedger::ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt)
{
    /// Held for the WHOLE recovery's LIST+replay: there is nothing safe to do with an unrecovered
    /// table's `state` anyway, so a concurrent second caller for the SAME namespace blocking here is
    /// correct, not a missed-concurrency opportunity -- and this only affects each table's FIRST touch
    /// per mounted `Pool`: one `LIST` caches the resulting complete table state. The recovery seal's
    /// encode and conditional `PUT` are the one exception and run below without the mutex --
    /// `recovery_in_progress` (not the mutex) is what serializes concurrent callers across that window;
    /// see its doc comment for why.
    {
    std::unique_lock lock(rt.state_mutex);
    /// Every touch, warm or cold,
    /// marks this table most-recently-used so `enforceRefTableCacheBudget` evicts idle tables first.
    rt.last_touch_tick = ref_table_access_tick.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rt.recovered)
        return;

    /// A concurrent second caller waits here rather than racing an independent
    /// LIST+replay+seal attempt against the first caller's unlocked seal PUT below.
    while (rt.recovery_in_progress)
    {
        ++rt.recovery_waiters_for_test;
        rt.recovery_cv.wait(lock);
        --rt.recovery_waiters_for_test;
    }
    if (rt.recovered)
        return;   /// the caller we waited on already finished it

    rt.recovery_in_progress = true;
    /// Cleared + broadcast on every exit from here, success or exception, so a waiter above is never
    /// left hanging on an error that escapes the unlocked seal PUT.
    SCOPE_EXIT({
        rt.recovery_in_progress = false;
        rt.recovery_cv.notify_all();
    });

    for (uint64_t attempt = 0; ; ++attempt)
    {
        if (attempt > 0)
        {
            if (attempt > kRefRecoveryMaxRestarts)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot or "
                    "log object kept vanishing between its LIST and GET) — giving up; this bound is a "
                    "runaway brake against a pathological cleanup race, not an expected steady state",
                    ns.string(), attempt - 1));
            ++rt.recovery_restarts;
            ProfileEvents::increment(ProfileEvents::CasRefRecoveryRestarts);
        }

        /// One namespace `LIST` returns every surviving snapshot, log,
        /// and `_cleanup` marker key.
        std::optional<RefTxnId> greatest_snapshot;
        std::vector<RefTxnId> log_ids;
        std::set<RefTxnId> cleanup_markers;
        const String prefix = layout.refsNamespacePrefix(ns);
        String cursor;
        for (;;)
        {
            const ListPage page = backend.list(prefix, cursor, /*limit=*/1000);
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = layout.parseRefObjectKey(lk.key);
                if (!parsed)
                    continue;   /// not a ref-object key (for example, a legacy shard-number key)
                /// Trust the parsed `ns` only when it names EXACTLY this
                /// namespace -- the same checkNamespace-level guarantee the key builders enforce, not
                /// position math (the scoped LIST prefix already implies this in practice, but a listed
                /// key is untrusted input and is treated as such).
                if (parsed->ns != ns)
                    continue;
                switch (parsed->kind)
                {
                    case RefObjectKind::Cleanup:
                        cleanup_markers.insert(parsed->txn_id);
                        break;
                    case RefObjectKind::Log:
                        log_ids.push_back(parsed->txn_id);
                        break;
                    case RefObjectKind::Snap:
                        if (!greatest_snapshot || *greatest_snapshot < parsed->txn_id)
                            greatest_snapshot = parsed->txn_id;
                        break;
                }
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
        std::sort(log_ids.begin(), log_ids.end());

        /// The greatest transaction id this attempt's `LIST` observed across
        /// BOTH snapshots and logs -- used below to decide whether a dead-epoch region exists to seal.
        /// Recomputed fresh on every restart-on-vanish attempt, exactly like `greatest_snapshot`/`log_ids`.
        std::optional<RefTxnId> greatest_listed_id = greatest_snapshot;
        if (!log_ids.empty() && (!greatest_listed_id || *greatest_listed_id < log_ids.back()))
            greatest_listed_id = log_ids.back();

        bool vanished = false;
        std::optional<RefTableSnapshot> snapshot;
        uint64_t snapshot_body_bytes = 0;   /// weight of the recovered base; 0 = never-born base
        if (greatest_snapshot)
        {
            const auto got = backend.get(layout.refSnapshotKey(ns, *greatest_snapshot));
            if (!got)
                vanished = true;   /// covered by a newer snapshot published-before-delete; restart
            else
            {
                snapshot = decodeRefTableSnapshot(openObject(FormatId::RefSnapshot, got->bytes), ns.string(), *greatest_snapshot);
                snapshot_body_bytes = got->bytes.size();
            }
        }

        std::vector<RefLogTxn> tail;
        std::vector<uint64_t> tail_bytes;
        if (!vanished)
        {
            for (const RefTxnId & id : log_ids)
            {
                if (greatest_snapshot && !(*greatest_snapshot < id))
                    continue;   /// at or below the selected snapshot: already covered
                const auto got = backend.get(layout.refLogKey(ns, id));
                if (!got)
                {
                    vanished = true;
                    break;
                }
                tail.push_back(decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id));
                tail_bytes.push_back(got->bytes.size());
            }
        }

        if (vanished)
            continue;   /// the selected object vanished; retry recovery from a fresh listing

        rt.state = replay(snapshot, tail);
        rt.cleanup_markers = std::move(cleanup_markers);

        /// At an UNCLEAN mount, close every dead epoch this
        /// recovery discovered with an immediate snapshot -- the seal -- published BEFORE the table is
        /// exposed as recovered. This runs BEFORE `rt.recovered = true` below on purpose: a failed seal
        /// PUT throws here, leaving the table unrecovered, so the NEXT touch restarts recovery from
        /// scratch (fresh LIST, fresh replay, fresh seal attempt) rather than exposing a table whose
        /// dead-epoch region was never actually closed; recovery therefore fails closed.
        /// The encode and conditional `PUT` run OUTSIDE `state_mutex` (unlocked/relocked just below,
        /// mirroring `trySnapshotPublishOnce`'s copy-under-lock/PUT-outside shape) -- it is the one part
        /// of recovery that can run the full ~90s retry envelope, and holding the mutex across it stalls
        /// every other touch of this table plus `wedgedRefLaneCount`'s whole-store walk (which locks each
        /// table's `state_mutex` in turn). `recovery_in_progress` (set above), not the mutex, is what
        /// keeps a concurrent second caller for this SAME table from redoing this same work during the
        /// unlocked window -- see its doc comment. Nothing else can mutate `rt.state`/`rt.cleanup_markers`
        /// meanwhile: every OTHER touch-point calls this function first and would block in the wait loop
        /// above; `rt` itself cannot be destroyed underneath us (the caller's own `shared_ptr` keeps it
        /// alive regardless of the mutex, which is also what protects it from `enforceRefTableCacheBudget`
        /// -- eviction only ever considers a table at `use_count() == 1`).
        ///
        /// `seal_id` is the UPPER BOUND of the dead-epoch region, not the greatest listed id: the wedge
        /// discipline places the predecessor's one possible in-flight PUT strictly above everything it
        /// resolved, so only the epoch-closing bound is guaranteed to dominate it --
        /// any later materialization from a dead epoch is born covered (`<=` seal_id) for every
        /// observer, forever. `sealed_from` records the greatest id this recovery actually listed, the
        /// observation horizon against which a later log is measured.
        const uint64_t my_epoch = live_epoch_fn();
        const RefTxnId seal_id{my_epoch - 1, std::numeric_limits<uint64_t>::max()};
        const bool dead_region_nonempty =
            greatest_listed_id.has_value() && greatest_listed_id->writer_epoch < my_epoch;
        const bool already_sealed = snapshot.has_value() && !(snapshot->snapshot_id < seal_id);
        /// Compare against the SPECIFIC epoch a reclaim was marked unclean for, not a
        /// sticky "ever" bool -- a table recovered for the first time (or reloaded after LRU eviction)
        /// under a LATER, perfectly clean epoch boundary must not get a parasitic seal just because
        /// SOME earlier, unrelated boundary in this incarnation's life was unclean.
        if (unclean_boundary_epoch() == my_epoch && my_epoch >= 2
            && dead_region_nonempty && !already_sealed && rt.state.lifecycle == RefLifecycle::Live)
        {
            RefTableSnapshot seal = snapshotOf(rt.state, ns.string());
            seal.snapshot_id = seal_id;                     /// upper bound of the covered region
            seal.sealed_from = rt.state.greatest_applied;    /// == greatest_listed_id: nothing else was applied
            const String seal_bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(seal));
            const auto fence_ok = [this] { return fence_ok_fn(); };
            /// The `SCOPE_EXIT` above mutates
            /// `recovery_in_progress` and notifies `recovery_cv`, and it MUST run with `state_mutex`
            /// held. `putIfAbsentControlled` can THROW (its contract: `CORRUPTED_DATA` when it
            /// observes DIFFERENT valid bytes at the key -- a cross-process seal conflict, which
            /// `recovery_in_progress` cannot serialize); if that exception escaped between `unlock`
            /// and `lock`, the unwind would run the `SCOPE_EXIT` UNLOCKED -- a data race on the plain
            /// bool and an unlocked notify. So re-acquire the lock before letting any exception
            /// propagate. NOTE this obligation is NEW relative to `trySnapshotPublishOnce` (the
            /// pattern this mirrors): that precedent has no scope-exit spanning its unlocked call and
            /// no cross-caller flag to clean up -- do not weaken this by analogy to it.
            lock.unlock();
            CasWriteOutcome outcome;
            try
            {
                outcome = ref_request_controller->putIfAbsentControlled(
                    layout.refSnapshotKey(ns, seal_id), seal_bytes, fence_ok);
            }
            catch (...)
            {
                lock.lock();
                throw;
            }
            lock.lock();
            if (outcome != CasWriteOutcome::Committed)
                throwCasWriteRetryLater(fmt::format(
                    "CAS recovery seal PUT for namespace '{}' did not commit; failing recovery closed "
                    "(the table stays unrecovered/non-writable; the next touch restarts recovery and "
                    "re-seals)", ns.string()));
            ProfileEvents::increment(ProfileEvents::CasRefRecoverySealPublished);

            /// The seal now covers everything replayed so far: feed it into the SAME bookkeeping below
            /// (`rt.newest_snapshot_id`, the tail counters, cache weight) as if it were the recovered
            /// snapshot and the tail were empty -- exactly what a fresh recovery against the now-durable
            /// seal would find.
            snapshot = std::move(seal);
            tail.clear();
            tail_bytes.clear();
            snapshot_body_bytes = seal_bytes.size();
        }

        rt.recovered = true;

        /// Seed the tail counters from this SAME recovery
        /// pass -- `rt.state` above already IS `Replay(snapshot, tail)` (the mount-time trigger's
        /// candidate body needs no separate base+tail bookkeeping any more), so only the count and byte
        /// sum of the logs strictly above the recovered snapshot need seeding.
        rt.newest_snapshot_id = snapshot ? std::optional<RefTxnId>(snapshot->snapshot_id) : std::nullopt;
        rt.tail_count_since_snapshot.store(tail.size(), std::memory_order_relaxed);
        uint64_t seeded_tail_bytes = 0;
        for (uint64_t bytes : tail_bytes)
            seeded_tail_bytes += bytes;
        rt.tail_bytes_since_snapshot.store(seeded_tail_bytes, std::memory_order_relaxed);
        /// Stale-precommit cleanup is dispatched once, from `appendRefOps`'s top level
        /// (never from here -- this call may itself be nested inside a queue leader's flush stack).
        rt.needs_stale_precommit_sweep = true;

        /// Per-table admission budgets pre-subtract this table's own wire
        /// overhead (`4 + ns.size()`, repeated once in a snapshot body and once in a removal txn body)
        /// plus a fixed safety margin from the raw hard limits, once, here.
        const uint64_t overhead = 4 + ns.string().size() + kRefAdmissionSafetyMargin;
        rt.snapshot_budget = overhead < ref_snapshot_max_bytes ? ref_snapshot_max_bytes - overhead : 0;
        rt.removal_budget = overhead < ref_removal_max_bytes ? ref_removal_max_bytes - overhead : 0;

        /// The cache-weight base is the encoded body size of the recovered
        /// base snapshot, captured for free from the GET above. The tail bytes are tracked separately in
        /// `tail_bytes_since_snapshot`; the two sum to this table's estimated resident weight.
        rt.base_snapshot_bytes.store(snapshot_body_bytes, std::memory_order_relaxed);
        break;
    }
    }

    /// A NEW table was just materialized; enforce the whole-table cache budget, protecting this one
    /// The pass runs OUTSIDE `rt.state_mutex` (that scope closed above) so
    /// the pass -- which acquires `ref_queue_mutex` and try-locks other tables' `state_mutex` -- never
    /// nests this table's `state_mutex` under `ref_queue_mutex`.
    enforceRefTableCacheBudget(ns);
}


void CasRefLedger::enforceRefTableCacheBudget(const RootNamespace & keep_ns)
{
    if (config.ref_table_cache_bytes == 0)
        return;   /// 0 = unbounded: eviction disabled

    /// Evicted runtimes are held alive here until AFTER every lock is released, so a runtime whose sole
    /// owner is its map slot is never destroyed while we still hold its `state_mutex` (that would destroy
    /// a locked mutex).
    std::vector<std::shared_ptr<RefTableRuntime>> evicted;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);

        /// Relaxed atomic reads: the `total` loop below reads this for EVERY table, including hot ones a
        /// concurrent append lane is mutating under `state_mutex` only (a cross-lock read). The gated
        /// candidate loop reads it too, but only for `use_count()==1` tables (no concurrent writer).
        const auto weightOf = [](const RefTableRuntime & rt)
        {
            return rt.base_snapshot_bytes.load(std::memory_order_relaxed)
                 + rt.tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        };

        uint64_t total = 0;
        for (const auto & [name, rt] : ref_tables)
            total += weightOf(*rt);
        if (total <= config.ref_table_cache_bytes)
            return;

        /// Idle candidates, least-recently-touched first. Idle == the map holds the SOLE `shared_ptr`
        /// (`use_count() == 1`: no in-flight caller, queued append, leader, or background publish holds a
        /// copy), no active queue leader, an empty pending queue, and not the just-recovered `keep_ns`.
        /// The `use_count() == 1` gate is what makes append-lane split-brain impossible: any thread that
        /// fetched this runtime keeps it non-evictable for as long as it holds the copy.
        struct Cand { String name; uint64_t tick; uint64_t weight; };
        std::vector<Cand> cands;
        for (const auto & [name, rt] : ref_tables)
        {
            if (name == keep_ns.string())
                continue;
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty())
                continue;
            cands.push_back(Cand{name, rt->last_touch_tick, weightOf(*rt)});
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand & a, const Cand & b) { return a.tick < b.tick; });

        for (const Cand & c : cands)
        {
            if (total <= config.ref_table_cache_bytes)
                break;
            auto it = ref_tables.find(c.name);
            if (it == ref_tables.end())
                continue;
            std::shared_ptr<RefTableRuntime> & rt = it->second;
            {
                /// `use_count() == 1` guarantees no other thread holds the runtime, so this try_lock
                /// cannot fail; the RAII scope releases `state_mutex` before `rt` is moved out. A wedged
                /// append lane is never evicted -- its uncertain in-flight PUT is not reconstructable from
                /// the durable objects, and re-recovery could re-allocate an id:
                /// Linearization forbids this).
                std::unique_lock<std::mutex> slock(rt->state_mutex, std::try_to_lock);
                if (!slock.owns_lock() || rt->wedge.has_value())
                    continue;
            }
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty())
                continue;   /// re-check under the still-held ref_queue_mutex
            total -= c.weight;
            evicted.push_back(std::move(rt));   /// keep alive past the erase and lock release
            ref_tables.erase(it);
            ProfileEvents::increment(ProfileEvents::CasRefTableEvictions);
        }
    }
    /// `evicted` destructs the dropped runtimes here, with no lock held.
}


bool CasRefLedger::refLanesSettledForRemount()
{
    /// Same wait mechanics as `drainRefLanesForShutdown`, without its `shutting_down` admission latch --
    /// self-remount mutations are already refused by the freshly-tripped mount fence, not an admission
    /// check, so there is nothing to latch here. Budget is exactly one attempt's worth: long enough for
    /// an in-flight leader to observe the tripped fence and settle, never unbounded.
    const uint64_t wait_budget_ms =
        cas_request_budget.attempt_timeout_ms + cas_request_budget.lease_safety_margin_ms;

    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }

    /// Wait for every table's queue to go idle (no pending item, no active leader), bounded overall by
    /// `wait_budget_ms` -- mirrors `drainRefLanesForShutdown`'s own wait loop exactly (see its comments).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(ref_queue_mutex);
        for (const auto & rt : runtimes)
        {
            while (!(rt->pending.empty() && !rt->leader_active))
            {
                if (rt->cv.wait_until(lk, deadline) == std::cv_status::timeout
                    && !(rt->pending.empty() && !rt->leader_active))
                {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out)
                break;
        }
    }

    /// A queue going idle does NOT by itself prove no PUT is in flight -- see `drainRefLanesForShutdown`'s
    /// identical comment on this same check. Every table is checked regardless of `timed_out`, purely for
    /// a complete diagnostic; the return value already fails closed on either condition alone.
    bool any_wedge = false;
    for (const auto & rt : runtimes)
    {
        std::lock_guard<std::mutex> lock(rt->state_mutex);
        if (rt->wedge.has_value())
            any_wedge = true;
    }

    return !timed_out && !any_wedge;
}


void CasRefLedger::quiesceRefTablesForRemount()
{
    /// Snapshot the current runtimes (copies keep them alive across the drain). New dispatches are
    /// already suppressed while the fence is lost (`maybeScheduleSnapshotPublish`'s fence guard), so the
    /// only publishers to drain are those dispatched before the fence dropped.
    std::vector<std::shared_ptr<RefTableRuntime>> tables;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        tables.reserve(ref_tables.size());
        for (auto & [name, rt] : ref_tables)
            tables.push_back(rt);
    }

    /// Wait for every in-flight background publisher to finish so none is mid-PUT when its runtime is
    /// detached. A publisher observes the lost fence (`fence_ok` false) and returns without committing,
    /// then decrements `pending_snapshot_publishes` under `state_mutex` and signals `publish_settle_cv`.
    for (auto & rt : tables)
    {
        std::unique_lock<std::mutex> slock(rt->state_mutex);
        rt->publish_settle_cv.wait(slock,
            [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
    }

    /// Detach every cached table. Mark it superseded FIRST (release, and before the caller re-arms the
    /// fence): a leader that raced in and holds one of these orphaned runtimes then fails closed at the
    /// `flushRefBatch` gate rather than allocating an id against a stale cache under the re-armed fence.
    /// Queued callers self-drain -- each `flushRefBatch` for a superseded runtime completes its whole
    /// carved batch with a retry error, so no caller hangs; the next touch creates a fresh runtime that
    /// re-recovers from the durable snapshot+log objects under `live_writer_epoch`. Dropping the map slot
    /// discards each runtime's in-memory wedge; `refLanesSettledForRemount` already consulted it above
    /// and `tryRemountOnce` paid `materialization_grace_ms` when it was unresolved, so nothing about this
    /// drop needs to be certified here.
    std::vector<std::shared_ptr<RefTableRuntime>> detached;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        detached.reserve(ref_tables.size());
        for (auto & [name, rt] : ref_tables)
        {
            rt->superseded_by_remount.store(true, std::memory_order_release);
            rt->cv.notify_all();   /// wake any waiter so it re-leads and fails closed against the flag
            detached.push_back(rt);
        }
        ref_tables.clear();
    }
    /// `detached` releases the map's references here (with no lock held); each runtime lives on only as
    /// long as an in-flight leader/caller still holds it.
}


uint64_t CasRefLedger::refRecoveryRestartsForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->recovery_restarts;
}

bool CasRefLedger::refLaneWedgedForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->wedge.has_value();
}

String CasRefLedger::wedgedKeyForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->wedge ? rt->wedge->key : String{};
}

void CasRefLedger::forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                              const String & key, const String & bytes)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    rt->wedge = RefAppendWedge{RefTxnId{writer_epoch, ref_sequence}, key, bytes};
}

bool CasRefLedger::needsStalePrecommitSweepForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->needs_stale_precommit_sweep;
}


size_t CasRefLedger::wedgedRefLaneCount()
{
    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }
    size_t wedged = 0;
    for (const auto & rt : runtimes)
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->wedge.has_value())
            ++wedged;
    }
    return wedged;
}


bool CasRefLedger::drainRefLanesForShutdown(uint64_t wait_budget_ms)
{
    /// Latch FIRST, then snapshot under `ref_queue_mutex` (see the `shutting_down` member comment): this
    /// ordering is what makes the check in `appendRefOps` -- performed inside the SAME critical section
    /// as its `pending.push_back` -- race-free against the snapshot below, for both an already-cached
    /// table and one whose very first touch races this call.
    shutting_down.store(true, std::memory_order_release);

    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_tables.size());
        for (const auto & [_, rt] : ref_tables)
            runtimes.push_back(rt);
    }

    /// Wait for every table's queue to go idle (no pending item, no active leader), bounded overall by
    /// `wait_budget_ms` -- `cv.wait_until` slices against one shared deadline, never a sleep. All the
    /// runtimes share the one `ref_queue_mutex` that guards `pending`/`leader_active` (see the
    /// `RefTableRuntime` field comments), so a single `lk` covers every table in the loop below; each
    /// table's OWN `cv` is what its leader/appendRefOps notifies on a state change, so the wait must
    /// target that specific `cv`, one table at a time.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(ref_queue_mutex);
        for (const auto & rt : runtimes)
        {
            while (!(rt->pending.empty() && !rt->leader_active))
            {
                if (rt->cv.wait_until(lk, deadline) == std::cv_status::timeout
                    && !(rt->pending.empty() && !rt->leader_active))
                {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out)
                break;
        }
    }

    /// A queue going idle does NOT by itself prove no PUT is in flight: a wedge is recorded (under
    /// `state_mutex`) strictly BEFORE the wedged item's caller is completed and the leader bookkeeping
    /// reset (see `flushRefBatch`'s `Unresolved` case), so this check -- performed AFTER the wait above
    /// -- observes it whenever the queue-idle wait itself raced a wedge. Every table is checked
    /// regardless of `timed_out`, purely for a complete diagnostic; the return value already fails
    /// closed on either condition alone.
    bool any_wedge = false;
    for (const auto & rt : runtimes)
    {
        std::lock_guard<std::mutex> lock(rt->state_mutex);
        if (rt->wedge.has_value())
            any_wedge = true;
    }

    return !timed_out && !any_wedge;
}


bool CasRefLedger::observedNamespaceCleanupMarker(const RootNamespace & ns, const RefTxnId & remove_txn_id)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    if (rt->cleanup_markers.contains(remove_txn_id))
        return true;

    /// Warm-mount re-observation: the recovery `LIST` that populated `cleanup_markers` may have
    /// run BEFORE GC's namespace-cleanup item published the `_cleanup/<remove_txn_id>` marker, so a
    /// warm-mounted writer that dropped a namespace and recreates it within the same mount lifetime would
    /// otherwise be rejected until it remounts. Do ONE exact-key backend check of the marker before
    /// answering; if it is durably present now, adopt it into the cached set. This preserves fail-close
    /// (a still-absent marker keeps recreation rejected -- evidence is refreshed, never assumed) and
    /// matches the recovery restart-on-vanish philosophy of consulting the durable object on a cache miss.
    const HeadResult head = backend.head(layout.refCleanupMarkerKey(ns, remove_txn_id));
    if (head.exists)
    {
        rt->cleanup_markers.insert(remove_txn_id);
        return true;
    }
    return false;
}


RefTxnId CasRefLedger::appendRefOps(const RootNamespace & ns, MutationScope scope,
                             std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                             RootMutationOrigin origin, RootMutationKind kind,
                             bool skip_stale_precommit_sweep)
{
    const auto rt = getRefTableRuntime(ns);
    /// Hoisted here (rather than left to `flushRefBatch`'s own idempotent call) so both
    /// triggers below run on the CALLING thread, strictly BEFORE this call enqueues its own item or
    /// becomes a queue leader -- `maybeSweepStalePrecommits`'s own nested `appendRefOps` calls are
    /// therefore always a fresh top-level invocation, never nested inside a leader's flush stack
    /// (which would deadlock the leader against itself).
    ensureRefTableRecovered(ns, *rt);
    if (!skip_stale_precommit_sweep)
        maybeSweepStalePrecommits(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    auto item = std::make_shared<RefMutationItem>();
    item->scope = std::move(scope);
    item->build_ops = std::move(build_ops);
    item->origin = origin;
    item->kind = kind;

    const auto enqueued_at = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(ref_queue_mutex);
    /// Refuse admission once a clean-release drain has begun (`drainRefLanesForShutdown`).
    /// Checked in the SAME critical section as the `pending.push_back` below -- the pairing that makes
    /// this race-free against the drain's snapshot-and-wait (see the `shutting_down` member comment).
    if (shutting_down.load(std::memory_order_acquire))
        throwCasWriteRetryLater(fmt::format(
            "CAS store is shutting down — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id));
    rt->pending.push_back(item);

    while (!item->done)
    {
        if (!rt->leader_active)
        {
            rt->leader_active = true;
            lk.unlock();
            try
            {
                runRefQueueLeader(ns, rt, item);
            }
            catch (...)
            {
                /// Any exceptional exit from the leader loop (e.g. an unhandled CORRUPTED_DATA the flush's
                /// controller sites now surface loudly) must restore `leader_active`, or every queued and
                /// future `appendRefOps` caller for this table blocks forever in `cv.wait` -- a silent hang
                /// instead of a fail-closed error. Idempotent with the flush's own resets; rethrow so the
                /// corruption surfaces to this caller rather than being swallowed.
                lk.lock();
                rt->leader_active = false;
                rt->cv.notify_all();
                throw;
            }
            lk.lock();
            rt->leader_active = false;
            rt->cv.notify_all();
        }
        else
        {
            rt->cv.wait(lk);
        }
    }
    lk.unlock();

    ProfileEvents::increment(ProfileEvents::CasRefQueueWaitMicroseconds,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - enqueued_at).count());
    if (item->error)
        std::rethrow_exception(item->error);
    return item->committed_id;
}


void CasRefLedger::runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                              const std::shared_ptr<RefMutationItem> & own)
{
    /// Fairness baton pass: serve flushes only until the caller's OWN item is done, then hand off to a
    /// woken waiter.
    while (true)
    {
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            if (own->done)
                return;
        }
        flushRefBatch(ns, rt);
    }
}

void CasRefLedger::flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// One flush = one carved batch through one attempted append. Contract: every ORDINARY outcome
    /// (validation reject, DefiniteFailure, Unresolved/wedge, Committed) lands in the affected items so
    /// waiters always wake, and this does NOT throw for any of them. The ONE
    /// exception is the provably-unreachable case where a DURABLY-committed transaction then fails to
    /// apply to the in-memory state (which the whole-item shape validation is supposed to preclude): that
    /// path completes every waiting survivor with the error, restores the leader bookkeeping, and RETHROWS
    /// a LOGICAL_ERROR (the object is already durable and every future recovery would re-hit it -- see the
    /// Committed-case catch below), so the caller learns this table's lane is bricked instead of hanging.
    auto complete_error = [&](const std::vector<std::shared_ptr<RefMutationItem>> & items, std::exception_ptr e)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        for (const auto & it : items)
        {
            it->error = e;
            it->done = true;
        }
        rt->cv.notify_all();
    };
    auto carve_all_pending = [&]() -> std::vector<std::shared_ptr<RefMutationItem>>
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        std::vector<std::shared_ptr<RefMutationItem>> all(rt->pending.begin(), rt->pending.end());
        rt->pending.clear();
        return all;
    };

    try
    {
        ensureRefTableRecovered(ns, *rt);
    }
    catch (...)
    {
        complete_error(carve_all_pending(), std::current_exception());
        return;
    }

    /// The local write fence ensures that a superseded or paused writer cannot race the live one.
    /// Fails the WHOLE queue -- every caller would have gotten the same refusal alone.
    if (!may_mutate())
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return;
    }

    /// Self-remount re-incarnation: this runtime was detached by a
    /// `quiesceRefTablesForRemount` swap, so its cache is a stale (pre-remount) view. Fail the whole
    /// carved batch closed -- allocating an id / applying against this orphaned runtime under the
    /// re-armed fence would split-brain against the fresh runtime the next touch re-recovers. The
    /// superseded flag is ordered before the fence re-arm (release/acquire through `mayMutate`), so
    /// reaching this AFTER passing `mayMutate` above proves the swap happened.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// The pre-attempt / post-write fence gate for THIS flush's controller calls. It also folds in
    /// `superseded_by_remount` so the append PUT is airtight against a self-remount that lands between a
    /// leader's pre-allocate re-check and its PUT: `superseded` is published before `armMountFence`, so a
    /// leader whose PUT observes a LIVE fence (`refAppendFenceOk` true) necessarily observes the flag too
    /// (release/acquire through the fence) and reports Unresolved instead of committing against a stale cache.
    const auto fence_ok = [this, &rt] { return fence_ok_fn() && !rt->superseded_by_remount.load(std::memory_order_acquire); };

    /// Resolve an outstanding wedge FIRST: "It does not start a later
    /// ref-log PUT for that table until the earlier result is resolved."
    {
        std::optional<RefAppendWedge> wedge_copy;
        {
            std::lock_guard lock(rt->state_mutex);
            wedge_copy = rt->wedge;
        }
        if (wedge_copy)
        {
            CasWriteOutcome resolved;
            try
            {
                resolved = ref_request_controller->resolveByExactGet(wedge_copy->key, wedge_copy->bytes);
            }
            catch (...)
            {
                /// `resolveByExactGet` throws CORRUPTED_DATA when the wedged key holds a DIFFERENT object
                /// than this attempt intended. The mount lease makes this key exclusively ours,
                /// so this is not a possible protocol outcome -- it is the anomaly policy's incidental
                /// detection case: fail closed LOUDLY (LOGICAL_ERROR, routed through
                /// `reportImpossibleInterference`) to every queued caller and KEEP the wedge, so the lane is
                /// left explicitly wedged for inspection rather than hanging every future caller.
                on_impossible_interference(wedge_copy->key,
                    fmt::format("ref-log wedge resolution for namespace '{}' txn {}-{} observed foreign bytes "
                        "at the wedged key ({})", ns.string(), wedge_copy->txn_id.writer_epoch,
                        wedge_copy->txn_id.ref_sequence, getCurrentExceptionMessage(/*with_stacktrace*/ false)),
                    ns.string());
                complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': impossible foreign interference observed at the "
                    "wedged key '{}' -- mount fenced closed and remount scheduled; see the anomaly "
                    "diagnostics log", ns.string(), wedge_copy->key)));
                return;
            }
            if (resolved == CasWriteOutcome::Committed)
            {
                std::lock_guard lock(rt->state_mutex);
                /// Apply BEFORE unwedging ("a wedged append later observed durable is applied to
                /// cache before unwedging"). Guard against a re-entrant double-apply: only if this is
                /// still the SAME wedge (single leader per table makes a mismatch impossible, but the
                /// check costs nothing and documents the invariant).
                if (rt->wedge && rt->wedge->txn_id == wedge_copy->txn_id)
                {
                    const RefLogTxn wedged_txn = decodeRefLogTxn(openObject(FormatId::RefLog, wedge_copy->bytes), ns.string(), wedge_copy->txn_id);
                    applyRefLogTxn(rt->state, wedged_txn);
                    rt->wedge.reset();
                }
                ProfileEvents::increment(ProfileEvents::CasRefAppendUnwedged);
            }
            else
            {
                /// Still Unresolved: fail every CURRENTLY queued item with the SAME uncertainty
                /// exception and do not allocate a new id. A later call into this namespace's queue
                /// retries the resolve.
                complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}' txn {}-{} is still UNCERTAIN — the append lane "
                    "stays wedged until the SAME key resolves durable or a conclusive rejection is observed",
                    ns.string(), wedge_copy->txn_id.writer_epoch, wedge_copy->txn_id.ref_sequence)));
                return;
            }
        }
    }

    /// Test-only (see `setRefPreCarveHookForTest`): a no-op in production.
    if (ref_pre_carve_hook_for_test)
        ref_pre_carve_hook_for_test();

    /// Carve a compatible batch. `lifecycle != Live` forces a solo carve:
    /// `namespace_birth` must run alone, and the flush already KNOWS the table's current lifecycle
    /// before carving (unlike a per-item property, which would need speculative undo).
    RefTableState working;
    bool table_live;
    {
        std::lock_guard lock(rt->state_mutex);
        working = rt->state;
        table_live = rt->state.lifecycle == RefLifecycle::Live;
    }

    std::vector<std::shared_ptr<RefMutationItem>> batch;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const size_t cap = table_live ? kMaxRefBatch : 1;
        std::set<String> seen_refs;
        while (!rt->pending.empty() && batch.size() < cap)
        {
            const auto & front = rt->pending.front();
            if (front->scope.kind == MutationScope::Kind::WholeShard)
            {
                if (!batch.empty())
                    break;
                batch.push_back(front);
                rt->pending.pop_front();
                break;
            }
            if (!seen_refs.insert(front->scope.ref_name).second)
            {
                ProfileEvents::increment(ProfileEvents::CasRefBatchScopeCuts);
                break;
            }
            batch.push_back(front);
            rt->pending.pop_front();
        }
    }
    if (batch.empty())
        return;   /// raced: everything was carved by a previous flush of this leader

    /// Per-item validation, in order, against `working` (per-request undo via `item_scratch`):
    /// business preconditions (thrown by `build_ops` itself) and the pre-encode admission budget both
    /// fail ONLY the offending item; survivors' ops accumulate into `final_ops` for one transaction.
    std::vector<RefOp> final_ops;
    std::vector<std::shared_ptr<RefMutationItem>> survivors;
    /// The trial epoch is ALWAYS `live_epoch_fn()` -- the SAME source `allocateRefTxnId` stamps the
    /// real id with, so the trial preview and the persisted id never disagree on epoch. The trial
    /// sequence continues `greatest_applied`'s counter only when its epoch already matches the live
    /// one (an ordinary same-incarnation append); otherwise -- a never-born table's `{0, 0}`, or a
    /// recovery seal's `{dead_epoch, UINT64_MAX}` -- the live epoch alone already
    /// dominates `greatest_applied` (mount exclusivity guarantees `live_epoch_fn() >=
    /// greatest_applied.writer_epoch`), so the sequence starts fresh at 0. This also keeps the `+= 1`
    /// previews below overflow-safe: a seal's sequence field is `UINT64_MAX`, which would otherwise wrap
    /// to 0 and be rejected as not strictly increasing. These trial ids are never
    /// persisted or compared outside this loop.
    RefTxnId trial_id;
    trial_id.writer_epoch = live_epoch_fn();
    trial_id.ref_sequence = (working.greatest_applied.writer_epoch == trial_id.writer_epoch)
        ? working.greatest_applied.ref_sequence
        : 0;
    for (const auto & it : batch)
    {
        RefTableState item_scratch = working;
        try
        {
            std::vector<RefOp> item_ops = it->build_ops(working);

            /// Whole-item shape validation (prerequisite to `dropNamespace`): the
            /// per-op loop below previews each op as its OWN single-op trial transaction, so a
            /// whole-transaction-shape rule like "remove_namespace must be the FINAL op" trivially
            /// passes on every singleton slice regardless of this item's REAL combined shape -- a
            /// malformed item (e.g. remove_namespace not last) would otherwise only be caught by the
            /// post-persist apply further below, AFTER its object is already durable (see that apply's
            /// own catch for why that would ALSO wedge this table's lane). Validate the item's COMPLETE
            /// ops array as ONE combined transaction, against a throwaway copy of the pre-item state,
            /// before doing any other per-op work -- exactly what the real persisted transaction will
            /// contain, using only the public two-phase `applyRefLogTxn` entry point (no need to reach
            /// into the state machine's private per-op helpers).
            if (!item_ops.empty())
            {
                RefTableState shape_check = working;
                RefTxnId shape_probe_id = trial_id;
                shape_probe_id.ref_sequence += 1;
                applyRefLogTxn(shape_check, RefLogTxn{ns.string(), shape_probe_id, item_ops});
            }

            for (const RefOp & op : item_ops)
            {
                /// Admission budget: only STATE-GROWING ops need the check --
                /// an `owner_transition` installing a binding (add or promote) and `set_payload`.
                /// `namespace_birth` is exempt (it grows nothing, and a never-born state's preview has
                /// no meaningful "current snapshot" to encode); `remove_namespace` and a pure
                /// owner_transition removal shrink state and can never violate the budget.
                const bool state_growing = (op.kind == RefOpKind::OwnerTransition && op.new_binding.has_value())
                    || op.kind == RefOpKind::SetPayload;
                if (state_growing && !admits(item_scratch, op, rt->snapshot_budget, rt->removal_budget))
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' would exceed the table's admission budget "
                        "(snapshot_budget={} removal_budget={}) — refusing before any object is created",
                        ns.string(), rt->snapshot_budget, rt->removal_budget);
                /// Apply THIS op to item_scratch now (a single-op trial transaction) so a LATER op of
                /// the SAME item (e.g. namespace_birth immediately followed by its first
                /// owner_transition) is validated -- both here and by admits's own preview -- against
                /// a state that already reflects it, exactly as the real combined transaction will.
                trial_id.ref_sequence += 1;
                applyRefLogTxn(item_scratch, RefLogTxn{ns.string(), trial_id, {op}});
            }
            working = std::move(item_scratch);
            final_ops.insert(final_ops.end(), item_ops.begin(), item_ops.end());
            survivors.push_back(it);
        }
        catch (...)
        {
            complete_error({it}, std::current_exception());
        }
    }
    if (final_ops.empty())
    {
        /// Either every item failed validation (already completed via complete_error above, nothing
        /// left to do), or every survivor's own `build_ops` legitimately contributed ZERO ops (an
        /// idempotent no-op, e.g. precommitAdd/promote re-targeting a manifest already exactly
        /// committed). Survivors of the latter kind still need marking done -- with no new object
        /// created, `committed_id` is simply the table's current (unchanged) high-water mark.
        if (!survivors.empty())
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            for (const auto & it : survivors)
            {
                it->committed_id = working.greatest_applied;
                it->done = true;
            }
            rt->cv.notify_all();
        }
        return;
    }

    /// Self-remount re-check BEFORE allocating an id: the top-of-flush gate
    /// is passed once, but a leader can stall between it and here -- in `build_ops`' caller I/O -- across
    /// the whole fence-loss + remount window, then resume after `armMountFence`. Allocating {new_epoch,
    /// seq} now and PUTting it (its live `fence_ok` would pass) would persist a transaction validated
    /// against this orphaned runtime's STALE cache -- the C1 data-loss class. `superseded_by_remount` is
    /// published before the fence re-arm, so failing closed here (no id, no PUT, no wedge -- a safe gap,
    /// cache unchanged) keeps the durable log free of any stale-view transaction. The append `fence_ok`
    /// (which also checks the flag) is the airtight backstop for the narrow window past this point.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// Wedge hard contract: at most one unresolved `PUT` per table, and the
    /// wedge-resolution block at the top of this flush either cleared `rt->wedge` (Committed) or already
    /// returned this whole batch closed (Unresolved / foreign interference) -- reaching HERE with a
    /// wedge STILL set is provably unreachable via any legitimate control flow. `chassert` catches it
    /// immediately in a debug build; the release-mode guard below refuses to allocate rather than trust
    /// an id minted against a lane that might still be uncertain, routing the violation into the same
    /// anomaly policy instead of an assert-only defense.
    {
        std::optional<String> wedged_key;
        {
            std::lock_guard lock(rt->state_mutex);
            chassert(!rt->wedge, "flushRefBatch: new-id allocation attempted while the ref-log lane was still wedged");
            if (rt->wedge)
                wedged_key = rt->wedge->key;
        }
        if (wedged_key)
        {
            on_impossible_interference(*wedged_key,
                fmt::format("flushRefBatch attempted new-id allocation for namespace '{}' while the lane "
                    "was still wedged -- the wedge hard contract was violated", ns.string()),
                ns.string());
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref-log append for namespace '{}': refusing to allocate a new ref-log id while the "
                "lane is wedged (wedge hard contract violated) -- mount fenced closed and remount scheduled",
                ns.string())));
            return;
        }
    }

    const RefTxnId id = allocateRefTxnId();
    const RefLogTxn final_txn{ns.string(), id, final_ops};
    String bytes;
    try
    {
        bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(final_txn));
    }
    catch (...)
    {
        complete_error(survivors, std::current_exception());
        return;
    }
    const String key = layout.refLogKey(ns, id);

    CasWriteOutcome outcome;
    try
    {
        outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    }
    catch (...)
    {
        /// `putIfAbsentControlled` throws CORRUPTED_DATA when resolve-before-reissue observes a DIFFERENT
        /// object already at this txn's key -- a proven different-object conflict, not an unresolved PUT.
        /// Fail every survivor loudly and do NOT wedge (this is a conclusive rejection, not an uncertain
        /// outcome): the id is a safe gap, the cache is unchanged, and the lane stays usable.
        complete_error(survivors, std::current_exception());
        return;
    }
    switch (outcome)
    {
        case CasWriteOutcome::Committed:
        {
            try
            {
                std::lock_guard lock(rt->state_mutex);
                applyRefLogTxn(rt->state, final_txn);
                /// COW-map materialization: fold this flush's overlay into a fresh immutable base HERE,
                /// under the SAME state_mutex critical section as the install above, so
                /// `rt->state.committed` is back to "base + empty overlay" before the next flush's
                /// trial copies (`working = rt->state`, CasRefLedger.cpp:1006) begin -- an O(n) fold
                /// once per flush, replacing what used to be an implicit O(n) copy on every trial
                /// anyway.
                rt->state.committed.materialize();
                /// This commit's own transaction joins the
                /// applied-above-newest-snapshot tail counters -- the live `rt->state` just mutated
                /// above IS the next publish candidate's body, so there is no per-entry log to retain.
                rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                rt->tail_bytes_since_snapshot.fetch_add(bytes.size(), std::memory_order_relaxed);
            }
            catch (...)
            {
                /// Provably unreachable given the whole-item shape validation above (every item's
                /// COMPLETE ops array is already validated as one combined transaction before this
                /// point) -- but `final_txn` is now durably PUT regardless, so if this line throws
                /// anyway (a bug the pre-check did not anticipate), every future recovery would replay
                /// and re-throw on it, bricking this table forever. Fail every waiting survivor's own
                /// caller with a clear diagnostic instead of leaving them hung on an `item->done` that
                /// would otherwise never be set, then rethrow: `appendRefOps`'s own catch is the SOLE
                /// authority that resets `leader_active` on an exceptional exit from the leader loop, so
                /// this path must NOT reset it too (a double reset would open a two-leader window -- a
                /// waiter woken by the first reset could become leader before this frame unwinds).
                const String detail = getCurrentExceptionMessage(false);
                Exception rethrown(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': the durably-committed transaction {}-{} "
                    "failed to apply to the in-memory table state -- this should be provably "
                    "unreachable (every item's ops are validated as one combined transaction before any "
                    "object is created); the object is already durable and every future recovery will "
                    "hit the same failure: {}",
                    ns.string(), id.writer_epoch, id.ref_sequence, detail);
                complete_error(survivors, std::make_exception_ptr(rethrown));
                throw rethrown;
            }
            ProfileEvents::increment(ProfileEvents::CasRefBatchFlushes);
            ProfileEvents::increment(ProfileEvents::CasRefBatchedMutations, survivors.size());
            {
                std::lock_guard<std::mutex> g(ref_queue_mutex);
                for (const auto & it : survivors)
                {
                    it->committed_id = id;
                    it->done = true;
                }
                rt->cv.notify_all();
            }
            /// The threshold trigger -- off the lane,
            /// dispatched AFTER waking every waiter above so this commit's own callers are never
            /// delayed by it.
            maybeScheduleSnapshotPublish(ns, rt);
            return;
        }
        case CasWriteOutcome::DefiniteFailure:
        {
            ProfileEvents::increment(ProfileEvents::CasRefAppendDefiniteFailure);
            complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' definitively failed (non-retryable rejection); "
                "cached state is unchanged and txn id {}-{} is a safe gap",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return;
        }
        case CasWriteOutcome::Unresolved:
        {
            {
                std::lock_guard lock(rt->state_mutex);
                rt->wedge = RefAppendWedge{id, key, bytes};
            }
            ProfileEvents::increment(ProfileEvents::CasRefAppendWedged);
            complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' txn {}-{} is UNCERTAIN (retry budget exhausted) — "
                "the append lane is wedged until the SAME key resolves durable or a conclusive rejection "
                "is observed; this outcome is unproven, not failure",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return;
        }
    }
}


void CasRefLedger::maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// Never dispatch a publisher while the fence is lost: a publish is a
    /// conditional PUT that would fail `fence_ok` and return non-Committed anyway, and dispatching one
    /// during the self-remount window is exactly the stale-cache-publish race the remount quiesce closes
    /// -- with no dispatch here, `quiesceRefTablesForRemount` only has to drain publishers already in
    /// flight before the fence dropped, never a moving target.
    if (!may_mutate())
        return;

    /// Bound the read-triggered dispatch. The whole decision --
    /// the threshold trigger, the single-in-flight gate, the backoff deadline -- and the
    /// `pending_snapshot_publishes` increment all happen under ONE `state_mutex` hold, so two racing
    /// dispatchers can never both admit a publish for this table.
    bool dispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        const uint64_t now = boot_ms_now_fn();
        if (rt->state.lifecycle == RefLifecycle::Live
            /// Single-in-flight gate: at most one background publish per table. A dropped trigger is
            /// re-evaluated on the next trigger (post-flush / mount-time / the next read), so no snapshot
            /// is permanently skipped -- compaction is best-effort, not a staleness bound.
            && rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0
            /// Backoff deadline: after a non-Committed publish, a saturated backend is not re-dispatched
            /// on the next read until the bounded backoff elapses (the read-triggered PUT-storm latch).
            && now >= rt->publish_backoff_until_ms)
        {
            /// The threshold trigger reads the tail counters
            /// directly -- no walk, no age filter. `tail_count_since_snapshot`/`tail_bytes_since_snapshot`
            /// count ONLY applied txns strictly above `newest_snapshot_id` (maintained incrementally by
            /// every commit and every adoption, see `flushRefBatch`/`trySnapshotPublishOnce`), so
            /// `over_threshold` here is never true without a real, immediately-coverable candidate: unlike
            /// the deleted grace-window scheme, there is no longer a decoupling between "counted" and
            /// "coverable" that a separate candidate-advance check would need to close.
            const uint64_t publishable_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
            const uint64_t publishable_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
            const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
                || publishable_bytes > config.snapshot_log_bytes_threshold;
            if (over_threshold)
            {
                rt->pending_snapshot_publishes.fetch_add(1, std::memory_order_relaxed);
                dispatch = true;
            }
        }
    }
    if (!dispatch)
        return;
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPublishDispatched);

    /// Off the mutation hot path: `trySnapshotPublishOnce` never
    /// touches the append queue, so dispatching it onto an unrelated global-pool thread can never
    /// deadlock a flush leader. `pin_owner()` (the Pool's `shared_from_this`) keeps the Pool -- and
    /// hence this ledger member -- alive for the thread's lifetime, exactly as the pre-decomposition
    /// `shared_from_this()` capture did.
    auto owner = pin_owner();
    try
    {
        ThreadFromGlobalPool([owner, this, ns, rt]
        {
            try
            {
                trySnapshotPublishOnce(ns);
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot publish attempt failed");
            }
            {
                std::lock_guard lock(rt->state_mutex);
                rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
            }
            rt->publish_settle_cv.notify_all();
        }).detach();
    }
    catch (...)
    {
        /// The `ThreadFromGlobalPool` ctor can throw (pool exhaustion) AFTER the
        /// count was incremented. Undo the count (else `waitForSnapshotPublishSettleForTest` hangs and the
        /// leaked pending count wedges every later settle) and SWALLOW the failure: read-path callers
        /// (`resolveRef`/`listRefs`) invoke this OUTSIDE any insulation, and dispatching a background
        /// publish is a best-effort maintenance trigger -- it must never fail an otherwise-successful read
        /// (consistent with the `CasRefSweepDeferred` read-insulation adjudication). The next trigger
        /// reschedules; a mutation caller has already committed regardless.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        }
        rt->publish_settle_cv.notify_all();
        tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot-publish dispatch failed to launch");
    }
}


void CasRefLedger::advancePublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. Double the interval from `initial` up to `max` per consecutive
    /// non-Committed publish outcome; arm the deadline off the boottime clock (`bootMsNow`), so an
    /// injected test clock drives it deterministically and a VM-suspend cannot shorten it.
    rt.publish_backoff_ms = rt.publish_backoff_ms == 0
        ? config.snapshot_publish_backoff_initial_ms
        : std::min<uint64_t>(rt.publish_backoff_ms * 2, config.snapshot_publish_backoff_max_ms);
    rt.publish_backoff_until_ms = boot_ms_now_fn() + rt.publish_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPublishBackoff);
}

void CasRefLedger::resetPublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A durable publish clears the cooldown.
    rt.publish_backoff_ms = 0;
    rt.publish_backoff_until_ms = 0;
}

void CasRefLedger::waitForSnapshotPublishSettleForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::unique_lock lock(rt->state_mutex);
    rt->publish_settle_cv.wait(lock, [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
}

int CasRefLedger::pendingSnapshotPublishesForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    std::lock_guard lock(rt->state_mutex);
    return rt->pending_snapshot_publishes.load(std::memory_order_relaxed);
}

std::optional<RefTxnId> CasRefLedger::newestPublishedSnapshotIdForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->newest_snapshot_id;
}

size_t CasRefLedger::tailSinceSnapshotCountForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
}

size_t CasRefLedger::committedOverlayEntriesForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->state.committed.overlayEntriesForTest();
}

namespace
{
/// A clamped-to-zero fetch-subtract for the tail counters. `trySnapshotPublishOnce` is public and NOT
/// serialized against itself (two overlapping attempts can finish out of order), so the monotonic guard
/// below
/// skips a stale (superseded) adoption's subtraction outright, but it cannot see a SMALLER-candidate
/// attempt that lands its adoption BEFORE a larger-candidate one already in flight: that ordering would
/// have the larger attempt's `captured_count`/`captured_bytes` double-count the smaller one's
/// already-subtracted region. A plain `fetch_sub` would then underflow the unsigned counter, wrapping it
/// to near `UINT64_MAX` and permanently re-latching the read-triggered PUT-storm trigger on every
/// subsequent read -- a release-build regression of the exact bug this guard prevents. Clamping to
/// zero instead settles for a
/// benign, self-healing under-count (a delayed next dispatch; the NEXT publish always captures the true
/// live state fresh, so snapshot CONTENT is never affected) over an unsafe wraparound.
void clampedCounterSub(std::atomic<uint64_t> & counter, uint64_t amount)
{
    uint64_t old_value = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(old_value, old_value > amount ? old_value - amount : 0,
        std::memory_order_relaxed))
    {
    }
}
}


bool CasRefLedger::trySnapshotPublishOnce(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);

    /// ONE copy of the live state, at a transaction boundary -- no
    /// replay, no per-entry retention. The tail counters are captured in the SAME critical section so
    /// adoption below subtracts exactly what this attempt's candidate actually covers.
    RefTableState candidate_state;
    RefTxnId candidate_x;
    uint64_t captured_count = 0;
    uint64_t captured_bytes = 0;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Live)
            return false;   /// nothing to (re)publish here; dropNamespace publishes its own Removed snapshot
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < rt->state.greatest_applied))
            return false;   /// nothing above the newest snapshot
        candidate_state = rt->state;
        candidate_x = rt->state.greatest_applied;
        captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
        captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
    }

    const RefTableSnapshot snap = snapshotOf(candidate_state, ns.string());
    String bytes;
    try
    {
        bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(snap));
    }
    catch (...)
    {
        /// Failure Handling: "Snapshot create fails: keep all logs; writer recovery remains unchanged."
        /// Treat like any other non-Committed outcome: arm the backoff so a persistent encode failure
        /// does not re-dispatch on every read.
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    const String key = layout.refSnapshotKey(ns, candidate_x);
    const auto fence_ok = [this] { return fence_ok_fn(); };
    const CasWriteOutcome outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
    {
        /// DefiniteFailure/Unresolved: DO NOT prune (no durable covering snapshot -- pruning the tail
        /// without one is data loss). Arm the bounded per-table backoff so the read path does not
        /// re-dispatch this full-snapshot encode+PUT until it elapses -- the read-triggered PUT-storm
        /// latch breaker. A later trigger past the deadline retries.
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPutBytes, bytes.size());   /// account published bytes

    {
        std::lock_guard lock(rt->state_mutex);
        /// A durable publish clears any backoff: progress was made this attempt (even if the
        /// monotonic guard below skips the in-memory adoption because a newer snapshot already won).
        resetPublishBackoff(*rt);
        /// Monotonic adoption guard (CRITICAL): publishes are NOT serialized, so two
        /// overlapping attempts can finish out of order (this OLDER-candidate attempt landing its PUT
        /// after a NEWER one already adopted). Adopting the older `candidate_x` here would REGRESS
        /// `newest_snapshot_id` below what a newer attempt already advanced it to -- the next published
        /// snapshot would then omit committed transactions and recovery would lose refs. Skip the
        /// in-memory adoption (and the counter subtraction below) whenever a newer-or-equal snapshot is
        /// already adopted; the already-durable `_snap/<candidate_x>` object is harmless (readers pick
        /// the greatest snapshot, GC reclaims covered ones). This also keeps the Removed path monotonic:
        /// a stale Live attempt can never drag `newest_snapshot_id` back below a `remove_txn_id` that
        /// `publishRemovedSnapshotNow` already adopted (`remove_txn_id` is allocated after every Live
        /// txn, so it is always the greatest and this guard trips).
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < candidate_x))
            return true;
        /// Subtract exactly the counters captured at copy time
        /// -- more appends (or even another publish's own commits) may have landed on the LIVE counters
        /// since, and only those should remain uncovered. Clamped (see `clampedCounterSub`): an
        /// out-of-order adoption ordering the guard above does not catch (a SMALLER candidate that
        /// adopts before a LARGER one already in flight) could otherwise subtract an already-subtracted
        /// region and underflow the unsigned counter.
        clampedCounterSub(rt->tail_count_since_snapshot, captured_count);
        clampedCounterSub(rt->tail_bytes_since_snapshot, captured_bytes);
        /// logs-per-table-after-snapshot: the tail this publish compacted.
        ProfileEvents::increment(ProfileEvents::CasRefSnapshotTailLogs, captured_count);
        rt->newest_snapshot_id = candidate_x;
        /// The new cache-weight base is exactly the snapshot
        /// we just encoded and PUT, so its body size is the fresh base weight -- no re-encode needed.
        rt->base_snapshot_bytes.store(bytes.size(), std::memory_order_relaxed);
    }
    return true;
}


void CasRefLedger::sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// A read-only caller (resolveRef/listRefs) must not fail its OWN
    /// otherwise-successful read because a piggybacked maintenance action (the stale-precommit sweep)
    /// hit an uncertain PUT -- the read asked for none of that; a mutation path (appendRefOps's own
    /// top-level hoisted call, which calls `maybeSweepStalePrecommits` directly, uncaught) keeps
    /// propagating instead, since it must not proceed past a wedged lane anyway. Swallowing here does
    /// Do NOT drop the sweep: the failed
    /// attempt already re-armed `needs_stale_precommit_sweep` (with a bounded cooldown) inside
    /// `maybeSweepStalePrecommits`, so a later read/mutation trigger on THIS mount retries until a
    /// sweep completes verified clean -- the old drop-the-shot behavior left a dead incarnation's
    /// precommit bindings (and the manifests they protect from the GC orphan sweep) live forever on a
    /// long-lived mount whenever the single attempt burned in the post-restart error window.
    try
    {
        maybeSweepStalePrecommits(ns, rt);
    }
    catch (...)
    {
        ProfileEvents::increment(ProfileEvents::CasRefSweepDeferred);
        tryLogCurrentException(getLogger("CasPool"),
            "CAS stale-precommit sweep deferred for namespace '" + ns.string()
                + "' (a read-only caller observed the failure and is proceeding with its own read)");
    }
}

void CasRefLedger::maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    {
        std::lock_guard lock(rt->state_mutex);
        if (!rt->needs_stale_precommit_sweep)
            return;
        /// A failed attempt armed a cooldown; do not re-attempt (and do not touch the flag)
        /// until it elapses -- the bounded-backoff storm latch, same shape as `publish_backoff_until_ms`.
        /// The boottime clock is injectable (`boot_ms_fn`), so tests drive this deterministically.
        if (boot_ms_now_fn() < rt->precommit_sweep_backoff_until_ms)
            return;
        /// Cleared FIRST: `sweepStalePrecommitsNow`'s own `appendRefOps` calls re-enter this same
        /// top-level check (via `appendRefOps`'s hoisted call), and must see it already cleared. This
        /// clear is for RE-ENTRANCY only, never consumption: any non-clean outcome re-arms below.
        rt->needs_stale_precommit_sweep = false;
    }
    try
    {
        sweepStalePrecommitsNow(ns, rt);
    }
    catch (...)
    {
        /// A failed or partial sweep
        /// failed or partial sweep must NOT consume the shot. Under kill-chaos the single attempt lands
        /// exactly inside the post-restart error window (an uncertain PUT, a fence blip), and with no
        /// retry the dead incarnation's durable precommit bindings -- and the manifests
        /// `activeManifestKeys` protects for them -- leaked forever on a long-lived mount (GC has no
        /// backstop). Re-arm with a bounded backoff and rethrow: the
        /// read path insulates the caller (`sweepStalePrecommitsForRead`), the mutation path propagates
        /// as before.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->needs_stale_precommit_sweep = true;
            advancePrecommitSweepBackoff(*rt);
        }
        throw;
    }
    /// Verified clean: `sweepStalePrecommitsNow` returns only after a full pass over the live state
    /// found zero stale bindings, so the flag stays cleared for the rest of this mount; reset the
    /// failure cooldown too.
    std::lock_guard lock(rt->state_mutex);
    resetPrecommitSweepBackoff(*rt);
}

void CasRefLedger::advancePrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. Double the interval
    /// from `initial` up to `max` per consecutive failed sweep attempt; arm the deadline off the
    /// boottime clock (`bootMsNow`), so an injected test clock drives it deterministically and a
    /// VM-suspend cannot shorten it.
    rt.precommit_sweep_backoff_ms = rt.precommit_sweep_backoff_ms == 0
        ? config.precommit_sweep_backoff_initial_ms
        : std::min<uint64_t>(rt.precommit_sweep_backoff_ms * 2, config.precommit_sweep_backoff_max_ms);
    rt.precommit_sweep_backoff_until_ms = boot_ms_now_fn() + rt.precommit_sweep_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CasRefSweepRearmed);
}

void CasRefLedger::resetPrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A verified-clean sweep clears the cooldown.
    rt.precommit_sweep_backoff_ms = 0;
    rt.precommit_sweep_backoff_until_ms = 0;
}


void CasRefLedger::sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// After a fresh mount fence and recovery, this writer
    /// knows the exact stale precommit bindings -- their `manifest_ref.writer_epoch` predates this
    /// incarnation's live writer_epoch, i.e. they belong to a build from a superseded incarnation that
    /// can never be promoted. Removed with ordinary exact `owner_transition(old_binding, none)`
    /// operations, chunked to `ref_txn_max_ops` per transaction. Interruption is harmless: each chunk
    /// re-reads the LIVE state, so a partial sweep just leaves fewer stale bindings for the next chunk
    /// (a later retry on this mount, or the next mount's recovery) to find; nothing here can loop
    /// forever since only OLDER-epoch bindings ever qualify, and this writer's own new work always uses
    /// `live_epoch_fn()` -- which a self-remount bumps in lockstep with the threshold below, so a
    /// remount's fresh precommits survive.
    ///
    /// A GC-side backstop stays deliberately OUT: the responsibility boundary assigns
    /// precommit-binding cleanup to the WRITER -- GC never mutates another writer's ref-table state, so
    /// a leader-side reclaim would be a new protocol capability (a question about GC-authored
    /// ref-log transactions and their fencing), not a bugfix. The retry-until-clean loop above is the
    /// writer-side answer; the follow-up (a GC visibility counter for "live precommit binding below the
    /// mount-lease epoch" would require a separate protocol decision.
    const uint64_t live_epoch = live_epoch_fn();
    while (true)
    {
        std::vector<std::pair<String, ManifestRef>> chunk;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : rt->state.precommits)
            {
                if (mref.writer_epoch >= live_epoch)
                    continue;
                chunk.emplace_back(ref_name, mref);
                if (chunk.size() >= ref_txn_max_ops)
                    break;
            }
        }
        if (chunk.empty())
            return;

        appendRefOps(ns, MutationScope::wholeShard(),
            [chunk](const RefTableState & state) -> std::vector<RefOp>
            {
                std::vector<RefOp> ops;
                for (const auto & [ref_name, mref] : chunk)
                    if (state.precommits.contains({ref_name, mref}))
                    {
                        RefOp op;
                        op.kind = RefOpKind::OwnerTransition;
                        op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                        ops.push_back(op);
                    }
                return ops;
            },
            RootMutationOrigin::Writer, RootMutationKind::ReclaimPrecommit);

        /// Audit each binding this sweep reclaimed, so
        /// `system.content_addressed_log` records the reclaim and the "abandoned precommits
        /// reclaimed" counter is falsifiable (it had ZERO emit sites before). A binding gathered above
        /// that is GONE from the live state after the committed append was reclaimed by this sweep's
        /// work -- either this chunk's own ops or this lane's just-resolved wedged predecessor txn (a
        /// PRIOR attempt of this same sweep whose ack was lost); one still present was skipped by the
        /// builder (raced by another owner transition) and will be gathered again next iteration.
        /// Collected under the lock, emitted outside it (the sink forwards to the SystemLog).
        std::vector<std::pair<String, ManifestRef>> reclaimed;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : chunk)
                if (!rt->state.precommits.contains({ref_name, mref}))
                    reclaimed.emplace_back(ref_name, mref);
        }
        ProfileEvents::increment(ProfileEvents::CasRefStalePrecommitsReclaimed, reclaimed.size());
        for (const auto & [ref_name, mref] : reclaimed)
        {
            EventEmitter{*this}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::PrecommitReclaim;
                e.namespace_ = ns.string();
                e.ref_name = ref_name;
                e.object_kind = CasEventObjectKind::Root;
                e.object_hash = manifestRefDebugString(mref);
                e.reason = "stale-precommit sweep: dangling precommit of a superseded writer incarnation "
                           "reclaimed by the successor's fenced sweep";
                e.detail = {{"stale_writer_epoch", std::to_string(mref.writer_epoch)},
                            {"live_writer_epoch", std::to_string(live_epoch)}};
            });
        }
    }
}


void CasRefLedger::publishRemovedSnapshotNow(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    RefTxnId remove_id;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Removed || !rt->state.remove_txn_id)
            return;
        remove_id = *rt->state.remove_txn_id;
        if (rt->newest_snapshot_id && *rt->newest_snapshot_id == remove_id)
            return;   /// already published this exact Removed snapshot
    }

    RefTableSnapshot removed_snap;
    removed_snap.ns = ns.string();
    removed_snap.snapshot_id = remove_id;
    removed_snap.lifecycle = RefLifecycle::Removed;
    removed_snap.remove_txn_id = remove_id;
    const String bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(removed_snap));
    const String key = layout.refSnapshotKey(ns, remove_id);
    const auto fence_ok = [this] { return fence_ok_fn(); };
    const CasWriteOutcome outcome = ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
        return;   /// best-effort; namespace cleanup republishes it idempotently later

    std::lock_guard lock(rt->state_mutex);
    rt->newest_snapshot_id = remove_id;
    rt->tail_count_since_snapshot.store(0, std::memory_order_relaxed);
    rt->tail_bytes_since_snapshot.store(0, std::memory_order_relaxed);
}


void CasRefLedger::dropRef(const RootNamespace & ns, const String & ref_name)
{
    /// One `owner_transition` removal ref-log transaction. The
    /// exact committed binding must exist; `build_ops` reads it off the CURRENT batch-validation state,
    /// so a concurrently-co-batched publish/drop of a DIFFERENT ref sees a consistent view.
    ManifestRef dropped_ref;
    const RefTxnId txn_id = appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.committed.find(ref_name);
            if (it == state.committed.end())
                /// Fail-closed (no silent no-op): this item's own exception, the batch survives.
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "dropRef: no such ref {} in namespace {}", ref_name, ns.string());

            dropped_ref = it->second.manifest_ref;
            RefOp op;
            op.kind = RefOpKind::OwnerTransition;
            op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, dropped_ref};
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::Drop);

    /// The ref was dropped (a removal operation GC folds as a true removal). `object_hash` is the
    /// manifest the ref named, so a part's "publish -> drop" life is reconstructable from the rows.
    if (hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::RefDrop;
        _ev3.namespace_ = ns.string();
        _ev3.ref_name = ref_name;
        _ev3.object_kind = CasEventObjectKind::Manifest;
        _ev3.object_hash = manifestRefDebugString(dropped_ref);
        _ev3.at_version = txn_id.ref_sequence;
        _ev3.outcome = "ok";
        _ev3.reason = "dropRef: appended an owner_transition removal ref-log transaction";
        emitEvent(std::move(_ev3));
    }
}


void CasRefLedger::updateRefPayload(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RefPayloadUpdate &)> mutator)
{
    /// One `set_payload` ref-log transaction. EVERY change (even
    /// payload-only) is an explicit logged operation -- the immutable append-only log has no other way
    /// to record it. The payload this op carries has shrunk to just
    /// `published_at_ms` (the mutable-file map is gone; every per-part file is an ordinary manifest
    /// tree entry now, republished via `repointRef`, never through this side channel).
    appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.committed.find(ref_name);
            if (it == state.committed.end())
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "updateRefPayload: no such ref {} in namespace {}", ref_name, ns.string());

            /// The mutator edits only `published_at_ms`; the carrier deliberately carries no
            /// `manifest_ref`, so a reachability change is structurally impossible here (it goes through
            /// publish/drop/repoint instead).
            RefPayloadUpdate update;
            update.published_at_ms = it->second.published_at_ms;

            mutator(update);

            RefOp op;
            op.kind = RefOpKind::SetPayload;
            op.ref_name = ref_name;
            op.expected_manifest_ref = it->second.manifest_ref;
            op.published_at_ms = update.published_at_ms;
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::UpdateRefPayload);
}


bool CasRefLedger::namespaceIsRemoved(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard<std::mutex> lock(rt->state_mutex);
    /// A genuinely-removed namespace is `Removed` WITH a `remove_txn_id` (RemoveNamespace sets both). The
    /// never-born default is also `Removed` but carries no `remove_txn_id`, and a recreated one is `Live`
    /// (NamespaceBirth resets the marker) — so `remove_txn_id.has_value()` is the exact born-then-removed
    /// discriminator, matching promote's own `state.remove_txn_id` recreation guard.
    return rt->state.lifecycle != RefLifecycle::Live && rt->state.remove_txn_id.has_value();
}


DropNamespaceStats CasRefLedger::dropNamespace(const RootNamespace & ns)
{
    /// One body transaction naming an exact `owner_transition`
    /// removal for every committed ref and precommit, followed by `remove_namespace` -- the removal
    /// class shares the bigger complete-table byte budget (encodeRefLogTxn's own `checkBudget`, keyed
    /// off the presence of a `RemoveNamespace` op) and is exempt from the ordinary per-op admission
    /// check (it only ever shrinks state; see `flushRefBatch`'s `state_growing` filter).
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    {
        /// "Repeated API removal observes the cached Removed state and returns success without
        /// appending a second transaction." `RefTableState::lifecycle` cannot distinguish "genuinely
        /// removed" from "never born" (both default to `Removed` -- see the representation note on
        /// `RefTableState`), so a never-touched namespace's drop is ALSO a harmless no-op here, which
        /// is the correct behavior either way (nothing to remove).
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.lifecycle != RefLifecycle::Live)
            return {};
    }

    /// This call's own removal
    /// transaction named, filled from the SAME `state` the ops below are built from -- a retried
    /// `build_ops` (a wedge resolving under a resumed leader) simply overwrites it with the final
    /// durable transaction's true counts.
    DropNamespaceStats stats;
    appendRefOps(ns, MutationScope::wholeShard(),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            if (state.lifecycle != RefLifecycle::Live)
                return {};   /// raced: another caller already removed it since our check above

            std::vector<RefOp> ops;
            for (const auto [ref_name, row] : state.committed)
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, row.manifest_ref};
                ops.push_back(op);
            }
            for (const auto & [ref_name, mref] : state.precommits)
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                ops.push_back(op);
            }
            RefOp remove;
            remove.kind = RefOpKind::RemoveNamespace;
            ops.push_back(remove);

            stats.committed_refs = state.committed.size();
            stats.precommits = state.precommits.size();
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::DropNamespace,
        /// The operations above already name (and remove) every current precommit
        /// binding regardless of epoch, making the ordinary stale-precommit maintenance sweep redundant
        /// for THIS call -- and, left enabled, a race: the hoisted sweep runs first and would reclaim an
        /// epoch-stale binding in its OWN transaction, so `state.precommits` above would already be
        /// missing it and undercount `stats.precommits`. See `appendRefOps`'s doc comment.
        /*skip_stale_precommit_sweep=*/true);

    /// "After the transaction is durable, it applies the same
    /// operations to memory, cancels local builds, and rejects further ordinary mutations." Reaching here
    /// means the removal is durable (this call's, or a concurrent caller's whose durable result the append
    /// lane observed) -- a FAILED append would have thrown above, so cancellation is only ever reached
    /// after durability (a failed append leaves the namespace `Live` and propagates). Cancel
    /// every in-flight build TARGETING this namespace so its next op fails closed (`requireAlive`),
    /// preventing it from promoting/precommitting a fresh owner into (or staging more debris in) the
    /// just-removed namespace. The append lane is the real linearization authority (an `owner_transition`
    /// on a non-Live namespace is rejected by the state machine regardless); this stops wasted work early
    /// and surfaces a clear error. Builds in OTHER namespaces self-filter (no-op). The build registry
    /// (`inflight_builds`) lives on the owning Pool, so the cancellation runs through the injected
    /// `cancel_inflight_builds` callback (which collects the live shared_ptrs under `builds_mutex` and
    /// cancels OUTSIDE it -- see `Pool::cancelInflightBuildsForNamespace`).
    cancel_inflight_builds(ns);

    /// "After the removal transaction is durable, the writer also publishes
    /// the constant-size Removed snapshot"; best-effort here (the removal itself already succeeded) --
    /// namespace cleanup republishes it idempotently if this writer stops first.
    try
    {
        publishRemovedSnapshotNow(ns);
    }
    catch (...)
    {
        tryLogCurrentException(getLogger("CasPool"), "CAS dropNamespace: publishing the Removed snapshot failed (best-effort)");
    }

    /// The writer performs NO physical deletion of ref-log/snapshot objects or verbatim namespace files
    /// -- GC's namespace-cleanup item ({namespace, remove_txn_id}, Pending->Completed) owns that reclaim,
    /// keyed off the durable `remove_namespace` this call just appended.
    /// Until GC reclaims it, a dropped namespace's ref-log objects and verbatim files remain as debris.
    return stats;
}



}
