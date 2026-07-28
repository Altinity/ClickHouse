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
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>
#include <Common/setThreadName.h>
#include <Common/ThreadPool.h>
#include <base/sleep.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <type_traits>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
    extern const int NETWORK_ERROR;
    extern const int S3_ERROR;
    extern const int POCO_EXCEPTION;
    extern const int SOCKET_TIMEOUT;
    extern const int CANNOT_READ_FROM_SOCKET;
    extern const int TIMEOUT_EXCEEDED;
}
}

namespace ProfileEvents
{
    extern const Event CasRefBatchFlushes;
    extern const Event CasRefBatchedMutations;
    extern const Event CasRefBatchScopeCuts;
    extern const Event CasRefQueueWaitMicroseconds;
    extern const Event CasRefRecoveryRestarts;
    extern const Event CasRefRecoveryRetries;
    extern const Event CasRefAppendWedged;
    extern const Event CasRefAppendPreAttemptRefused;
    extern const Event CasRefAppendUnwedged;
    extern const Event CasRefAppendDefiniteFailure;
    extern const Event CasRefApplyPoisoned;
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

namespace
{
/// Classifies whether an exception thrown out of a ref-table recovery attempt (namespace LIST,
/// snapshot/log GETs, or the seal PUT) is a TRANSIENT object-store transport failure worth retrying,
/// vs. a terminal condition (corruption, decode failure, logic error, resource limit) that must fail
/// fast. The recovery reads call the backend directly (not through `ref_request_controller`), so a
/// transient blip surfaces as the object storage's native code -- `S3_ERROR` for the S3 backend, or a
/// socket/timeout/Poco transport code -- NOT the `NETWORK_ERROR` that only the seal PUT's controller
/// re-mints. Retrying only `NETWORK_ERROR` would leave the LIST/GET legs unprotected, which is exactly
/// the path the motivating stuck-load incident hit.
bool isTransientRecoveryError(int code)
{
    return code == ErrorCodes::NETWORK_ERROR
        || code == ErrorCodes::S3_ERROR
        || code == ErrorCodes::POCO_EXCEPTION
        || code == ErrorCodes::SOCKET_TIMEOUT
        || code == ErrorCodes::CANNOT_READ_FROM_SOCKET
        || code == ErrorCodes::TIMEOUT_EXCEEDED;
}
}

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

    /// Default backoff sleep for the recovery retry loop (`ensureRefTableRecovered`): sleep in short
    /// slices and stop early if the mount fence drops (shutdown / lease loss), so teardown never waits
    /// out a full 30s backoff. This is deliberate, bounded backoff against external object-store I/O
    /// failure -- NOT masking a race -- exactly like `CasRequestControl`'s own inter-attempt
    /// `threadSleepMs`; the slice loop additionally makes it interruptible, which that one is not.
    recovery_retry_sleep_fn = [this](uint64_t total_ms)
    {
        constexpr uint64_t slice_ms = 200;
        uint64_t slept = 0;
        while (slept < total_ms && fence_ok_fn())
        {
            const uint64_t chunk = std::min(slice_ms, total_ms - slept);
            sleepForMilliseconds(chunk);
            slept += chunk;
        }
    };
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

CasOverwriteResult CasRefLedger::stagingConditionalOverwrite(std::string_view key, std::string_view bytes, const Token & expected)
{
    /// The supplied write is controlled by the same retry and mount-fence policy as other staged
    /// writes.
    return ref_request_controller->putOverwriteControlled(key, bytes, expected, fence_ok_fn);
}

CasOverwriteResult CasRefLedger::stagingPutIfAbsentMutable(std::string_view key, std::string_view bytes)
{
    return ref_request_controller->putIfAbsentControlledMutable(key, bytes, fence_ok_fn);
}

void CasRefLedger::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    ref_request_controller->setSleepFnForTest(sleep_fn);
    recovery_retry_sleep_fn = std::move(sleep_fn);
}


std::optional<Resolved> CasRefLedger::resolveRef(const RootNamespace & ns, const String & ref_name, bool /*allow_stale*/,
                                                 ResolveAudit audit)
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

    /// Capture the resolved edge under `state_mutex`, but emit AFTER releasing it (Task 2): the audit
    /// sink may re-enter a ledger read that itself takes `state_mutex` (e.g. `resolveRef`), so emitting
    /// while holding the lock self-deadlocks that reentrant read on the same thread. The reentrancy-safe
    /// dispatcher additionally serializes delivery, but the same-thread relock is prevented here by the
    /// lock discipline, not the dispatcher.
    ManifestRef resolved_ref;
    uint64_t resolved_published_at_ms = 0;
    std::optional<CasEvent> pending_event;
    {
        std::lock_guard lock(rt->state_mutex);
        const auto it = rt->state.getCommitted().find(ref_name);
        if (it == rt->state.getCommitted().end())
            return std::nullopt;

        const RefCommittedRow & row = it->second;
        resolved_ref = row.manifest_ref;
        resolved_published_at_ms = row.published_at_ms;
        /// A resolved ref points to its manifest (the read-path entry point). `object_hash` is the manifest
        /// instance id the ref names; pairs with a later readManifest ReadMissing if that body is gone.
        /// `Deferred` (used only by `CachedPartFolderAccess::resolve` on the `getView` call path) skips this
        /// emit; the caller decides, once it knows whether the access as a whole did real resolve work,
        /// whether to emit the identical event itself — see `ResolveAudit`'s doc comment.
        if (audit == ResolveAudit::Emit && hasEventSink())
        {
            CasEvent _ev0;
            _ev0.type = CasEventType::RefResolve;
            _ev0.namespace_ = ns.string();
            _ev0.ref_name = ref_name;
            _ev0.object_kind = CasEventObjectKind::Manifest;
            _ev0.object_hash = manifestRefDebugString(row.manifest_ref);
            _ev0.outcome = "resolved";
            _ev0.reason = "read-side resolve of a ref to its part manifest";
            pending_event = std::move(_ev0);
        }
    }
    if (pending_event)
        emitEvent(std::move(*pending_event));
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = resolved_ref},
        .manifest_size = 0,
        .published_at_ms = resolved_published_at_ms,
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
    for (const auto [ref_name, row] : rt->state.getCommitted())
        result.emplace(ref_name, Resolved{
            .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
            .manifest_size = 0,
            .published_at_ms = row.published_at_ms,
        });
    return result;
}

bool CasRefLedger::hasAnyRefWithPrefix(const RootNamespace & ns, std::string_view prefix)
{
    /// Same recovery/maintenance preamble as `listRefs`; see there for why an empty or never-touched
    /// namespace still costs exactly one `LIST` (recovery) and a warm namespace costs nothing at all.
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::lock_guard lock(rt->state_mutex);
    for (const auto [ref_name, row] : rt->state.getCommitted())
        if (prefix.empty() || std::string_view(ref_name).starts_with(prefix))
            return true;
    return false;
}


ConfirmAnswer CasRefLedger::confirmExactRef(const RootNamespace & ns, const String & ref_name,
                                           const ManifestRef & manifest_ref) const
{
    /// Gate 1 of the relink confirm (spec §confirm-primitive). A `Yes` authorizes a REMOTE receiver to
    /// promote a manifest whose blobs are protected only by this writer's committed binding of that
    /// exact manifest, so a `Yes` is an assertion about the durable table, not about this cache. Every
    /// rule below exists to make that assertion true; the answer to anything a rule cannot establish is
    /// `Unknown`, which costs the receiver a retry and costs correctness nothing.
    ///
    /// Two structural properties, both load-bearing:
    ///
    ///   ZERO object-store I/O. This runs on an interserver request, so anything it could be made to
    ///   do is something a remote peer can make this writer do. It therefore reads only what is already
    ///   resident, never recovers, never resolves a wedge, and -- see the `find` below -- never even
    ///   materializes a runtime. Deliberately absent for the same reason: `ensureRefTableRecovered`,
    ///   `sweepStalePrecommitsForRead` and `maybeScheduleSnapshotPublish`, the three maintenance calls
    ///   `resolveRef` performs and all three of which can do I/O.
    ///
    ///   ONE snapshot across BOTH lane mutexes. `pending`/`leader_active` live under
    ///   `ref_queue_mutex`, the rows and the wedge under `state_mutex`, and the whole point of the
    ///   rules is their CONJUNCTION -- read at different instants they would prove nothing. The lock
    ///   ORDER is the one the rest of this file already establishes (`enforceRefTableCacheBudget`
    ///   nests `state_mutex` under `ref_queue_mutex`, and nothing anywhere takes them the other way
    ///   round). Because admission (`appendRefOps`' `pending.push_back`) happens under
    ///   `ref_queue_mutex`, an append is either entirely before this snapshot -- and then visible as a
    ///   pending item -- or entirely after it. There is no interleaving in which a removal is admitted
    ///   and this function still answers `Yes`.
    ///
    /// What a `Yes` does NOT prove, stated so nobody has to rediscover it: that this runtime's
    /// recovered view is a COMPLETE replay of the durable log. Completeness is recovery's contract, not
    /// this function's, and it cannot be re-established here without I/O. Rules 2-4 exclude every way
    /// this MOUNT can have fallen behind its own durable writes; a recovery that silently observed less
    /// than it should have is a different defect, in a different component.
    std::lock_guard<std::mutex> qlock(ref_queue_mutex);

    /// Rule 2 (residency). `find`, NOT `getRefTableRuntime`: the latter INSERTS an empty, unrecovered
    /// runtime for any namespace named, so a read-only query would let a peer grow this writer's table
    /// cache -- and the very next reader would then pay a recovery this query invented. A cold or
    /// evicted table is simply unknown here.
    const auto it = ref_tables.find(ns.string());
    if (it == ref_tables.end())
        return ConfirmAnswer::Unknown;
    RefTableRuntime & rt = *it->second;

    /// `try_to_lock`, not a blocking acquire: `ensureRefTableRecovered` holds `state_mutex` across its
    /// whole LIST + replay, so blocking here would make a confirm WAIT on someone else's recovery --
    /// up to the full retry envelope -- while holding `ref_queue_mutex`, which is pool-wide append
    /// admission. That is the zero-I/O contract broken by proxy: the query would not issue a request,
    /// it would merely be paid for by one, and it would stall every table's lane meanwhile. Failing to
    /// take the lock is just one more ambiguity, so it answers like every other one. (Same technique,
    /// and same non-blocking rationale, as `enforceRefTableCacheBudget`'s candidate loop.)
    std::unique_lock<std::mutex> slock(rt.state_mutex, std::try_to_lock);
    if (!slock.owns_lock())
        return ConfirmAnswer::Unknown;

    /// Rule 2 (warm). An unrecovered or mid-recovery runtime has an EMPTY `state`, which would read as
    /// "the ref does not exist" -- knowledge it does not have. `superseded_by_remount` is the same
    /// class: this runtime was detached by a self-remount and its view belongs to a dead incarnation.
    /// Recovery publishes atomically (`installRecoveryResult` sets `recovered` LAST under this mutex),
    /// so there is no half-recovered view to catch in between.
    if (!rt.recovered || rt.recovery_in_progress || rt.superseded_by_remount.load(std::memory_order_acquire))
        return ConfirmAnswer::Unknown;

    /// Rule 3 (lane quiescent). A wedge is "an object that may be durable and is not applied" -- it may
    /// BE the removal being asked about. A pending item or an active leader tenure is a mutation this
    /// table has already admitted; mid-tenure, a chunked flush has committed some of its transactions
    /// and not others, and `leader_active` spans the whole tenure, so that partially-durable window is
    /// covered too. None of the three says anything about WHICH ref is affected, so all three are
    /// table-scoped refusals.
    if (rt.wedge.has_value() || !rt.pending.empty() || rt.leader_active)
        return ConfirmAnswer::Unknown;

    /// Rule 4 (poison). `Poisoned` means an install failed over a possibly-durable object, i.e. this
    /// cached state may be MISSING a durable transaction whose contents are by definition unknown --
    /// so no row of this table can be confirmed. `ApplyPending` is the same statement about a
    /// transaction still in flight. This is the ONE consumer that reads the marker to make a decision;
    /// it remains an assert layer for everything else (see `RefApplyState`).
    if (rt.apply_state.load(std::memory_order_relaxed) != RefApplyState::Clean)
        return ConfirmAnswer::Unknown;

    /// Rule 5 (exact row equality) -- the only rule that can answer `No` at all. On a table that passed
    /// rules 2-4 the committed map is this writer's view, so a missing row or a different `ManifestRef`
    /// is a real disagreement rather than an ambiguity about this cache. It is NOT a proof about the
    /// DURABLE table: the fence has not been checked yet (rule 6, below, states why that order is
    /// deliberate and why it is sound). Equality is exact and total:
    /// mint-tightening (spec §A3) guarantees a repoint or a recreation mints a fresh `ManifestRef`, so
    /// there is no ABA to defend against here.
    const auto & committed = rt.state.getCommitted();
    const auto row = committed.find(ref_name);
    if (row == committed.end() || !(row->second.manifest_ref == manifest_ref))
        return ConfirmAnswer::No;

    /// Rule 6 (mount fence), LAST and still under both locks -- the order the spec fixes. Everything
    /// above describes what this process believes; this is the check that it is still entitled to
    /// believe it: a fenced-out mount is no longer the namespace's single writer, so another writer may
    /// already have repointed the ref. Being last means a token that does not match is reported as `No`
    /// even under a lost fence: `No` and `Unknown` are the same outcome for the caller (both are
    /// `SourceProofFailed`, spec §failure-taxonomy), and only `Yes` is gated on the fence.
    /// `superseded_by_remount` is folded in exactly as `commitRefChunk`'s own `fence_ok` folds it: the
    /// flag is published BEFORE the remount re-arms the fence, so a stale runtime can never pass both.
    if (!fence_ok_fn() || rt.superseded_by_remount.load(std::memory_order_acquire))
        return ConfirmAnswer::Unknown;

    return ConfirmAnswer::Yes;
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

    /// Outer transient-retry loop (Layer 1 of the stuck-table-load fix): a whole recovery attempt
    /// (LIST + snapshot/log GETs + seal PUT) that fails with a TRANSIENT object-store transport error
    /// (`isTransientRecoveryError` -- `S3_ERROR`/socket/timeout from the direct LIST/GET backend calls,
    /// or the seal PUT's controller `NETWORK_ERROR`) is retried with capped-exponential backoff until
    /// `recovery_retry_budget_ms` is spent, instead of propagating and failing this table's async load
    /// permanently. Non-transient errors (corruption, decode, logic, resource limits) fail fast; so does
    /// the inner vanish-race brake below (which is `NETWORK_ERROR` too but DELIBERATELY terminal -- the
    /// `vanish_brake_tripped` latch keeps the outer loop from re-driving it).
    const uint64_t recovery_start_ms = boot_ms_now_fn();
    uint64_t recovery_retry_num = 0;
    bool vanish_brake_tripped = false;
    for (;;)
    {
        try
        {

            for (uint64_t attempt = 0; ; ++attempt)
            {
                if (attempt > 0)
                {
                    if (attempt > kRefRecoveryMaxRestarts)
                    {
                        /// Terminal, NOT a transient object-store outage: latch so the outer retry loop rethrows
                        /// immediately instead of re-driving this pathological-cleanup-race brake for the budget.
                        vanish_brake_tripped = true;
                        throwCasWriteRetryLater(fmt::format(
                            "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot or "
                            "log object kept vanishing between its LIST and GET) — giving up; this bound is a "
                            "runaway brake against a pathological cleanup race, not an expected steady state",
                            ns.string(), attempt - 1));
                    }
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

                /// Stream the post-snapshot tail through one builder: GET -> decode -> applyOne ->
                /// discard, one transaction at a time into a PRIVATE candidate, so a long tail of
                /// (post-op-cap) up-to-20-MiB transactions never materializes as a whole-tail vector. The
                /// candidate never touches `rt.state`; it is published atomically as one `RecoveryResult`
                /// (`installRecoveryResult`) only after the whole tail -- and any seal -- succeeds.
                RefReplayBuilder builder(std::move(snapshot), snapshot_body_bytes);
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
                        RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
                        /// Account the decoded transaction's resident footprint to the memory probe for
                        /// exactly this iteration (see `reportReplayMemoryDelta`): the streaming loop holds
                        /// one at a time. No-op in production.
                        const int64_t footprint = static_cast<int64_t>(decodedRefLogTxnFootprint(txn));
                        reportReplayMemoryDelta(footprint);
                        SCOPE_EXIT({ reportReplayMemoryDelta(-footprint); });
                        builder.applyOne(std::move(txn), got->bytes.size());
                    }
                }

                if (vanished)
                    continue;   /// the selected object vanished; retry from a fresh listing (candidate discarded)

                RecoveryResult result = std::move(builder).finish();
                /// `finish` returns the candidate WITHOUT materializing: `stateFromSnapshot` loads every
                /// committed row and owned-manifest entry into the COW OVERLAY, and no tail transaction
                /// materializes either. This state is the table's long-lived working state, so fold both
                /// `committed` and `owned_manifests` into fresh shared bases ONCE here -- rather than making
                /// the first flush's scratch copy (and every per-item/shape-check copy on it) deep-copy an
                /// N-row overlay. The O(N) fold rides inside recovery, which is already O(N).
                result.state.materializeCommitted();
                result.cleanup_markers = std::move(cleanup_markers);
                /// Stale-precommit cleanup is dispatched once, from `appendRefOps`'s top level (never from
                /// here -- this call may itself be nested inside a queue leader's flush stack).
                result.needs_stale_precommit_sweep = true;
                /// Per-table admission budgets pre-subtract this table's own wire overhead
                /// (`4 + ns.size()`, once in a snapshot body and once in a removal txn body) plus a fixed
                /// safety margin from the raw hard limits, once, here.
                const uint64_t overhead = 4 + ns.string().size() + kRefAdmissionSafetyMargin;
                result.snapshot_budget = overhead < ref_snapshot_max_bytes ? ref_snapshot_max_bytes - overhead : 0;
                result.removal_budget = overhead < ref_removal_max_bytes ? ref_removal_max_bytes - overhead : 0;

                /// At an UNCLEAN mount, close every dead epoch this recovery discovered with an immediate
                /// snapshot -- the seal -- published BEFORE the table is installed as recovered. A failed
                /// seal PUT throws here, leaving the table unrecovered, so the NEXT touch restarts recovery
                /// from scratch (fresh LIST, fresh replay, fresh seal attempt) rather than exposing a table
                /// whose dead-epoch region was never closed; recovery therefore fails closed.
                /// The encode and conditional `PUT` run OUTSIDE `state_mutex` (unlocked/relocked just below,
                /// mirroring `trySnapshotPublishOnce`'s copy-under-lock/PUT-outside shape) -- it is the one
                /// part of recovery that can run the full ~90s retry envelope, and holding the mutex across
                /// it stalls every other touch of this table plus `wedgedRefLaneCount`'s whole-store walk
                /// (which locks each table's `state_mutex` in turn). `recovery_in_progress` (set above), not
                /// the mutex, keeps a concurrent second caller for this SAME table from redoing this work
                /// during the unlocked window -- see its doc comment. The seal reads the PRIVATE candidate
                /// (`result.state`), not `rt.state`: nothing installs into the runtime before the single
                /// atomic `installRecoveryResult` at the end, so no observer ever sees a half-published
                /// table (`rt` itself cannot be destroyed underneath us -- the caller's own `shared_ptr`
                /// keeps it alive, which is also what protects it from `enforceRefTableCacheBudget`).
                ///
                /// `seal_id` is the UPPER BOUND of the dead-epoch region, not the greatest listed id: the
                /// wedge discipline places the predecessor's one possible in-flight PUT strictly above
                /// everything it resolved, so only the epoch-closing bound is guaranteed to dominate it --
                /// any later materialization from a dead epoch is born covered (`<=` seal_id) for every
                /// observer, forever. `sealed_from` records the greatest id this recovery actually listed.
                const uint64_t my_epoch = live_epoch_fn();
                const RefTxnId seal_id{my_epoch - 1, std::numeric_limits<uint64_t>::max()};
                const bool dead_region_nonempty =
                    greatest_listed_id.has_value() && greatest_listed_id->writer_epoch < my_epoch;
                const bool already_sealed =
                    result.newest_snapshot_id.has_value() && !(*result.newest_snapshot_id < seal_id);
                /// Compare against the SPECIFIC epoch a reclaim was marked unclean for, not a
                /// sticky "ever" bool -- a table recovered for the first time (or reloaded after LRU eviction)
                /// under a LATER, perfectly clean epoch boundary must not get a parasitic seal just because
                /// SOME earlier, unrelated boundary in this incarnation's life was unclean.
                if (unclean_boundary_epoch() == my_epoch && my_epoch >= 2
                    && dead_region_nonempty && !already_sealed && result.state.getLifecycle() == RefLifecycle::Live)
                {
                    RefTableSnapshot seal = snapshotOf(result.state, ns.string());
                    seal.snapshot_id = seal_id;                            /// upper bound of the covered region
                    seal.sealed_from = result.state.getGreatestApplied();  /// == greatest_listed_id: nothing else was applied
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
                    CasWriteOutcome outcome{};
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

                    /// The seal now covers everything replayed so far: fold it into the publication as if it
                    /// were the recovered snapshot and the tail were empty -- exactly what a fresh recovery
                    /// against the now-durable seal would find. `sealed_from` must move to the seal's own
                    /// value too: leaving the (pre-seal) base's `sealed_from` behind would describe the new
                    /// seal with the predecessor's observed-region bound.
                    result.newest_snapshot_id = seal_id;
                    result.sealed_from = seal.sealed_from;
                    result.tail_count = 0;
                    result.tail_bytes = 0;
                    result.base_snapshot_bytes = seal_bytes.size();
                }

                /// One atomic publication under `state_mutex`: `installRecoveryResult` copies every seeded
                /// field from `result` and sets `recovered` LAST, so no waiter (woken only by the
                /// function-scope SCOPE_EXIT's `recovery_cv` notify, which runs after this returns) ever
                /// observes a partially-installed table.
                installRecoveryResult(rt, std::move(result));
                break;
            }
            break;   /// recovery succeeded -> exit the outer retry loop
        }
        catch (...)
        {
            /// `catch (...)`, not `catch (const Exception &)`: recovery LIST/GET failures can surface as
            /// a raw object-storage transport exception (even a non-`DB::Exception` Poco timeout), which
            /// a `catch (const Exception &)` would not even see. `getCurrentExceptionCode()` normalises
            /// every exception (DB, Poco, std) to a code so the transient classifier can decide.
            const int code = getCurrentExceptionCode();
            if (vanish_brake_tripped || !isTransientRecoveryError(code))
                throw;   /// the terminal vanish brake, or a non-transient failure -- fail fast

            const uint64_t elapsed_ms = boot_ms_now_fn() - recovery_start_ms;
            /// Fail closed BEFORE sleeping: budget spent, mount fence lost, or this runtime superseded by
            /// a self-remount (mirrors `appendRefOps`' fence gate -- recovery must not re-drive on an
            /// orphaned, pre-remount runtime).
            if (elapsed_ms >= cas_request_budget.recovery_retry_budget_ms
                || !fence_ok_fn()
                || rt.superseded_by_remount.load(std::memory_order_acquire))
                throw;

            /// Saturating `initial << recovery_retry_num` (mirrors `CasRequestController::backoffBefore
            /// Attempt`): `initial > cap >> n` implies the unshifted product already exceeds the cap, so
            /// return the cap without ever computing an overflowing/UB shift for large retry counts.
            const uint64_t init_backoff = cas_request_budget.recovery_retry_initial_backoff_ms;
            const uint64_t cap_backoff = cas_request_budget.recovery_retry_max_backoff_ms;
            const uint64_t backoff_ms = (recovery_retry_num >= 63 || init_backoff > (cap_backoff >> recovery_retry_num))
                ? cap_backoff
                : std::min(cap_backoff, init_backoff << recovery_retry_num);
            ++recovery_retry_num;
            ProfileEvents::increment(ProfileEvents::CasRefRecoveryRetries);
            LOG_WARNING(getLogger("CasRefLedger"),
                "CAS ref-table recovery for namespace '{}' hit a transient object-store error "
                "(code {}: {}); retry #{} after {}ms backoff (elapsed {}ms / budget {}ms)",
                ns.string(), code, getCurrentExceptionMessage(/*with_stacktrace=*/false),
                recovery_retry_num, backoff_ms, elapsed_ms, cas_request_budget.recovery_retry_budget_ms);

            lock.unlock();
            /// Re-acquire the lock before letting any exception from the sleep unwind, so the SCOPE_EXIT
            /// (which mutates `recovery_in_progress` + notifies `recovery_cv` and MUST run under
            /// `state_mutex`) never runs unlocked -- same obligation as the seal-PUT window above.
            try
            {
                recovery_retry_sleep_fn(backoff_ms);
            }
            catch (...)
            {
                lock.lock();
                throw;
            }
            lock.lock();
            /// The fence/supersession/budget can all change during the unlocked sleep -- re-check before
            /// starting the next full attempt so we never re-drive recovery on an orphaned runtime, past
            /// the budget, or under a lost fence (the sliced sleep may have woken early on fence loss).
            if (boot_ms_now_fn() - recovery_start_ms >= cas_request_budget.recovery_retry_budget_ms
                || !fence_ok_fn()
                || rt.superseded_by_remount.load(std::memory_order_acquire))
                throw;
            /// loop: re-run recovery from a fresh LIST (fresh snapshot/log/replay/seal)
        }
    }
    }

    /// A NEW table was just materialized; enforce the whole-table cache budget, protecting this one
    /// The pass runs OUTSIDE `rt.state_mutex` (that scope closed above) so
    /// the pass -- which acquires `ref_queue_mutex` and try-locks other tables' `state_mutex` -- never
    /// nests this table's `state_mutex` under `ref_queue_mutex`.
    enforceRefTableCacheBudget(ns);
}

void CasRefLedger::installRecoveryResult(RefTableRuntime & rt, RecoveryResult && result)
{
    /// One place that seeds a recovered table's runtime, copying EVERY `RecoveryResult` field so the
    /// publication cannot drift from the struct. `recovered` is set LAST: the caller holds `state_mutex`
    /// throughout and the function-scope SCOPE_EXIT notifies `recovery_cv` only after this returns, so a
    /// parked waiter re-checking `recovered` under the same lock sees a fully-installed table or none.
    rt.state = std::move(result.state);
    rt.cleanup_markers = std::move(result.cleanup_markers);
    rt.newest_snapshot_id = result.newest_snapshot_id;
    /// `sealed_from` pairs with `newest_snapshot_id`: when the newest snapshot is a recovery seal this
    /// is the seal's `sealed_from`, otherwise `nullopt`. Copied for a complete, drift-proof inventory --
    /// the ledger has no hot-read consumer for it (see the field's doc comment), but leaving it out would
    /// make the "copies EVERY field" contract false.
    rt.sealed_from = result.sealed_from;
    rt.tail_count_since_snapshot.store(result.tail_count, std::memory_order_relaxed);
    rt.tail_bytes_since_snapshot.store(result.tail_bytes, std::memory_order_relaxed);
    rt.base_snapshot_bytes.store(result.base_snapshot_bytes, std::memory_order_relaxed);
    rt.snapshot_budget = result.snapshot_budget;
    rt.removal_budget = result.removal_budget;
    rt.needs_stale_precommit_sweep = result.needs_stale_precommit_sweep;
    rt.recovered = true;   /// set LAST
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
    /// A wedge -- synthetic or not -- IS "a ref-log object that may be durable and is not applied", so
    /// the seam reproduces the whole runtime state the production `Unresolved` arm leaves behind, marker
    /// included. Nothing reads the marker to make a decision (spec §A2: assert layer, not fence), so this
    /// changes no behaviour; it only keeps the seam from constructing a state production cannot reach.
    armApplyPending(*rt);
}

RefApplyState CasRefLedger::applyStateForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    return rt->apply_state.load(std::memory_order_relaxed);
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
            /// The set of items THIS leader is responsible for: its own enqueued `item` plus every item a
            /// flush carves out of `pending` (recorded by `flushRefBatch` as it carves). Whatever the
            /// leader loop does below -- return normally, or throw at ANY point including BEFORE it ever
            /// carves -- every one of these items must leave here `done` (its waiter woken), never left
            /// stranded in `pending` for a future leader to carve after this caller's stack (and its
            /// `build_ops` closure) is gone: a use-after-free that concurrent per-part commit makes
            /// immediate. `completeOwnedItemsAndReleaseLeadership` enforces that on EVERY exit and folds in
            /// the `leader_active` release the old catch used to own. Items NOT owned by this leader (other
            /// callers' still-queued items) are untouched -- they stay validly owned by their blocked
            /// callers.
            ///
            /// Build the responsibility set (its own `item`) BEFORE publishing the baton, so becoming
            /// leader contains NO throwing operation once `leader_active` is set: the only allocation is
            /// this first `push_back`, done here while still holding `lk` and NOT yet leader. If it throws
            /// (a `bad_alloc` at the pre-tenure point; codex stage-1 review, Important), the baton is never
            /// taken -- but `item` is already in `pending` (pushed above), so it must be un-enqueued before
            /// propagating, else a future leader would carve an item whose `build_ops` closure died with
            /// this unwinding caller (the same use-after-free the exit guard prevents post-publication).
            /// Publishing the baton and reaching the exit guard is then a pure no-throw sequence.
            std::vector<std::shared_ptr<RefMutationItem>> owned_items;
            try
            {
                if (ref_pre_tenure_hook_for_test)
                    ref_pre_tenure_hook_for_test();
                owned_items.push_back(item);
            }
            catch (...)
            {
                std::erase(rt->pending, item);
                throw;
            }

            rt->leader_active = true;
            lk.unlock();
            std::exception_ptr flush_exception;
            try
            {
                runRefQueueLeader(ns, rt, item, owned_items);
            }
            catch (...)
            {
                flush_exception = std::current_exception();
            }
            /// Single exit authority (normal AND exceptional): complete every still-incomplete owned
            /// item with `flush_exception` (nullptr on the normal path -> a fail-closed LOGICAL_ERROR)
            /// and release leadership. This does NOT rethrow. Under chunked flush the leader's OWN item
            /// may already have succeeded in an earlier committed chunk, and a later exception -- from a
            /// subsequent chunk, the reseed, or chunk-N processing -- must NOT be handed to this caller
            /// whose mutation is already durable (tenure exception containment, spec §3): the guard
            /// leaves such an item `done` with no error, and the loop re-check + tail below return its
            /// `committed_id`. An item that genuinely failed carries `item->error` and the tail rethrows
            /// it, exactly as the old unconditional rethrow did for the single-chunk case.
            completeOwnedItemsAndReleaseLeadership(ns, rt, owned_items, flush_exception);
            lk.lock();
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
                              const std::shared_ptr<RefMutationItem> & own,
                              std::vector<std::shared_ptr<RefMutationItem>> & owned_items)
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
        flushRefBatch(ns, rt, owned_items);
    }
}

void CasRefLedger::completeOwnedItemsAndReleaseLeadership(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
    const std::vector<std::shared_ptr<RefMutationItem>> & owned_items,
    std::exception_ptr flush_exception)
{
    std::lock_guard<std::mutex> g(ref_queue_mutex);
    for (const auto & owned : owned_items)
    {
        if (!owned->done)
        {
            owned->error = flush_exception
                ? flush_exception
                : std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': the append-lane leader exited without "
                    "completing an owned queue item -- failing it closed rather than leaving it stranded "
                    "in the pending queue for a future leader to carve", ns.string()));
            owned->done = true;
        }
        /// Never leave an owned item in `pending`: a stranded item would be carved by a future leader
        /// which would then invoke its (now dangling) `build_ops` closure -- the use-after-free this
        /// guard exists to prevent. Carved items were already popped during the carve, so this is a
        /// no-op for them; it only matters for an item the leader owned but never got to carve.
        std::erase(rt->pending, owned);
    }
    rt->leader_active = false;
    rt->cv.notify_all();
}

void CasRefLedger::armApplyPending(RefTableRuntime & rt) noexcept
{
    /// A CAS from `Clean` ONLY. That is what makes `Poisoned` terminal BY CONSTRUCTION rather than by
    /// call-site discipline: this and `clearApplyPending` are the only writers besides
    /// `poisonApplyState`, both name their expected value explicitly, and neither names `Poisoned` --
    /// so no store that could clear a poison exists anywhere in the translation unit. A failed CAS is
    /// not an error on either of its two possible losers: `ApplyPending` is already the value we want,
    /// and `Poisoned` must stay.
    RefApplyState expected = RefApplyState::Clean;
    rt.apply_state.compare_exchange_strong(expected, RefApplyState::ApplyPending, std::memory_order_relaxed);
}

void CasRefLedger::clearApplyPending(RefTableRuntime & rt) noexcept
{
    RefApplyState expected = RefApplyState::ApplyPending;
    rt.apply_state.compare_exchange_strong(expected, RefApplyState::Clean, std::memory_order_relaxed);
}

void CasRefLedger::poisonApplyState(RefTableRuntime & rt, const RootNamespace & ns, std::string_view region) noexcept
{
    /// The marker and the metric first, and both are non-allocating: this runs from a `catch` that is
    /// about to rethrow, so everything that can fail must come after everything that must not.
    const RefApplyState previous = rt.apply_state.exchange(RefApplyState::Poisoned, std::memory_order_relaxed);
    if (previous == RefApplyState::Poisoned)
        return;   /// one event per TRANSITION: a runtime that is already poisoned is already accounted for
    ProfileEvents::increment(ProfileEvents::CasRefApplyPoisoned);
    try
    {
        LOG_ERROR(getLogger("CasPool"),
            "CAS ref table '{}' is POISONED at {}: an install failed although its ref-log object may "
            "already be durable, so this cached table may be MISSING a durable transaction. The "
            "allocation-free install regions make this unreachable by construction, so reaching it is a "
            "bug in that construction. The marker is terminal for this runtime -- only a remount, which "
            "replaces the runtime, clears it.",
            ns.string(), region);
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// `noexcept`, and the logging allocates. A failure to REPORT the poison must not become a
        /// `std::terminate` and must not replace the post-durable exception the caller is rethrowing;
        /// the marker and the metric above already carry the signal.
    }
}

void CasRefLedger::flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                                 std::vector<std::shared_ptr<RefMutationItem>> & owned_items)
{
    /// One flush = one carved batch through one attempted append. Contract: every ORDINARY outcome
    /// (validation reject, DefiniteFailure, Unresolved/wedge, Committed) lands in the affected items so
    /// waiters always wake, and this does NOT throw for any of them. Neither `commitRefChunk` nor the
    /// wedge-resolution block below throws past the point where its object is proven durable -- both
    /// installs are allocation-free by construction (spec §A1) -- so the paths that can still throw are
    /// the wedge-resolution candidate build, which runs BEFORE the resolving GET and therefore before
    /// anything is proven, and an allocation failure in this function's own bookkeeping (e.g. the
    /// chunk-boundary reseed). Both are contained by `appendRefOps`' catch, which completes every
    /// still-unfinished survivor and restores the leader bookkeeping, so no caller hangs.
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

    /// Resolve an outstanding wedge FIRST: "It does not start a later
    /// ref-log PUT for that table until the earlier result is resolved."
    {
        std::optional<RefAppendWedge> wedge_copy;
        /// Prepared in the SAME hold that reads the wedge, for the same reason `commitRefChunk` prepares
        /// its candidate before the PUT (spec §A1, here site 2): a successful `resolveByExactGet` PROVES
        /// the wedged object is durable, so everything between that proof and "the runtime records it"
        /// must be incapable of throwing. The COW copy is cheap -- it shares the live state's bases -- and
        /// is only taken when there is actually a wedge to resolve, so the ordinary flush pays nothing.
        std::optional<RefTableState> candidate;
        RefTxnId candidate_base_id;
        {
            std::lock_guard lock(rt->state_mutex);
            wedge_copy = rt->wedge;
            if (wedge_copy)
            {
                candidate.emplace(rt->state);
                candidate_base_id = rt->state.getGreatestApplied();
            }
        }
        if (wedge_copy)
        {
            /// Decode and apply BEFORE the resolving GET. The wedge carries the encoded body, so this
            /// costs no extra I/O -- only the decode and the overlay build, both of which can throw
            /// (allocation) and both of which used to run AFTER the GET had already proved the object
            /// durable. A throw here now happens while the outcome is still UNKNOWN and the wedge is
            /// still set, i.e. it is indistinguishable from "the resolve has not been attempted yet": the
            /// lane stays wedged and a later flush retries the whole resolution. It propagates to
            /// `appendRefOps`' catch exactly as the old post-durable apply did.
            ///
            /// The candidate is deliberately NOT cached in the wedge across attempts: a wedge can live
            /// until remount, and retaining a full state copy for that long is a real memory cost on a
            /// path that is rare by construction. Recomputing it per attempt is the cheaper trade.
            const RefLogTxn wedged_txn = decodeRefLogTxn(
                openObject(FormatId::RefLog, wedge_copy->bytes), ns.string(), wedge_copy->txn_id);
            applyRefLogTxn(*candidate, wedged_txn);

            CasWriteOutcome resolved{};
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
                ///
                /// The one wedge resolution that is CONCLUSIVELY NEGATIVE, and therefore the third
                /// "nothing became durable, no apply is owed" clear (spec §A2). The ref-log key is
                /// write-once: a DIFFERENT object sitting at it proves OUR body never landed there --
                /// our `putIfAbsent` would have been rejected. (`resolveByExactGet` never reports a
                /// plain absent verdict: an absent or unreadable key returns `Unresolved`, since
                /// another attempt may still be legal. This foreign-bytes arm is the only shape in
                /// which a resolution proves the transaction is not durable.) Cleared BEFORE the
                /// anomaly reaction below, whose `LOGICAL_ERROR` aborts the process outright in
                /// debug/sanitizer builds. The wedge is deliberately KEPT for inspection, so this is
                /// the one state where a wedged lane is not `ApplyPending`; the lane is fenced closed
                /// and headed for a remount anyway.
                clearApplyPending(*rt);
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
                {
                    std::lock_guard lock(rt->state_mutex);
                    /// Install BEFORE unwedging ("a wedged append later observed durable is applied to
                    /// cache before unwedging"). Guard against a re-entrant double-apply: only if this is
                    /// still the SAME wedge (single leader per table makes a mismatch impossible, but the
                    /// check costs nothing and documents the invariant).
                    if (rt->wedge && rt->wedge->txn_id == wedge_copy->txn_id)
                    {
                        /// Only this leader mutates `rt->state`, and the resolving GET above ran without
                        /// the lock, so this is the assertion that nothing advanced the state underneath
                        /// the candidate during that round trip -- the same argument (and the same reason
                        /// for hoisting it out of the region under a SHORT name) as in `commitRefChunk`:
                        /// `chassert` stringifies its condition, so a long condition would heap-allocate
                        /// ON FAILURE inside the very region that must not allocate.
                        [[maybe_unused]] const bool state_unchanged = rt->state.getGreatestApplied() == candidate_base_id;
                        /// Receives the resolved wedge so it is destroyed OUTSIDE the region: clearing
                        /// `rt->wedge` in place would free its two `String` bodies there, and the region's
                        /// contract is that it touches no allocator at all.
                        std::optional<RefAppendWedge> displaced_wedge;
                        static_assert(std::is_nothrow_swappable_v<std::optional<RefAppendWedge>>,
                            "the wedge hand-off below must be non-throwing: it runs after the wedged object "
                            "is proven durable, where a throw would re-apply the transaction on the next "
                            "resolution");
                        /// Post-durable install region 2 of 3 (spec §A2). The `catch` cannot fire while
                        /// §A1 holds -- the body below allocates nothing -- and is what makes a
                        /// violation of §A1 VISIBLE rather than silent: the GET proved the object
                        /// durable, so an install that does not complete leaves this table's cached
                        /// state missing it. It rethrows unchanged, so the lane's error handling is
                        /// exactly what it was; only the marker and the metric are added.
                        try
                        {
                            DENY_ALLOCATIONS_IN_SCOPE;
                            /// The negative control (`setInstallRegionProbeForTest`), fired with the
                            /// guard already armed, exactly as in `commitRefChunk`'s region.
                            if (install_region_probe_for_test)
                                install_region_probe_for_test();
                            chassert(state_unchanged);
                            /// The install, allocation-free by construction: a member-wise swap of
                            /// pointers and PODs, two atomic increments, and a second swap of pointers.
                            /// The GET proved the object durable, so the transaction MUST be recorded --
                            /// and recording it MUST be inseparable from clearing the wedge. It was not:
                            /// the apply and the `materializeCommitted` fold used to sit between them
                            /// WITHOUT the ordinary commit arm's swallow, so a fold failure left the
                            /// transaction applied and the wedge still set, and the next resolution
                            /// re-applied the same transaction and double-bumped these very counters.
                            /// A wedge-resolved transaction is a commit like any other: it joins the
                            /// applied-above-newest-snapshot tail counters exactly as the ordinary
                            /// Committed arm's does, or the snapshot-publish threshold and the
                            /// resident-weight estimate undercount by one transaction per resolved wedge
                            /// until the next recovery reseeds.
                            rt->state.swap(*candidate);
                            rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                            rt->tail_bytes_since_snapshot.fetch_add(wedge_copy->bytes.size(), std::memory_order_relaxed);
                            rt->wedge.swap(displaced_wedge);
                            /// Last statement of the install, in the SAME allocation-free region as the
                            /// swap that performs it: "the transaction is recorded" and "no apply is
                            /// owed" become true together or not at all (spec §A2). A relaxed CAS on a
                            /// `uint8_t` -- it cannot allocate and cannot throw.
                            clearApplyPending(*rt);
                        }
                        catch (...)
                        {
                            poisonApplyState(*rt, ns, "wedge-resolution install");
                            throw;
                        }
                        /// `candidate` now holds the DISPLACED state, which still shares the COW bases
                        /// `rt->state` uses; destroying it here restores unique base ownership so the fold
                        /// below keeps its O(overlay) in-place path instead of rebuilding the whole base.
                        /// Both `reset`s only destroy: they allocate nothing and cannot throw.
                        candidate.reset();
                        displaced_wedge.reset();
                        /// Fold the just-installed overlay back into the base right here, exactly as the
                        /// ordinary Committed arm does at its install point, so `rt->state` returns to
                        /// "base + empty overlay" and the next flush's trial copies stay cheap. Cheap: no
                        /// scratch copy shares the base at this point in the flush (`working` is not taken
                        /// until below), so this is the O(overlay) in-place fold. Coherent-on-throw (see
                        /// `CasRefCowMap.cpp`), and now SWALLOWING, symmetrically with the ordinary commit
                        /// arm: the transaction is durable, installed and unwedged before this runs, so a
                        /// mid-fold allocation failure merely defers the fold to the next flush -- it must
                        /// not unwind past a completed install.
                        try
                        {
                            rt->state.materializeCommitted();
                        }
                        catch (...)
                        {
                            tryLogCurrentException(getLogger("CasPool"), fmt::format(
                                "CAS ref-log append for namespace '{}': wedged txn {}-{} resolved durable and "
                                "was installed, but the post-install overlay fold failed and was retained "
                                "coherently for the next flush",
                                ns.string(), wedge_copy->txn_id.writer_epoch, wedge_copy->txn_id.ref_sequence));
                        }
                    }
                    ProfileEvents::increment(ProfileEvents::CasRefAppendUnwedged);
                }
                /// The wedge's tail bump above may have crossed the snapshot-publish threshold. This flush
                /// can still return early below WITHOUT reaching the post-commit scheduler -- an empty
                /// carve, or an all-no-op survivor batch (every survivor's `build_ops` contributes zero
                /// ops), both of which return before `maybeScheduleSnapshotPublish`. Trigger it HERE so a
                /// resolved wedge never leaves the table over-threshold until some later unrelated
                /// mutation happens to arrive. Idempotent with the post-commit call below (single-in-flight
                /// gate), and off-lock as that call requires.
                maybeScheduleSnapshotPublish(ns, rt);
            }
            else
            {
                /// Still Unresolved: fail every CURRENTLY queued item with the SAME uncertainty
                /// exception and do not allocate a new id. A later call into this namespace's queue
                /// retries the resolve -- rebuilding the candidate from scratch, which is why nothing of
                /// it is kept in the wedge (see the candidate build above).
                ///
                /// There is deliberately NO `NoAttemptSent` counterpart to `commitRefChunk`'s arm here,
                /// and the asymmetry is not an oversight: this `Unresolved` comes from
                /// `resolveByExactGet`, which has no pre-attempt gate at all -- it always issues its GET,
                /// and reports `Unresolved` for "absent" and "the read failed" alike. So a resolution can
                /// never mean "nothing was sent", and more to the point the wedge it would skip ALREADY
                /// EXISTS and describes an object that may well be durable. Clearing it on a failed read
                /// is the one thing this path must never do.
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
    bool table_live = false;
    {
        std::lock_guard lock(rt->state_mutex);
        working = rt->state;
        table_live = rt->state.getLifecycle() == RefLifecycle::Live;
    }

    /// Two-phase carve (spec §2). The old carve popped from `pending` while interleaving the allocating
    /// `seen_refs`/`batch` growth and only recorded the batch into `owned_items` afterwards, so any throw
    /// after the first pop stranded already-popped items -- neither in `pending` nor in `owned_items` --
    /// and their waiters hung forever. Instead:
    ///   PLAN (may throw, mutates NOTHING): under `ref_queue_mutex`, scan `pending` WITHOUT popping and
    ///   build the selection count, reserving every container (`batch`, `owned_items`) that the publish
    ///   below grows. A throw here leaves `pending`/`owned_items` byte-for-byte unchanged, so the
    ///   leadership-exit guard completes only the leader's own item and the untouched followers stay
    ///   queued for a later leader.
    ///   PUBLISH (no-throw): still under the SAME continuous `ref_queue_mutex` hold (no TOCTOU by
    ///   construction), pop the selected front items and append them to `batch` and `owned_items` using
    ///   only non-throwing operations (capacity pre-reserved; `shared_ptr` copies and `deque::pop_front`
    ///   never throw). ProfileEvents increments are deferred past the plan so the plan is literally
    ///   non-mutating.
    std::vector<std::shared_ptr<RefMutationItem>> batch;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const size_t cap = table_live ? kMaxRefBatch : 1;

        /// --- PLAN ---
        std::set<String> seen_refs;
        size_t selected = 0;         /// contiguous front items to carve
        bool scope_cut = false;      /// a duplicate ref name ended the selection early
        for (const auto & candidate : rt->pending)
        {
            if (selected >= cap)
                break;
            if (candidate->scope.kind == MutationScope::Kind::WholeShard)
            {
                /// A whole-shard mutation carves solo -- it may only be the FIRST (and then only) item.
                if (selected != 0)
                    break;
                if (carve_hook_for_test)
                    carve_hook_for_test(CarvePhaseForTest::PlanBatchGrow);
                batch.reserve(1);
                ++selected;
                break;
            }
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PlanSeenRefs);
            if (!seen_refs.insert(candidate->scope.ref_name).second)
            {
                scope_cut = true;
                break;
            }
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PlanBatchGrow);
            batch.reserve(selected + 1);
            ++selected;
        }
        /// Reserve the leader's responsibility set for the whole selection BEFORE any pop, so the publish
        /// append below cannot throw. `owned_items` already holds the leader's own item (recorded by
        /// `appendRefOps`), which the carve re-adds as it re-appears at the front of `pending` -- a
        /// harmless idempotent double-listing the guard tolerates, matching the pre-fix behavior.
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PlanReserveOwned);
        owned_items.reserve(owned_items.size() + selected);

        /// --- PUBLISH (no-throw) ---
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PublishPop);
        for (size_t i = 0; i < selected; ++i)
        {
            batch.push_back(rt->pending.front());        /// shared_ptr copy, capacity reserved
            owned_items.push_back(rt->pending.front());  /// same item into the responsibility set
            rt->pending.pop_front();
        }

        /// Deferred past the plan (spec §2) so the plan phase performs no observable mutation.
        if (scope_cut)
            ProfileEvents::increment(ProfileEvents::CasRefBatchScopeCuts);
    }
    if (batch.empty())
        return;   /// raced: everything was carved by a previous flush of this leader

    /// Per-item validation, in order, against `working` (per-request undo via `item_scratch`):
    /// business preconditions (thrown by `build_ops` itself) and the pre-encode admission budget both
    /// fail ONLY the offending item; survivors' ops accumulate into `final_ops` for ONE CHUNK. When
    /// admitting the next item's ops would exceed `ref_txn_max_ops`, the accumulated chunk is committed
    /// as a complete ref-log transaction and validation continues into a fresh chunk against the
    /// reseeded live state (spec §3 chunked flush): one tenure may emit several transactions.
    std::vector<RefOp> final_ops;
    std::vector<std::shared_ptr<RefMutationItem>> survivors;
    /// Every preview below stamps its throwaway transaction with `nextRefTxnId` of the state it is about
    /// to be applied to, under `live_epoch_fn()` -- the SAME rule and the same epoch source
    /// `allocateRefTxnId` uses for the real id, so a preview can never be rejected for an id shape the
    /// persisted transaction would have been given. Deriving each preview id from ITS OWN state, rather
    /// than carrying a running counter across the loop, is what makes failure isolation hold under
    /// INV-1: an item that throws part-way through its per-op previews leaves `working` untouched, and
    /// the next item's preview is still the successor of `working` rather than of the abandoned item's
    /// last trial id. These ids are never persisted or compared outside this loop.
    for (size_t item_index = 0; item_index < batch.size(); ++item_index)
    {
        const auto & it = batch[item_index];

        /// Step 1: build this item's ops and apply the counts-only per-item caps. `build_ops` runs at
        /// most once per item, HERE -- the overflowing item's ops are built once and reused in the fresh
        /// chunk it lands in (the at-most-once contract holds across a chunk boundary). A failure here
        /// (a business precondition thrown by `build_ops`, or an over-cap item/op) fails ONLY this item;
        /// the chunk in progress and the remaining items are untouched.
        std::vector<RefOp> item_ops;
        bool removal_class = false;
        try
        {
            item_ops = it->build_ops(working);

            /// Counts-only admission caps (spec §3), checked before any op is touched further so an
            /// oversized item or op never reaches `working` or the state-machine preview below and
            /// fails ALONE -- neighbors in this same batch are unaffected. Removal-class items are
            /// exempt from both: they share the larger `ref_removal_max_bytes` byte budget instead
            /// (`checkBudget`, `CasRefLogFormat.cpp`) and are already carved as singletons (`WholeShard`
            /// scope forces a solo carve above). `refLogTxnIsRemovalClass` is the ONE canonical
            /// discriminator (built ops contain `RemoveNamespace`) shared with the codec's own
            /// `checkBudget` -- `WholeShard` scope alone is NOT a substitute (the stale-precommit
            /// reclaim sweep is also `WholeShard`-scoped but is not removal-class).
            removal_class = refLogTxnIsRemovalClass(item_ops);
            if (!removal_class)
            {
                if (item_ops.size() > ref_txn_max_ops)
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' has {} operations, exceeding the normal-class "
                        "per-item op-count cap {} — refusing before any object is created",
                        ns.string(), item_ops.size(), ref_txn_max_ops);
                for (const RefOp & op : item_ops)
                {
                    const size_t op_bytes = encodedOpSize(op);
                    if (op_bytes > ref_op_max_bytes)
                        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                            "ref mutation on namespace '{}' contains an op of encoded size {}, exceeding "
                            "the normal-class per-op cap {} — refusing before any object is created",
                            ns.string(), op_bytes, ref_op_max_bytes);
                }
            }
        }
        catch (...)
        {
            complete_error({it}, std::current_exception());
            continue;
        }

        /// Step 2: chunk boundary (spec §3). If admitting this item's ops would push the current
        /// (non-empty) chunk over `ref_txn_max_ops`, COMMIT the accumulated chunk now as a COMPLETE
        /// ref-log transaction and start a fresh one. Removal-class items are always solo-carved
        /// (`WholeShard` scope), so `final_ops` is empty when one is processed and this branch never
        /// fires for them.
        if (!removal_class && !final_ops.empty()
            && final_ops.size() + item_ops.size() > ref_txn_max_ops)
        {
            /// Release the scratch `working` so `commitRefChunk`'s post-commit overlay fold is in place
            /// (the E5 fast path), exactly as the single-chunk path does before its commit arm.
            working = RefTableState{};
            const bool committed = commitRefChunk(ns, rt, final_ops, survivors);
            if (!committed)
            {
                /// Failure isolation (spec §3): chunk N's survivors were already failed inside
                /// `commitRefChunk`. Fail THIS item and the entire not-yet-attempted remainder too, so no
                /// owned item is left stranded (its waiter would hang and its `build_ops` closure become
                /// unsafe). Earlier chunks that already committed keep their callers' success -- an
                /// unresolved wedge from `commitRefChunk` therefore contains ONLY this chunk.
                std::vector<std::shared_ptr<RefMutationItem>> remainder(batch.begin() + item_index, batch.end());
                complete_error(remainder, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}': a preceding chunk of this multi-transaction "
                    "flush did not commit — this item was not attempted and can be retried", ns.string())));
                return;
            }
            /// Reseed `working` from the now-live state: the speculative `working` with trial ids from
            /// the just-committed chunk is discarded, so a later zero-op item is completed against the
            /// REAL committed id, never a trial id that never persisted. The preview ids need no reseed
            /// of their own -- each is derived from the state it is applied to, so re-seating `working`
            /// re-seats them. A throw at the boundary -- the injected `ChunkReseed` fault, or a genuine
            /// reseed allocation failure -- propagates to `appendRefOps`' tenure-containment catch, which
            /// preserves the already-committed chunk's callers' success.
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::ChunkReseed);
            {
                std::lock_guard lock(rt->state_mutex);
                working = rt->state;
            }
            final_ops.clear();
            survivors.clear();
        }

        /// Step 3: validate this item into the current (possibly fresh) chunk and, past all throwing
        /// points, publish its effects into `working`/`final_ops`/`survivors`. A failure here fails ONLY
        /// this item. `item_ops` was built in step 1 against the pre-boundary state; the carve
        /// deduplicates ref names within a batch, so the overflowing item operates on a ref distinct
        /// from the just-committed chunk's and re-validating it against the reseeded `working` is
        /// consistent.
        RefTableState item_scratch = working;
        try
        {
            /// Whole-item shape validation (prerequisite to `dropNamespace`): the
            /// per-op loop below previews each op as its OWN single-op trial transaction, so a
            /// whole-transaction-shape rule like "remove_namespace must be the FINAL op" trivially
            /// passes on every singleton slice regardless of this item's REAL combined shape -- a
            /// malformed item (e.g. remove_namespace not last) would otherwise only be caught by
            /// `commitRefChunk`'s candidate apply -- which fails the whole chunk, taking every innocent
            /// co-batched item with it, and (before the candidate moved ahead of the `PUT`) did so only
            /// after the object was already durable. Validate the item's COMPLETE
            /// ops array as ONE combined transaction, against a throwaway copy of the pre-item state,
            /// before doing any other per-op work -- exactly what the real persisted transaction will
            /// contain, using only the public two-phase `applyRefLogTxn` entry point (no need to reach
            /// into the state machine's private per-op helpers).
            if (!item_ops.empty())
            {
                RefTableState shape_check = working;
                applyRefLogTxn(shape_check, RefLogTxn{ns.string(),
                    nextRefTxnId(shape_check.getGreatestApplied(), live_epoch_fn()), item_ops, std::nullopt});
            }

            for (const RefOp & op : item_ops)
            {
                /// Admission budget: only STATE-GROWING ops need the check --
                /// an `owner_transition` installing a binding (add or promote) and `set_published_at`.
                /// `namespace_birth` is exempt (it grows nothing, and a never-born state's preview has
                /// no meaningful "current snapshot" to encode); `remove_namespace` and a pure
                /// owner_transition removal shrink state and can never violate the budget.
                const bool state_growing = (op.kind == RefOpKind::OwnerTransition && op.new_binding.has_value())
                    || op.kind == RefOpKind::SetPublishedAt;
                if (state_growing && !admits(item_scratch, op, rt->snapshot_budget, rt->removal_budget))
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' would exceed the table's admission budget "
                        "(snapshot_budget={} removal_budget={}) — refusing before any object is created",
                        ns.string(), rt->snapshot_budget, rt->removal_budget);
                /// Apply THIS op to item_scratch now (a single-op trial transaction) so a LATER op of
                /// the SAME item (e.g. namespace_birth immediately followed by its first
                /// owner_transition) is validated -- both here and by admits's own preview -- against
                /// a state that already reflects it, exactly as the real combined transaction will.
                applyRefLogTxn(item_scratch, RefLogTxn{ns.string(),
                    nextRefTxnId(item_scratch.getGreatestApplied(), live_epoch_fn()), {op}, std::nullopt});
            }
            /// Reserve the growth of BOTH accumulators BEFORE this item's effects are published. These
            /// reservations are the ONLY remaining throwing steps; once they succeed the publish below is
            /// no-throw -- `working`'s move-assignment is `noexcept`, and the `RefOp` moves and the
            /// `shared_ptr` copy land in pre-reserved capacity. Before the fix, `working` was moved and
            /// `final_ops` appended before these allocations, so a failure here left a failed item applied
            /// to `working` (corrupting later items' validation) and -- when the throw fell between the
            /// two accumulator writes -- its ops already in the durably-committed transaction while its
            /// own caller was told the append failed.
            final_ops.reserve(final_ops.size() + item_ops.size());
            survivors.reserve(survivors.size() + 1);
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::ValidateFinalOps);
            working = std::move(item_scratch);
            for (RefOp & op : item_ops)
                final_ops.push_back(std::move(op));
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
        /// left to do), or every survivor of the LAST chunk contributed ZERO ops (an idempotent no-op,
        /// e.g. precommitAdd/promote re-targeting a manifest already exactly committed). Survivors of the
        /// latter kind still need marking done -- with no new object created, `committed_id` is the
        /// table's current high-water mark. After an earlier committed chunk, `working` was reseeded from
        /// the live state, so that mark is the REAL id the earlier chunk persisted, never a discarded
        /// trial id.
        if (!survivors.empty())
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            for (const auto & it : survivors)
            {
                it->committed_id = working.getGreatestApplied();
                it->done = true;
            }
            rt->cv.notify_all();
        }
        return;
    }

    /// Commit the FINAL chunk of this tenure (spec §3): the remaining accumulated ops form the last --
    /// possibly only -- ref-log transaction. Release the scratch `working` first so `commitRefChunk`'s
    /// post-commit overlay fold is in place (the E5 fast path), then run the full committed arm. Its
    /// survivors are completed (success or failure) inside it, so nothing is owed here on any outcome,
    /// and it no longer throws past its durable `PUT` at all (spec §A1).
    working = RefTableState{};
    commitRefChunk(ns, rt, final_ops, survivors);
}

bool CasRefLedger::commitRefChunk(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                                  const std::vector<RefOp> & chunk_ops,
                                  const std::vector<std::shared_ptr<RefMutationItem>> & chunk_survivors)
{
    /// Reconstructed locally so this arm has the SAME completion + fence semantics as when it lived
    /// inline in `flushRefBatch`: `complete_error` wakes a chunk's waiters under `ref_queue_mutex`, and
    /// `fence_ok` folds `superseded_by_remount` into the append fence so a self-remount landing between a
    /// leader's pre-allocate re-check and its `PUT` reports Unresolved rather than committing against a
    /// stale cache.
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
    const auto fence_ok = [this, &rt] { return fence_ok_fn() && !rt->superseded_by_remount.load(std::memory_order_acquire); };

    /// Self-remount re-check BEFORE allocating an id: the top-of-flush gate
    /// is passed once, but a leader can stall between it and here -- in `build_ops`' caller I/O -- across
    /// the whole fence-loss + remount window, then resume after `armMountFence`. Allocating {new_epoch,
    /// seq} now and PUTting it (its live `fence_ok` would pass) would persist a transaction validated
    /// against this orphaned runtime's STALE cache -- the C1 data-loss class. `superseded_by_remount` is
    /// published before the fence re-arm, so failing closed here (no id, no PUT, no wedge, cache
    /// unchanged) keeps the durable log free of any stale-view transaction. The append `fence_ok`
    /// (which also checks the flag) is the airtight backstop for the narrow window past this point.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
        return false;
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
            chassert(!rt->wedge, "commitRefChunk: new-id allocation attempted while the ref-log lane was still wedged");
            if (rt->wedge)
                wedged_key = rt->wedge->key;
        }
        if (wedged_key)
        {
            on_impossible_interference(*wedged_key,
                fmt::format("commitRefChunk attempted new-id allocation for namespace '{}' while the lane "
                    "was still wedged -- the wedge hard contract was violated", ns.string()),
                ns.string());
            complete_error(chunk_survivors, std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref-log append for namespace '{}': refusing to allocate a new ref-log id while the "
                "lane is wedged (wedge hard contract violated) -- mount fenced closed and remount scheduled",
                ns.string())));
            return false;
        }
    }

    /// Build the candidate state BEFORE the PUT (spec §A1), so that the region between "this chunk's
    /// object is durable" and "the runtime records it" is allocation-free and therefore cannot throw.
    /// It used to be the other way round -- PUT, then `applyRefLogTxn(rt->state, chunk_txn)` -- and that
    /// apply CAN throw on an allocation failure (the COW containers allocate their overlays), which left
    /// the transaction durable but invisible to the writer: the apply check of the day admitted any
    /// strictly greater id, so a later transaction sailed over the hole and a snapshot published
    /// afterwards was labelled with THAT id -- recovery then skips the stranded transaction forever
    /// while GC, which folds the ref LOGS, still applies it. That divergence is a data-loss class, not a
    /// stale cache. INV-1 now refuses the same hole from the read side too, so the accident this
    /// ordering prevents would also have to survive the density check to do any damage.
    ///
    /// A throw HERE, by contrast, is a clean PRE-durability failure -- the same class as an ordinary
    /// validation reject: no object exists yet, the cache is untouched, and the id is simply never used
    /// (the next attempt re-derives it from this same unchanged state).
    ///
    /// The copy is cheap because it SHARES the live state's COW bases; the apply allocates only the
    /// overlay. The candidate is deliberately NOT materialized here: a state that shares its base cannot
    /// fold in place, so folding now would rebuild the whole base (O(table) per chunk). The install
    /// below restores unique base ownership before the existing post-install fold, which therefore stays
    /// O(overlay), exactly as it is today.
    ///
    /// The id is derived in the SAME critical section that snapshots the state (INV-1): it is a function
    /// of `greatest_applied`, so reading it at a different instant than the state the transaction is
    /// applied to would be deriving this chunk's id from a different stream. Only this leader mutates
    /// `rt->state`, so the two reads cannot disagree today -- taking them together is what keeps that
    /// from being an invariant a future edit has to rediscover.
    std::optional<RefTableState> candidate;
    RefTxnId candidate_base_id;
    RefTxnId id;
    {
        std::lock_guard lock(rt->state_mutex);
        candidate.emplace(rt->state);
        candidate_base_id = rt->state.getGreatestApplied();
        id = allocateRefTxnId(*rt);
    }
    const RefLogTxn chunk_txn{ns.string(), id, chunk_ops, std::nullopt};
    try
    {
        applyRefLogTxn(*candidate, chunk_txn);
    }
    catch (...)
    {
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }

    /// Preconstruct the COMPLETE wedge here, BEFORE the PUT (spec §A1 site 3), and let the request read
    /// its key and body straight out of it. The `Unresolved` arm below used to build
    /// `RefAppendWedge{id, key, bytes}` AFTER the PUT, which copies two `String`s: an allocation failure
    /// there would leave a possibly-DURABLE object with NEITHER the transaction nor the wedge recorded --
    /// strictly worse than a wedge, because the next append then mints a fresh id and proceeds against a
    /// state that is missing a landed transaction, exactly the divergence the candidate above exists to
    /// prevent. With the wedge already built, that arm only has to MOVE it into the runtime.
    /// This is a rename, not an extra copy: the seal writes its result directly into the wedge's body,
    /// and the key is computed directly into the wedge's key, so nothing is copied twice on the ordinary
    /// committed path either. The key is computed OUTSIDE the seal's catch on purpose -- that keeps its
    /// (pre-durability) failure behaviour exactly as it was, propagating to `appendRefOps`' catch.
    RefAppendWedge prepared_wedge;
    prepared_wedge.txn_id = id;
    prepared_wedge.key = layout.refLogKey(ns, id);
    try
    {
        prepared_wedge.bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(chunk_txn));
    }
    catch (...)
    {
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }

    /// Arm the apply-pending marker (spec §A2) IMMEDIATELY before the `PUT` -- the last statement that
    /// still runs while nothing of this transaction can possibly be durable. A relaxed CAS on a
    /// `uint8_t`: allocation-free, because arming the marker must never be the thing that throws, and
    /// because everything BEFORE this point (the superseded re-check, the wedge contract, the candidate
    /// apply, the seal) is a pre-durability rejection that owes no clear -- which is precisely why the
    /// arm sits here and not at the top of the function.
    armApplyPending(*rt);

    CasWriteOutcome outcome{};
    /// WHY an Unresolved came back. Two jobs (finding #37 defect 3): the wedge message stops claiming an
    /// exhausted retry budget when in fact no request was ever sent, and -- see the `Unresolved` arm --
    /// the one reason that PROVES nothing was sent decides whether the lane wedges at all.
    CasUnresolvedReason unresolved_reason = CasUnresolvedReason::NotUnresolved;
    try
    {
        outcome = ref_request_controller->putIfAbsentControlled(
            prepared_wedge.key, prepared_wedge.bytes, fence_ok, /*out_token=*/nullptr, &unresolved_reason);
    }
    catch (...)
    {
        /// `putIfAbsentControlled` throws CORRUPTED_DATA when resolve-before-reissue observes a DIFFERENT
        /// object already at this txn's key -- a proven different-object conflict, not an unresolved PUT.
        /// Fail every survivor loudly and do NOT wedge (this is a conclusive rejection, not an uncertain
        /// outcome): the cache is unchanged and the lane stays usable. The id is not consumed either --
        /// but a foreign object sitting on this table's next key means the next attempt derives that
        /// same id and hits the same conflict, which is the correct fail-closed behaviour: something
        /// else is writing this table's log under our own epoch, and guessing a different id would only
        /// hide a mount-exclusivity violation.
        /// Conclusive also means PROVEN NON-DURABLE for our bytes (the key is write-once and holds
        /// someone else's object), so no apply is owed and the marker goes back to `Clean` (spec §A2).
        clearApplyPending(*rt);
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }
    switch (outcome)
    {
        case CasWriteOutcome::Committed:
        {
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PostDurableInstall);
            {
                std::lock_guard lock(rt->state_mutex);
                /// Only this leader mutates `rt->state`, so the candidate's base snapshot is still the
                /// current one: there is one append-lane leader per table at a time (the `leader_active`
                /// baton), the wedge-resolution apply ran earlier in this same flush on this same thread,
                /// recovery installs a state exactly once per runtime and has already completed for this
                /// table, and every other consumer (readers, the snapshot publisher) only COPIES the
                /// state under this mutex. Evaluated here, one statement before the install, and
                /// asserted inside it: the comparison allocates nothing, and the identifier is short
                /// enough that even the failure path's message is inline-buffered rather than heap
                /// allocated, so no build can turn the assert itself into an allocation in the region.
                [[maybe_unused]] const bool state_unchanged = rt->state.getGreatestApplied() == candidate_base_id;
                /// Post-durable install region 1 of 3 (spec §A2). The `catch` is unreachable while §A1
                /// holds -- the body allocates nothing -- and exists so that a violation of §A1 is
                /// VISIBLE (`Poisoned` + `CasRefApplyPoisoned`) instead of silently leaving this table's
                /// cached state missing a durable transaction. It rethrows unchanged: the lane's error
                /// handling (the survivors' completion in `appendRefOps`' catch) is exactly what it was.
                try
                {
                    DENY_ALLOCATIONS_IN_SCOPE;
                    /// The negative control (`setInstallRegionProbeForTest`): fired with the guard
                    /// already armed, so a probe that allocates aborts a debug build. Null in production.
                    if (install_region_probe_for_test)
                        install_region_probe_for_test();
                    chassert(state_unchanged);
                    /// The install. Allocation-free by construction -- a member-wise swap of pointers and
                    /// PODs plus two atomic increments -- hence non-throwing, which is the whole point:
                    /// the object is already durable, and anything that could throw between here and the
                    /// durable PUT would strand the transaction (see the candidate's construction above).
                    /// The tail counters are bumped in the SAME region as the install, so no failure mode
                    /// can record one without the other.
                    rt->state.swap(*candidate);
                    rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                    rt->tail_bytes_since_snapshot.fetch_add(prepared_wedge.bytes.size(), std::memory_order_relaxed);
                    /// Last statement of the install, in the SAME allocation-free region: "recorded" and
                    /// "no apply owed" become true together or not at all (spec §A2).
                    clearApplyPending(*rt);
                }
                catch (...)
                {
                    poisonApplyState(*rt, ns, "commitRefChunk install");
                    throw;
                }
                /// `candidate` now holds the DISPLACED state, which still shares the COW bases
                /// `rt->state` uses. Destroy it before the fold and the fold takes its O(overlay)
                /// in-place path (`use_count() == 1`); leave it alive and every commit would rebuild the
                /// whole base instead. `reset` only destroys: it allocates nothing and cannot throw,
                /// which is also why it is safe to do here, after the transaction is already recorded.
                /// This is the same work the pre-fix `applyRefLogTxn(rt->state, ...)` did when it
                /// move-assigned its scratch over the live state, moved one statement later; it stays
                /// under `state_mutex` because the release decrements bases whose `use_count()` the fold
                /// below reads (see both COW headers' materialize safety argument).
                candidate.reset();
                /// COW-map materialization: fold this chunk's overlay into the base HERE, under the SAME
                /// state_mutex critical section as the install above, so `rt->state.getCommitted()` is
                /// back to "base + empty overlay" before the next flush's -- or the next chunk's reseed --
                /// trial copies (`working = rt->state`) begin. With `working` already released by the
                /// caller and the displaced state destroyed just above, `rt->state`'s bases are uniquely
                /// owned here (barring a concurrent publisher holding a copy, in which case the fold
                /// correctly falls back to build-fresh-and-swap), so this is an O(overlay) IN-PLACE fold,
                /// not the O(n) base copy it once was.
                ///
                /// This fold is an OPTIMIZATION, NOT part of the commit: it runs after the txn is durable
                /// AND installed, and each container's fold is coherent at every intermediate throw point
                /// (see `CasRefCowMap.cpp`). So a mid-fold allocation failure (the tracked allocator can
                /// throw `MEMORY_LIMIT_EXCEEDED`) leaves `rt->state` EXACTLY coherent -- only with a
                /// non-empty overlay the next flush re-folds. Swallow it: the commit succeeded, the
                /// survivors below must be told so, and nothing is bricked. It is deliberately OUTSIDE
                /// the deny region: it is allowed to allocate, and it is allowed to fail.
                try
                {
                    rt->state.materializeCommitted();
                }
                catch (...)
                {
                    tryLogCurrentException(getLogger("CasPool"), fmt::format(
                        "CAS ref-log append for namespace '{}': committed txn {}-{} was applied durably, but "
                        "the post-commit overlay fold failed and was retained coherently for the next flush",
                        ns.string(), id.writer_epoch, id.ref_sequence));
                }
            }
            /// The OLD outer catch around the install is gone. It turned an apply that threw AFTER the
            /// PUT into a `LOGICAL_ERROR` -- an honest report of a lane that its own comment predicted
            /// would be bricked on every future recovery. The install can no longer throw, so there is
            /// nothing left for it to convert. The catch that remains around the region changes no
            /// behaviour: it only marks the table `Poisoned` and rethrows the original exception.
            ProfileEvents::increment(ProfileEvents::CasRefBatchFlushes);
            ProfileEvents::increment(ProfileEvents::CasRefBatchedMutations, chunk_survivors.size());
            {
                std::lock_guard<std::mutex> g(ref_queue_mutex);
                for (const auto & it : chunk_survivors)
                {
                    it->committed_id = id;
                    it->done = true;
                }
                rt->cv.notify_all();
            }
            /// The threshold trigger -- off the lane,
            /// dispatched AFTER waking every waiter above so this commit's own callers are never
            /// delayed by it. Per chunk (spec §3): each committed chunk schedules its own publication,
            /// and settlement coalesces the triggers so a mid-tenure publisher never suppresses a later
            /// chunk (`settleSnapshotPublish`).
            maybeScheduleSnapshotPublish(ns, rt);
            return true;
        }
        case CasWriteOutcome::DefiniteFailure:
        {
            /// PROVEN never applied (that is the whole meaning of this outcome), so no apply is owed
            /// and the marker returns to `Clean` (spec §A2).
            clearApplyPending(*rt);
            ProfileEvents::increment(ProfileEvents::CasRefAppendDefiniteFailure);
            complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' definitively failed (non-retryable rejection); "
                "cached state is unchanged and txn id {}-{} was never used (a retry re-derives it)",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return false;
        }
        case CasWriteOutcome::Unresolved:
        {
            /// The ONE `Unresolved` shape that must NOT wedge (finding #37 defect 3). The wedge exists
            /// because an `Unresolved` PUT MAY HAVE LANDED: the durable log may or may not contain this
            /// transaction, only `resolveByExactGet` on that exact key can settle it, and until it does,
            /// minting a later id would build on a state that may be missing a landed transaction. All of
            /// that presupposes an attempt was SENT.
            ///
            /// `unresolvedProvesNothingWasSent` is true only for `NoAttemptSent`, which
            /// `putIfAbsentControlled` reports only when a pre-attempt gate -- the mount fence or the
            /// operation deadline -- rejected while `attempts_sent == 0`, i.e. strictly before the first
            /// `backend->putIfAbsent`. Nothing reached the network, so the key is provably unwritten:
            /// there is nothing for a wedge to resolve, and wedging is not merely pointless but harmful.
            /// A wedge over a key that was never written can NEVER clear -- `resolveByExactGet` reads the
            /// key absent and reports `Unresolved` forever -- so it blocks every ref append for this table
            /// on this replica, inserts included, until a remount. A transient fence blip in the
            /// pre-attempt gate would cost the table its write availability.
            ///
            /// The counterexample this argument deliberately excludes: a fence lost or a deadline reached
            /// AFTER at least one attempt is `FenceLostMidWay`/`DeadlineMidWay`, and an attempt that
            /// COMMITTED but returned under a dropped fence is `FenceLostPostWrite`. Each of those may
            /// have left a durable object, so each keeps wedging -- as does anything a future contributor
            /// adds to the enum without classifying it (see the predicate's allow-list construction).
            if (unresolvedProvesNothingWasSent(unresolved_reason))
            {
                /// Nothing can be durable, so no apply is owed and the marker returns to `Clean`, exactly
                /// as on the `DefiniteFailure` arm. Leaving it `ApplyPending` would claim this table may
                /// be missing a durable transaction for the rest of the runtime's life.
                clearApplyPending(*rt);
                /// Count it. Before this arm existed these refusals bumped `CasRefAppendWedged`, so
                /// removing the wedge also removed the only signal they were happening at all -- and a
                /// soak oracle watching that counter fall could not tell "the fix works" from "nothing
                /// happened". A separate event keeps both readings available: the wedge counter now means
                /// only genuinely ambiguous appends, and this one means availability preserved.
                ProfileEvents::increment(ProfileEvents::CasRefAppendPreAttemptRefused);
                /// The id is not consumed (INV-1): it was derived from `greatest_applied`, which this
                /// refusal leaves exactly as it was, so the next caller on this table derives the SAME id
                /// and the durable stream keeps no trace of the refusal. That is the free half of the
                /// every-attempt rule -- an attempt that provably sent nothing owes nothing.
                /// `prepared_wedge` is simply discarded -- building it before the PUT costs nothing here
                /// and is what makes the genuinely ambiguous path below allocation-free.
                complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}' txn {}-{} was refused BEFORE any request was "
                    "sent ({}) — the append lane is NOT wedged (nothing can be durable, so there is "
                    "nothing to resolve) and the txn id is not consumed (a retry re-derives it)",
                    ns.string(), id.writer_epoch, id.ref_sequence,
                    describeUnresolvedReason(unresolved_reason))));
                return false;
            }
            {
                std::lock_guard lock(rt->state_mutex);
                static_assert(std::is_nothrow_move_constructible_v<RefAppendWedge>,
                    "the wedge install below must be non-throwing: the object it describes may already be "
                    "durable, and the wedge is the only record that can ever resolve it");
                /// Post-durable install region 3 of 3 (spec §A2). The marker deliberately STAYS
                /// `ApplyPending` on the success path: an `Unresolved` outcome is precisely "an object
                /// that may be durable and is not applied", and the wedge is what will later resolve it
                /// -- so the wedged lane's steady state is the pending state, cleared only when the
                /// resolution installs the transaction or proves it never landed. Losing the wedge here
                /// is strictly WORSE than being wedged (neither the transaction nor the record of it
                /// exists, and the next append mints a fresh id against a state that may be missing a
                /// landed transaction), so a throw poisons even though durability is unproven here.
                try
                {
                    DENY_ALLOCATIONS_IN_SCOPE;
                    /// The negative control, as in the other two regions.
                    if (install_region_probe_for_test)
                        install_region_probe_for_test();
                    /// The install of the OTHER post-durable record (spec §A1 site 3). `Unresolved` means
                    /// the object may well be durable, so this wedge is the only thing that can ever
                    /// resolve it -- recording it must not be able to fail. `rt->wedge` is provably
                    /// disengaged here (the wedge hard contract, enforced above before the id was minted),
                    /// so this engages the optional by MOVE-CONSTRUCTING from the prepared wedge: two
                    /// pointer-stealing `String` moves and a POD id, no allocation, no free. Had it been
                    /// engaged, the same statement would move-ASSIGN and free the displaced buffers inside
                    /// the region -- which is exactly why the contract is checked, not assumed.
                    rt->wedge = std::move(prepared_wedge);
                }
                catch (...)
                {
                    poisonApplyState(*rt, ns, "commitRefChunk wedge install");
                    throw;
                }
            }
            ProfileEvents::increment(ProfileEvents::CasRefAppendWedged);
            complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' txn {}-{} is UNCERTAIN ({}) — "
                "the append lane is wedged until the SAME key resolves durable or a conclusive rejection "
                "is observed; this outcome is unproven, not failure",
                ns.string(), id.writer_epoch, id.ref_sequence,
                describeUnresolvedReason(unresolved_reason))));
            return false;
        }
    }
    /// Unreachable: the switch above covers every `CasWriteOutcome`. Kept explicit so the function has a
    /// defined return on all control-flow paths.
    return false;
}


bool CasRefLedger::admitSnapshotPublishUnderStateLock(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex` (the `may_mutate` fence check is the caller's responsibility, since
    /// it is not held under `state_mutex`). The whole decision -- the threshold trigger, the
    /// single-in-flight gate, the backoff deadline -- and the `pending_snapshot_publishes` increment all
    /// happen under that ONE hold, so two racing dispatchers can never both admit a publish for this
    /// table, and the settlement re-evaluation can decrement-and-re-admit without the count transiently
    /// reaching zero.
    const uint64_t now = boot_ms_now_fn();
    if (rt.state.getLifecycle() == RefLifecycle::Live
        /// Single-in-flight gate: at most one background publish per table.
        && rt.pending_snapshot_publishes.load(std::memory_order_relaxed) == 0
        /// Backoff deadline: after a non-Committed publish, a saturated backend is not re-dispatched
        /// until the bounded backoff elapses (the read-triggered PUT-storm latch).
        && now >= rt.publish_backoff_until_ms)
    {
        /// The threshold trigger reads the tail counters directly -- no walk, no age filter.
        /// `tail_count_since_snapshot`/`tail_bytes_since_snapshot` count ONLY applied txns strictly above
        /// `newest_snapshot_id` (maintained incrementally by every commit in `commitRefChunk` and by the
        /// wedge-resolution apply in `flushRefBatch`), so `over_threshold` here is never true without a
        /// real, immediately-coverable candidate.
        const uint64_t publishable_count = rt.tail_count_since_snapshot.load(std::memory_order_relaxed);
        const uint64_t publishable_bytes = rt.tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
            || publishable_bytes > config.snapshot_log_bytes_threshold;
        if (over_threshold)
        {
            rt.pending_snapshot_publishes.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void CasRefLedger::dispatchSnapshotPublisher(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// `admitSnapshotPublishUnderStateLock` already incremented `pending_snapshot_publishes` for THIS
    /// dispatch. Off the mutation hot path: `trySnapshotPublishOnce` never touches the append queue, so
    /// dispatching it onto an unrelated global-pool thread can never deadlock a flush leader.
    /// `pin_owner()` (the Pool's `shared_from_this`) keeps the Pool -- and hence this ledger member --
    /// alive for the thread's lifetime.
    ProfileEvents::increment(ProfileEvents::CasRefSnapshotPublishDispatched);
    auto owner = pin_owner();
    try
    {
        ThreadFromGlobalPool([owner, this, ns, rt]
        {
            setThreadName(ThreadName::CAS_REF_SNAPSHOT_PUBLISH);
            try
            {
                trySnapshotPublishOnce(ns);
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot publish attempt failed");
            }
            settleSnapshotPublish(ns, rt);
        }).detach();
    }
    catch (...)
    {
        /// The `ThreadFromGlobalPool` ctor can throw (pool exhaustion) AFTER the count was incremented.
        /// Undo the count WITHOUT the settlement re-evaluation (else a persistently-failing dispatch could
        /// re-fire itself in a loop) and SWALLOW the failure: dispatching a background publish is a
        /// best-effort maintenance trigger and must never fail an otherwise-successful read or mutation.
        /// The next trigger reschedules.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        }
        rt->publish_settle_cv.notify_all();
        tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot-publish dispatch failed to launch");
    }
}

void CasRefLedger::settleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// Fence re-checked outside `state_mutex` (as in `maybeScheduleSnapshotPublish`): a fence lost
    /// between this publish's dispatch and its settlement must suppress a follow-up.
    const bool live_mount = may_mutate();
    bool redispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        /// Drop THIS publish's in-flight count and, under the SAME hold, re-evaluate the accumulated
        /// tail. A chunked tenure (or any concurrent mutation) that raised more log above the newest
        /// snapshot while this publish was capturing an earlier prefix had its trigger discarded by the
        /// single-flight gate; settlement re-fires it here so chunks 2..N are not suppressed until an
        /// unrelated later trigger (spec §3 snapshot coalescing). Re-admitting under the SAME lock as the
        /// decrement means `pending_snapshot_publishes` never transiently reaches 0 across the handoff, so
        /// `waitForSnapshotPublishSettleForTest` never observes a false "settled". A durable publish
        /// already subtracted its captured tail, so this self-terminates once the tail is back at/under
        /// threshold; a non-durable one armed the backoff, which `admit...` respects -- no PUT storm.
        rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        if (live_mount)
            redispatch = admitSnapshotPublishUnderStateLock(*rt);
    }
    if (redispatch)
        dispatchSnapshotPublisher(ns, rt);
    else
        rt->publish_settle_cv.notify_all();
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

    bool dispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        dispatch = admitSnapshotPublishUnderStateLock(*rt);
    }
    if (dispatch)
        dispatchSnapshotPublisher(ns, rt);
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

std::optional<RefTxnId> CasRefLedger::sealedFromForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->sealed_from;
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
    return rt->state.getCommitted().overlayEntriesForTest();
}

std::set<std::pair<String, ManifestRef>> CasRefLedger::livePrecommitsForTest(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard lock(rt->state_mutex);
    return rt->state.getPrecommits();
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
        if (rt->state.getLifecycle() != RefLifecycle::Live)
            return false;   /// nothing to (re)publish here; dropNamespace publishes its own Removed snapshot
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < rt->state.getGreatestApplied()))
            return false;   /// nothing above the newest snapshot
        candidate_state = rt->state;
        candidate_x = rt->state.getGreatestApplied();
        captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
        captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
    }

    const RefTableSnapshot snap = snapshotOf(candidate_state, ns.string());

    /// `candidate_state` is a COW copy that SHARES `rt->state`'s committed/owned-manifest bases. It is
    /// dead past `snapshotOf`. Destroy it HERE, under `state_mutex` -- not at function return outside any
    /// lock. Its destruction is a `shared_ptr` release-DECREMENT of those shared bases; the flush thread's
    /// in-place `materializeCommitted()` reads their `use_count()` (relaxed) under this same mutex. Doing
    /// the release off-lock would leave that load racing this atomic decrement with no happens-before
    /// (TSan-reportable) and could momentarily let a flush observe a `use_count()` of 1 while this
    /// decrement is in flight. Under the lock the two are serialized. Every subsequent exit path (encode
    /// failure, non-Committed PUT, the monotonic-guard early return, success) then destroys an already
    /// empty `candidate_state`, which touches no shared base. See both COW headers' materialize safety
    /// argument, which relies on exactly this: every cross-thread copy is created AND destroyed under the
    /// state lock.
    {
        std::lock_guard lock(rt->state_mutex);
        candidate_state = RefTableState{};
    }

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
        /// `sealed_from` pairs with `newest_snapshot_id` (CasRefLedger.h): it is the newest snapshot's own
        /// seal bound, `nullopt` for an ordinary Live snapshot. Take it from the snapshot just published
        /// (`snap`, always `nullopt` here) so this later publication CLEARS any predecessor recovery seal's
        /// `sealed_from` -- leaving it would describe this non-seal snapshot with the seal's observed-region
        /// bound, a false introspection contract.
        rt->sealed_from = snap.sealed_from;
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
            for (const auto & [ref_name, mref] : rt->state.getPrecommits())
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
                    if (state.getPrecommits().contains({ref_name, mref}))
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
                if (!rt->state.getPrecommits().contains({ref_name, mref}))
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
        if (rt->state.getLifecycle() != RefLifecycle::Removed || !rt->state.getRemoveTxnId())
            return;
        remove_id = *rt->state.getRemoveTxnId();
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
    /// A Removed snapshot is never a recovery seal; take `sealed_from` from it (`nullopt`) so this
    /// publication CLEARS any predecessor seal's `sealed_from`, keeping the runtime field paired with
    /// `newest_snapshot_id` (CasRefLedger.h).
    rt->sealed_from = removed_snap.sealed_from;
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
            const auto it = state.getCommitted().find(ref_name);
            if (it == state.getCommitted().end())
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


void CasRefLedger::updateRefPublishedAt(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RefPublishedAtUpdate &)> mutator)
{
    /// One `set_published_at` ref-log transaction. EVERY change (even timestamp-only) is an explicit
    /// logged operation -- the immutable append-only log has no other way to record it.
    /// `published_at_ms` is the only metadata this op carries (the mutable-file map is gone; every
    /// per-part file is an ordinary manifest tree entry now, republished via `repointRef`, never
    /// through this side channel).
    appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.getCommitted().find(ref_name);
            if (it == state.getCommitted().end())
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "updateRefPublishedAt: no such ref {} in namespace {}", ref_name, ns.string());

            /// The mutator edits only `published_at_ms`; the carrier deliberately carries no
            /// `manifest_ref`, so a reachability change is structurally impossible here (it goes through
            /// publish/drop/repoint instead).
            RefPublishedAtUpdate update;
            update.published_at_ms = it->second.published_at_ms;

            mutator(update);

            RefOp op;
            op.kind = RefOpKind::SetPublishedAt;
            op.ref_name = ref_name;
            op.expected_manifest_ref = it->second.manifest_ref;
            op.published_at_ms = update.published_at_ms;
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::UpdateRefPublishedAt);
}


bool CasRefLedger::namespaceIsRemoved(const RootNamespace & ns)
{
    const auto rt = getRefTableRuntime(ns);
    ensureRefTableRecovered(ns, *rt);
    std::lock_guard<std::mutex> lock(rt->state_mutex);
    /// A genuinely-removed namespace is `Removed` WITH a `remove_txn_id` (RemoveNamespace sets both). The
    /// never-born default is also `Removed` but carries no `remove_txn_id`, and a recreated one is `Live`
    /// (NamespaceBirth resets the marker) — so `remove_txn_id.has_value()` is the exact born-then-removed
    /// discriminator, matching promote's own `state.getRemoveTxnId()` recreation guard.
    return rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value();
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
        if (rt->state.getLifecycle() != RefLifecycle::Live)
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
            if (state.getLifecycle() != RefLifecycle::Live)
                return {};   /// raced: another caller already removed it since our check above

            std::vector<RefOp> ops;
            for (const auto [ref_name, row] : state.getCommitted())
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, row.manifest_ref};
                ops.push_back(op);
            }
            for (const auto & [ref_name, mref] : state.getPrecommits())
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                ops.push_back(op);
            }
            RefOp remove;
            remove.kind = RefOpKind::RemoveNamespace;
            ops.push_back(remove);

            stats.committed_refs = state.getCommitted().size();
            stats.precommits = state.getPrecommits().size();
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::DropNamespace,
        /// The operations above already name (and remove) every current precommit
        /// binding regardless of epoch, making the ordinary stale-precommit maintenance sweep redundant
        /// for THIS call -- and, left enabled, a race: the hoisted sweep runs first and would reclaim an
        /// epoch-stale binding in its OWN transaction, so `state.getPrecommits()` above would already be
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
