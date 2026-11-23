#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcPhaseTimer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <Common/scope_guard_safe.h>
#include <Common/threadPoolCallbackRunner.h>
#include <base/defines.h>
#include <base/scope_guard.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <variant>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace ProfileEvents
{
    extern const Event CASGCClampSuppressedPasses;
    extern const Event CASGCDeadPrecommitSkipped;
    extern const Event CASGCRetiredCondemned;
    extern const Event CASGCRetiredSpared;
    extern const Event CASGCRetiredIntentConfirmed;
    extern const Event CASGCRetiredBodyDeleted;
    extern const Event CASGCRetiredBodyDeleteFailed;
    extern const Event CASGCRetiredGraduated;
    extern const Event CASGCRetiredRedeleted;
    extern const Event CASGCRetiredRedeleteFailed;
    extern const Event CASGCRetireReplaced;
    extern const Event CASGCHeartbeatFenceOuts;
    extern const Event CASGCIntentCreated;
    extern const Event CASGCIntentConfirmedExisting;
    extern const Event CASGCIntentUnconfirmedCarry;
    extern const Event CASGCIntentDeleted;
    extern const Event CASGCIntentDeletedOnSpare;
    extern const Event CASGCUndecodableBlobBody;
    extern const Event CASGCEnumerationPages;
    extern const Event CASGCRefWalkPlansBuilt;
    extern const Event CASGCUnmatchedAdoptedParentLives;
    extern const Event CASGCStuckRemovals;
    extern const Event CASGCNamespaceCleanupLeaks;
    extern const Event CASGCRebuildVirginByEnumeration;
    extern const Event CASGCUnappliedFoldedTransactions;
    extern const Event CASRefGlobalListPages;
    extern const Event CASRefLogBodyGets;
    extern const Event CASRefManifestBodyFoldGets;
    extern const Event CASRefEmittedEdges;
    extern const Event CASRefCleanupObjectsDeleted;
    extern const Event CASRefCleanupObjectsPlanned;
    extern const Event CASRefCleanupNamespacesSkipped;
    extern const Event CASRefCleanupStoppedLeaseMoved;
    extern const Event CASRefCleanupStoppedAuthorityUnverified;
    extern const Event CASRefCleanupChunksUnderCatalogChange;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_SCHEDULE_TASK;
    extern const int CORRUPTED_DATA;
    extern const int INVALID_STATE;
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

/// Soft limit on the namespace janitor's phase: no page starts after it, the page in progress finishes.
constexpr uint64_t kJanitorBudgetMs = 20'000;

enum class RetiredCount : uint8_t
{
    IntentConfirmed,
    BodyDeleted,
    BodyDeleteFailed,
};

/// Each of these counters keeps its released name beside it, for the dashboards and tests that read that name.
void countRetired(RetiredCount count)
{
    switch (count)
    {
        case RetiredCount::IntentConfirmed:
            ProfileEvents::increment(ProfileEvents::CASGCRetiredIntentConfirmed);
            ProfileEvents::increment(ProfileEvents::CASGCRetiredGraduated);
            return;
        case RetiredCount::BodyDeleted:
            ProfileEvents::increment(ProfileEvents::CASGCRetiredBodyDeleted);
            ProfileEvents::increment(ProfileEvents::CASGCRetiredRedeleted);
            return;
        case RetiredCount::BodyDeleteFailed:
            ProfileEvents::increment(ProfileEvents::CASGCRetiredBodyDeleteFailed);
            ProfileEvents::increment(ProfileEvents::CASGCRetiredRedeleteFailed);
            return;
    }
}

GcState decodeGenerationFourGcState(std::string_view bytes)
{
    GcState state = decodeGcState(bytes);
    requireGenerationFourGcState(state);
    return state;
}

/// What one janitor delete job did with its chunk.
struct WriteOnceJobResult
{
    uint64_t deleted = 0;
    uint64_t leaked = 0;
    uint64_t requests = 0;
    String error;
};

/// Deletes `chunk` as one request. A storage that refuses the batch gets one request per key, each
/// accounted on its own, so a failure late in the chunk leaks only the keys it did not delete.
void removeChunkWriteOnceCountingKeys(CasOperation & op, const std::vector<WriteOnceKey> & chunk, WriteOnceJobResult & result)
{
    ++result.requests;
    try
    {
        op.removeManyWriteOnce(chunk, Retry::standard());
        result.deleted += chunk.size();
        return;
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::NOT_IMPLEMENTED)
            throw;
    }
    for (const WriteOnceKey & key : chunk)
    {
        ++result.requests;
        try
        {
            op.removeManyWriteOnce({key}, Retry::standard());
            ++result.deleted;
        }
        catch (...)
        {
            ++result.leaked;
            if (result.error.empty())
                result.error = getCurrentExceptionMessage(false);
        }
    }
}

/// The `on_page_fetched` hook GC passes to every `forEachListedKey`/`recoverRefTable`
/// call it owns (never passed by fsck/offline-repair callers of those shared helpers) -- one increment
/// per physical LIST page, never per listed key.
void onGcEnumerationPage()
{
    ProfileEvents::increment(ProfileEvents::CASGCEnumerationPages);
}

/// Defined below; forward-declared so the post-CAS hand-off delete in `runRegularRound` can
/// reach the same wholesale LIST-delete helper the retention prune uses.
uint64_t deletePrefixWholesale(CasOperation & op, const String & prefix, uint64_t bounded_remaining,
                               bool * out_fully_drained = nullptr);

/// The text an incarnation carries into the event log. Persisted and live incarnations render the
/// same way, so the column speaks one vocabulary whichever half of the pipeline wrote the row.
String renderIncarnation(const PersistedEtag & token)
{
    return token.dialect + ":" + token.value;
}

/// The label one removal carries into the event log and the outcomes audit rows.
std::string_view removalName(Removal removal)
{
    switch (removal)
    {
        case Removal::Removed:  return "deleted";
        case Removal::Gone:     return "absent";
        case Removal::Mismatch: return "replaced";
    }
    UNREACHABLE();
}

}

std::set<UInt128> RefPlan::lifeIds() const
{
    std::set<UInt128> out;
    for (const auto & [life_id, row] : rows)
        out.insert(life_id);
    return out;
}

std::vector<NamespaceLifeId> RefPlan::lives() const
{
    std::vector<NamespaceLifeId> out;
    out.reserve(rows.size());
    for (const auto & [life_id, row] : rows)
        out.push_back(row.life);
    return out;
}

std::map<UInt128, RefLifeFoldState> RefPlan::parentFoldStates() const
{
    std::map<UInt128, RefLifeFoldState> out;
    for (const auto & [life_id, row] : rows)
        if (row.has_parent_fold_state)
            out.emplace(life_id, row.fold_state);
    return out;
}

std::map<UInt128, RefLifeFoldState> RefPlan::successorFoldStates() const
{
    std::map<UInt128, RefLifeFoldState> out;
    for (const auto & [life_id, row] : rows)
        out.emplace(life_id, row.fold_state);
    return out;
}

size_t RefPlan::changedRows() const
{
    return std::count_if(rows.begin(), rows.end(), [](const auto & item)
    {
        const RefWalkPlanRow & row = item.second;
        return row.tail_observation
            && row.fold_state.coverage.last_folded_ref_id < *row.tail_observation;
    });
}

std::optional<String> stuckRemovalWarning(
    const RefWalkPlanRow & row, uint64_t current_round, uint64_t threshold_rounds,
    const Layout & layout)
{
    if (!row.removal_started_round || row.fold_state.cleanup_evidence)
        return std::nullopt;
    const uint64_t started = *row.removal_started_round;
    if (current_round < started || current_round - started < threshold_rounds)
        return std::nullopt;

    const uint64_t age = current_round - started;
    const std::optional<RefHold> & hold = row.fold_state.coverage.hold;
    if (hold && hold->reason == HoldReason::BodyUndecodable)
        return fmt::format(
            "CAS GC namespace removal is stuck: namespace='{}', life_id={}, removal_started_round={}, "
            "current_round={}, age_rounds={}; cleanup evidence is absent because ref-log body '{}' is unreadable; "
            "restore the exact object or recreate the pool",
            row.life.ns.string(), u128ToHex(row.life.incarnation), started, current_round, age,
            layout.refLogKey(row.life, hold->offending_position));

    return fmt::format(
        "CAS GC namespace removal is stuck: namespace='{}', life_id={}, removal_started_round={}, "
        "current_round={}, age_rounds={}; cleanup evidence is absent because terminal has not folded",
        row.life.ns.string(), u128ToHex(row.life.incarnation), started, current_round, age);
}

RefPlan buildRefWalkPlan(RoundInput && round_input)
{
    ProfileEvents::increment(ProfileEvents::CASGCRefWalkPlansBuilt);
    RefPlan plan{std::move(round_input.ref_scan), std::move(round_input.catalog_cut)};
    const CasRefCatalog::Snapshot & catalog_cut = plan.catalog_cut;
    const RefScanSummary & ref_scan = plan.ref_scan;
    catalog_cut.life_index.throwIfAmbiguous("CAS ref walk plan");

    /// The sole admission loop. Everything below can only find one of these rows.
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
    {
        if (entry.state == NsState::Creating)
            continue;
        plan.rows.emplace(entry.incarnation, RefWalkPlanRow{
            .life = NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation),
            .fold_state = {},
            .removal_started_round = entry.removal_started_round,
            .has_parent_fold_state = false,
            .tail_observation = std::nullopt});
    }

    for (const auto & [life_id, state] : ref_scan.parent_ref_lives)
    {
        const auto it = plan.rows.find(life_id);
        if (it == plan.rows.end())
        {
            /// The ordinary end of a namespace's life: its removal completed, GC deleted the catalog
            /// row, and the parent seal still carries the fold state of a life the current cut no
            /// longer names. There is nothing to attach it to and nothing for anyone to do, so this
            /// is counted, not narrated -- a per-drop log line would be pure noise, and it could not
            /// discriminate the expected case from an illegitimately vanished row anyway: this site
            /// sees only the absence, never the evidence. The signal lives in
            /// `CASGCUnmatchedAdoptedParentLives` and in the round's own
            /// `walk_plan_dropped_parent_rows` phase metric; proving that a row disappeared WITHOUT a
            /// completed removal belongs to fsck, which can compare against the removal evidence.
            ProfileEvents::increment(ProfileEvents::CASGCUnmatchedAdoptedParentLives);
            ++plan.dropped_parent_rows;
            continue;
        }
        it->second.fold_state = state;
        it->second.has_parent_fold_state = true;
    }
    for (const UInt128 & life_id : ref_scan.listed_lives)
        if (!plan.rows.contains(life_id))
            ++plan.dropped_listed_lives;
    for (const auto & [life_id, tail] : ref_scan.max_log_by_life)
    {
        const auto it = plan.rows.find(life_id);
        if (it == plan.rows.end())
        {
            ++plan.dropped_tails;
            continue;
        }
        it->second.tail_observation = tail;
    }
    return plan;
}

namespace tests
{

RefPlan buildRefWalkPlanForTest(RefScanSummary ref_scan, CasRefCatalog::Snapshot catalog_cut)
{
    return buildRefWalkPlan(RoundInput{std::move(ref_scan), std::move(catalog_cut)});
}

}

bool shouldDeferRound(size_t changed_shards, bool row_decision_due, uint64_t rounds_since_last_fold,
                      uint64_t fold_threshold, uint64_t fold_max_defer_rounds)
{
    if (row_decision_due)
        return false;
    if (changed_shards >= fold_threshold)
        return false;
    if (rounds_since_last_fold >= fold_max_defer_rounds)
        return false;
    return true;
}

Gc::Gc(PoolPtr store_, UInt128 gc_id_, std::function<uint64_t()> now_ms_fn_,
       std::function<uint64_t()> mono_ms_fn_, LoggerPtr log_)
    : store(std::move(store_))
    , gc_id(gc_id_)
    , logger(log_ ? std::move(log_) : getLogger("CasGc"))
    , now_ms_fn(std::move(now_ms_fn_))
    , mono_ms_fn(std::move(mono_ms_fn_))
{
    if (!store)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: store must not be null");
    if (gc_id == UInt128(0))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: gc_id must not be 0 (reserved for 'lease never held')");
    if (store->poolConfig().gc_stuck_removal_rounds == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Cas::Gc: gc_stuck_removal_rounds must be nonzero");
    if (store->poolConfig().intent_sweep_list_budget_keys == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Cas::Gc: intent_sweep_list_budget_keys must be nonzero");
    if (!now_ms_fn)
        now_ms_fn = []() -> uint64_t
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
    /// Use `store->bootMsNow()`, not the raw static `Pool::bootMs()`
    /// -- the latter bypasses the Pool's own injectable `config.boot_ms_fn`, so a time-controlled test
    /// that fakes the mount's clock via `boot_ms_fn` (but constructs a `Gc` without an explicit
    /// `mono_ms_fn`) would silently run its GC-side threshold math against the REAL wall clock while the
    /// mount side runs against the fake one -- two desynced clocks passing for the same test.
    /// `bootMsNow()` already falls back to `bootMs()` itself when no `boot_ms_fn` is injected, so
    /// production (no test seam in play) is unaffected.
    if (!mono_ms_fn)
        mono_ms_fn = [s = store]() -> uint64_t { return s->bootMsNow(); };
    /// The GC I/O pool (read-ahead, the `pending_deletes` fan-out and the namespace janitor's batch deletes),
    /// built here (ctor body), not in a member-initializer, so it can read `store->poolConfig()` after the null
    /// check above.
    /// The queue is UNBOUNDED because the hinting sites throttle themselves against
    /// `GcReadAhead::window`; a bounded queue would only move the throttle into
    /// `scheduleOrThrowOnError`, blocking the round thread instead of the hint loop that already knows
    /// how much it wants in flight.
    const size_t io_concurrency = std::max<size_t>(1, store->poolConfig().gc_io_concurrency);
    /// Keep `shutdown_on_exception` false: every task on this pool (read-ahead, re-delete, janitor delete) is submitted
    /// through a callback runner that stores its exception in the task's future, and no caller invokes
    /// `wait` on this pool.
    io_pool = std::make_unique<ThreadPool>(
        CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
        /*max_threads*/ io_concurrency, /*max_free_threads*/ io_concurrency, /*queue_size*/ 0,
        /*shutdown_on_exception*/ false);
    io_pool_refuse_at_for_test = store->poolConfig().gc_io_pool_refuse_at_for_test;
}

void Gc::runNamespaceJanitor(
    const GcState & leased_state, bool suppress_destructive, uint64_t cleanup_evidence_rows)
{
    GcPhaseTimer t(phase_sink, "namespace_cleanup");
    t.metric("evidence_rows", cleanup_evidence_rows);
    NamespaceJanitorResult janitor_result;
    bool budget_exhausted = false;
    try
    {
        CasRequests & requests = store->openRequests();
        const Layout & layout = store->layout();
        NamespaceJanitor janitor(requests, layout, CasOperation::kCasListPageKeys,
            [this](CasOperation & op, const std::vector<WriteOnceKey> & keys, NamespaceJanitorResult & out)
            {
                removeWriteOnceOnIoPool(op, keys, out);
            });
        const uint64_t deadline_ms = mono_ms_fn() + kJanitorBudgetMs;
        while (true)
        {
            /// ONE authority read per page, made here rather than from the predicate: the janitor's
            /// operation samples its liveness before every request, and a page walks up to a thousand keys.
            refreshAuthority(leased_state.lease.seq);
            const NamespaceJanitorResult page = janitor.runOnePage(
                suppress_destructive, [this] { return authority_held; }, /*first_page_of_pass=*/janitor_result.pages == 0);
            janitor_result.pages += page.pages;
            janitor_result.keys += page.keys;
            janitor_result.deleted += page.deleted;
            janitor_result.leaked += page.leaked;
            janitor_result.delete_requests += page.delete_requests;
            for (const String & anomaly : page.anomalies)
                LOG_WARNING(logger, "CAS namespace janitor: {}", anomaly);
            if (!page.more)
                break;
            if (mono_ms_fn() >= deadline_ms)
            {
                budget_exhausted = true;
                break;
            }
        }
    }
    catch (const std::exception & e)
    {
        LOG_WARNING(logger, "CAS namespace janitor stopped this round: {}", e.what());
    }
    if (janitor_result.leaked)
        ProfileEvents::increment(ProfileEvents::CASGCNamespaceCleanupLeaks, janitor_result.leaked);
    t.metric("janitor_pages", janitor_result.pages);
    t.metric("janitor_keys", janitor_result.keys);
    t.metric("janitor_deleted", janitor_result.deleted);
    t.metric("leaked", janitor_result.leaked);
    t.metric("delete_requests", janitor_result.delete_requests);
    t.metric("budget_exhausted", budget_exhausted ? 1 : 0);
}

void Gc::removeWriteOnceOnIoPool(CasOperation & op, const std::vector<WriteOnceKey> & keys, NamespaceJanitorResult & out)
{
    const size_t chunk_keys = bulkDeleteChunkKeys();
    std::vector<std::vector<WriteOnceKey>> chunks;
    for (size_t begin = 0; begin < keys.size(); begin += chunk_keys)
        chunks.emplace_back(keys.begin() + begin, keys.begin() + std::min(keys.size(), begin + chunk_keys));

    /// One per chunk, written by its job and read after every job finished.
    std::vector<WriteOnceJobResult> results(chunks.size());
    ThreadPoolCallbackRunnerLocal<void> runner(*io_pool, ThreadName::CAS_GC_JANITOR);
    std::vector<std::shared_ptr<ThreadPoolCallbackRunnerLocal<void>::Task>> handles;
    handles.reserve(chunks.size());
    SCOPE_EXIT_SAFE({ ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles); });
    size_t scheduled = 0;
    String refusal;
    for (; scheduled < chunks.size(); ++scheduled)
    {
        try
        {
            if (io_pool_refuse_at_for_test == scheduled)
            {
                io_pool_refuse_at_for_test.reset();
                throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "Injected CAS namespace janitor enqueue refusal at index {}", scheduled);
            }
            /// The round thread waits for every job before it refreshes `authority_held` again.
            handles.emplace_back(runner.enqueueAndGiveOwnership(
                [this, chunk = &chunks[scheduled], result = &results[scheduled], admitted_generation = op.generation()]
                {
                    try
                    {
                        CasOperation job_op = store->openRequests().resume(admitted_generation, [this] { return authority_held; });
                        removeChunkWriteOnceCountingKeys(job_op, *chunk, *result);
                    }
                    catch (...)
                    {
                        result->leaked = chunk->size() - result->deleted;
                        result->error = getCurrentExceptionMessage(false);
                    }
                }));
        }
        catch (...)
        {
            refusal = getCurrentExceptionMessage(false);
            break;
        }
    }
    ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles);

    for (size_t i = 0; i < scheduled; ++i)
    {
        handles[i]->future.get();
        out.deleted += results[i].deleted;
        out.leaked += results[i].leaked;
        out.delete_requests += results[i].requests;
        if (results[i].leaked)
            out.anomalies.push_back(fmt::format(
                "leaked {} dead-life objects starting at '{}': batch delete failed: {}",
                results[i].leaked, chunks[i].front().str(), results[i].error));
    }
    /// Leak-only like a failed request: the keys stay for the cursor's next pass over this page.
    uint64_t unscheduled = 0;
    for (size_t i = scheduled; i < chunks.size(); ++i)
        unscheduled += chunks[i].size();
    if (unscheduled)
    {
        out.leaked += unscheduled;
        out.anomalies.push_back(fmt::format(
            "leaked {} dead-life objects starting at '{}': delete job not scheduled: {}",
            unscheduled, chunks[scheduled].front().str(), refusal));
    }
}

uint64_t removeChunkWriteOnceOrOneByOne(CasOperation & op, const std::vector<WriteOnceKey> & chunk, const Retry & policy)
{
    try
    {
        op.removeManyWriteOnce(chunk, policy);
        return 1;
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::NOT_IMPLEMENTED)
            throw;
        for (const WriteOnceKey & key : chunk)
            op.removeManyWriteOnce({key}, policy);
        /// +1: the failed bulk attempt above is itself a call this helper made.
        return 1 + chunk.size();
    }
}

namespace
{
/// Deletes `cohort` as consecutive requests of at most `request_keys` keys; returns the call count.
uint64_t removeCohortWriteOnce(
    CasOperation & op, const std::vector<WriteOnceKey> & cohort, size_t request_keys, const Retry & policy)
{
    uint64_t calls = 0;
    for (size_t begin = 0; begin < cohort.size(); begin += request_keys)
    {
        const size_t end = std::min(cohort.size(), begin + request_keys);
        const std::vector<WriteOnceKey> request(cohort.begin() + begin, cohort.begin() + end);
        calls += removeChunkWriteOnceOrOneByOne(op, request, policy);
    }
    return calls;
}
}

size_t Gc::bulkDeleteChunkKeys() const
{
    const uint64_t configured = store->poolConfig().gc_bulk_delete_chunk_keys;
    const uint64_t storage_limit = store->poolBackendPtr()->bulkDeleteKeyLimit();
    return std::clamp<size_t>(std::min(configured, storage_limit), 1, kBulkDeleteMaxKeys);
}

void Gc::sendIntentDeletes(CasOperation & op, const std::vector<WriteOnceKey> & keys)
{
    const size_t request_keys = bulkDeleteChunkKeys();
    for (size_t begin = 0; begin < keys.size(); begin += request_keys)
    {
        const std::vector<WriteOnceKey> chunk(keys.begin() + begin, keys.begin() + std::min(keys.size(), begin + request_keys));
        try
        {
            removeChunkWriteOnceOrOneByOne(op, chunk, Retry::standard());
        }
        catch (Exception & e)
        {
            /// A refusal carries the store's text only; the round's error must name what it failed to delete.
            e.addMessage("while deleting {} blob intents starting at '{}'", chunk.size(), chunk.front().str());
            throw;
        }
    }
}

Gc::RedeleteIo Gc::performRedeleteIo(const BodyDelete & body_delete, const Layout & layout, CasOperation & op)
{
    RedeleteIo io;
    io.blob_key = layout.blobKey(body_delete.entry.ref);
    /// Conditional on the raw token the fold's `HEAD` saw: a body replaced since then is refused by the store.
    io.del = op.remove(io.blob_key, body_delete.observed->etag, Retry::standard());
    return io;
}

void Gc::applyRedeleteOutcome(RedeleteRoundContext & ctx, const RetiredEntry & entry, const RedeleteIo & io)
{
    if (const auto & hook = store->poolConfig().gc_redelete_apply_hook_for_test)
        hook(entry.ref);
    const OutcomeKind outcome_kind = io.del == Removal::Removed ? OutcomeKind::Deleted
        : io.del == Removal::Gone                               ? OutcomeKind::Absent
                                                                : OutcomeKind::Replaced;
    OutcomeEntry outcome{.kind = entry.kind, .ref = entry.ref, .token = entry.token, .outcome = outcome_kind};
    const String del_outcome{removalName(io.del)};
    /// The single content-delete site is attributable per row. A mismatch (a writer recreated
    /// the incarnation) is terminal-OK: the fresh incarnation is a live object.
    EventEmitter{*store}.emit(
        [&](CasEvent & e)
        {
            e.type = CasEventType::BlobDelete;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = blobIdOf(entry.ref);
            e.token = renderIncarnation(entry.token);
            e.round = ctx.new_round;
            e.gen = ctx.snap_generation;
            e.outcome = del_outcome;
            e.reason = "pending row at zero whose HEAD showed its incarnation; exact-incarnation delete (pre-CAS)";
            e.detail = {{"condemn_round", std::to_string(entry.condemn_round)}, {"key", io.blob_key}};
        });
    /// The audit row is observability only -- the delete in `performRedeleteIo` already executed
    /// regardless of this cap. Skipping it here bounds the per-shard `GcOutcomes` body without
    /// skipping or deferring any destructive work.
    ctx.outcome_tallies[ctx.shard].add(outcome_kind);
    if (ctx.round_work_budget.admitOutcomeRow(ctx.shard, outcome))
        ctx.outcomes[ctx.shard].entries.push_back(std::move(outcome));
    ++ctx.report.bodies_deleted;
    countRetired(RetiredCount::BodyDeleted);
}

void Gc::reportRedeleteFailure(
    RedeleteRoundContext & ctx,
    const RetiredEntry & entry,
    const Layout & layout,
    std::exception_ptr exception,
    std::optional<size_t> unsent_count)
{
    countRetired(RetiredCount::BodyDeleteFailed);
    ++ctx.report.body_delete_failed;
    LOG_WARNING(logger,
        "CAS gc: pending delete of blob {} (key `{}`, condemned at round {}) failed; the round fails before its commit "
        "and the row stays pending: {}{}",
        blobIdOf(entry.ref), layout.blobKey(entry.ref), entry.condemn_round, getExceptionMessage(exception, false),
        unsent_count ? fmt::format("; {} re-delete entries were not submitted", *unsent_count) : String{});
}

void Gc::redeleteBlobs(
    RedeleteRoundContext & ctx,
    const std::vector<BodyDelete> & entries,
    ThreadPool & pool,
    size_t concurrency,
    const Layout & layout,
    CasOperation & op)
{
    std::vector<RedeleteIo> io_results(entries.size());
    std::vector<std::exception_ptr> io_errors(entries.size());
    std::optional<IndexedException> schedule_error;
    size_t scheduled = 0;
    if (concurrency <= 1 || entries.size() <= 1)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            try
            {
                io_results[i] = performRedeleteIo(entries[i], layout, op);
            }
            catch (...)
            {
                io_errors[i] = std::current_exception();
            }
        }
        scheduled = entries.size();
    }
    else
    {
        ThreadPoolCallbackRunnerLocal<void> runner(pool, ThreadName::CAS_GC_REDELETE);
        std::vector<std::shared_ptr<ThreadPoolCallbackRunnerLocal<void>::Task>> handles;
        handles.reserve(entries.size());
        SCOPE_EXIT_SAFE({ ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles); });

        const uint64_t admitted_generation = op.generation();
        try
        {
            for (; scheduled < entries.size(); ++scheduled)
            {
                if (io_pool_refuse_at_for_test == scheduled)
                {
                    io_pool_refuse_at_for_test.reset();
                    throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "Injected CAS GC re-delete enqueue refusal at index {}", scheduled);
                }
                handles.emplace_back(runner.enqueueAndGiveOwnership(
                    [slot = &io_results[scheduled],
                     body_delete = entries[scheduled],
                     layout_ptr = &layout,
                     pool_store = store,
                     admitted_generation]
                    {
                        CasOperation job_op = pool_store->openRequests().resume(admitted_generation);
                        *slot = performRedeleteIo(body_delete, *layout_ptr, job_op);
                    }));
            }
        }
        catch (...)
        {
            schedule_error = IndexedException{scheduled, std::current_exception()};
        }
        ctx.jobs_scheduled += handles.size();

        ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles);
        for (size_t i = 0; i < handles.size(); ++i)
        {
            try
            {
                handles[i]->future.get();
            }
            catch (...)
            {
                io_errors[i] = std::current_exception();
            }
        }
    }

    std::optional<IndexedException> first_error;
    for (size_t i = 0; i < scheduled; ++i)
    {
        if (!io_errors[i])
            continue;
        reportRedeleteFailure(ctx, entries[i].entry, layout, io_errors[i]);
        if (!first_error)
            first_error = IndexedException{i, io_errors[i]};
    }
    if (schedule_error)
    {
        reportRedeleteFailure(
            ctx, entries[schedule_error->index].entry, layout, schedule_error->exception, entries.size() - schedule_error->index);
        if (!first_error || schedule_error->index < first_error->index)
            first_error = schedule_error;
    }

    /// Every submitted delete of the batch has already run. If `applyRedeleteOutcome` throws, the batch stays deleted
    /// but unrecorded: the round fails before its outcome logs and `gc/state` commit, every entry stays
    /// `delete_pending`, and the next round records the deleted ones as `Absent`.
    for (size_t i = 0; i < scheduled; ++i)
    {
        if (!io_errors[i])
            applyRedeleteOutcome(ctx, entries[i].entry, io_results[i]);
    }

    if (first_error)
        std::rethrow_exception(first_error->exception);
}

RoundReport Gc::runRegularRound(std::function<void()> on_lease_acquired, bool allow_steal, UniversePolicy policy,
                                RoundReport * progress)
{
    RoundReport local_report;
    RoundReport & report = progress ? *progress : local_report;
    report = RoundReport{};
    GcState state;
    std::optional<Etag> state_etag;

    /// Phase `lease`. Also the ONLY phase a `NotALeader` round emits, which is why the phase rows
    /// are correlated by `round_id` and not by the round number a follower never learns.
    {
        GcPhaseTimer t(phase_sink, "lease");
        report.acquired_lease = acquireOrRenewLease(state, state_etag, allow_steal);
        t.metric("acquired", report.acquired_lease ? 1 : 0);
        t.metric("steal_allowed", allow_steal ? 1 : 0);
    }
    if (!report.acquired_lease)
        return report;

    /// Fire the acquire-time hook BEFORE the long fold below, not after the round
    /// returns - a new leader's first round could otherwise run for the whole fold with no
    /// heartbeat cover, letting a follower steal deterministically once it freezes (owner, seq)
    /// across two of its own ticks.
    if (on_lease_acquired)
        on_lease_acquired();

    /// ONE-PASS round. There is no crash-resume step anymore: the round commits everything in the
    /// SINGLE gc/state CAS at the end, so a crashed pass leaves only attempt-scoped debris that is
    /// never adopted (retention prunes it), and every destructive PRE-CAS action below is justified by
    /// PREVIOUSLY PUBLISHED durable state only (delete_pending entries), so replay under a fresh
    /// attempt is idempotent.

    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    const uint64_t new_round = state.round + 1;

    /// ONE budget instance for the WHOLE round, threaded into every destructive-or-observability-write
    /// family below: `fold` (blob intent creates and body deletes, the `GcOutcomes` audit rows, the orphan-manifest
    /// planner), `pruneSupersededGenerations`, the post-CAS hand-off reclaim (its OWN reserve, never
    /// `pruneSupersededGenerations`' shared remainder), the post-CAS manifest-body cleanup, and
    /// `cleanupRefObjects`. A cap is therefore cumulative over the round, never reset between families or
    /// shards, except the outcome log byte limit, which is per shard. See `GcRoundWorkBudget`'s own
    /// comment for the fail-closed contract each family applies on exhaustion.
    GcRoundWorkBudget round_work_budget;
    round_work_budget.intent_creates.cap = store->poolConfig().gc_round_intent_create_budget;
    round_work_budget.intent_creates.resume_after = creates_resume_after_;
    round_work_budget.redeletes.cap = store->poolConfig().gc_round_redelete_budget;
    round_work_budget.redeletes.resume_after = redeletes_resume_after_;
    round_work_budget.max_sweep_namespaces = store->poolConfig().gc_round_sweep_namespace_budget;
    round_work_budget.max_sweep_recovery_ops = store->poolConfig().gc_round_sweep_recovery_op_budget;
    round_work_budget.max_ref_cleanup_objects = store->poolConfig().gc_round_ref_cleanup_budget;
    round_work_budget.max_prefix_wholesale_objects = store->poolConfig().gc_round_prefix_wholesale_budget;
    round_work_budget.max_handoff_prefix_wholesale_objects = store->poolConfig().gc_round_handoff_prefix_wholesale_budget;
    round_work_budget.max_outcome_entries = store->poolConfig().gc_round_outcome_entry_budget;
    if (const auto limit = store->poolConfig().gc_outcome_log_byte_limit_for_test)
        round_work_budget.max_outcome_log_bytes = *limit;

    /// The helping barrier precedes heartbeat work, DEFER, the hot stream LIST, and every successor
    /// artifact. A deferred invocation therefore cannot leave a row the adopted parent already proved
    /// complete, and a folding invocation takes its catalog cut only after the deletion settles.
    {
        GcPhaseTimer t(phase_sink, "pre_fold_ref_drain");
        uint64_t completed_erases = 0;
        uint64_t cancelled_creating = 0;
        /// Declared after `t`, so it runs before the timer emits its record, on a throw too.
        SCOPE_EXIT({
            t.metric("deleted", completed_erases);
            t.metric("cancelled_creating", cancelled_creating);
        });
        const CatalogLifecycleReconcileResult drain_result
            = drainCompletedRemoving(state, state_etag, completed_erases, cancelled_creating);
        if (drain_result.authority_status != AuthorityStatus::Authoritative
            || drain_result.catalog_resolution != CatalogResolution::DrainComplete)
            throwCasRequestRetryLater("CAS GC pre-fold drain lost authority before the catalog settled");
    }

    /// Etag-guarded fence-out of dead mounts (liveness only — row decisions pace on GC
    /// rounds via `new_round`, not on heartbeat acks). A slot is fenced only after `mount_obs` has
    /// watched its token hold unchanged on this leader's own monotonic clock for
    /// `mountObservationThresholdMs` over the longer of this server's lease and the one the slot
    /// declares, with the full renewal period as the step.
    const uint64_t ttl_ms = static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count());
    const uint64_t renew_period_ms = static_cast<uint64_t>(store->poolConfig().mount_renew_period.count());

    /// Phase `heartbeat_floor`: one LIST of `gc/server-roots/`, one GET per mount slot, and a fence
    /// PUT per newly-fenced mount.
    {
        GcPhaseTimer t(phase_sink, "heartbeat_floor");
        const HeartbeatFloor floor = computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn,
                                                           ttl_ms, renew_period_ms, mount_obs, lease_mismatch_warned);
        report.fence_outs = floor.fenced_now;
        if (floor.fenced_now > 0)
            ProfileEvents::increment(ProfileEvents::CASGCHeartbeatFenceOuts, floor.fenced_now);

        /// GcFenceOut audit row per expired mount fenced-out this round: the round latched a fence-out to
        /// re-arm a sleeper's write fence (its held token is now invalid). One row per srid so the log
        /// reconstructs which mount was reclaimed.
        for (const String & srid : floor.fenced_srids)
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcFenceOut;
                e.object_kind = CasEventObjectKind::Snap;
                e.round = new_round;
                e.gen = state.snap_generation;
                e.outcome = "fenced";
                e.reason = "expired mount lease past skew margin; token-guarded fence-out re-arms the write "
                           "fence (prevents a resumed sleeper from mutating)";
                e.detail = {{"server_root_id", srid}};
            });

        /// Emit the round's heartbeat classification (what mounts are live/terminated/fenced this round).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcFence;
            e.object_kind = CasEventObjectKind::Snap;
            e.round = new_round;
            e.gen = state.snap_generation;
            e.outcome = "floor";
            e.reason = "R1: heartbeat classification (live/terminated/fenced mounts)";
            e.detail = {{"live", std::to_string(floor.live)},
                        {"terminated", std::to_string(floor.terminated)},
                        {"fenced_now", std::to_string(floor.fenced_now)},
                        {"already_fenced", std::to_string(floor.already_fenced)}};
        });

        t.metric("live", floor.live);
        t.metric("terminated", floor.terminated);
        t.metric("fenced_now", floor.fenced_now);
        t.metric("already_fenced", floor.already_fenced);
    }

    /// Decide DEFER vs FOLD from cheap pre-fold signals.
    /// A DEFER round re-adopts the sealed generation — no fold, no delete, no gc/state write — so a
    /// slow idle/small-delta round no longer rebuilds the whole in-degree snapshot. Safety: a due
    /// row decision forces a FOLD (rowDecisionDue), so no destructive decision runs on a stale snapshot.
    ///
    /// `listRefPrefix` is the round's one full enumeration of `cas/ns/stream/`. Its result is retained
    /// (rather than discarded once the defer decision is taken) because `fold` regroups the very same
    /// keys instead of listing the prefix again. A deferred round simply drops it.
    ///
    /// Phase `defer_decision`. `ref_scan` OUTLIVES the timer because the fold consumes it, and
    /// `report.deferred` is set INSIDE the scope so the row already reflects the verdict when the
    /// timer's destructor fires on the deferred round's early return. This phase also performs TWO of
    /// the round's reads of the adopted fold seal (`rowDecisionDue` and `listRefPrefix` each read the
    /// same key) -- see `fold_seal_reads` below.
    std::optional<RefPlan> walk_plan;
    bool defer_round = false;
    {
        GcPhaseTimer t(phase_sink, "defer_decision");
        const bool row_decision_due = rowDecisionDue(state, new_round);
        walk_plan.emplace(buildRefWalkPlan(listRefPrefix(state, state_etag)));
        reportStuckRemovals(*walk_plan, state.round);
        const RefScanSummary & ref_scan = walk_plan->refScan();
        const size_t changed = walk_plan->changedRows();
        defer_round = shouldDeferRound(changed, row_decision_due, rounds_since_last_fold_,
                                       store->poolConfig().gc_fold_threshold,
                                       store->poolConfig().gc_fold_max_defer_rounds);
        uint64_t ref_log_keys = 0;
        for (const auto & [scanned_life, ids] : ref_scan.logs_by_life)
            ref_log_keys += ids.size();
        t.metric("changed_shards", changed);
        t.metric("namespaces_seen", ref_scan.max_log_by_life.size());
        t.metric("ref_log_keys_listed", ref_log_keys);
        t.metric("ref_keys_listed", ref_scan.keys.size());
        t.metric("row_decision_due", row_decision_due ? 1 : 0);
        t.metric("dead_life_debris", ref_scan.dead_life_debris);
        t.metric("walk_plan_builds", 1);
        t.metric("walk_plan_rows", walk_plan->size());
        t.metric("walk_plan_dropped_parent_rows", walk_plan->droppedParentRows());
        t.metric("walk_plan_dropped_listed_lives", walk_plan->droppedListedLives());
        t.metric("walk_plan_dropped_tails", walk_plan->droppedTails());
        t.metric("deferred", defer_round ? 1 : 0);
        /// The number of consecutive rounds already deferred BEFORE this one (this round's own verdict is
        /// `deferred` above), so the pair reads unambiguously against `gc_fold_max_defer_rounds`.
        t.metric("rounds_deferred_before", rounds_since_last_fold_);
        /// `rowDecisionDue` and `listRefPrefix` each GET the adopted fold seal at the SAME
        /// (generation, attempt). Recorded, not fixed -- see the `fold_seal_read` phase, which records
        /// the other duplicate pair; the round GETs that one key FIVE times on a folding round.
        t.metric("fold_seal_reads", 2);

        if (defer_round)
        {
            ++rounds_since_last_fold_;
            report.deferred = true;
            /// A DEFER round mints no new round -- unlike the fold path below, which sets
            /// `report.round = state.round` only AFTER the round's single `gc/state` CAS has committed
            /// `next.round = new_round` and `state` was reassigned to that committed `next` (so on that
            /// path `state.round` reads the FRESH round number). Here the round CAS never runs, so `state`
            /// is still the round that was already adopted BEFORE this round started: `state.round` is the
            /// honest, already-durable round number, while `new_round` (`state.round + 1`) would report a
            /// round that never actually happened. Use `state.round` so `RoundReport::round` and the
            /// `system.cas_gc_log` row it feeds never print a fabricated
            /// round number on a deferred round.
            report.round = state.round;
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcFence;   /// reuse the Snap round-event channel; outcome = "deferred"
                e.object_kind = CasEventObjectKind::Snap;
                e.round = state.round;
                e.gen = state.snap_generation;
                e.outcome = "deferred";
                e.reason = "skip-unchanged: no changed shard reached the fold threshold and no row decision "
                           "is due; re-adopting the sealed generation (snapshot rebuild elided)";
                e.detail = {{"changed_shards", std::to_string(changed)},
                            {"rounds_since_last_fold", std::to_string(rounds_since_last_fold_)}};
            });
            /// Return after the timer scope so the independently timed janitor phase is not nested
            /// inside `defer_decision`.
        }
        else
            rounds_since_last_fold_ = 0;   /// this round folds
    }

    if (defer_round)
    {
        /// DEFER has no `FoldResult`, hence no complete global destructive verdict. The janitor still
        /// takes its bounded page and catalog cut, but suppression keeps both deletes and valid-page
        /// cursor progress at the same position for the bounded forced fold to retry.
        runNamespaceJanitor(state, /*suppress_destructive=*/true, /*cleanup_evidence_rows=*/0);
        return report;   /// no fold, no pre-CAS deletes, no gc/state CAS — sealed generation stays pinned
    }

    /// Emit that the round's single pass begins.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldBegin;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = new_round;
        e.gen = state.snap_generation;
        e.reason = "R2: one-pass fold (edges x deltas x retired) into a new durable generation";
    });

    /// Capture the PARENT seal's run refs BEFORE fold mutates
    /// `state.snap_generation`/`snap_attempt` in-memory. We compare these against the
    /// NEW seal's refs post-CAS to detect a ref that moved OFF an already-pruned generation (the
    /// wholesale prune skipped it while it was still referenced and its cursor advanced past it), and
    /// hand-off delete that generation's now-unreferenced leftover. Absent parent seal => empty.
    ///
    /// Phase `parent_seal_read`: the round's THIRD GET of the adopted fold seal (`rowDecisionDue`
    /// and `listRefPrefix` already read it in `defer_decision`, and `fold` reads it twice more). One
    /// small GET, given its own row rather than left untimed, because "the same key, five times a round"
    /// is only actionable if each read is attributable to a phase.
    std::vector<RunRef> parent_seal_runs;
    {
        GcPhaseTimer t(phase_sink, "parent_seal_read");
        if (const auto parent_seal = readFoldSeal(state.snap_generation, state.snap_attempt))
            parent_seal_runs = parent_seal->blob_target_runs;
        t.metric("parent_runs", parent_seal_runs.size());
    }

    /// The pass performs discovery, windowing, and the three-cursor merge (spare / confirm intent / condemn).
    /// It emits phases 5..10 of its own.
    FoldResult folded = fold(state, state_etag, report, new_round, *walk_plan, policy, round_work_budget);

    /// THE ROUND'S DESTRUCTIVE GATE, read once, here, and consulted at EVERY destructive site below.
    /// It is available this early because `fold` computes it (see `FoldResult::suppress_destructive`),
    /// and it has to be: the first destructive site of the post-CAS tail is the hand-off reclaim, which
    /// used to run before this value was ever read.
    const bool suppress_destructive = folded.suppress_destructive;

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldEnd;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = new_round;
        e.gen = state.snap_generation;
        e.outcome = "ok";
        e.reason = "R2 complete";
        e.detail = {{"shards", std::to_string(folded.root_shards.size())},
                    {"anomalies", std::to_string(report.anomalies.size())}};
    });

    const uint64_t generation = state.snap_generation;   /// set in-memory by fold; committed below
    const uint64_t attempt = state.snap_attempt;

    /// PRE-CAS deletes: the body deletes of `pending` rows read from the adopted state, then the intent deletes
    /// of incarnations observed gone, plus outcome bookkeeping for every settled entry. THE SINGLE
    /// CONTENT-DELETE SITE. Any failed delete fails the round before its commit, so every row keeps its state.
    ///
    /// Phase `pending_deletes`, covering both the exact-token delete loop and the outcome-log
    /// writes it feeds. Held in an `optional` rather than a `{ }` scope purely so this long, delicate
    /// block is not reindented wholesale; `reset()` below is what emits the row, and an exception
    /// escaping before it still emits from the destructor.
    std::optional<GcPhaseTimer> pending_deletes_timer;
    pending_deletes_timer.emplace(phase_sink, "pending_deletes");
    const uint64_t bodies_deleted_before = report.bodies_deleted;
    const uint64_t intent_confirmed_before = report.intent_confirmed;
    const uint64_t body_delete_failed_before = report.body_delete_failed;
    uint64_t redelete_jobs_scheduled = 0;
    const auto record_redelete_job_metrics = [&]
    {
        pending_deletes_timer->metric("jobs_scheduled", redelete_jobs_scheduled);
        pending_deletes_timer->metric("jobs_failed", report.body_delete_failed - body_delete_failed_before);
    };
    SCOPE_EXIT_SAFE({
        if (pending_deletes_timer)
            record_redelete_job_metrics();
    });
    /// Called inside each emit callback, so a disabled event formats neither the hash nor the token.
    const auto describeRow = [&](CasEvent & e, CasEventType type, const RetiredEntry & entry)
    {
        e.type = type;
        e.object_kind = CasEventObjectKind::Blob;
        e.object_hash = blobIdOf(entry.ref);
        e.token = renderIncarnation(entry.token);
        e.round = new_round;
        e.gen = generation;
    };
    std::map<uint64_t, OutcomeLog> outcomes;
    std::map<uint64_t, OutcomeTally> outcome_tallies;
    for (uint64_t shard = 0; shard < folded.retired_merge.size(); ++shard)
    {
        RetiredMergeResult & merge = folded.retired_merge[shard];
        /// The gate, stated at the site, and scoped to the deletes alone. The merge decides no delete in a
        /// suppressed round; if the two ever disagree, the round deletes nothing rather than deleting on a
        /// frontier it cannot prove.
        static const std::vector<BodyDelete> kNothingToDelete;
        const std::vector<BodyDelete> & redelete_now =
            suppress_destructive ? kNothingToDelete : merge.body_deletes;
        RedeleteRoundContext redelete_ctx{
            .new_round = new_round,
            .snap_generation = generation,
            .shard = shard,
            .round_work_budget = round_work_budget,
            .report = report,
            .outcomes = outcomes,
            .outcome_tallies = outcome_tallies,
            .jobs_scheduled = redelete_jobs_scheduled};
        redeleteBlobs(redelete_ctx, redelete_now, *io_pool, store->poolConfig().gc_io_concurrency, layout, op);
        for (const RetiredEntry & entry : merge.to_standing)
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                describeRow(e, CasEventType::GcRecheckVerdict, entry);
                e.outcome = "standing";
                e.reason = "pending row at zero: the body delete was answered or the body is absent; the intent stays "
                           "until a later cut at zero";
                e.detail = {{"condemn_round", std::to_string(entry.condemn_round)}};
            });
        for (const RetiredEntry & entry : merge.adopted)
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                describeRow(e, CasEventType::BlobIntentAdopt, entry);
                e.outcome = entry.standing ? "standing" : "candidate";
                e.reason = "a listed deletion intent had no row; the body's HEAD decided the row";
                if (const auto identity = entry.token.canonical())
                    e.detail = {{"key", layout.blobIntentKey(entry.ref, *identity)}};
            });
        for (const RetiredEntry & entry : merge.spared)
        {
            /// Emit the spare verdict: a reference reached the blob again, so the row is dropped and its intent stays.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                describeRow(e, CasEventType::GcRecheckVerdict, entry);
                e.outcome = "spared";
                e.reason = "in-degree recovered in the pass merge; entry dropped";
            });
            /// The audit row is observability only -- `settleEntry` already unconditionally spared this
            /// entry (recovery wins past any budget), so this cap can never re-condemn it;
            /// it only bounds whether the decision gets a `GcOutcomes` row.
            OutcomeEntry spared_row{.kind = entry.kind, .ref = entry.ref,
                                    .token = entry.token, .outcome = OutcomeKind::Spared};
            outcome_tallies[shard].add(OutcomeKind::Spared);
            if (round_work_budget.admitOutcomeRow(shard, spared_row))
                outcomes[shard].entries.push_back(std::move(spared_row));
            ProfileEvents::increment(ProfileEvents::CASGCRetiredSpared);
        }
        for (const RetiredEntry & entry : merge.confirmed)
        {
            ++report.intent_confirmed;
            countRetired(RetiredCount::IntentConfirmed);
            /// The intent is confirmed; a later round whose cut shows zero deletes the body.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                describeRow(e, CasEventType::GcRecheckVerdict, entry);
                e.outcome = "pending";
                e.reason = "deletion intent confirmed; pending until a later cut shows in-degree 0";
                e.detail = {{"condemn_round", std::to_string(entry.condemn_round)}};
            });
        }
        for (const ReplacedEntry & replaced : merge.replaced)
        {
            const RetiredEntry & entry = merge.still_retired[replaced.row];
            ProfileEvents::increment(ProfileEvents::CASGCRetireReplaced);
            /// RESURRECT-REUPLOAD-ORPHAN: the current object token differed from a stale retired entry;
            /// the fold superseded that entry and re-condemned the current token in the same window.
            /// `detail["superseded_token"]` carries the STALE token the supersede
            /// dropped — `entry.token` above is only the fresh CURRENT token, and without the old one
            /// this event cannot tell an operator WHICH incarnation was replaced.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                describeRow(e, CasEventType::BlobRetireReplaced, entry);
                e.outcome = "replaced";
                e.reason = "current object token differs from the row's token: a republication replaced the "
                           "incarnation; the row moved to the current token";
                e.detail = {{"superseded_token", renderIncarnation(replaced.old_token)}};
            });
        }
    }

    /// After every body delete answered: an intent removal never shares a job with the delete of its body.
    {
        std::vector<WriteOnceKey> intent_deletes;
        CountWithExample unconfirmed_creates;
        CountWithExample rejected_tokens;
        CountWithExample standing_anomalies;
        CountWithExample undecodable_bodies;
        CountWithExample undecodable_spares;
        for (const RetiredMergeResult & merge : folded.retired_merge)
            intent_deletes.insert(intent_deletes.end(), merge.intent_deletes.begin(), merge.intent_deletes.end());
        /// The spares' deletes share the batch and are the tail of it from here on.
        const size_t first_spare_delete = intent_deletes.size();
        for (const RetiredMergeResult & merge : folded.retired_merge)
            intent_deletes.insert(intent_deletes.end(), merge.spare_intent_deletes.begin(), merge.spare_intent_deletes.end());
        for (const RetiredMergeResult & merge : folded.retired_merge)
        {
            unconfirmed_creates.merge(merge.unconfirmed_creates);
            rejected_tokens.merge(merge.rejected_tokens);
            standing_anomalies.merge(merge.standing_anomalies);
            undecodable_bodies.merge(merge.undecodable_bodies);
            undecodable_spares.merge(merge.undecodable_spares);
        }
        if (!suppress_destructive && !intent_deletes.empty())
        {
            sendIntentDeletes(op, intent_deletes);
            ProfileEvents::increment(ProfileEvents::CASGCIntentDeleted, intent_deletes.size());
            ProfileEvents::increment(ProfileEvents::CASGCIntentDeletedOnSpare, intent_deletes.size() - first_spare_delete);
            for (size_t i = 0; i < intent_deletes.size(); ++i)
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    const WriteOnceKey & key = intent_deletes[i];
                    e.type = CasEventType::BlobIntentDelete;
                    e.object_kind = CasEventObjectKind::Blob;
                    if (const auto parsed = layout.parseBlobIntentKey(key.str()))
                    {
                        e.object_hash = blobIdOf(parsed->ref);
                        if (const auto tag = opaqueTagFromHex(parsed->identity.dialect, parsed->identity.tag_hex))
                            e.token = String(dialectWord(parsed->identity.dialect)) + ":" + *tag;
                    }
                    e.round = new_round;
                    e.gen = generation;
                    e.outcome = "deleted";
                    e.reason = i >= first_spare_delete
                        ? "the blob is referenced again and its body has another incarnation than the intent names; the "
                          "deletion intent is removed"
                        : "the incarnation the intent names is gone from the key; the deletion intent is removed";
                    e.detail = {{"key", key.str()}};
                });
        }
        pending_deletes_timer->metric("intents_deleted", suppress_destructive ? 0 : intent_deletes.size());
        if (unconfirmed_creates.count)
            LOG_WARNING(logger,
                "CAS gc: {} deletion-intent creates were not confirmed this round; those blobs stay candidates and "
                "are retried in a later round. First: {}", unconfirmed_creates.count, unconfirmed_creates.example);
        if (rejected_tokens.count)
            LOG_ERROR(logger,
                "CAS gc: {} blob observations or rows this round carry a token that names no incarnation; GC never "
                "deletes such a body. First: `{}`", rejected_tokens.count, rejected_tokens.example);
        if (standing_anomalies.count)
            LOG_ERROR(logger,
                "CAS gc: {} standing rows observed their own token again after their body delete; carried, nothing "
                "deleted. First: `{}`", standing_anomalies.count, standing_anomalies.example);
        if (undecodable_bodies.count)
            LOG_ERROR(logger,
                "CAS gc: {} blob bodies this round are shorter than the pool's blob envelope; GC recorded no row for "
                "them, carried their rows and deleted nothing. First: `{}`", undecodable_bodies.count, undecodable_bodies.example);
        if (undecodable_spares.count)
            LOG_ERROR(logger,
                "CAS gc: {} blob bodies of spared rows this round are shorter than the pool's blob envelope; GC dropped "
                "their rows, kept their intents and deleted nothing. First: `{}`", undecodable_spares.count,
                undecodable_spares.example);
    }

    /// Outcome logs: write-once + byte-adopt (observation-bearing HEAD tokens — never the
    /// deterministic-artifact path). The report counts this round's decisions, except for a shard whose
    /// log an earlier attempt of the same round already wrote: that durable log is adopted and counted.
    uint64_t outcome_rows_recorded = 0;
    uint64_t outcome_decisions = 0;
    for (const auto & [shard, log] : outcomes)
        outcome_rows_recorded += log.entries.size();
    for (const auto & [shard, tally] : outcome_tallies)
        outcome_decisions += tally.total();
    for (auto & [shard, log] : outcomes)
    {
        const String key = layout.outcomesKey(generation, attempt, new_round, shard);
        if (const auto ledger = round_work_budget.outcome_log_ledgers.find(shard);
            ledger != round_work_budget.outcome_log_ledgers.end() && ledger->second.dropped > 0)
            LOG_WARNING(logger,
                "CAS gc: the outcome log of shard {} reached its {}-byte limit; {} audit rows were not recorded "
                "(the deletes and spares they describe happened)",
                shard, round_work_budget.max_outcome_log_bytes, ledger->second.dropped);
        const String body = sealObject(FormatId::GcOutcomes, encodeOutcomeLog(log));
        WriteResult written = op.create(key, body, Retry::standard());
        if (const auto * conflict = std::get_if<Conflict>(&written))
        {
            /// The conflict's observation IS the read that used to follow the refused create. Only
            /// something that read actually OBSERVED can support a verdict about the key; a resolve
            /// read that settled nothing says nothing about whether the object is there.
            if (std::holds_alternative<NotObserved>(conflict->seen))
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc: the create of the outcome log at {} was refused and its resolve read "
                    "observed nothing, so what the key holds is unknown", key);
            const auto * existing = std::get_if<Object>(&conflict->seen);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc: outcome log at {} refused the create and its resolve read observed {}",
                    key, detail::renderObservation(conflict->seen));
            if (existing->bytes != body)
            {
                try { log = decodeOutcomeLog(openObject(FormatId::GcOutcomes, existing->bytes)); }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc: undecodable outcome log at {} cannot be adopted: {}", key, e.message());
                }
                OutcomeTally adopted;
                for (const OutcomeEntry & o : log.entries)
                    adopted.add(o.outcome);
                outcome_tallies[shard] = adopted;
            }
        }
        else
            orThrow(std::move(written), fmt::format("CAS gc: outcome log at {}", key));
    }
    for (const auto & [shard, tally] : outcome_tallies)
    {
        report.deleted += tally.deleted;
        report.absent += tally.absent;
        report.replaced += tally.replaced;
        report.spared += tally.spared;
    }
    pending_deletes_timer->metric("bodies_deleted", report.bodies_deleted - bodies_deleted_before);
    pending_deletes_timer->metric("intents_confirmed", report.intent_confirmed - intent_confirmed_before);
    pending_deletes_timer->metric("deleted", report.deleted);
    pending_deletes_timer->metric("absent", report.absent);
    pending_deletes_timer->metric("replaced", report.replaced);
    pending_deletes_timer->metric("spared", report.spared);
    pending_deletes_timer->metric("outcome_logs_written", outcomes.size());
    pending_deletes_timer->metric("outcome_rows_dropped", outcome_decisions - outcome_rows_recorded);
    record_redelete_job_metrics();
    pending_deletes_timer.reset();   /// emits the `pending_deletes` row

    /// Retired-in-snapshot — there is NO separate retired-list object to publish anymore. The
    /// round's surviving condemned entries were already sealed as `RunMarker::Condemned` rows inside the fold's
    /// `blob_target_runs` (durable before this CAS, via `putDeterministicArtifact`), and the per-shard
    /// `condemned_summary` the seal carries makes the next round's row-decision/carry decisions zero-I/O.

    /// The SINGLE round CAS publishes the round, adopted (generation, attempt), and retention cursor.
    ///
    /// Phase `round_commit`. It deliberately covers BOTH the retention prune (heavy: LISTs and
    /// wholesale deletes, bounded at 64 generations a round) and the single `gc/state` CAS (trivial),
    /// because the prune's writes are only safe as a pre-CAS action and splitting the two would suggest
    /// they are independently retryable. `generations_visited` vs the prune's actual deletes is what
    /// separates the two costs inside the row.
    std::optional<GcPhaseTimer> round_commit_timer;
    round_commit_timer.emplace(phase_sink, "round_commit");
    const String manifest_sweep_cursor_before = state.manifest_sweep_cursor;
    GcState next = state;
    next.round = new_round;
    if (!suppress_destructive && store->poolConfig().manifest_sweep_list_budget_keys > 0)
        next.manifest_sweep_cursor = folded.orphan_sweep.next_cursor;
    if (folded.intent_sweep_next_cursor)
        next.intent_sweep_cursor = *folded.intent_sweep_next_cursor;
    /// The generations the adopted seal's runs physically live in (reference-parent carry can point a
    /// current shard's run back at an older generation's key). Retention must never reclaim these.
    std::set<uint64_t> referenced_generations;
    for (const RunRef & r : folded.fold_seal.blob_target_runs)
        referenced_generations.insert(r.key_generation);
    /// ALSO protect every generation the PARENT (currently-adopted, pre-fold) seal references
    /// (`parent_seal_runs`, captured above): this prune runs BEFORE the round's own gc/state CAS below, so
    /// a losing leader must not destroy what the winning leader's already-adopted seal still points at —
    /// pre-CAS destructive actions may only rely on PREVIOUSLY PUBLISHED state.
    for (const RunRef & r : parent_seal_runs)
        referenced_generations.insert(r.key_generation);
    /// Retention floor uses THIS round's (post-fold) `generation`, so `gc_snapshot_generations_to_keep`
    /// keeps exactly that many generations back from the current one. If this round's `gc/state` CAS
    /// then LOSES, the prune reclaimed one generation deeper than the durably-adopted generation would
    /// imply -- an accepted forensics-window slack, not a data-loss risk: every still-reachable
    /// run/blob is independently protected via `referenced_generations` (captured pre-fold above).
    const uint64_t pruned_through_before = state.snap_pruned_through;
    pruneSupersededGenerations(generation, attempt, next, referenced_generations, suppress_destructive,
                               round_work_budget);
    round_commit_timer->metric("generations_visited", next.snap_pruned_through - pruned_through_before);
    round_commit_timer->metric("pruned_through", next.snap_pruned_through);
    round_commit_timer->metric("generations_referenced", referenced_generations.size());
    if (round_point_hook_for_test)
        runRoundPointHookForTest(RoundPointForTest::AfterRequestsBeforeCommit);
    WriteResult commit = op.replace(layout.gcStateKey(), encodeGcState(next), *state_etag,
                                    Retry::standard());
    if (const auto * conflict = std::get_if<Conflict>(&commit))
    {
        /// A refused precondition whose resolve read settled nothing proves only that this commit did
        /// not apply -- naming a competing leader would assert something nobody observed.
        if (std::holds_alternative<NotObserved>(conflict->seen))
            throw Exception(ErrorCodes::ABORTED,
                "CAS gc round: the gc/state commit was refused and its resolve read observed nothing; "
                "retry next round");
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc round: gc/state moved during the round (another leader advanced it, observed {}); "
            "retry next round", detail::renderObservation(conflict->seen));
    }
    state_etag = orThrow(std::move(commit), "CAS gc round commit");
    state = std::move(next);
    creates_resume_after_ = round_work_budget.intent_creates.nextResumeAfter();
    /// A suppressed round takes no redelete slot, so its empty budget would look like the end of the queue.
    if (!suppress_destructive)
        redeletes_resume_after_ = round_work_budget.redeletes.nextResumeAfter();
    report.round = state.round;
    round_commit_timer->metric("round", report.round);
    round_commit_timer->metric("generation", generation);
    round_commit_timer.reset();   /// emits the `round_commit` row

    /// The retire pipeline's REMAINING sizes, read from the seal this round's CAS just
    /// committed (`folded.fold_seal.condemned_summary` is TOTAL over every gc-shard -- see its own doc
    /// comment in `CasFoldSealFormat.h`). Zero-shard pools (never folded) leave these at 0.
    for (const auto & [shard, summary] : folded.fold_seal.condemned_summary)
    {
        report.pending_retired += summary.pending_total;
        report.pending_candidates += summary.condemned_total - summary.pending_total;
        report.pending_condemned += summary.condemned_total;
    }

    /// Post-CAS reference-parent HAND-OFF DELETE. `pruneSupersededGenerations` SKIPS a
    /// generation the live seal still references AND advances `snap_pruned_through` PAST it
    /// (`pruneSupersededGenerations` computes the cursor as `g - 1` after the loop increments `g` over every skipped
    /// generation). So once a skipped generation is behind the cursor, the wholesale prune NEVER revisits
    /// it — a ref that later moves off it would strand that generation's WHOLE prefix (fold seal, retired/
    /// outcomes sets, all shards' runs), not just the single carried run object. Reclaim it HERE, now that
    /// the ref has moved: for every parent ref whose generation is already pruned-through and whose
    /// generation NO new live ref still references, wholesale-delete that generation's prefix — the exact
    /// reclaimer the normal prune would have used, deferred until the ref finally moved off. Best-effort:
    /// a crash between the CAS and here leaks the prefix to fsck (single-crash window, no permanent leak —
    /// but note the cursor already advanced, so a plain retry will NOT re-attempt it; fsck is the backstop).
    ///
    /// Phase `handoff_reclaim`.
    {
        GcPhaseTimer t(phase_sink, "handoff_reclaim");
        uint64_t objects_reclaimed = 0;
        std::set<uint64_t> new_referenced_generations;
        for (const RunRef & r : folded.fold_seal.blob_target_runs)
            new_referenced_generations.insert(r.key_generation);

        std::set<uint64_t> handed_off;   /// dedupe: multiple parent refs can share one generation
        /// GATED like every other destructive site, and it is also the FIRST destructive site of the
        /// post-CAS tail -- which is why the gate is read before the tail begins rather than partway
        /// down it.
        ///
        /// UNLIKE EVERY OTHER GATED SITE, SUPPRESSION HERE LOSES THE WORK RATHER THAN POSTPONING IT.
        /// The hand-off is a one-shot DIFFERENCE between the parent seal's runs and the new seal's, and
        /// a suppressed round still FOLDS -- only the irreversible half stops -- so the ref moves off
        /// the old generation on this very round and the next round's parent seal no longer names it.
        /// Nothing revisits it: `snap_pruned_through` is already past that generation and the wholesale
        /// prune only walks forward. The prefix is left to `fsck`, which is the same outcome this site
        /// already documents for a crash in this window (see the `handoff_reclaim` comment above). Bounded --
        /// one small run per shard per occurrence of a suppressed round that also folded a delta -- and
        /// not a correctness problem, but it is the one place where the gate costs something permanent,
        /// so it is asserted rather than left to be discovered.
        static const std::vector<RunRef> kNoRuns;
        const std::vector<RunRef> & handoff_candidates =
            suppress_destructive ? kNoRuns : parent_seal_runs;
        for (const RunRef & old_ref : handoff_candidates)
        {
            /// Only generations the wholesale prune already passed AND that no live ref still pins.
            if (old_ref.key_generation > state.snap_pruned_through)
                continue;   /// not yet pruned-through: the normal prune will reclaim it when it ages out
            if (new_referenced_generations.contains(old_ref.key_generation))
                continue;   /// still referenced by a (possibly different-shard) live ref: keep it
            if (!handed_off.insert(old_ref.key_generation).second)
                continue;   /// already reclaimed this round via another shard's ref
            /// `bounded_remaining` draws from the hand-off's OWN reserve, never `UINT64_MAX` and never
            /// `pruneSupersededGenerations`' shared remainder: this hand-off is a ONE-SHOT event (see the
            /// `handoff_reclaim` comment above) -- a generation this call reclaims only PARTIALLY is left
            /// exactly like a crash in this window already is -- to `fsck`, never revisited by a later
            /// round's hand-off (the parent-seal difference that triggers it does not recur once the ref
            /// has moved). The prune, by contrast, safely retries an under-served generation next round
            /// via its cursor, so sharing one pool would let a prune-heavy round strand this one-shot
            /// reclaim at zero every time; the separate reserve makes that impossible.
            const uint64_t remaining = round_work_budget.handoffPrefixWholesaleRemaining();
            if (remaining == 0)
                break;
            const uint64_t reclaimed = deletePrefixWholesale(
                op, layout.gcGenPrefix(old_ref.key_generation), remaining);
            round_work_budget.handoff_prefix_wholesale_objects_used += reclaimed;
            objects_reclaimed += reclaimed;
            LOG_TRACE(logger,
                "CAS GC hand-off: generation {} moved out of the live seal below the retention cursor "
                "({} objects) — post-CAS wholesale reclaim (the prune had skipped it while referenced)",
                old_ref.key_generation, reclaimed);
        }
        t.metric("generations_reclaimed", handed_off.size());
        t.metric("objects_reclaimed", objects_reclaimed);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
    }

    /// Post-CAS: owner-removed manifest bodies — deleted ONLY now, after their decrements were
    /// ADOPTED by the round CAS (delete-after-sealed-decrements). NOT durable across rounds: the
    /// ref-log intake cursor that discovered each `-1` edge is committed by THIS round's CAS above,
    /// so a log already folded is never re-visited and never re-populates `mf_cleanup`. This phase is
    /// deliberately unbudgeted: a cap would leave a declined entry unreachable from any live ref AND
    /// never re-derived by this pipeline, converting a bounded burst into a permanent leak. It drains
    /// the whole of `folded.mf_cleanup` every round it runs; only a crash (or the destructive-suppression
    /// gate below) leaves an entry for the orphan-manifest sweep to reclaim later.
    /// The bodies go in batch requests of write-once keys; see the block.
    ///
    /// Phase `manifest_deletes`.
    {
        GcPhaseTimer t(phase_sink, "manifest_deletes");
        const uint64_t manifests_deleted_before = report.manifests_deleted;
        /// GATED. A manifest body is content the ref graph still describes until its decrements are both
        /// sealed AND taken on a round that could prove its frontier -- an unprovable round's `-1` may
        /// itself be the observation that is missing an owner elsewhere, so deleting the body on it is
        /// exactly the irreversible step the gate exists to withhold.
        static const std::map<ManifestId, Etag> kNoManifestCleanup;
        const std::map<ManifestId, Etag> & mf_cleanup_now =
            suppress_destructive ? kNoManifestCleanup : folded.mf_cleanup;

        /// Chunks of write-once keys, one request each, with no per-key precondition: a manifest key is
        /// never written twice, so the body at it is the one the fold observed or nothing. The engine
        /// reissues a failed chunk whole; a chunk that exhausts its policy throws here, and the chunks
        /// before it are already recorded below. A key that one of the exhausted chunk's own attempts
        /// did delete is not recorded either: deletion and recording are all-or-nothing per request,
        /// never per key, so the next round's fold sees that key as already gone. The etag the fold
        /// observed rides the event as information only.
        const size_t chunk_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        const size_t request_keys = bulkDeleteChunkKeys();
        uint64_t attempted = 0;
        uint64_t requests = 0;
        std::vector<WriteOnceKey> chunk;
        std::vector<const std::pair<const ManifestId, Etag> *> chunk_entries;
        const auto flush = [&]
        {
            if (chunk.empty())
                return;
            /// A backend without `DeleteObjects` (GCS) falls back to one admitted delete per key here;
            /// `chunk_entries`' per-key bookkeeping below is unaffected either way -- it counts objects
            /// that are gone after this call returns, not how many requests it took to get them there.
            requests += removeCohortWriteOnce(op, chunk, request_keys, Retry::standard());
            for (const auto * entry : chunk_entries)
            {
                ++report.manifests_deleted;
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    e.type = CasEventType::ManifestDelete;
                    e.namespace_ = entry->first.root_namespace.string();
                    e.object_kind = CasEventObjectKind::Manifest;
                    e.object_hash = manifestRefDebugString(entry->first.ref);
                    e.token = entry->second.render();
                    e.round = new_round;
                    e.gen = generation;
                    e.outcome = "deleted_or_absent";
                    e.reason = "owner-removed manifest body; batch delete of a write-once key after decrements adopted";
                });
            }
            chunk.clear();
            chunk_entries.clear();
        };
        for (const auto & entry : mf_cleanup_now)
        {
            ++attempted;
            chunk.push_back(layout.writeOnceManifestKey(entry.first));
            chunk_entries.push_back(&entry);
            if (chunk.size() >= chunk_keys)
                flush();
        }
        flush();
        t.metric("attempted", attempted);
        t.metric("accepted", report.manifests_deleted - manifests_deleted_before);
        t.metric("requests", requests);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
    }

    /// Removal completion has no physical pass. The terminal fold placed positive evidence in the
    /// life row; a later invocation's catalog-only pre-fold drain owns lifecycle deletion, while the
    /// perpetual janitor owns dead-life bytes.
    uint64_t cleanup_evidence_rows = 0;
    for (const auto & [life_id, ref_life_state] : folded.fold_seal.ref_lives)
        cleanup_evidence_rows += ref_life_state.cleanup_evidence ? 1 : 0;
    runNamespaceJanitor(state, suppress_destructive, cleanup_evidence_rows);
    /// Phase `ref_object_cleanup`. Emitted even when the whole pass is skipped (`trim_enabled` is
    /// a test seam, `suppressed` gates the deletes), because "this phase did nothing and why" is exactly
    /// what a reader of a round that reclaimed nothing needs to see.
    {
        GcPhaseTimer t(phase_sink, "ref_object_cleanup");
        RefCleanupPassStats cleanup_stats;
        /// Runs before `t` emits its row, also when a delete chunk throws: the pass has then already
        /// deleted objects that the row must account for.
        SCOPE_EXIT_SAFE({
            t.metric("suppressed", suppress_destructive ? 1 : 0);
            t.metric("trim_enabled", trim_enabled ? 1 : 0);
            t.metric("namespaces_planned", folded.ref_tables.size());
            t.metric("objects_planned", cleanup_stats.objects_planned);
            t.metric("objects_deleted", cleanup_stats.objects_deleted);
            t.metric("namespaces_skipped", cleanup_stats.namespaces_skipped);
            t.metric("stopped_lease_moved", cleanup_stats.stopped_lease_moved ? 1 : 0);
            t.metric("stopped_authority_unverified", cleanup_stats.stopped_authority_unverified ? 1 : 0);
            t.metric("chunks_under_catalog_change", cleanup_stats.chunks_under_catalog_change);
        });
        if (trim_enabled)
            cleanupRefObjects(folded, state.lease, suppress_destructive, round_work_budget, cleanup_stats);
    }

    /// Bounded orphan-manifest backstop. The fold already exact-read each candidate, retired its exact
    /// source edges into the adopted runs and placed cursor progress in the SAME `gc/state` CAS above.
    /// Only this post-CAS tail may delete candidate bodies.
    /// Phase `orphan_sweep`.
    {
        GcPhaseTimer t(phase_sink, "orphan_sweep");
        ManifestSweepResult & sweep = folded.orphan_sweep;
        for (const ManifestSweepResult::Nomination & nomination : sweep.nominations)
        {
            /// The nominated token was recorded from an earlier read, so the body is observed and compared by version
            /// before the removal names a precondition.
            const std::optional<Meta> observed = op.head(nomination.key, Retry::standard());
            Removal outcome = Removal::Gone;
            if (observed)
                outcome = sameVersion(nomination.token, observed->etag)
                    ? op.remove(nomination.key, observed->etag, Retry::standard())
                    : Removal::Mismatch;
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::ManifestDelete;
                e.namespace_ = nomination.id.root_namespace.string();
                e.object_kind = CasEventObjectKind::Manifest;
                e.object_hash = nomination.key;
                e.token = renderIncarnation(nomination.token);
                e.round = new_round;
                e.gen = generation;
                e.outcome = String{removalName(outcome)};
                e.reason = "orphan-manifest sweep: source edges retired and adopted before exact-incarnation delete";
            });
            if (outcome == Removal::Mismatch)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS orphan sweep: manifest key {} changed incarnation after the exact read; immutable "
                    "manifest identity suffered illegal ABA, retained replacement", nomination.key);
            if (outcome == Removal::Removed)
                ++sweep.deleted;
            else
                ++sweep.skipped;
        }
        reportSweepRetention(sweep);
        t.metric("cursor_advanced", state.manifest_sweep_cursor != manifest_sweep_cursor_before ? 1 : 0);
        t.metric("list_budget_keys", store->poolConfig().manifest_sweep_list_budget_keys);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
        t.metric("listed", sweep.listed);
        t.metric("floor_lookups", sweep.floor_lookups);
        t.metric("floor_reads", sweep.floor_reads);
        t.metric("deleted", sweep.deleted);
        t.metric("skipped", sweep.skipped);
        t.metric("undecodable", sweep.undecodable);
        /// THE DELETION PREMISE'S SHARE OF `skipped`, BY REASON CLASS. The premise holds only for a
        /// closed-and-folded epoch, so a pass in which everything examined was RETAINED is an ordinary
        /// outcome, and without these numbers it is indistinguishable on the row from a pass that found
        /// nothing to do.
        /// A row where `deleted` is 0 and all five are 0 means the sweep genuinely had no candidates.
        t.metric("retained_no_coverage", sweep.retained_no_coverage);
        t.metric("retained_hold", sweep.retained_hold);
        t.metric("retained_unconsumed_seal", sweep.retained_unconsumed_seal);
        t.metric("retained_tail_removal", sweep.retained_tail_removal);
        t.metric("retained_work_budget", sweep.retained_work_budget);
    }

    return report;
}

void Gc::reportStuckRemovals(const RefPlan & plan, uint64_t current_round)
{
    for (const UInt128 & life_id : plan.lifeIds())
    {
        const auto warning = stuckRemovalWarning(
            plan.row(life_id), current_round, store->poolConfig().gc_stuck_removal_rounds,
            store->layout());
        if (!warning)
            continue;
        ProfileEvents::increment(ProfileEvents::CASGCStuckRemovals);
        LOG_WARNING(logger, "{}", *warning);
    }
}

bool Gc::foldManifestEdges(GcReadAhead & reads, const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Etag> & mf_cleanup, uint32_t txn_ordinal)
{
    const Layout & layout = store->layout();

    const String key = layout.manifestKey(id);
    /// ONE ROUND TRIP PER EDGE. The GET alone carries the absence signal a HEAD would have carried, so
    /// the HEAD that used to precede it bought nothing and cost a second serial round trip on the
    /// hottest read path of the round (one per manifest edge, on every folded log). `!got` is the SAME
    /// absent outcome the missing HEAD used to produce -- record-and-continue, and the caller decides
    /// what an absent body means for that edge (a missing-body precommit is a barrier; a committed one
    /// fails closed). Never a throw: a 404 during the fold is an observation, not an error.
    ///
    /// The bytes may already have been fetched when the log that named this edge was decoded (all of
    /// that log's edges are hinted together, since one decode names them all). The absence signal, the
    /// decode and every decision below still happen HERE, in edge order, exactly as they always did.
    const auto got = reads.takeRead(key, ReadFamily::Manifest);
    if (!got)
        return false;   /// absent body: caller decides (missing-body precommit OK; committed => fail closed)
    ProfileEvents::increment(ProfileEvents::CASRefManifestBodyFoldGets);   /// one body GET per manifest fold

    const PartManifest body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body ref mismatch at {} (refMatchesBody fail-closed)", key);
    if (!manifestNamespaceMatches(id.root_namespace, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body namespace mismatch at {} (manifestNamespaceMatches fail-closed)", key);
    /// The manifest-wide `blob_hash_len` foreign-width gate is GONE (the field
    /// itself was deleted — entries now carry their own per-entry algo/width). `decodePartManifest`
    /// already fail-closes on an algo byte this BUILD does not know (`blobHashAlgoFromWord` throws
    /// `CORRUPTED_DATA` there) -- but a known algo may still not be ADMITTED to THIS pool yet (a stale
    /// in-memory `admitted_algos` cache reading a manifest another node already admitted a new algo
    /// for). Per-entry admission validation refreshes on miss BEFORE
    /// failing closed, so a genuinely fresh admission is never mistaken for corruption.
    for (const ManifestEntry & entry : body.entries)
        if (entry.placement == EntryPlacement::Blob && !store->isAlgoAdmitted(entry.ref.algo))
        {
            const std::vector<uint8_t> refreshed = store->refreshAdmittedAlgos();
            if (!store->isAlgoAdmitted(entry.ref.algo))
            {
                String names;
                for (size_t i = 0; i < refreshed.size(); ++i)
                {
                    if (i != 0)
                        names += ", ";
                    names += blobHashAlgoName(static_cast<BlobHashAlgo>(refreshed[i]));
                }
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc fold: manifest entry algo {} not admitted to this pool (algos_used {{{}}})",
                    blobHashAlgoName(entry.ref.algo), names);
            }
        }

    for (const ManifestEntry & entry : body.entries)
        if (entry.placement == EntryPlacement::Blob)
        {
            /// The fold settles the FULL `BlobRef` pair natively -- no bare-digest bridge remains.
            deltas.push_back(BlobDelta{
                .ref = entry.ref,
                .source_id = sourceEdgeId(id, entry.path),
                .remove = (sign < 0),
                .txn_ordinal = txn_ordinal});
            /// A folded owner edge over this blob (the manifest-model analog of the old
            /// `RootAdd`). +1 = the manifest's owner activated this blob's reference; -1 =
            /// the owner was removed, dropping the reference. Reconstructs WHY a blob's in-degree moved.
            EventEmitter{*store}.emit([&](CasEvent & ev)
            {
                ev.type = sign > 0 ? CasEventType::RootAdd : CasEventType::RootRemove;
                ev.namespace_ = id.root_namespace.string();
                ev.object_kind = CasEventObjectKind::Blob;
                ev.object_hash = blobIdOf(entry.ref);
                ev.outcome = sign > 0 ? "edge_added" : "edge_removed";
                ev.reason = sign > 0
                    ? "fold: manifest owner activated; +1 blob edge"
                    : "fold: manifest owner removed; -1 blob edge";
                ev.detail = {{"manifest_ref_instance", manifestRefDebugString(id.ref)},
                             {"path", entry.path}};
            });
        }

    if (sign < 0)
        mf_cleanup.emplace(id, got->etag);   /// owner removed: defer the exact body delete to recheck
    return true;
}

Gc::CheckpointWitnesses Gc::readCheckpointWitnesses(GcReadAhead & reads,
                                                    const std::map<String, RefTableListing> & ref_tables,
                                                    const CasRefCatalog::Snapshot & catalog_cut)
{
    /// Read the checkpoint of every namespace in the round's catalog cut, every namespace `ref_tables`
    /// names, PLUS every namespace a HELD row in `parent_cursors` names. A catalog-only namespace is the
    /// one whose second witness matters MOST: a genuinely empty listing cannot distinguish a namespace
    /// that has no records from one whose records the same enumeration missed.
    ///
    /// EXACT KEY, ALWAYS -- never `RefTableListing::has_ckpt`. Skipping the read because the listing did
    /// not show a `_ckpt` would make the second witness a function of the first, which is precisely the
    /// dependency it exists to break: the listing is a SNAPSHOT, and a `_ckpt` that became durable after
    /// the enumeration is exactly the one whose namespace has records the same enumeration also missed.
    ///
    /// READS UNDER THE CALLER'S ADMISSION, not one of its own. This function used to admit a fresh
    /// operation, which meant that a fence moving mid-round would hand it a NEWER generation than the
    /// round holds and let it read on regardless; taking the caller's read-ahead makes the same fence
    /// movement fail this read the way it fails every other read of the round. Strictly the
    /// fail-closed direction, and it is why there is no `admit` here any more.
    const Layout & layout = store->layout();

    std::set<String> witness_namespaces;
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
        if (entry.state == NsState::Live || entry.state == NsState::Removing)
            witness_namespaces.insert(entry.ns.string());
    for (const auto & [ns_str, listing] : ref_tables)
        witness_namespaces.insert(ns_str);

    /// WHICH KEYS, decided before any of them is read. Nothing here depends on a body, so the whole
    /// set is known up front and the fetches can overlap; the decisions below still run in the
    /// `std::set` order they always ran in, one namespace at a time.
    struct WitnessKey
    {
        String ns;
        String ckpt_key;
    };
    std::vector<WitnessKey> witness_keys;
    witness_keys.reserve(witness_namespaces.size());
    for (const String & ns_str : witness_namespaces)
    {
        const RootNamespace ns{ns_str};
        /// Use the SAME complete catalog cut the round's walk resolved, never an independent
        /// catalog re-read. A namespace absent from the cut, or present only as a non-walkable
        /// `Creating` row, has no admitted witness key to read this round.
        const auto entry_it = std::lower_bound(
            catalog_cut.catalog.entries.begin(), catalog_cut.catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        if (entry_it == catalog_cut.catalog.entries.end() || entry_it->ns != ns
            || (entry_it->state != NsState::Live && entry_it->state != NsState::Removing))
            continue;
        witness_keys.push_back(
            {ns_str, layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(entry_it->ns, entry_it->incarnation))});
    }

    size_t next_hint = 0;
    const auto topUpWitnessHints = [&]
    {
        while (next_hint < witness_keys.size() && reads.pending() < reads.window())
            reads.hintRead(witness_keys[next_hint++].ckpt_key, ReadFamily::RefCheckpoint);
    };

    CheckpointWitnesses out;
    for (const WitnessKey & witness_key : witness_keys)
    {
        const String & ns_str = witness_key.ns;
        const String & ckpt_key = witness_key.ckpt_key;
        topUpWitnessHints();
        /// THE GET AND THE DECODE ARE SPLIT HERE, rather than taken together through `readCkpt`, so the
        /// catch below can scope to the DECODE ALONE. Wrapping the read too would turn a transport
        /// failure -- which says nothing about this object and everything about the round's ability to
        /// read anything -- into a per-namespace hold, silently narrowing a pool-wide outage to one
        /// namespace. A backend throw still propagates and fails the round, exactly as it always did --
        /// a read-ahead worker's failure is rethrown by the take below, at this same site.
        const std::optional<Object> got = reads.takeRead(ckpt_key, ReadFamily::RefCheckpoint);
        /// ABSENT IS NORMAL AND IS NOT A WITNESS: a namespace has no `_ckpt` until its first snapshot
        /// publication commits, and one that 404s mid-round is a namespace being reclaimed. Neither says
        /// anything about which ids exist, so neither may hold the walk -- and neither may throw
        /// (a GC fold never fails a round on a 404).
        if (!got)
            continue;

        RefCkpt ckpt;
        try
        {
            /// Materialized read, then decode (`readCkpt`'s rule): the object is MUTABLE, so the body
            /// must be fixed before it is parsed.
            ckpt = decodeRefCkpt(got->bytes);
        }
        catch (const Exception & e)
        {
            /// PER-NAMESPACE, NEVER ROUND-WIDE (every per-namespace failure is a clamp or a
            /// hold). This object belongs to exactly one namespace, so it can never be grounds for
            /// refusing to fold another one -- and refusing the whole round is what a single unreadable
            /// 4 KiB object used to do, stopping every namespace's cursor, seal and cleanup for as long
            /// as it stayed unrepaired. It is recorded, named, and left to the walk to hold.
            ///
            /// NAME THE OBJECT. The decode's own message says what is wrong with the bytes and nothing
            /// about WHICH bytes, so without the key an operator cannot find the object to repair.
            out.undecodable.emplace(ns_str, ckpt_key + ": " + e.message());
            continue;
        }
        /// A checkpoint without `checkpoint_snapshot_id` is silent rather than empty: the namespace has
        /// published no snapshot yet.
        if (ckpt.checkpoint_snapshot_id)
            out.witnesses.emplace(ns_str, *ckpt.checkpoint_snapshot_id);
        out.recovery_checkpoints.emplace(ns_str, CkptSample{std::move(ckpt), got->etag});
    }
    return out;
}

std::optional<std::pair<uint64_t, uint64_t>> Gc::newestFoldSealRef()
{
    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();
    const String gen_prefix = layout.gcGenPrefix(0);
    const String top = gen_prefix.substr(0, gen_prefix.size() - 2);   /// ".../gc/gen/"

    /// THE WIDE ENUMERATION IS A HINT HERE TOO, exactly as it is in the ref walk. Trusting it for
    /// NEWEST-ness would reopen the same hole one layer up: an enumeration that omits the true newest
    /// seal hands back an older one, and every hold detected since that older seal is silently lost --
    /// an under-carry, which is the failure this whole path exists to prevent.
    std::set<uint64_t> listed_generations;
    bool listed_anything = false;
    std::optional<std::pair<uint64_t, uint64_t>> newest;
    op.forEachListedKey(top, [&](const ListedKey & k)
    {
        listed_anything = true;
        const size_t from = top.size();
        const size_t gen_end = k.key.find('/', from);
        if (gen_end == String::npos)
            return true;
        uint64_t generation = 0;
        try
        {
            generation = std::stoull(k.key.substr(from, gen_end - from));
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            return true;   /// foreign key shape under `gc/gen` is debris, not a generation number
        }
        listed_generations.insert(generation);
        return true;
    }, Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
    const uint64_t listed_max_generation = listed_generations.empty() ? 0 : *listed_generations.rbegin();

    /// STEP DOWN THROUGH THE GENERATIONS THE LISTING ITSELF REPORTED until one carries a seal. The
    /// newest generation routinely exists WITHOUT one: a round writes its runs during the reduce phase
    /// and its fold seal only in phase `fold_seal_write`, so an ordinary crash in between leaves exactly that
    /// shape. Stopping at the maximum would then refuse a pool whose holds are sitting readable one
    /// generation down -- turning a plain crash into "recreate the pool".
    ///
    /// Stepping down costs no trust that has not already been spent: these are the generations the wide
    /// listing reported, and its maximum -- which the probes above are checking -- is one of them. What
    /// is NOT weakened is the refusal above: a seal found ABOVE the maximum stays terminal, because
    /// that is the listing being caught in a lie rather than merely being incomplete about seals.
    ///
    /// "Never step PAST an unreadable seal" falls out of returning the FIRST generation that carries
    /// one: the caller decodes it and refuses if it cannot, so an undecodable seal ends the search
    /// instead of being skipped over in favour of an older, readable one.
    for (auto it = listed_generations.rbegin(); it != listed_generations.rend(); ++it)
    {
        if (const auto probe = probeGenerationForSeal(*it); probe.seal_attempt)
        {
            newest = std::make_pair(*it, *probe.seal_attempt);
            break;
        }
    }

    /// DETECTION, NOT PROOF -- and labelled as such deliberately, in the same spirit as probe A. Two
    /// NARROW single-generation probes above the wide listing's maximum ask whether that maximum was a
    /// lie. The generation half of the question is arithmetic (generations are dense in minting: a fold
    /// takes `snap_generation + 1`, a rebuild `max_gen + 1`), but the attempt half is not and cannot be
    /// made so -- `attempt` is `lease.seq`, a global counter that advances on EVERY round including
    /// deferred ones, so consecutive generations carry attempts separated by unbounded gaps and there is
    /// no `attempt + 1` to point-read. So the step is an enumeration WITHIN ONE DIRECTORY: strictly
    /// narrower than the pool-wide listing whose maximum it is checking, and honestly not the exact
    /// read the ref walk gets.
    ///
    /// A seal found above the maximum means the wide listing lied about the very thing this path is
    /// deciding, so the answer is REFUSAL, not adoption of the newer seal: a store that misreports its
    /// own enumeration DURING DISASTER RECOVERY does not get a second guess, and silently adopting
    /// whatever the second query returned would just move the trust one query along.
    static constexpr uint64_t kProbeGenerationsAbove = 2;
    for (uint64_t above = 1; above <= kProbeGenerationsAbove; ++above)
    {
        const uint64_t generation = listed_max_generation + above;
        const GenerationSealProbe probe = probeGenerationForSeal(generation);
        if (!probe.seal_attempt)
            continue;
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS GC rebuild: a fold seal exists at generation {} (attempt {}) while the pool-wide "
            "enumeration of {} reported nothing above generation {}. The enumeration this rebuild "
            "would have taken its baseline from is demonstrably incomplete, so the holds it carries "
            "cannot be trusted to be all of them. GC refuses to rebuild; this pool must be recreated.",
            generation, *probe.seal_attempt, top, listed_max_generation);
    }

    if (newest)
        return newest;

    /// THE VIRGIN VERDICT, and everything it rests on. `gc/state` is already known absent or
    /// unreadable (the caller's precondition); the wide listing found nothing; and one narrow probe of
    /// generation 1 -- the first generation any pool would ever mint -- also finds nothing. That is
    /// three pieces of ENUMERATION evidence and no point read, because the seal key's attempt component
    /// has no arithmetic successor to probe.
    ///
    /// NAMED RESIDUAL, and the generation-1 probe NARROWS it rather than closing it. On a pool that has
    /// been pruned, generation 1 LEGITIMATELY does not exist -- `pruneSupersededGenerations` deletes
    /// whole old generation prefixes once they age past `gc_snapshot_generations_to_keep` -- so an empty
    /// generation-1 probe proves nothing there. A total enumeration blackout on a lived-in, pruned pool
    /// therefore still reads virgin here, and grants it a clean slate with no holds. What the probe
    /// does buy is the un-pruned case: a young pool whose seals the wide listing hid is caught.
    ///
    /// No closure exists in the current key shapes, because a fold seal cannot be point-read from its
    /// generation alone. The fix is a derivable per-generation marker that CAN be point-read, and it
    /// would survive the blackout precisely because it needs no enumeration. Anything the listing DOES
    /// show above its own maximum is caught by the refusal above instead.
    const GenerationSealProbe genesis = probeGenerationForSeal(1);
    if (listed_anything || genesis.generation_exists)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS GC rebuild: the pool-wide enumeration of {} yielded no fold seal, yet {} -- so the "
            "pool is NOT provably new and its holds cannot be enumerated. GC refuses to rebuild; this "
            "pool must be recreated.",
            top,
            listed_anything ? "that enumeration did return objects under it"
                            : "a narrow probe of generation 1 found objects the wide listing omitted");

    ProfileEvents::increment(ProfileEvents::CASGCRebuildVirginByEnumeration);
    LOG_WARNING(logger,
        "CAS GC rebuild PROCEEDING AS NEVER-SEALED: no fold seal was found by the broad listing of {} "
        "or by the generation-1 probe, and gc/state is absent or unreadable, so NO durable hold is "
        "carried forward. IMPLICATION: if this pool HAS sealed and then pruned, generation 1 is gone "
        "legitimately and this verdict rests on a total enumeration blackout -- holds may be lost and "
        "GC may reclaim blobs a held namespace still protects. The verdict rests on ENUMERATION ALONE: "
        "no point read can prove it, because a fold seal key needs an attempt component that is a "
        "lease sequence number. Verify with the store operator that the object listing is complete "
        "before trusting this rebuild.",
        top);
    return std::nullopt;
}

Gc::GenerationSealProbe Gc::probeGenerationForSeal(uint64_t generation)
{
    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();

    GenerationSealProbe probe;
    op.forEachListedKey(layout.gcGenPrefix(generation), [&](const ListedKey & k)
    {
        probe.generation_exists = true;   /// ANY object proves this generation was minted
        /// Parse a candidate attempt out of the path and then PROVE it by rebuilding the key: only a
        /// string `foldSealKey` itself would have produced is a fold seal. Everything else under a
        /// generation -- run objects, outcome sets, debris of a lost era -- must not get to decide
        /// which baseline this pool's holds are read from.
        static constexpr std::string_view kAttempt = "/attempt/";
        const size_t a_begin = k.key.find(kAttempt);
        if (a_begin == String::npos)
            return true;
        const size_t a_from = a_begin + kAttempt.size();
        const size_t a_end = k.key.find('/', a_from);
        if (a_end == String::npos)
            return true;
        uint64_t attempt = 0;
        try
        {
            attempt = std::stoull(k.key.substr(a_from, a_end - a_from));
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            return true;   /// foreign key shape is debris, not an attempt
        }
        if (layout.foldSealKey(generation, attempt) != k.key)
            return true;
        if (!probe.seal_attempt || *probe.seal_attempt < attempt)
            probe.seal_attempt = attempt;
        return true;
    }, Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
    return probe;
}

uint64_t Gc::FoldResult::FrontierDeficit::total() const
{
    return checkpoint_unusable + held + intake_slice_cut + discovery_incomplete + probe_budget + fold_aborted + unattributed;
}

String Gc::FoldResult::FrontierDeficit::describe() const
{
    String out;
    const auto add = [&](const char * name, uint64_t count)
    {
        if (count == 0)
            return;
        if (!out.empty())
            out += ", ";
        out += fmt::format("{}={}", name, count);
    };
    add("checkpoint_unusable", checkpoint_unusable);
    add("held", held);
    add("intake_slice_cut", intake_slice_cut);
    add("discovery_incomplete", discovery_incomplete);
    add("probe_budget", probe_budget);
    add("fold_aborted", fold_aborted);
    add("unattributed", unattributed);
    return out;
}

void Gc::FoldResult::FrontierDeficit::count(FrontierUnproven reason)
{
    switch (reason)
    {
        case FrontierUnproven::Proven: return;
        case FrontierUnproven::CheckpointUnusable: ++checkpoint_unusable; return;
        case FrontierUnproven::Held: ++held; return;
        case FrontierUnproven::IntakeSliceCut: ++intake_slice_cut; return;
        case FrontierUnproven::DiscoveryIncomplete: ++discovery_incomplete; return;
        case FrontierUnproven::Unattributed: ++unattributed; return;
    }
}

namespace
{

/// The sorted `_log` ids of one physical life that the round's hint source names, possibly none. It chooses
/// which keys planning reads first and decides nothing. A view: the source outlives the planning call.
struct RefLifeHint
{
    std::span<const RefTxnId> logs;
};

RefLifeHint hintFromListing(const RefTableListing & listing)
{
    return RefLifeHint{.logs = listing.logs};
}

/// Where one life's walk ends in this attempt, fixed by exact `GET`s before the first manifest read. It is
/// never carried to another attempt: a key read absent then may be present now.
struct RefStopPlan
{
    /// Per epoch planning reached, the greatest sequence it read present; 0 when the first key it read there
    /// was absent.
    std::map<uint64_t, uint64_t> last_present;
    /// The key read absent right after the last present record of the final epoch. Unset when planning ended
    /// at a terminal record, an undecodable body or the crossing limit.
    std::optional<RefTxnId> stop;
    /// The hint's ids above the first key.
    std::vector<RefTxnId> hint_ids;
    bool first_present = false;
    uint64_t crossings = 0;
    bool crossing_limit_reached = false;

    bool isStop(const RefTxnId & id) const { return stop && id == *stop; }

    /// At or below a sequence planning read present in a reached epoch.
    bool covers(const RefTxnId & id) const
    {
        const auto it = last_present.find(id.writer_epoch);
        return it != last_present.end() && id.ref_sequence <= it->second;
    }
};

/// Seal crossings one life's planning follows. A longer chain of epochs leaves the life unproven this round and
/// the walk keeps the prefix it applied.
constexpr uint64_t kRefPlanMaxCrossings = 64;

/// Least gap between two progress lines of pass 2.
constexpr uint64_t kRefIntakeProgressIntervalMs = 60 * 1000;

/// Ref-log bodies planning read, kept by exact key for the walk under a byte cap summed over the round's lives.
class RetainedRefBodies
{
public:
    explicit RetainedRefBodies(size_t cap_bytes_) : cap_bytes(cap_bytes_) {}

    void keep(const String & key, Object object)
    {
        if (object.bytes.size() > cap_bytes - kept_bytes)
            return;
        kept_bytes += object.bytes.size();
        bodies.emplace(key, std::move(object));
    }

    std::optional<Object> take(const String & key)
    {
        const auto it = bodies.find(key);
        if (it == bodies.end())
            return std::nullopt;
        Object object = std::move(it->second);
        bodies.erase(it);
        return object;
    }

    bool contains(const String & key) const { return bodies.contains(key); }
    /// Bytes kept over the round, including bodies the walk has taken since.
    size_t keptBytes() const { return kept_bytes; }

private:
    const size_t cap_bytes;
    size_t kept_bytes = 0;
    std::unordered_map<String, Object> bodies;
};

/// How the chain continues after a record planning read present.
struct PlannedRecord
{
    enum class Kind : uint8_t
    {
        /// The successor is the next sequence of the same epoch.
        Ordinary,
        /// The successor is `head`, in a later epoch.
        Seal,
        /// A terminal record or a body the walk will hold at: planning has no key to read after it.
        End,
    };
    Kind kind = Kind::End;
    RefTxnId head{};
};

PlannedRecord classifyPlannedRecord(const Object & object, const NamespaceLifeId & life, const RefTxnId & id)
{
    std::optional<RefTxnId> successor;
    std::optional<RefEpochTransition> transition;
    try
    {
        const RefLogTxn txn = decodeRefLogTxn(openObject(FormatId::RefLog, object.bytes), life.ns.string(), id);
        successor = refChainSuccessor(txn);
        transition = refEpochTransitionOf(txn);
    }
    catch (const Exception &)
    {
        /// The walk decodes the same body when it reaches it and holds the life there.
        return {};
    }
    if (!successor)
        return {};
    if (!transition)
        return {.kind = PlannedRecord::Kind::Ordinary};
    /// A seal must lead to a later epoch; planning stops at any other and the walk validates the link.
    if (transition->next_epoch <= id.writer_epoch)
        return {};
    return {.kind = PlannedRecord::Kind::Seal, .head = *successor};
}

uint64_t saturatingAdd(uint64_t a, uint64_t b)
{
    return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b;
}

/// Fixes where the walk of `life` ends in this attempt, starting at its first expected key `first`, which it
/// always reads. In each epoch it reads the greatest hinted id above the last record read present, doubles past
/// that record to the first key read absent and bisects strictly between the two; a seal sends it to the head
/// the seal's body names. Every absence it relies on is an exact `GET` answered absent, and every read error
/// propagates. Bodies it reads go to `retained`; its absent reads are added to `absent_probes`. It asks
/// `stop_requested` before every read and returns no plan once it answers true, because what it read so far
/// fixes no stop.
std::optional<RefStopPlan> planRefStops(GcReadAhead & reads, const Layout & layout, const NamespaceLifeId & life,
                                        const RefTxnId & first, const RefLifeHint & hint, RetainedRefBodies & retained,
                                        uint64_t & absent_probes, const std::function<bool()> & stop_requested)
{
    RefStopPlan plan;
    plan.hint_ids.assign(std::upper_bound(hint.logs.begin(), hint.logs.end(), first), hint.logs.end());

    bool stopped = false;
    const auto read = [&](const RefTxnId & id) -> std::optional<PlannedRecord>
    {
        if (stop_requested())
        {
            stopped = true;
            return std::nullopt;
        }
        const String key = layout.refLogKey(life, id);
        std::optional<Object> got = reads.takeRead(key, ReadFamily::RefLog);
        if (!got)
        {
            ++absent_probes;
            return std::nullopt;
        }
        ProfileEvents::increment(ProfileEvents::CASRefLogBodyGets);
        const PlannedRecord record = classifyPlannedRecord(*got, life, id);
        retained.keep(key, std::move(*got));
        return record;
    };

    RefTxnId head = first;
    while (true)
    {
        const uint64_t epoch = head.writer_epoch;
        uint64_t & last_present = plan.last_present[epoch];
        std::optional<PlannedRecord> record = read(head);
        if (stopped)
            return std::nullopt;
        if (head == first)
            plan.first_present = record.has_value();
        if (!record)
        {
            plan.stop = head;
            return plan;
        }
        uint64_t lo = head.ref_sequence;
        last_present = lo;
        /// Set only from a key read absent.
        std::optional<uint64_t> hi;
        const auto probe = [&](uint64_t sequence)
        {
            std::optional<PlannedRecord> got = read(RefTxnId{epoch, sequence});
            if (stopped)
                return;
            if (got)
            {
                record = got;
                lo = sequence;
                last_present = sequence;
            }
            else
                hi = sequence;
        };

        const auto past_epoch = std::upper_bound(
            hint.logs.begin(), hint.logs.end(), RefTxnId{epoch, std::numeric_limits<uint64_t>::max()});
        if (record->kind == PlannedRecord::Kind::Ordinary && past_epoch != hint.logs.begin()
            && std::prev(past_epoch)->writer_epoch == epoch && lo < std::prev(past_epoch)->ref_sequence)
            probe(std::prev(past_epoch)->ref_sequence);

        const uint64_t base = lo;
        for (uint64_t step = 1; !stopped && record->kind == PlannedRecord::Kind::Ordinary && !hi; step = saturatingAdd(step, step))
        {
            const uint64_t sequence = saturatingAdd(base, step);
            if (sequence <= lo)
                break;
            probe(sequence);
        }

        while (!stopped && record->kind == PlannedRecord::Kind::Ordinary && hi && *hi - lo > 1)
            probe(lo + (*hi - lo) / 2);
        if (stopped)
            return std::nullopt;

        switch (record->kind)
        {
            case PlannedRecord::Kind::Ordinary:
                if (hi && *hi - lo == 1)
                    plan.stop = RefTxnId{epoch, *hi};
                return plan;
            case PlannedRecord::Kind::End:
                return plan;
            case PlannedRecord::Kind::Seal:
                if (plan.crossings == kRefPlanMaxCrossings)
                {
                    plan.crossing_limit_reached = true;
                    return plan;
                }
                ++plan.crossings;
                head = record->head;
                break;
        }
    }
}

}

Gc::FoldResult Gc::fold(GcState & state, std::optional<Etag> & state_etag,
                        RoundReport & report,
                        uint64_t current_round, const RefPlan & walk_plan, UniversePolicy policy,
                        GcRoundWorkBudget & work_budget)
{
    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();
    /// The fold's read-ahead. It fetches through `op`'s own admitted generation and hands every result
    /// back at the site that would otherwise have read inline, so the walk's order, its counters, its
    /// holds and its events are what they were; only the moment of the fetch moves. At
    /// `gc_io_concurrency` 1 it hints nothing and every take IS the original inline read.
    GcReadAhead reads(op, store->openRequests(), *io_pool, store->poolConfig().gc_io_concurrency);
    reads.enableFamilyAccounting(intake_clock_ns);
    FoldResult result;

    /// 1. Group the round's one enumeration of `cas/ns/stream/` (taken before the defer decision) into
    /// per-table immutable-object listings. That single enumeration serves the defer signal, the
    /// ref-log intake, and ref-object cleanup planning alike.
    ///
    /// THE ROUND LISTS THIS PREFIX ONCE. The intake does not need a second opinion about the listing
    /// because it does not consult the listing for completeness at all -- it walks by exact key from
    /// the cursor.
    ///
    /// Phase `fold_ref_group`: the strict regrouping -- what this round will fold, decided before it
    /// reads a single body. No I/O: the keys are already in hand.
    std::optional<GcPhaseTimer> ref_list_timer;
    ref_list_timer.emplace(phase_sink, "fold_ref_group");
    const RefScanSummary & ref_scan = walk_plan.refScan();
    const std::vector<String> & ref_object_keys = ref_scan.keys;

    /// The round's ONE catalog `GET`. `live_incarnation` names, for every
    /// namespace the catalog admits as `Live`/`Removing`, the ONE incarnation the fold may act on --
    /// `Creating` is excluded, matching `discoverUniverse` (no publication can exist yet). This is what
    /// makes the fold catalog-authoritative rather than LIST-authoritative: the pool-wide ref LIST
    /// remains the round's intra-namespace hint (what a namespace the catalog already named has
    /// listed), never the source of WHICH namespaces exist. Reused below for the catalog-only walk
    /// targets, so the round pays this GET once.
    const CasRefCatalog::Snapshot & catalog_snapshot = walk_plan.catalogCut();
    std::map<String, UInt128> live_incarnation;
    /// A `Creating` life IS named by the catalog -- `live_incarnation` excludes it
    /// only because it is not yet WALKABLE (no publication can exist), not because the
    /// namespace is unaccounted. `completeCreation` publishes `_ckpt` (step 2) strictly BEFORE the
    /// `Creating -> Live` CAS (step 3), so every ordinary namespace creation has a real window --
    /// crash-stalled or merely mid-flight -- where a `Creating` entry's own `_ckpt` is durable and
    /// listed while the entry itself is absent from `live_incarnation`. R10's un-cataloged anomaly
    /// below must tell that apart from genuine "nothing in the catalog names this at all" debris, or
    /// an ordinary or stalled creation suppresses the whole round's reclamation until someone
    /// recreates the exact name and drives `reconcileStaleCreator` -- unbounded in the stalled case.
    for (const NamespaceLifeId & life : walk_plan.lives())
        live_incarnation.emplace(life.ns.string(), life.incarnation);
    /// Carry the complete cut on the result so every later consumer this round --
    /// `cleanupRefObjects` and terminal-evidence attribution -- retains both the chosen incarnation and
    /// the lifecycle/absence distinction instead of re-reading or reducing the catalog independently.
    result.catalog_cut = catalog_snapshot;
    /// THE POSITIVE EMPTY-UNIVERSE PROOF (see the destructive gate below). `etag` is guaranteed by
    /// `CasRefCatalog::read` on every operational path -- absence there is `CORRUPTED_DATA`, never an
    /// empty snapshot -- but the check stays here so this fails closed if a bootstrap/test snapshot
    /// ever reaches this line. `entries` (not `live_incarnation`, which drops `Creating`) is the right
    /// source: a catalog holding only `Creating` rows must NOT read as an empty universe, and `entries`
    /// is the one view that still carries those rows.
    result.catalog_cut_proved_empty = catalog_snapshot.etag.has_value() && catalog_snapshot.catalog.entries.empty();

    /// A malformed ref-object key or namespace aborts ref folding for the whole round: the
    /// round produces no ref delta, advances no cursor, and authorizes no destructive work -- recorded as
    /// an anomaly (which drives `suppress_destructive`), never a throw that wedges the round.
    std::map<String, RefTableListing> ref_tables;
    bool ref_folding_aborted = false;
    try
    {
        const auto physical_tables = groupRefKeys(layout, ref_object_keys);
        for (const auto & [life_id, listing] : physical_tables)
        {
            const auto life = catalog_snapshot.life_index.resolve(life_id);
            if (!life)
                continue;   /// absent from the post-LIST cut: inert dead-life debris
            const auto live_it = live_incarnation.find(life->ns.string());
            if (live_it != live_incarnation.end() && live_it->second == life_id)
                ref_tables.emplace(life->ns.string(), listing);
        }
    }
    catch (const Exception & e)
    {
        ref_folding_aborted = true;
        report.recordAnomaly(RootNamespace{}, 0, ManifestId{},
                             "malformed ref-object key: ref folding aborted this round");
        LOG_WARNING(logger,
                    "CAS GC ref intake: {} -- aborting ref folding for the round", e.message());
    }
    for (const auto & [ns_str, listing] : ref_tables)
        result.root_shards.emplace_back(RootNamespace{ns_str}, 0);

    ref_list_timer->metric("ref_keys_listed", ref_object_keys.size());
    ref_list_timer->metric("namespaces_seen", ref_tables.size());
    /// The round's only remaining whole-round ref abort: a key attributable to no namespace. Reported on
    /// every round, healthy or not — a column that is always 0 is what makes the one round where it is
    /// not stand out.
    ref_list_timer->metric("ref_folding_aborted", ref_folding_aborted ? 1 : 0);
    ref_list_timer.reset();   /// emits the `fold_ref_group` row

    /// Parent cursors — the per-(ns,shard) cursors a prior round sealed. `listRefPrefix` read them from
    /// the fold seal at the adopted (snap_generation, snap_attempt) before it built this round's one
    /// walk plan (the fold seal IS the coverage record).
    /// Absent => fresh pool (cursor 0). A folded event must never be re-folded from 0 (that double-counts
    /// blob in-degree => silent over-pin/leak).
    /// A live `gc/state` whose adopted
    /// fold seal OBJECT is MISSING is corrupt bookkeeping, never an empty baseline — treating it as
    /// empty would re-fold only journal tails and mass-condemn everything the lost snapshot
    /// protected. NOTE the distinction from a PRESENT seal with empty `ref_lives` (a legitimate
    /// empty-universe generation) — the audit keys on object absence, not coverage emptiness.
    ///
    /// Phase `fold_seal_read`. The scope reaches down to `discover_ref_seal` below, because that is
    /// a SECOND GET of the SAME key at the SAME (generation, attempt) -- the two reads belong on one row
    /// or the redundancy is invisible. Everything between them is I/O-free (a `resize`, three lambda
    /// DEFINITIONS, and plain assignments), so the duration is honestly the two GETs and their decodes.
    /// Instrumented, NOT fixed: removing the second read is a behaviour change, and the `redundant_reads`
    /// metric is the evidence for deciding it.
    std::optional<GcPhaseTimer> seal_read_timer;
    seal_read_timer.emplace(phase_sink, "fold_seal_read");
    const std::optional<CasFoldSeal> adopted_seal = readFoldSeal(state.snap_generation, state.snap_attempt);
    if (!adopted_seal && state.snap_generation > 0)
    {
        /// A deposed leader hears only the fact: a rebuild does not repair a lost leadership.
        const String missing = fmt::format(
            "CAS GC: the adopted fold seal (generation {}, attempt {}) is missing", state.snap_generation, state.snap_attempt);
        throwIfStateMoved(op, state_etag, missing);
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "{} under a live gc/state. {}", missing, kCorruptBookkeeping);
    }
    /// Every fold decision below starts from the immutable plan, never from the raw scan that supplied
    /// it. The successor seal owns its mutable copy; this separate const snapshot remains the prior
    /// coverage/hold view while the successor earns changes later in this fold.
    const std::map<UInt128, RefLifeFoldState> parent_ref_lives = walk_plan.parentFoldStates();
    const uint64_t dropped_parent_ref_lives = walk_plan.droppedParentRows();
    result.fold_seal.ref_lives = walk_plan.successorFoldStates();

    /// Retired-in-snapshot: the prior generation's condemned entries RIDE the source-edge run as
    /// `RunMarker::Condemned` sentinel rows, so the round no longer reads any separate retired-list object —
    /// the parent seal's `blob_target_runs` ARE the retired input. The per-gc-shard `condemned_summary`
    /// the seal carries below is distilled from the `still_retired` rows each shard re-emits, making the
    /// next round's `rowDecisionDue` / pure-carry decisions zero-I/O.
    const uint64_t condemn_round = state.round + 1;
    result.retired_merge.resize(state.gc_shards);

    /// HEAD READ-AHEAD FOR THE REDUCE PHASE. `head_candidates[shard]` is filled in that phase with the
    /// blobs the merge can bring to in-degree zero, in the merge's own ascending key order; `head_blob`
    /// below tops the hints up a window deep before each take. It is EMPTY everywhere else, the whole of
    /// intake included, so every take outside that phase is the plain inline HEAD.
    ///
    /// Hints are issued from INSIDE the lambda rather than in one burst at phase start, so the requests
    /// this can ever add are bounded by one window past the last candidate the merge actually reaches.
    /// A superset that overshoots badly therefore costs a window, not its own size -- which matters
    /// because nothing bounds a round's condemnation count.
    std::vector<std::vector<BlobRef>> head_candidates(state.gc_shards);
    size_t head_hint_shard = 0;
    size_t next_head_hint = 0;
    const auto topUpHeadHints = [&]
    {
        const std::vector<BlobRef> & shard_candidates = head_candidates[head_hint_shard];
        while (next_head_hint < shard_candidates.size() && reads.pendingHeads() < reads.window())
            reads.hintHead(layout.blobKey(shard_candidates[next_head_hint++]));
    };

    /// Condemn-time observation: ONE HEAD per new zero-transition captures the exact incarnation token
    /// the intent and the eventual delete name (absent, or a token with no identity => nothing to condemn).
    /// Emits the Candidate trail (IndegZero / GcRetireObserve / BlobRetire) where the decision is made.
    ///
    /// THE READ-AHEAD NEVER RUNS THIS LAMBDA, only feeds it: the trail and the counters below must not run
    /// over a superset of the candidates. The prefetch is a bare HEAD issued after the cut.
    /// A body shorter than the pool's envelope is not a blob GC can account for: counted and recorded, and the
    /// fold neither condemns, adopts nor deletes it, so one bad object cannot stop every round.
    const uint64_t blob_header_len = store->poolMeta().blob_header_len;
    const auto undecodableHead = [&](const BlobRef & ref, const Meta & observed, bool spared) -> BlobHead
    {
        ProfileEvents::increment(ProfileEvents::CASGCUndecodableBlobBody);
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::CorruptDecode;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = blobIdOf(ref);
            e.token = observed.etag.render();
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.outcome = spared ? "spared" : "carried";
            e.reason = spared
                ? "the body is shorter than the pool's blob envelope; the blob is referenced again, so GC drops its row, "
                  "keeps its intent and deletes nothing"
                : "the body is shorter than the pool's blob envelope; GC neither condemns nor deletes it";
            e.detail = {{"key", layout.blobKey(ref)}, {"size", std::to_string(observed.size)},
                        {"envelope", std::to_string(blob_header_len)}};
        });
        return BlobHead::undecodableBody();
    };
    /// GC accounts a blob by its payload size, without the pool's envelope.
    const auto blobHeadOf = [&](const BlobRef & ref, std::optional<Meta> observed, bool spared = false) -> BlobHead
    {
        if (!observed)
            return std::nullopt;
        if (observed->size < blob_header_len)
            return undecodableHead(ref, *observed, spared);
        observed->size -= blob_header_len;
        return std::move(*observed);
    };

    const auto head_blob = [&](const BlobRef & ref) -> BlobHead
    {
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::IndegZero;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = blobIdOf(ref);
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.reason = "last folded owner edge dropped; in-degree reached 0";
        });
        topUpHeadHints();
        const std::optional<Meta> observed = reads.takeHead(layout.blobKey(ref));
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcRetireObserve;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = blobIdOf(ref);
            e.token = observed ? observed->etag.render() : "";
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.outcome = observed ? "present" : "absent";
            e.reason = "zero-in-degree candidate; observe the current incarnation";
        });
        BlobHead head = blobHeadOf(ref, observed);
        if (!head.meta || !head.meta->etag.canonical())
            return head;
        ++report.candidates;
        ++report.condemned;
        ProfileEvents::increment(ProfileEvents::CASGCRetiredCondemned);
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::BlobRetire;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = blobIdOf(ref);
            e.token = head.meta->etag.render();
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.outcome = "retired";
            e.reason = "condemned zero-in-degree candidate; entering the current retired list";
        });
        return head;
    };

    /// The decision `HEAD` of a touched key that has a row: every rule of that row in this round reads it,
    /// whatever it answers. A plain HEAD with no events and no counters; a move to another incarnation emits
    /// `blob_retire_replaced` once, from `merge.replaced`. It is sent inline, not read ahead, so it is
    /// never older than the cut and the observations that preceded it.
    const auto peek_head = [&](const BlobRef & ref) -> BlobHead
    {
        return blobHeadOf(ref, op.head(layout.blobKey(ref), Retry::standard()));
    };

    /// Blob keys of pending rows whose `HEAD` is hinted and not yet taken.
    std::unordered_set<String> pending_head_reserved;
    /// Blob keys of spared rows whose `HEAD` is hinted and not yet taken.
    std::unordered_set<String> spare_head_reserved;
    /// The pending row whose redelete slot the fold has taken and whose `HEAD` it takes next.
    std::optional<String> settling_pending_head;

    /// The `HEAD` before a pending row's body delete, taken when the merge settles the row. It is sent in the
    /// merge, so after the round's cut and before the row's run is written. A failed `HEAD` fails the round before
    /// any body delete and before its commit, so the row stays pending.
    const auto pending_head = [&](const BlobRef & ref) -> BlobHead
    {
        const String key = layout.blobKey(ref);
        pending_head_reserved.erase(key);
        settling_pending_head.reset();
        try
        {
            return blobHeadOf(ref, reads.takeHead(key));
        }
        catch (...)
        {
            countRetired(RetiredCount::BodyDeleteFailed);
            ++report.body_delete_failed;
            LOG_WARNING(logger,
                "CAS gc: the HEAD before the pending delete of blob {} (key `{}`) failed; the round fails before its "
                "commit and the row stays pending: {}",
                blobIdOf(ref), key, getCurrentExceptionMessage(false));
            throw;
        }
    };

    /// The `HEAD` of a spared row, sent in the merge, so after the round's cut. Only an answer of this round may
    /// show that the row's token is gone: one read earlier can predate the body the row names.
    const auto spare_head = [&](const BlobRef & ref) -> BlobHead
    {
        const String key = layout.blobKey(ref);
        spare_head_reserved.erase(key);
        try
        {
            return blobHeadOf(ref, reads.takeHead(key), /*spared*/ true);
        }
        catch (...)
        {
            LOG_WARNING(logger,
                "CAS gc: the HEAD of spared blob {} (key `{}`) failed; the round fails before its commit and the row "
                "keeps its state: {}", blobIdOf(ref), key, getCurrentExceptionMessage(false));
            throw;
        }
    };

    /// Sent in the fold, on the I/O pool up to `gc_io_concurrency` at a time, and answered before the row is
    /// written: only a confirmed create may record `pending`. Once a create throws, the round fails with that
    /// error, and `GcReadAhead::startCreate` sends none of the creates not started yet.
    const auto create_request = [this, layout_ptr = &layout](const BlobRef & ref, const CanonicalIdentity & identity)
    {
        return [this, layout_ptr, ref, identity](CasOperation & worker) -> IntentCreateResult
        {
            IntentCreateResult created;
            try
            {
                created = createBlobIntent(worker, *layout_ptr, ref, identity, Retry::standard());
            }
            catch (...)
            {
                LOG_WARNING(logger,
                    "CAS gc: the deletion-intent create of blob {} failed; the round fails before its commit and sends no "
                    "further create: {}", blobIdOf(ref), getCurrentExceptionMessage(false));
                throw;
            }
            if (created.outcome == IntentCreate::Created)
                ProfileEvents::increment(ProfileEvents::CASGCIntentCreated);
            else if (created.outcome == IntentCreate::ConfirmedExisting)
                ProfileEvents::increment(ProfileEvents::CASGCIntentConfirmedExisting);
            else
                ProfileEvents::increment(ProfileEvents::CASGCIntentUnconfirmedCarry);
            return created;
        };
    };
    IntentCreates create_intent;
    create_intent.start = [&reads, &create_request](const BlobRef & ref, const CanonicalIdentity & identity)
    {
        return reads.startCreate(create_request(ref, identity));
    };
    create_intent.window = store->poolConfig().gc_io_concurrency;

    const uint64_t new_generation = state.snap_generation + 1;
    /// The fold mints THIS round's attempt id from `lease.seq` (the renew/steal paths bump it every
    /// round; a rebuild's give-back resets it to 0). EVERY fold-artifact WRITE below lands under this
    /// attempt; the PARENT-generation READS keep using `state.snap_attempt` (the attempt the prior round
    /// adopted). The fold-adopt CAS #1 then commits `(new_generation, attempt)` together — a deposed
    /// leader's fold lands under its own unadopted attempt and is invisible to every reader.
    const uint64_t attempt = state.lease.seq;
    result.fold_seal.generation = new_generation;
    result.fold_seal.parent_generation = state.snap_generation;

    std::vector<BlobDelta> deltas;
    /// PROBE B2's round-local ledger (see `TxnApplyLedger`). Grows one entry per ref log the intake
    /// opens; the reducers mark `applied` through `&ledger.applied` at the point they consume a delta.
    TxnApplyLedger ledger;
    bool folded_any = false;

    /// PROBE B1 -- intake-layer identity. `logs_applied` counts, at the SINGLE cursor-advance site
    /// below, every log whose whole body folded. At seal time it is compared against a recomputation
    /// from the sealed coverage and the listing. The two are derived differently (a running counter vs
    /// a recomputation), so a control-flow bug that advances a cursor without folding breaks the
    /// equality.
    ///
    /// REACH, stated so this is not over-trusted: the recomputation reads the SAME listing the intake
    /// read, so B1 is BLIND to a record missing from that listing. It is a control-flow assertion, not
    /// a detector for the skipped-transaction defect -- probe A covers the listing, probe B2 covers
    /// everything below the intake.
    uint64_t logs_applied = 0;

    /// The adopted parent seal, read once at the ADOPTED (snap_generation, snap_attempt): it carries the
    /// parent generation's `blob_target_runs` (resolved below into per-gc-shard prior runs) and the parent
    /// `condemned_summary` (the pure-carry decision). A completed round leaves its fold seal there; a fresh
    /// pool has none (empty seal). Under the snapshot+log ref model there is no per-shard token-diff Skip:
    /// the "did this table change" signal is simply whether the global LIST returned any log id above the
    /// table's durable cursor, which the per-table loop below tests directly.
    /// SECOND read of the key `adopted_seal` already holds: same generation, same attempt, same bytes
    /// (nothing between the two touches `state.snap_generation` / `snap_attempt` or writes that key).
    /// One redundant GET per folding round, and the round's FIFTH GET of this one key overall --
    /// `rowDecisionDue` and `listRefPrefix` make two in `defer_decision`, `parent_seal_read` a third,
    /// `adopted_seal` above a fourth. Recorded on this row, not fixed here.
    CasFoldSeal discover_ref_seal;
    if (const auto fold_seal = readFoldSeal(state.snap_generation, state.snap_attempt))
        discover_ref_seal = *fold_seal;
    seal_read_timer->metric("seal_reads", 2);
    seal_read_timer->metric("redundant_reads", 1);
    seal_read_timer->metric("parent_ref_lives", parent_ref_lives.size());
    seal_read_timer->metric("dropped_parent_ref_lives", dropped_parent_ref_lives);
    seal_read_timer->metric("parent_runs", discover_ref_seal.blob_target_runs.size());
    seal_read_timer->metric("parent_cleanup_evidence", std::count_if(
        discover_ref_seal.ref_lives.begin(), discover_ref_seal.ref_lives.end(),
        [](const auto & row) { return row.second.cleanup_evidence.has_value(); }));
    seal_read_timer.reset();   /// emits the `fold_seal_read` row

    /// Each fully-folded `remove_namespace` transaction earns terminal cleanup evidence.
    std::vector<std::pair<RootNamespace, RefTxnId>> new_removals;

    /// 2-3. Ref-log intake, along the CHAIN. For each table the walk reads exact keys in successor order
    /// (`refChainSuccessor`): the next record's id follows from the last one's body, so the round never
    /// asks the listing what to read. Each body is decoded+validated and every explicit
    /// owner-change folds into `foldManifestEdges` (which reads the manifest body and appends per-blob
    /// `BlobDelta`s to `deltas`). The durable cursor advances per FULLY folded log; a missing manifest body
    /// clamps this table below the log (barrier, re-read next round) while other tables keep folding.
    ///
    /// THE LISTING IS A HINT, and demoting it is the point of this loop. It used to be the source of
    /// truth for which records exist, so a store that omitted a durable key from an enumeration -- the
    /// observed `0x1430c`/`0x1430d` shape -- made the round skip those records' owner edges and then seal
    /// a cursor ABOVE them, which is unrecoverable (a record below the cursor is never re-read). Under
    /// chain intake such an omission is a NON-EVENT: the exact GET finds the record anyway. The hint
    /// keeps two jobs here:
    ///   (a) the WITNESS set that makes an absent key decidable:
    ///         absent, no listed id above it  => this namespace's frontier this round (normal end);
    ///         absent, a listed id above it   => impossible on the chain, so the store is lying or a
    ///                                           durable record was lost: HOLD the namespace at
    ///                                           classification `Clamped` with its cursor unmoved;
    ///   (b) the bound of the read-ahead inside an epoch.
    ///
    /// An epoch is crossed only through a tombstone, to the successor its body names (`{next_epoch,1}`,
    /// or `{E+1,1}` for one without the field), and the head there must link back to it
    /// (`validateRefChainLink`). A cursor of unknown kind resumes by `resolveRefCursorSuccessor`.
    ///
    /// PER-NAMESPACE FAILURES ARE PER-NAMESPACE: an unreadable or undecodable body belongs to
    /// exactly one namespace and clamps only it. The whole-round abort survives ONLY for a key that cannot
    /// be attributed to any namespace at all (`groupRefKeys` above), which is why nothing in this loop
    /// sets `ref_folding_aborted` anymore.
    ///
    /// Phase `fold_ref_intake`: one GET per record (always owed -- the round read every body anyway),
    /// plus the exact 404 probes that fix each namespace's stop -- one for a quiet namespace, up to about
    /// `1 + log2(m + 1)` for one `m` records past its listed tail -- the reads that resolve a cold cursor,
    /// and one GET per manifest edge, which on a busy pool is where the round's object-read budget goes.
    /// It also carries probe B1's two numbers -- reported on EVERY healthy round, so
    /// "logs_accounted always equals logs_applied" becomes an observable property of the table rather than
    /// a claim in a comment.
    ///
    /// THE REQUESTS ARE THE SAME ONES; ONLY THEIR TIMING MOVED. Four sites below hand their keys to the
    /// round's `GcReadAhead` before the walk reaches them -- the checkpoints, each namespace's first walk
    /// position, this epoch's next positions, and a decoded log's manifest edges -- so the phase's round
    /// trips overlap instead of running strictly one after another. Every take happens where the inline
    /// read happened, in the same order, and increments the same counters, so this row's semantic metrics
    /// do not depend on `gc_io_concurrency` unless the slice cuts the walk: pass 1 reads one read-ahead
    /// window, whose size follows `gc_io_concurrency`. Its S3 VERB counts are not: a request
    /// a worker performed lands on that worker's ProfileEvents. Read `CASGCReadAheadHit`/`Miss`/`Wasted` on this row for the read-ahead's own behaviour.
    std::optional<GcPhaseTimer> intake_timer;
    intake_timer.emplace(phase_sink, "fold_ref_intake");
    /// The slice of `kRefIntakeBudgetMs` that bounds pass 2 of the walk below. It starts before planning, so
    /// planning time is taken from the walk.
    const uint64_t intake_deadline_ms = mono_ms_fn() + kRefIntakeBudgetMs;
    uint64_t intake_tables_changed = 0;
    uint64_t intake_tables_clamped = 0;
    uint64_t intake_tables_held = 0;
    uint64_t intake_dead_precommits_skipped = 0;
    uint64_t intake_absent_probes = 0;
    uint64_t intake_epoch_crossings = 0;
    uint64_t intake_cursor_resolves = 0;
    uint64_t intake_cursor_resolve_ns = 0;

    /// Probe B1's raw material: per namespace, the CONTIGUOUS runs of ids this round walked, one per epoch
    /// entered, recorded as `[first, last]`. The recomputation below turns them back into a count and
    /// compares it with the counter the advance site incremented -- so a cursor that moved over a position
    /// nothing applied inflates the first number and fails the round closed.
    std::map<String, std::vector<std::pair<RefTxnId, RefTxnId>>> walked_segments;

    /// The round's SECOND witness source, independent of the listing -- see `readCheckpointWitnesses`
    /// for what it decides and why a listing alone cannot decide it. Its `undecodable` half names the
    /// namespaces whose `_ckpt` is present and unreadable; each of those is HELD below, and only those.
    const CheckpointWitnesses checkpoints = readCheckpointWitnesses(reads, ref_tables, catalog_snapshot);
    reads.closeApply();
    const std::map<String, RefTxnId> & checkpoint_witness = checkpoints.witnesses;

    /// WHICH NAMESPACES THIS ROUND WALKS -- i.e. THE ROUND'S UNIVERSE, the set the destructive gate owes
    /// a frontier proof for (`UniversePolicy` says why only the catalog can bound it): EVERY
    /// `Live`/`Removing` row of this round's frozen catalog cut, and nothing else.
    ///
    /// The listing is a HINT and cannot change that set in either direction. It cannot SHRINK it -- a
    /// store that goes quiet about a namespace, or stops listing one to clear its hold, leaves the
    /// catalog row and therefore the obligation untouched -- and it cannot GROW it either, since a
    /// physical id the catalog does not name is inert debris rather than a namespace to prove.
    ///
    /// A namespace with no hint entry walks against an EMPTY listing: it has no hint witnesses, but it
    /// still reads its next key by exact key, whose absence IS the frontier proof and whose PRESENCE
    /// means the namespace was wrongly quiet and gets walked properly this round. A
    /// carried hold additionally supplies the witness that keeps an absent below its position from
    /// reading as a frontier.
    ///
    /// COST: held namespaces are always walked (their retry is a liveness obligation, not a budgeted
    /// nicety); the merely-QUIET ones are bounded by `gc_frontier_probe_budget`, and the ones the budget
    /// does not reach are simply unproven, which suppresses the round's destruction. In a healthy pool
    /// that budget is rarely touched; dead physical lives are excluded by the catalog-built plan.

    /// WHAT MAKES A ROUND END.
    ///
    /// The walk reads by exact key, so on its own it walks WHILE RECORDS EXIST -- and a namespace whose
    /// writer appends concurrently has no last record to reach. Round time would be
    /// `backlog / (walker_rate - writer_rate)`, which diverges the moment a writer keeps up with the
    /// walker; then the fold seal, its cursors and ref-object cleanup stop too.
    ///
    /// Two things end it. The STOP: before the first manifest read, `planRefStops` fixes each life's end
    /// by exact `GET`s, and the walk ends at that key or at a terminal record without another request;
    /// nothing above it is folded this round even if it lands later. The SLICE (`kRefIntakeBudgetMs`):
    /// the slice never cuts pass 1, which gives every namespace one read window, so a round not asked to stop
    /// proves every quiet namespace; pass 2 continues the rest from `round mod n` and starts no read once the slice
    /// is over.
    /// A namespace the slice cuts keeps the cursor it reached and is unproven, which suppresses the
    /// round's destruction but not its fold.
    ///
    /// THE PROOF IS A READ. A quiet namespace costs one exact `GET` of its successor because that read
    /// IS the frontier proof: an unprovable namespace leaves `frontier_complete` false forever, which
    /// stops ref-object cleanup, so its listing never drains and it never becomes provable by any other
    /// route either.
    ///
    /// The listed tail bounds no walk; it is CLASSIFIED for the phase row, in three classes:
    ///   * `tail > cursor` -- the round has records to fold;
    ///   * `tail == cursor` -- the listing shows nothing new: a warm walk reads only its successor probe,
    ///     a cold one resolves its cursor first. On a wide pool this is most namespaces, most rounds;
    ///   * `tail < cursor` -- the listing's greatest id is BELOW a cursor we folded through. Cleanup
    ///     deletes logs from the bottom up, so a listed log below the cursor with none at or above it is
    ///     a stale or lying listing, not a shape cleanup produces. The cursor is the truth (the sampled
    ///     store-quality detector is this class's observer).
    struct WalkTarget
    {
        String ns;
        UInt128 life_id{};
        const RefTableListing * listing;
    };

    static const RefTableListing kNoListing;
    std::vector<WalkTarget> walk_targets;
    /// The three buckets classify the hinted, UNHELD namespaces -- the ones whose tail comparison
    /// decided how much they could fold. A held namespace folds up to a bound its HOLD also determines
    /// and is reported by `tables_held`, so counting it here would make `tails_unchanged` stop meaning
    /// "namespaces with no work this round", which is the number the row exists to publish.
    uint64_t intake_tails_advanced = 0;
    uint64_t intake_tails_unchanged = 0;
    uint64_t intake_tails_below_cursor = 0;
    uint64_t intake_unhinted_held = 0;
    uint64_t intake_unhinted_quiet = 0;
    uint64_t frontier_probe_budget = store->poolConfig().gc_frontier_probe_budget;
    uint64_t intake_unprobed_budget = 0;
    uint64_t intake_catalog_only = 0;
    for (const auto & [catalog_ns_str, catalog_incarnation] : live_incarnation)
    {
        const auto listing_it = ref_tables.find(catalog_ns_str);
        const RefTableListing * listing = listing_it != ref_tables.end() ? &listing_it->second : &kNoListing;
        const auto parent_it = parent_ref_lives.find(catalog_incarnation);
        const RefCoverage * parent_cov = parent_it != parent_ref_lives.end() ? &parent_it->second.coverage : nullptr;

        if (listing_it == ref_tables.end() && parent_cov)
        {
            if (parent_cov->hold)
                ++intake_unhinted_held;
            else if (checkpoints.recovery_checkpoints.contains(catalog_ns_str))
                ++intake_unhinted_quiet;
            else if (frontier_probe_budget == 0)
            {
                ++intake_unprobed_budget;
                continue;
            }
            else
            {
                --frontier_probe_budget;
                ++intake_unhinted_quiet;
            }
        }
        else if (listing_it == ref_tables.end())
            ++intake_catalog_only;

        walk_targets.push_back({catalog_ns_str, catalog_incarnation, listing});
        if (listing->logs.empty() || (parent_cov && parent_cov->hold))
            continue;

        const RefTxnId tail = listing->logs.back();
        const RefTxnId cursor = parent_cov ? parent_cov->last_folded_ref_id : RefTxnId{};
        if (cursor < tail)
            ++intake_tails_advanced;
        else if (cursor == tail)
            ++intake_tails_unchanged;
        else
            ++intake_tails_below_cursor;
    }

    /// Memo entries of lives that left the catalog cut can never match a durable cursor again.
    {
        std::set<UInt128> live_lives;
        for (const auto & [live_ns, incarnation] : live_incarnation)
            live_lives.insert(incarnation);
        std::erase_if(ref_fold_memo, [&](const auto & entry) { return !live_lives.contains(entry.first); });
    }

    /// ONE NAMESPACE'S WALK, kept across the two passes of the slice.
    struct NamespaceIntake
    {
        NamespaceIntake(const WalkTarget & target_, NamespaceLifeId life_) : target(&target_), life(std::move(life_)) {}

        const WalkTarget * target;
        NamespaceLifeId life;
        RefTxnId cursor;
        std::optional<RefHold> carried_hold;
        std::optional<RefCkpt> checkpoint;
        std::optional<RecoveryGrounding> grounding;
        /// The cursor is nonzero and the memo does not name it: planning resolves its successor first.
        bool resolve_start = false;
        /// The next key on the chain. Unset once the walk ended: at an absent key, at a terminal record, or
        /// at a hold. Still set at the end of the round means the slice cut it.
        std::optional<RefTxnId> expected;
        /// The tombstone the walk last crossed, while `expected` is its successor.
        std::optional<RefEpochTransition> open_transition;
        /// The key the walk read absent, when that is how it ended.
        std::optional<RefTxnId> ended_absent_at;
        /// The hold THIS round detected, at the position it stopped. It IS the clamp signal: there is
        /// no separate boolean that could disagree with it about whether the namespace stopped.
        std::optional<RefHold> fired;
        RefTxnId resolved_through;   /// advances per fully-folded log; a clamp keeps it below the log
        bool table_changed = false;
        /// THE FRONTIER PROOF: the walk read an absent key with no witness above it, or a terminal record.
        bool frontier_proven = false;
        /// Which exit left this namespace unproven, for the round's deficit tally. It starts at
        /// `Unattributed` so that an exit which forgets to name itself is reported as such.
        FoldResult::FrontierUnproven unproven_reason = FoldResult::FrontierUnproven::Unattributed;
        /// Probe B1's per-epoch contiguous run: opened at the first position folded in an epoch, closed
        /// when the walk leaves that epoch or stops.
        std::optional<RefTxnId> segment_first;
        std::vector<std::pair<RefTxnId, RefTxnId>> segments;
        /// Read-ahead hinted in `expected`'s epoch and not yet taken, in key order.
        std::deque<std::pair<RefTxnId, String>> lookahead;
        /// Empty until planning ran: the walk then reads nothing.
        RefStopPlan plan;
    };

    /// The witness role: the smallest id above the absent `id` that SOMETHING says exists. An absent
    /// key means "frontier" without a witness and "impossible shape" with one, so the witness set
    /// decides whether the walk stops quietly or holds. Sources, none of them authoritative alone:
    ///   * the hint, which may omit durable records;
    ///   * `_ckpt.checkpoint_snapshot_id`: the snapshot's own log is on the chain and cleanup keeps it;
    ///   * `committed_through` of a generation 1 or 2 `_ckpt`, which counts AT `id` too: that body says
    ///     the log at its frontier is durable;
    ///   * the CARRIED HOLD's offending position, durable proof that a previous round reached that
    ///     position -- except `CheckpointUndecodable`, minted before the walk reads anything at a label
    ///     the strict comparison below never lets witness against a position this walk goes on to read.
    /// The SMALLEST wins, because the nearest witness is the one that decides the hold's reason.
    const auto witnessAbove = [&](const NamespaceIntake & s, const RefTxnId & id) -> std::optional<RefTxnId>
    {
        std::optional<RefTxnId> nearest;
        const auto consider = [&](const RefTxnId & w)
        {
            if (id < w && (!nearest || w < *nearest))
                nearest = w;
        };
        if (s.grounding && s.grounding->committed_through && *s.grounding->committed_through == id)
            nearest = id;
        const std::vector<RefTxnId> & listed = s.target->listing->logs;
        const auto it = std::upper_bound(listed.begin(), listed.end(), id);
        if (it != listed.end())
            consider(*it);
        if (const auto ck = checkpoint_witness.find(s.target->ns); ck != checkpoint_witness.end())
            consider(ck->second);
        if (s.grounding && s.grounding->committed_through)
            consider(*s.grounding->committed_through);
        if (s.carried_hold)
            consider(s.carried_hold->offending_position);
        return nearest;
    };

    /// The label of a hold above the cursor. Never read back, so it saturates at the maximum sequence
    /// rather than wrapping.
    const auto label_after = [](const RefTxnId & cursor)
    {
        if (cursor.ref_sequence == std::numeric_limits<uint64_t>::max())
            return cursor;
        return RefTxnId{cursor.writer_epoch, cursor.ref_sequence + 1};
    };

    /// Record the position an impossible shape was detected at, hold the namespace, and stop walking
    /// it. The reason is a BOUNDED enum because it is persisted in the seal: an operator reading a
    /// held row learns what stopped the namespace and exactly where, without correlating logs. The hold
    /// logs one line; `detail` is the cause the caller observed.
    const auto hold = [&](NamespaceIntake & s, const RefTxnId & at, HoldReason reason, const char * message,
                          std::string_view detail = {})
    {
        const String & ns_str = s.target->ns;
        const RootNamespace & ns = s.life.ns;
        const String life_id = renderIncarnation(s.life.incarnation);
        report.recordAnomaly(ns, 0, ManifestId{ns, {}}, message);
        EventEmitter{*store}.emit([&](CasEvent & ev)
        {
            ev.type = CasEventType::GcFoldClamp;
            ev.namespace_ = ns_str;
            ev.object_kind = CasEventObjectKind::Root;
            ev.outcome = "held";
            ev.reason = message;
            ev.detail = {{"expected", renderRefTxnId(at)},
                         {"resolved_through",
                          s.resolved_through == RefTxnId{} ? "none" : renderRefTxnId(s.resolved_through)},
                         {"life", life_id}};
        });
        LOG_ERROR(logger,
            "CAS GC ref intake: namespace {} (life {}) HELD at {} -- {}{}{}. The cursor stays at {} and this "
            "namespace folds nothing further this round.",
            ns_str, life_id, renderRefTxnId(at), message, detail.empty() ? "" : ": ", detail,
            s.resolved_through == RefTxnId{} ? "none" : renderRefTxnId(s.resolved_through));
        s.fired = RefHold{.reason = reason, .offending_position = at,
                          .retry_count = 0, .next_retry_round = 0};   /// retry fields filled in at the seal below
        s.expected.reset();
    };

    const auto closeSegment = [](NamespaceIntake & s)
    {
        if (s.segment_first)
            s.segments.emplace_back(*s.segment_first, s.resolved_through);
        s.segment_first.reset();
    };

    /// Waits for the reads already sent, so a stop request returns only after they are answered.
    const auto dropLookahead = [&](NamespaceIntake & s)
    {
        for (const auto & [id, key] : s.lookahead)
            reads.discardRead(key);
        s.lookahead.clear();
    };

    /// Bodies planning read, which the walk takes instead of reading them again.
    RetainedRefBodies retained_ref_bodies(ref_plan_retained_bytes);

    /// READ-AHEAD of the hint's ids in `expected`'s epoch, above `expected` and at or below the greatest
    /// sequence planning read present there, skipping bodies planning kept. Only an id the hint names is
    /// fetched speculatively: a key written after the hint was built is taken inline, and nothing past the plan
    /// is read ahead. `budget` is how many more records the walk may take: neither the count of hints nor their
    /// distance from `expected` exceeds it, so a walk that pauses leaves no fetched result it would fetch again.
    const auto topUpLookahead = [&](NamespaceIntake & s, size_t budget)
    {
        const size_t limit = std::min<size_t>(reads.window(), budget);
        if (limit == 0)
            return;
        const RefTxnId at = *s.expected;
        const auto present = s.plan.last_present.find(at.writer_epoch);
        if (present == s.plan.last_present.end())
            return;
        const RefTxnId bound{at.writer_epoch, std::min(present->second, saturatingAdd(at.ref_sequence, budget))};
        const RefTxnId after = s.lookahead.empty() ? at : s.lookahead.back().first;
        const std::vector<RefTxnId> & hinted = s.plan.hint_ids;
        for (auto it = std::upper_bound(hinted.begin(), hinted.end(), after);
             it != hinted.end() && *it <= bound && s.lookahead.size() < limit; ++it)
        {
            String key = layout.refLogKey(s.life, *it);
            if (retained_ref_bodies.contains(key))
                continue;
            reads.hintRead(key, ReadFamily::RefLog);
            s.lookahead.emplace_back(*it, std::move(key));
        }
    };

    /// SETUP, without I/O: the authority each walk starts from, and where it starts when that is known
    /// without a read.
    std::vector<NamespaceIntake> intakes;
    intakes.reserve(walk_targets.size());
    for (const WalkTarget & target : walk_targets)
    {
        if (ref_folding_aborted)
            break;
        const String & ns_str = target.ns;
        const RefTableListing & listing = *target.listing;
        const RootNamespace ns{ns_str};
        /// Every walk target came out of this round's own catalog read, so its incarnation is the REAL
        /// one and the life below is minted from a catalog entry rather than guessed from a key.
        NamespaceIntake & s = intakes.emplace_back(target, NamespaceLifeId::fromCatalogEntry(ns, target.life_id));

        /// Parent cursor = the durable last_folded_ref_id this table folded to (absent => {0,0}).
        ///
        /// Id ordering does NOT subsume remove+recreate: a namespace removed and recreated restarts its
        /// ids at or below a cursor sealed for the PREVIOUS life, so its edges would never fold and the
        /// recreated refs' manifests would look unreferenced. That is closed STRUCTURALLY, not by
        /// comparing ids: this map and the walk targets are both keyed by CATALOG INCARNATION, so a
        /// rebirth is a different key and inherits no cursor, and a life the catalog no longer names
        /// contributes no walk target and therefore re-carries no cursor into the new seal. Do not "fix"
        /// it by comparing ids.
        const auto cursor_it = parent_ref_lives.find(target.life_id);
        s.cursor = cursor_it != parent_ref_lives.end() ? cursor_it->second.coverage.last_folded_ref_id : RefTxnId{};
        s.resolved_through = s.cursor;

        /// Baseline guard: a table with NO sealed cursor whose logs at/below its
        /// newest snapshot have all been cleaned means a prior fold advanced+cleaned them and then gc/state
        /// was lost -- folding from {0,0} would miss those edges and mass-condemn their blobs. Fail closed;
        /// recover with the explicit rebuild. A fresh table (writer already snapshotted, logs still present)
        /// passes because its logs at or below the snapshot survive.
        if (cursor_it == parent_ref_lives.end() && !listing.snapshots.empty()
            && !checkpoints.recovery_checkpoints.contains(ns_str))
        {
            const RefTxnId newest_snapshot = listing.snapshots.back();
            const bool logs_below_snapshot_gone = listing.logs.empty() || newest_snapshot < listing.logs.front();
            if (logs_below_snapshot_gone)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS GC baseline guard: table {} has snapshot {} but no surviving log at or below it and "
                    "no sealed fold cursor -- gc/state was lost after cleaning covered logs. {}",
                    ns_str, renderRefTxnId(newest_snapshot), kCorruptBookkeeping);
        }

        /// The hold the PARENT seal left on this namespace, if any. It is three things at once: the
        /// position this round must retry, a durable witness (see `witnessAbove`), and the hold that
        /// rides forward unless this round resolves that position.
        s.carried_hold = cursor_it != parent_ref_lives.end() ? cursor_it->second.coverage.hold : std::nullopt;

        /// Use the same frozen catalog row + decoded checkpoint authority as read-only recovery. The
        /// ref LIST is only a hint; it may neither choose this life's genesis nor end its chain.
        const auto catalog_entry_it = std::lower_bound(
            catalog_snapshot.catalog.entries.begin(), catalog_snapshot.catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        chassert(catalog_entry_it != catalog_snapshot.catalog.entries.end());
        chassert(catalog_entry_it->ns == ns);
        chassert(catalog_entry_it->incarnation == target.life_id);
        if (const auto checkpoint_it = checkpoints.recovery_checkpoints.find(ns_str);
            checkpoint_it != checkpoints.recovery_checkpoints.end())
            s.checkpoint = checkpoint_it->second.ckpt;

        String checkpoint_failure;
        if (const auto bad_checkpoint = checkpoints.undecodable.find(ns_str);
            bad_checkpoint != checkpoints.undecodable.end())
        {
            checkpoint_failure = bad_checkpoint->second;
        }
        else
        {
            try
            {
                s.grounding = chooseRecoveryGrounding(std::optional<CatalogEntry>{*catalog_entry_it}, s.checkpoint);
            }
            catch (const Exception & e)
            {
                checkpoint_failure = e.message();
            }
        }

        /// An unavailable or invalid `_ckpt` quarantines its own namespace and nothing else. The object
        /// belongs to exactly one namespace and both of its consumers are keyed by that namespace --
        /// `witnessAbove` below, and `cleanupRefObjects`' delete boundaries via `result.checkpoints` --
        /// so the damage is confinable, and confining it is the difference between one namespace waiting
        /// for a repair and the whole pool's cursors, seals and cleanup stopping on one 4 KiB object.
        ///
        /// FOLD NOTHING FOR IT, rather than fold and merely refuse the frontier proof. The cursor
        /// advance is the one irreversible thing the walk does (nothing ever re-reads below it), and
        /// this namespace has just proved that a piece of its own durable state cannot be read.
        ///
        /// The hold sits at `cursor + 1`, a durable label that is never read. A never-folded namespace
        /// has no cursor to label; the anomaly alone carries it. The other consumer,
        /// `cleanupRefObjects`, does not run this round: the hold or anomaly recorded here sets
        /// `suppress_destructive`.
        if (!s.grounding)
        {
            if (s.cursor != RefTxnId{})
                hold(s, label_after(s.cursor), HoldReason::CheckpointUndecodable,
                     "ref intake: the namespace's `_ckpt` is unavailable or invalid -- its durable "
                     "checkpoint authority cannot be read, so nothing above the cursor is accountable",
                     checkpoint_failure);
            else
            {
                LOG_WARNING(logger,
                    "CAS GC ref intake: namespace {} has no usable checkpoint -- {}. This namespace "
                    "folds nothing and reclaims nothing until that object is repaired; every other "
                    "namespace folds normally.",
                    ns_str, checkpoint_failure);
                report.recordAnomaly(ns, 0, ManifestId{ns, {}},
                    "ref intake: the catalog life has no usable `_ckpt` (no authoritative walk position)");
            }
            s.unproven_reason = FoldResult::FrontierUnproven::CheckpointUnusable;
            continue;
        }

        /// The first key: `{life_epoch,1}` on the first fold, the remembered successor of an unchanged
        /// cursor, or else what `resolveRefCursorSuccessor` proves when planning starts on this life.
        if (s.cursor == RefTxnId{})
            s.expected = RefTxnId{*s.checkpoint->life_epoch, 1};
        else if (const auto memo_it = ref_fold_memo.find(target.life_id);
                 memo_it != ref_fold_memo.end() && memo_it->second.cursor == s.cursor)
        {
            s.expected = memo_it->second.successor_key;
            s.open_transition = memo_it->second.open_transition;
        }
        else
            s.resolve_start = true;
    }

    /// FIRST WALK POSITIONS, hinted in planning order a window deep: only the ones setup already knows. A cold
    /// cursor's `cursor + 1` is never hinted, because after a tombstone that key may be one cleanup
    /// emptied and a late create refilled.
    std::vector<String> first_walk_keys;
    first_walk_keys.reserve(intakes.size());
    for (const NamespaceIntake & s : intakes)
        if (s.expected)
            first_walk_keys.push_back(layout.refLogKey(s.life, *s.expected));
    size_t next_first_walk_hint = 0;
    const auto topUpFirstWalkHints = [&]
    {
        while (next_first_walk_hint < first_walk_keys.size() && reads.pending() < reads.window())
            reads.hintRead(first_walk_keys[next_first_walk_hint++], ReadFamily::RefLog);
    };

    /// Planning resolves where a cold cursor resumes. A cursor below the snapshot id is never read: cleanup
    /// may have emptied its key and a late create refilled it.
    const auto resolveStart = [&](NamespaceIntake & s)
    {
        s.resolve_start = false;
        const UInt64 resolve_start_ns = intake_clock_ns();
        const RefCursorResume resume = resolveRefCursorSuccessor(
            op, layout, s.life, s.cursor, s.checkpoint->checkpoint_snapshot_id);
        ++intake_cursor_resolves;
        intake_cursor_resolve_ns += intake_clock_ns() - resolve_start_ns;
        intake_absent_probes += resume.absent_probes;
        ProfileEvents::increment(ProfileEvents::CASRefLogBodyGets, resume.body_gets);
        switch (resume.kind)
        {
            case RefCursorResumeKind::Key:
                s.expected = resume.next_key;
                s.open_transition = resume.open_transition;
                return;
            case RefCursorResumeKind::Terminal:
                /// The cursor's own record, read at or above the snapshot id where cleanup never deletes.
                s.frontier_proven = true;
                return;
            case RefCursorResumeKind::Hold:
                hold(s, label_after(s.cursor), HoldReason::UnconsumedSealCrossing,
                     "ref intake: the successor of the sealed cursor cannot be proved from the chain", resume.detail);
                return;
        }
    };

    /// What planning did this round.
    struct RefPlanningCounters
    {
        uint64_t lives = 0;
        uint64_t hot = 0;
        uint64_t empty_hint = 0;
        uint64_t crossings = 0;
        uint64_t crossing_limit = 0;
        uint64_t misses = 0;
        uint64_t retained_bytes = 0;
    };
    RefPlanningCounters planning;

    /// The walk reached the key planning read absent: the life's frontier, unless something witnesses a record
    /// at or above it.
    const auto endAtStop = [&](NamespaceIntake & s)
    {
        const RefTxnId absent = *s.expected;
        s.expected.reset();
        const auto witness = witnessAbove(s, absent);
        if (!witness)
        {
            s.frontier_proven = true;
            s.ended_absent_at = absent;
        }
        else if (*witness == absent)
            hold(s, absent, HoldReason::GapBelowWitness,
                 "ref intake: a checkpoint-committed ref log is absent -- its authoritative "
                 "frontier cannot be complete");
        else if (witness->writer_epoch == absent.writer_epoch)
            hold(s, absent, HoldReason::GapBelowWitness,
                 "ref intake: expected next id absent below a same-epoch witness -- "
                 "contiguity says this cannot happen, so a durable record is missing");
        else
            hold(s, absent, HoldReason::UnconsumedSealCrossing,
                 "ref intake: a later epoch's records are witnessed but the chain ends below them -- "
                 "no record links to that epoch");
    };

    /// Pass 2 only, so a long walk is visible to an operator. It paces itself and never changes a decision.
    std::optional<uint64_t> progress_logged_ms;
    const auto logProgress = [&](const NamespaceIntake & s, uint64_t now_ms)
    {
        if (!progress_logged_ms)
        {
            progress_logged_ms = now_ms;
            return;
        }
        if (now_ms - *progress_logged_ms < kRefIntakeProgressIntervalMs)
            return;
        progress_logged_ms = now_ms;
        const size_t finished = std::count_if(intakes.begin(), intakes.end(), [](const NamespaceIntake & i) { return !i.expected; });
        LOG_INFO(logger,
            "CAS GC ref intake progress: round={} namespace={} applied={} stop={} logs_applied={} lives_finished={} lives={}",
            current_round, s.target->ns, s.resolved_through == RefTxnId{} ? "none" : renderRefTxnId(s.resolved_through),
            s.plan.stop ? renderRefTxnId(*s.plan.stop) : "none", logs_applied, finished, intakes.size());
    };

    /// Only the first life the walk ends past its plan is named in the log: one cause can end every life the same way.
    bool discovery_incomplete_logged = false;

    /// Once true, stays true for the rest of the intake: no further planning or walk step starts.
    bool stop_seen = false;
    const auto stopRequested = [&]
    {
        if (!stop_seen && stop_signal && stop_signal())
        {
            stop_seen = true;
            LOG_INFO(logger, "CAS GC ref intake: round {} was asked to stop; it deletes nothing, and every namespace it "
                     "has not finished stays unproven and keeps the cursor it reached", current_round);
        }
        return stop_seen;
    };

    /// Walk `s` in successor order to the stop its plan fixed: at most `take_limit` records when it is set,
    /// and with `sliced` no read once the slice is over. Ending at the stop reads nothing. A stop request ends
    /// the walk where it is, in either pass, as the end of the slice does.
    const auto walkNamespace = [&](NamespaceIntake & s, std::optional<size_t> take_limit, bool sliced)
    {
        const String & ns_str = s.target->ns;
        const RootNamespace & ns = s.life.ns;
        const NamespaceLifeId & life = s.life;
        std::optional<RefHold> & fired = s.fired;
        RefTxnId & resolved_through = s.resolved_through;
        size_t takes = 0;
        while (s.expected && (!take_limit || takes < *take_limit))
        {
            if (stopRequested())
                break;
            if (s.plan.isStop(*s.expected))
            {
                endAtStop(s);
                break;
            }
            /// Nothing this attempt observed bounds a key past the plan, so reading on would chase writers.
            if (!s.plan.covers(*s.expected))
            {
                if (!discovery_incomplete_logged)
                {
                    discovery_incomplete_logged = true;
                    LOG_INFO(logger,
                        "CAS GC ref intake: namespace {} (life {}) reached {}, past what planning observed; it stays "
                        "unproven this round and keeps the cursor {}",
                        ns_str, renderIncarnation(s.life.incarnation), renderRefTxnId(*s.expected),
                        resolved_through == RefTxnId{} ? "none" : renderRefTxnId(resolved_through));
                }
                s.expected.reset();
                s.unproven_reason = FoldResult::FrontierUnproven::DiscoveryIncomplete;
                break;
            }
            if (sliced)
            {
                const uint64_t now_ms = mono_ms_fn();
                if (now_ms >= intake_deadline_ms)
                    break;
                logProgress(s, now_ms);
            }

            /// Take + decode the expected record. An invalid body is a per-namespace hold: the key belongs
            /// to exactly one namespace, so it can never be grounds for discarding another namespace's fold.
            topUpLookahead(s, take_limit ? *take_limit - takes - 1 : std::numeric_limits<size_t>::max());
            const String expected_key = layout.refLogKey(life, *s.expected);
            std::optional<Object> got = retained_ref_bodies.take(expected_key);
            if (!got)
            {
                got = reads.takeRead(expected_key, ReadFamily::RefLog);
                if (got)
                    ProfileEvents::increment(ProfileEvents::CASRefLogBodyGets);
            }
            ++takes;
            if (!s.lookahead.empty() && s.lookahead.front().first == *s.expected)
                s.lookahead.pop_front();
            if (!got)
            {
                /// Planning read this key or a later one of its epoch present, so a record durable then is gone:
                /// lost history, never the end of the chain.
                ++intake_absent_probes;
                const RefTxnId absent = *s.expected;
                hold(s, absent, HoldReason::GapBelowWitness,
                     "ref intake: probed present, read absent -- a ref log planning saw at or above this "
                     "position is gone, so a durable record is missing");
                break;
            }
            const RefTxnId log_id = *s.expected;

            /// Probe B2: open this transaction's ledger entry. A clamped log is opened but never
            /// committed, so it is correctly not reported unapplied.
            const uint32_t txn_ordinal = ledger.open(ns, log_id);

            RefLogTxn txn;
            std::vector<RefManifestEdge> edges;
            std::optional<RefTxnId> successor;
            std::optional<RefEpochTransition> transition;
            /// An undecodable body, a broken chain link and an unrecognized owner_transition shape
            /// (`manifestEdgesOfTxn` -> `classifyOwnerTransitionShape`) all mean this log cannot be
            /// folded, so all get the same per-namespace hold. Do NOT widen this over
            /// `foldManifestEdges`: only intake shares this discipline.
            const char * invalid_reason = nullptr;
            String invalid_detail;
            try
            {
                txn = decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns_str, log_id);
            }
            catch (const Exception & e)
            {
                invalid_reason = "ref log body invalid: namespace held below it";
                invalid_detail = e.message();
            }
            if (!invalid_reason)
            {
                try
                {
                    validateRefChainLink(s.open_transition, txn);
                }
                catch (const Exception & e)
                {
                    invalid_reason = "ref intake: chain link invalid -- namespace held below it";
                    invalid_detail = e.message();
                }
            }
            if (!invalid_reason)
            {
                try
                {
                    edges = manifestEdgesOfTxn(txn);
                    successor = refChainSuccessor(txn);
                    transition = refEpochTransitionOf(txn);
                }
                catch (const Exception & e)
                {
                    invalid_reason = "ref log body invalid: namespace held below it";
                    invalid_detail = e.message();
                }
            }
            if (invalid_reason)
            {
                hold(s, log_id, HoldReason::BodyUndecodable, invalid_reason,
                     fmt::format("log {}: {}", expected_key, invalid_detail));
                break;
            }

            /// Fold every explicit manifest edge of the log. A transaction applies
            /// ATOMICALLY ("either the complete transaction applies or none of it applies"): stage this
            /// log's blob deltas and owner-removed manifest cleanup in PER-LOG buffers and merge them into
            /// the round buffers only once the WHOLE log folds. A mid-log clamp DISCARDS the staged buffers
            /// so the cursor stays coherent -- merging a partially folded log's `-1` cleanup would let the
            /// post-CAS body delete remove a body whose edge is still unfolded behind the clamp, and the
            /// re-fold would then clamp on that missing body forever. A missing manifest body is a per-table
            /// CLAMP (barrier), never a round abort: keep the cursor below THIS log and re-read it next
            /// round. A removed precommit whose body never existed emitted no edge -- skip, no clamp.
            ///
            /// The decode above named every manifest key this log folds, so they are fetched together
            /// here and taken one at a time in edge order below. A log with a single edge gains
            /// nothing; a merge or a mutation log with dozens turns dozens of serial round trips into
            /// one. A clamp mid-log leaves the rest of this log's bodies fetched and untaken, which is
            /// the bounded waste the read-ahead counts.
            for (const RefManifestEdge & edge : edges)
                reads.hintRead(layout.manifestKey(edge.manifest_id), ReadFamily::Manifest);

            std::vector<BlobDelta> log_deltas;
            std::map<ManifestId, Etag> log_mf_cleanup;
            for (const RefManifestEdge & edge : edges)
            {
                ProfileEvents::increment(ProfileEvents::CASRefEmittedEdges);   /// one manifest-edge event
                if (foldManifestEdges(reads, edge.manifest_id, edge.change, log_deltas, log_mf_cleanup,
                                      txn_ordinal))
                    continue;

                if (edge.change < 0 && edge.owner_kind == RefOwnerKind::Precommit)
                    continue;   /// removed precommit that never activated: no edge to mirror, no clamp

                /// A `+1` precommit whose body is absent normally holds the fold barrier (the writer may
                /// still be uploading it). But a precommit naming a build PROVABLY DEAD by the durable
                /// watermark floor -- the SAME fact the orphan sweep uses to reclaim the body -- can never
                /// activate, and its body will never return. Skip it (non-activating, advance the log) so
                /// the barrier is not held forever on a body no writer will complete; otherwise this table
                /// clamps every round with no terminal resolution. Fail-closed: no durable watermark (the
                /// build cannot be proven dead) keeps the barrier.
                if (edge.change > 0 && edge.owner_kind == RefOwnerKind::Precommit
                    && buildPrefixEligibleUnderFloor(*store, ns,
                           BuildPrefix{.writer_epoch = edge.manifest_id.ref.writer_epoch,
                                       .build_sequence = edge.manifest_id.ref.build_sequence}))
                {
                    ProfileEvents::increment(ProfileEvents::CASGCDeadPrecommitSkipped);
                    ++intake_dead_precommits_skipped;
                    EventEmitter{*store}.emit([&](CasEvent & ev)
                    {
                        ev.type = CasEventType::GcFoldClamp;   /// reuse the fold-decision channel; outcome distinguishes
                        ev.namespace_ = ns_str;
                        ev.object_kind = CasEventObjectKind::Root;
                        ev.object_hash = manifestRefDebugString(edge.manifest_id.ref);
                        ev.outcome = "dead_precommit_skipped";
                        ev.reason = "live precommit body absent AND its build is below the watermark floor "
                                    "(provably dead); skip the non-activating edge instead of clamping forever";
                        ev.detail = {{"log", renderRefTxnId(log_id)}};
                    });
                    continue;
                }

                const char * reason = edge.change > 0
                    ? (edge.owner_kind == RefOwnerKind::Precommit
                           ? "fold barrier: live precommit body not yet present (non-activating)"
                           : "committed/promoted ref names a missing manifest body")
                    : "owner-removal: edge-bearing committed body missing at removal-fold";
                report.recordAnomaly(ns, 0, edge.manifest_id, reason);
                EventEmitter{*store}.emit([&](CasEvent & ev)
                {
                    ev.type = CasEventType::GcFoldClamp;
                    ev.namespace_ = ns_str;
                    ev.object_kind = CasEventObjectKind::Root;
                    ev.object_hash = manifestRefDebugString(edge.manifest_id.ref);
                    ev.outcome = "clamped";
                    ev.reason = reason;
                    ev.detail = {{"log", renderRefTxnId(log_id)},
                                 {"resolved_through",
                                  resolved_through == RefTxnId{} ? "none" : renderRefTxnId(resolved_through)}};
                });
                /// The barrier is a HOLD like any other -- same durable shape, same clearing rule (fold
                /// through `log_id`), and the same suppression consequence. Its reason is the one whose
                /// ordinary cause is benign (a writer that appended its record before finishing the
                /// manifest upload), and naming it durably is what lets an operator tell that apart
                /// from a namespace that has been stuck for hours. The anomaly and event were already
                /// emitted above with the manifest identity, which `hold` cannot carry, so this site
                /// sets the hold directly instead of calling it.
                fired = RefHold{.reason = HoldReason::ManifestBodyMissing, .offending_position = log_id,
                                .retry_count = 0, .next_retry_round = 0};
                break;   /// stop folding this log's edges; the cursor stays at resolved_through (< log_id)
            }

            if (fired)
                break;   /// discard the staged log_deltas / log_mf_cleanup (never merged) and stop this table

            /// The whole log folded: merge its staged transaction into the round buffers.
            if (!log_deltas.empty())
                ledger.markProduced(txn_ordinal);
            ledger.markCommitted(txn_ordinal);
            for (BlobDelta & d : log_deltas)
                deltas.push_back(std::move(d));
            for (const auto & [mid, tok] : log_mf_cleanup)
                result.mf_cleanup.emplace(mid, tok);

            /// A fully-folded `remove_namespace` transaction hands its `{ns, remove_txn_id}` to the
            /// life row's cleanup evidence; its owner-removal edges were folded above.
            if (const auto removal = removalTxnId(txn))
                new_removals.emplace_back(ns, *removal);
            if (!s.segment_first)
                s.segment_first = log_id;   /// this epoch's contiguous run opens at the first applied id
            resolved_through = log_id;      /// this log fully folded
            ++logs_applied;                 /// probe B1: the SINGLE cursor-advance site
            s.table_changed = true;
            s.open_transition = transition;

            /// A terminal record ends the stream; no key after it is read. Its key lies above the cursor,
            /// where cleanup never deleted, so its only occupant is the chain's record.
            if (!successor)
            {
                s.frontier_proven = true;
                s.expected.reset();
                break;
            }
            if (successor->writer_epoch != log_id.writer_epoch)
            {
                ++intake_epoch_crossings;
                closeSegment(s);
                dropLookahead(s);
            }
            s.expected = *successor;
        }
        reads.closeApply();
        dropLookahead(s);
    };

    /// Each namespace's seal row, counters and memo, once its walk is over for this round.
    const auto finishNamespace = [&](NamespaceIntake & s)
    {
        const WalkTarget & target = *s.target;
        const String & ns_str = target.ns;
        const RootNamespace & ns = s.life.ns;
        const std::optional<RefHold> & fired = s.fired;
        const std::optional<RefHold> & carried_hold = s.carried_hold;
        const RefTxnId & resolved_through = s.resolved_through;
        bool & frontier_proven = s.frontier_proven;
        FoldResult::FrontierUnproven & unproven_reason = s.unproven_reason;
        const bool table_changed = s.table_changed;

        dropLookahead(s);
        if (s.expected || s.resolve_start)
            unproven_reason = FoldResult::FrontierUnproven::IntakeSliceCut;
        closeSegment(s);
        if (!s.segments.empty())
            walked_segments[ns_str] = std::move(s.segments);

        RefCoverage cov;
        cov.last_folded_ref_id = resolved_through;

        /// THE CLEARING RULE, and the only place it is decided. A hold clears by exactly one
        /// event: this walk RESOLVING the offending position -- folding through it and sealing a cursor
        /// at or above it. It never clears by observing another absent, because an absent is precisely
        /// what a lying store produces and precisely what made the hold necessary; and it never clears
        /// by the hint going quiet, because a quiet hint is not evidence about anything.
        ///
        /// Three cases, total and in order:
        ///   * this round detected a hold -- adopt it. It sits at or above the cursor, so any carried
        ///     hold at a HIGHER position is simply not reached yet and will be re-detected once this
        ///     one clears; a carried hold at a LOWER position was folded through, which is its
        ///     clearance.
        ///   * no new hold, but the walk stopped BELOW a carried hold's position -- it did not resolve
        ///     it, so the hold rides VERBATIM (same reason, same position).
        ///   * otherwise -- either there was no hold, or the walk folded through it. Cleared.
        std::optional<RefHold> effective;
        if (fired)
            effective = fired;
        else if (carried_hold && resolved_through < carried_hold->offending_position)
        {
            effective = carried_hold;
            /// The round produced no anomaly of its own for this namespace (the walk ended quietly),
            /// yet the namespace IS held: record one so this round's destructive work is suppressed on
            /// today's `anomalies`-based rule as well as on the hold set itself. Without it a hold
            /// carried through a quiet round would suppress nothing, which is the hole the carry exists
            /// to close.
            report.recordAnomaly(ns, 0, ManifestId{ns, {}},
                "ref intake: namespace still held below an unresolved position");
            EventEmitter{*store}.emit([&](CasEvent & ev)
            {
                ev.type = CasEventType::GcFoldClamp;
                ev.namespace_ = ns_str;
                ev.object_kind = CasEventObjectKind::Root;
                ev.outcome = "held";
                ev.reason = "carried hold: the offending position did not resolve this round";
                ev.detail = {{"expected", renderRefTxnId(carried_hold->offending_position)},
                             {"resolved_through",
                              resolved_through == RefTxnId{} ? "none" : renderRefTxnId(resolved_through)},
                             {"retry_count", std::to_string(carried_hold->retry_count)}};
            });
        }

        if (effective)
        {
            /// Retry bookkeeping. The count belongs to a POSITION, so it continues only while the hold
            /// stays at the same one; a hold that moved is a different stop and starts over. The count
            /// saturates rather than wrapping -- a wrapped counter would report a namespace stuck for
            /// four billion rounds as freshly held.
            const bool same_position = carried_hold
                && carried_hold->offending_position == effective->offending_position;
            effective->retry_count = same_position && carried_hold->retry_count < UINT32_MAX
                ? carried_hold->retry_count + 1
                : (same_position ? UINT32_MAX : 0);
            effective->next_retry_round = current_round + 1;
            cov.hold = effective;
            cov.classification = CoverageClass::Clamped;
            ++intake_tables_held;
            /// A held namespace is unproven BY DEFINITION -- the hold names a position the walk could
            /// not resolve, so everything at or above it is unaccounted. Stated here rather than left to
            /// the loop's control flow: a carried hold rides forward on a round whose own walk ended
            /// quietly, and that quiet end must not be mistaken for a proof.
            frontier_proven = false;
            unproven_reason = FoldResult::FrontierUnproven::Held;
        }
        else
            cov.classification = table_changed ? CoverageClass::Folded : CoverageClass::Unchanged;
        if (effective || !s.ended_absent_at)
            ref_fold_memo.erase(target.life_id);
        else
            ref_fold_memo[target.life_id] = RefFoldMemo{
                .cursor = resolved_through, .successor_key = *s.ended_absent_at, .open_transition = s.open_transition};

        result.fold_seal.ref_lives.at(target.life_id).coverage = cov;
        ++result.frontier_namespaces;
        if (frontier_proven)
            ++result.frontier_proven;
        else
            result.frontier_deficit.count(unproven_reason);
        if (table_changed)
        {
            folded_any = true;
            ++intake_tables_changed;
        }
        if (fired)
            ++intake_tables_clamped;
    };

    /// The slice never cuts PASS 1: every namespace gets one read window, which proves every quiet namespace in
    /// every round not asked to stop.
    const auto afterWalkHook = [&](uint8_t pass, const NamespaceIntake & walked, const NamespaceIntake * next)
    {
        if (round_point_hook_for_test)
            runRoundPointHookForTest({.point = RoundPointForTest::AfterNamespaceWalk, .pass = pass,
                                      .walked = walked.target->ns, .next = next ? next->target->ns : String{}});
    };

    /// PLANNING: every life that still has a key to read gets its stop before any manifest is read. A life whose
    /// planning a stop request ends keeps an empty plan and is not walked, like the lives after it.
    const std::function<bool()> planning_stop_requested = stopRequested;
    for (NamespaceIntake & s : intakes)
    {
        if (!s.expected && !s.resolve_start)
            continue;
        if (stopRequested())
            break;
        topUpFirstWalkHints();
        if (s.resolve_start)
            resolveStart(s);
        if (!s.expected)
            continue;
        const uint64_t absent_before = intake_absent_probes;
        std::optional<RefStopPlan> plan = planRefStops(reads, layout, s.life, *s.expected,
            hintFromListing(*s.target->listing), retained_ref_bodies, intake_absent_probes, planning_stop_requested);
        if (!plan)
            break;
        s.plan = std::move(*plan);
        ++planning.lives;
        planning.hot += s.plan.first_present;
        planning.empty_hint += s.plan.hint_ids.empty();
        planning.crossings += s.plan.crossings;
        planning.crossing_limit += s.plan.crossing_limit_reached;
        planning.misses += intake_absent_probes - absent_before;
    }
    reads.closeApply();
    planning.retained_bytes = retained_ref_bodies.keptBytes();
    LOG_INFO(logger,
        "CAS GC ref intake planned: round={} lives={} quiet={} hot={} empty_hint={} crossings={} crossing_limit={} "
        "planner_misses={} retained_bytes={}",
        current_round, planning.lives, planning.lives - planning.hot, planning.hot, planning.empty_hint,
        planning.crossings, planning.crossing_limit, planning.misses, planning.retained_bytes);

    if (round_point_hook_for_test)
        runRoundPointHookForTest(RoundPointForTest::AfterDiscovery);
    const size_t pass_one_takes = std::max<size_t>(reads.window(), 1);
    for (size_t i = 0; i < intakes.size(); ++i)
    {
        NamespaceIntake & s = intakes[i];
        walkNamespace(s, pass_one_takes, /*sliced=*/false);
        if (i + 1 < intakes.size())
            afterWalkHook(1, s, &intakes[i + 1]);
    }

    /// PASS 2 continues the unfinished walks from index `round mod n`, so the slice does not always end
    /// on the same namespaces, and starts no read once the slice is over.
    std::vector<NamespaceIntake *> unfinished;
    for (NamespaceIntake & s : intakes)
        if (s.expected)
            unfinished.push_back(&s);
    const auto pass_two_at = [&](size_t i) { return unfinished[(current_round + i) % unfinished.size()]; };
    if (round_point_hook_for_test && !intakes.empty())
        afterWalkHook(1, intakes.back(), unfinished.empty() ? nullptr : pass_two_at(0));
    for (size_t i = 0; i < unfinished.size(); ++i)
    {
        NamespaceIntake & s = *pass_two_at(i);
        walkNamespace(s, std::nullopt, /*sliced=*/true);
        afterWalkHook(2, s, i + 1 < unfinished.size() ? pass_two_at(i + 1) : nullptr);
    }
    /// The final intake poll. An empty catalog gives the walk nothing to poll, and a stop after the last walk would
    /// otherwise go unseen; either way the round must not delete.
    stopRequested();

    for (NamespaceIntake & s : intakes)
        finishNamespace(s);

    /// Namespaces the round KNOWS about but never probed, because the frontier-probe budget ran out
    /// before reaching them. Their cursors ride VERBATIM -- dropping a cursor because a round ran out of
    /// budget would hand the next round a namespace to re-fold from `{0,0}`, which is a far worse
    /// outcome than the unproven frontier this already is. They count toward the universe and NOT toward
    /// the proofs, which is what suppresses the round.
    ///
    /// THE DENOMINATOR IS THE SEALED SET, BY CONSTRUCTION. `frontier_namespaces` is incremented by the
    /// number of rows this loop actually ADDED to the seal, never by the count the skip loop predicted.
    /// The two are the same set under the same filters, so they agree -- but agreeing "because both
    /// filters were written the same way" is exactly the kind of coincidence that decays under editing,
    /// and `frontier_namespaces` is the denominator an operator (and the integration test) reads as THE
    /// universe. Deriving it from the seal removes the possibility of publishing a number that
    /// describes a different universe than the round sealed. The `chassert` states the equality so a
    /// future divergence surfaces in debug rather than silently; it is metric integrity, not a safety
    /// gate, because both paths suppress the round either way.
    if (intake_unprobed_budget > 0)
    {
        result.frontier_namespaces += intake_unprobed_budget;
        result.frontier_deficit.probe_budget += intake_unprobed_budget;
        LOG_WARNING(logger,
            "CAS GC ref intake: the frontier-probe budget ({}) ran out with {} known namespace(s) "
            "unprobed; their cursors ride unchanged and ALL destructive work is suppressed this round",
            store->poolConfig().gc_frontier_probe_budget, intake_unprobed_budget);
    }

    /// All-or-nothing: a malformed key/body anywhere aborts the round's ref folding. Discard
    /// every ref delta and cursor advance already accumulated, carry each table's parent cursor verbatim,
    /// and let the recorded anomaly suppress destructive work.
    if (ref_folding_aborted)
    {
        deltas.clear();
        ledger = TxnApplyLedger{};   /// the deltas are gone, so nothing can be unapplied
        result.mf_cleanup.clear();
        folded_any = false;
        /// An abort discards this round's walk, so it discards its proofs with it: nothing this round
        /// observed may be offered as a frontier proof to the destructive gate.
        result.frontier_proven = 0;
        result.frontier_deficit = FoldResult::FrontierDeficit{};
        result.frontier_deficit.fold_aborted = result.frontier_namespaces;
        for (const WalkTarget & target : walk_targets)
        {
            RefCoverage cov;
            cov.classification = CoverageClass::Unchanged;
            if (const auto pit = parent_ref_lives.find(target.life_id); pit != parent_ref_lives.end())
            {
                cov.last_folded_ref_id = pit->second.coverage.last_folded_ref_id;
                /// An abort discards this round's work; it does not resolve anything, so a hold it
                /// found in the parent seal rides forward untouched -- not even the retry count moves,
                /// because nothing was retried.
                if (pit->second.coverage.hold)
                {
                    cov.hold = pit->second.coverage.hold;
                    cov.classification = CoverageClass::Clamped;
                }
            }
            RefLifeFoldState & ref_life_state = result.fold_seal.ref_lives.at(target.life_id);
            ref_life_state.coverage = cov;
        }
    }

    /// PROBE B1's recomputation, taken HERE rather than at the seal write: every input it reads
    /// (`walked_segments`, the sealed ref-life coverage) is final as of this line, and nothing
    /// between here and the seal write touches any of them. Computing it inside the intake phase is what
    /// lets the `fold_ref_intake` row carry both numbers; the comparison and its fail-closed throw stay
    /// where they were, just before the seal write. Both stay 0 on a ref-folding abort -- that path
    /// discards every cursor advance and carries the parent cursors, so the identity does not apply.
    ///
    /// It counts the CUT ARITHMETICALLY, not by listed ids. Under arithmetic intake a listed-id count is
    /// not even the right question: a hint hole means a round legitimately applies records the listing
    /// never mentioned, so the old recomputation would report fewer logs than folded and fail every
    /// healthy round on a lying store.
    ///
    /// BE HONEST ABOUT WHAT IS LEFT. The old formula could disagree with reality because it was derived
    /// from a different source (the listing) than the counter. This one is derived from the runs the
    /// single advance site produced, so for the current code shape it is close to tautological, and B1's
    /// discriminating power went DOWN with this change rather than up. What it still asserts is worth
    /// keeping and is not free: THE SEALED CURSOR IS THE WALK'S CURSOR. The last run of each namespace is
    /// measured against the DURABLE ref-life coverage the next round will trust, not against the
    /// walk's own end, so a cursor sealed from anywhere other than this walk -- a stale carry, a mutated
    /// coverage row, a future edit that advances the cursor away from the advance site -- either fails
    /// the epoch/order check below or lands as a count that no longer matches `logs_applied`.
    logs_accounted_this_round = 0;
    logs_applied_this_round = 0;
    if (!ref_folding_aborted)
    {
        uint64_t logs_accounted = 0;
        for (const auto & [ns_str, segments] : walked_segments)
        {
            const UInt128 life_id = live_incarnation.at(ns_str);
            RefTxnId sealed{};
            if (const auto sit = result.fold_seal.ref_lives.find(life_id);
                sit != result.fold_seal.ref_lives.end())
                sealed = sit->second.coverage.last_folded_ref_id;

            for (size_t i = 0; i < segments.size(); ++i)
            {
                const auto & [first, last] = segments[i];
                /// The final run must end exactly where the seal says this namespace stopped, in the same
                /// epoch. A disagreement is a sealed cursor that did not come from this walk, so it fails
                /// closed here rather than travelling into the next round's baseline.
                const RefTxnId end = i + 1 == segments.size() ? sealed : last;
                if (end.writer_epoch != first.writer_epoch || end < first)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS GC fold: namespace {} sealed a cursor at {} that does not close the run it "
                        "walked (opened at {}). {}",
                        ns_str, end == RefTxnId{} ? "none" : renderRefTxnId(end), renderRefTxnId(first), kCorruptBookkeeping);
                logs_accounted += end.ref_sequence - first.ref_sequence + 1;
            }
        }
        logs_accounted_this_round = logs_accounted;
        logs_applied_this_round = logs_applied;
    }

    intake_timer->metric("logs_accounted", logs_accounted_this_round);
    intake_timer->metric("logs_applied", logs_applied_this_round);
    intake_timer->metric("deltas_emitted", deltas.size());
    intake_timer->metric("transactions_opened", ledger.txns.size());
    intake_timer->metric("tables_scanned", ref_tables.size());
    intake_timer->metric("tables_changed", intake_tables_changed);
    intake_timer->metric("tables_clamped", intake_tables_clamped);
    /// `tables_held` counts the coverage rows this round SEALED held -- the ones it detected plus the
    /// ones it carried. It is what `suppress_destructive` keys on, so a round that reclaims nothing has
    /// this column to explain itself. `unhinted_held_walked` is the subset the hint never mentioned:
    /// nonzero means the store stopped listing a namespace that is still held, which is the shape that
    /// used to clear a hold silently.
    intake_timer->metric("tables_held", intake_tables_held);
    intake_timer->metric("unhinted_held_walked", intake_unhinted_held);
    /// THE LISTED WORK, ON THE ROW THAT PAID FOR IT. `tails_advanced` is how many hinted namespaces
    /// had records to fold this round because their listed tail sat above their cursor -- the round's
    /// real work -- and `tails_unchanged` is how many listed nothing new, which on a wide pool with a few
    /// hot namespaces is most of them and is the number that explains a short round.
    /// `tails_below_cursor` is the anomaly of the three: cleanup deletes logs
    /// from the bottom up, so a listing whose greatest id is below a cursor we folded through is stale or
    /// lying, and a column that is normally 0 is what makes the pool where it is not stand out.
    intake_timer->metric("tails_advanced", intake_tails_advanced);
    intake_timer->metric("tails_unchanged", intake_tails_unchanged);
    intake_timer->metric("tails_below_cursor", intake_tails_below_cursor);
    /// THE FRONTIER OBLIGATION, on the row that explains a round which reclaimed nothing:
    /// `frontier_namespaces` is the round's universe (hint ∪ sealed cursors ∪ catalog `Live`/`Removing`
    /// entries), `frontier_proven` the part of it that reached an honest end-of-stream, and the
    /// remaining columns say where the rest went -- walked because the hint had gone quiet about them,
    /// not walked at all because the probe budget ran out, or walked ONLY because the catalog named a
    /// namespace neither the hint nor a carried cursor did (`catalog_only_walked` -- always 0 until a
    /// namespace is admitted with no listed objects and no sealed cursor yet).
    intake_timer->metric("frontier_namespaces", result.frontier_namespaces);
    intake_timer->metric("frontier_proven", result.frontier_proven);
    /// `catalog_entries` is the hot-scan catalog cut's own row count (every lifecycle state, `Creating`
    /// included), and `catalog_proved_empty` is the derived verdict the destructive gate's non-vacuity
    /// term consults. Together they let an operator tell a proved-empty `0/0` (success) apart from a
    /// `Creating`-only or otherwise unprovable `0/0` (still suppressed) without re-deriving either fact.
    intake_timer->metric("catalog_entries", catalog_snapshot.catalog.entries.size());
    intake_timer->metric("catalog_proved_empty", result.catalog_cut_proved_empty ? 1 : 0);
    intake_timer->metric("unhinted_quiet_walked", intake_unhinted_quiet);
    intake_timer->metric("frontier_unprobed_budget", intake_unprobed_budget);
    intake_timer->metric("catalog_only_walked", intake_catalog_only);
    intake_timer->metric("dead_precommits_skipped", intake_dead_precommits_skipped);
    /// `absent_probes` counts EVERY `_log` read that came back absent: the key that ended a walk, and
    /// the absent reads of `resolveRefCursorSuccessor` for a cold cursor. `epoch_crossings` counts the
    /// tombstones the walks crossed after folding them; a start that resumes in another epoch is not one.
    intake_timer->metric("absent_probes", intake_absent_probes);
    intake_timer->metric("epoch_crossings", intake_epoch_crossings);
    intake_timer->metric("namespace_removals", new_removals.size());
    intake_timer->metric("ref_folding_aborted", ref_folding_aborted ? 1 : 0);
    const auto familyMetrics = [&](const String & prefix, ReadFamily family)
    {
        const ReadFamilyStats & stats = reads.familyStats(family);
        intake_timer->metric(prefix + "_reads_issued", stats.issued);
        intake_timer->metric(prefix + "_takes", stats.takes);
        intake_timer->metric(prefix + "_awaited", stats.awaited);
        intake_timer->metric(prefix + "_wait_us", stats.wait_ns / 1000);
        intake_timer->metric(prefix + "_apply_us", stats.apply_ns / 1000);
    };
    familyMetrics("ckpt", ReadFamily::RefCheckpoint);
    familyMetrics("reflog", ReadFamily::RefLog);
    familyMetrics("manifest", ReadFamily::Manifest);
    intake_timer->metric("cursor_resolves", intake_cursor_resolves);
    intake_timer->metric("cursor_resolve_us", intake_cursor_resolve_ns / 1000);
    intake_timer.reset();   /// emits the `fold_ref_intake` row

    result.frontier_unprobed_budget = intake_unprobed_budget;

    /// Reuse the round's single ref LIST for post-CAS ref-object cleanup: one LIST serves
    /// intake AND cleanup planning).
    result.ref_tables = ref_tables;
    /// Same reuse for the checkpoints: the intake walk already paid for them as its second witness, and
    /// the cleanup ranges below are the other consumer of the same fact.
    ///
    /// An UNDECODABLE `_ckpt` contributes no entry here and therefore grants no cleanup authority. The
    /// walk above also held (or recorded an anomaly for) every such namespace, so the round's destructive
    /// gate is shut and `cleanupRefObjects` deletes nothing at all this round. Any future change that
    /// narrows the gate from round-wide to per-namespace must carry this set with it.
    for (const auto & [ns_str, sample] : checkpoints.recovery_checkpoints)
        result.checkpoints.emplace(ns_str, sample.ckpt);

    /// Folding the terminal record earns positive cleanup evidence directly on the catalog-admitted
    /// life row. It does not claim that any physical debris was removed: `_ckpt`, stream and `_files`
    /// residue belongs to the perpetual janitor, while orphan manifests belong to the manifest sweep.
    /// Consequently namespace removal performs no physical LIST and has no Pending/Completed handshake.
    if (!ref_folding_aborted)
        for (const auto & [rns, remove_txn_id] : new_removals)
        {
            const auto life_it = live_incarnation.find(rns.string());
            if (life_it == live_incarnation.end())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS GC fold: terminal removal for namespace '{}' has no row in the round catalog cut",
                    rns.string());
            auto row_it = result.fold_seal.ref_lives.find(life_it->second);
            if (row_it == result.fold_seal.ref_lives.end())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS GC fold: terminal removal for namespace '{}' has no admitted ref-life row",
                    rns.string());
            row_it->second.cleanup_evidence = RefCleanupEvidence{.remove_txn_id = remove_txn_id};
        }

    /// The parent generation's per-shard run segments, resolved from
    /// the parent fold seal's `blob_target_runs` and grouped by the ref's explicit `shard`. The same seal
    /// `discover_ref_seal` the token-diff already read is the run source — consumers resolve runs THROUGH
    /// refs (a run sealed for the parent generation may physically live under an older generation's key),
    /// never by `blobTargetRunKey` construction.
    std::map<uint64_t, std::vector<RunRef>> parent_runs_by_shard;
    for (const RunRef & r : discover_ref_seal.blob_target_runs)
        parent_runs_by_shard[r.shard].push_back(r);

    /// PURE REF-CARRY: a gc-shard with an EMPTY delta bucket
    /// AND an EMPTY retired input list neither reads nor writes its run — the new fold_seal copies the
    /// parent's `RunRef`s VERBATIM (key/checksum/shard/generation) so the next round resolves them. This
    /// is deterministic (same refs for the same inputs), so seal determinism / crash-replay adoption hold.
    /// An empty delta with a NON-EMPTY retired list still runs the merge: settlement must happen every
    /// pass (carried, intent-confirmed and body-deleted entries), and that pass reads the run to recompute in-degrees.
    /// Distill one shard's `condemned_summary` entry from the `RunMarker::Condemned` rows it re-emitted this pass
    /// (`still_retired` mirrors those rows exactly). Folding shards call this; it makes the next
    /// round's `rowDecisionDue` and pure-carry decisions read only the seal, never a run.
    auto summarize = [](const std::vector<RetiredEntry> & still) -> CondemnedSummary
    {
        CondemnedSummary s;
        s.condemned_total = still.size();
        for (const RetiredEntry & e : still)
        {
            if (e.delete_pending)
                ++s.pending_total;
            else
                s.oldest_nonpending_condemn_round =
                    std::min(s.oldest_nonpending_condemn_round, e.condemn_round);
        }
        return s;
    };
    /// The parent seal's summary for a shard, used for the pure-carry DECISION (condemned_total==0) and to skip the
    /// condemned-row read-ahead sets when no parent row is pending, which costs read-ahead only.
    /// Missing => zero only on a fresh pool, which has no parent seal: `readFoldSeal` refuses a seal whose summary
    /// does not cover every shard, so the parent check below sees every shard's `pending_total`.
    auto summaryOfParent = [&](uint64_t shard) -> CondemnedSummary
    {
        const auto it = discover_ref_seal.condemned_summary.find(shard);
        return it != discover_ref_seal.condemned_summary.end() ? it->second : CondemnedSummary{};
    };
    auto carryParentRefs = [&](uint64_t shard)
    {
        const auto it = parent_runs_by_shard.find(shard);
        if (it != parent_runs_by_shard.end())
            for (const RunRef & r : it->second)
                result.fold_seal.blob_target_runs.push_back(r);   /// verbatim: parent key/checksum/gen
        /// Totality: a pure-carry shard settled nothing, so its `condemned_summary` entry is the parent's
        /// VERBATIM. On a fresh pool (no adopted parent seal) it is the explicit zero baseline; otherwise
        /// the parent seal MUST carry the entry (a live seal is total over gc_shards) — a missing entry is
        /// corrupt bookkeeping, never silently treated as zero.
        if (state.snap_generation == 0)
            result.fold_seal.condemned_summary[shard] = CondemnedSummary{};
        else
        {
            const auto sit = discover_ref_seal.condemned_summary.find(shard);
            if (sit == discover_ref_seal.condemned_summary.end())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc fold: parent fold seal (generation {}, attempt {}) lacks a condemned_summary "
                    "entry for gc-shard {} — the seal is not total over gc_shards; GC bookkeeping is corrupt",
                    state.snap_generation, state.snap_attempt, shard);
            result.fold_seal.condemned_summary[shard] = sit->second;
        }
    };
    auto priorRunsFor = [&](uint64_t shard) -> const std::vector<RunRef> &
    {
        static const std::vector<RunRef> empty;
        const auto it = parent_runs_by_shard.find(shard);
        return it != parent_runs_by_shard.end() ? it->second : empty;
    };


    /// CLAMP SUPPRESSION: any clamp
    /// this pass means landed-before-cut events may be UNFOLDED behind a clamped cursor, so this pass
    /// must not delete a body or an intent. Deletes resume on the first clamp-free pass. This is the honest-mode
    /// counterpart of the model's SabotageSkipChangedShard counterexample.
    ///
    /// Phase `fold_reduce`: the prior-run streaming GETs, one HEAD per zero-transition candidate and per
    /// settled pending row, and the run PUTs -- the heaviest phase of a folding round on a pool with churn. It
    /// also carries probe B2's verdict (`transactions_unapplied`), which is 0 on every committed round because a nonzero
    /// value throws a few lines below; the row is therefore the forensic record of a round that failed.
    std::optional<GcPhaseTimer> reduce_timer;
    const uint64_t deltas_in = deltas.size();
    const uint64_t condemned_before = report.condemned;
    uint64_t shards_pure_carry = 0;
    /// ============================ THE DESTRUCTIVE GATE ============================
    ///
    /// Computed ONCE, here, from three independent terms, and consulted at every destructive site of
    /// the round (see `FoldResult::suppress_destructive`). It sits at this point in the fold because
    /// every input is final: the coverage rows the seal will carry are written, the anomalies are
    /// recorded, and the frontier tally is closed.
    ///
    /// Term 2 is STRUCTURAL. Today every hold also records an anomaly, so term 1 happens to imply it --
    /// but that is a property of the current code, not the invariant, and a gate that relies on a
    /// coincidence opens the day the coincidence stops holding. The hold SET is the invariant, so the
    /// gate reads the seal it is about to make durable.
    const std::vector<std::pair<UInt128, RefHold>> carried_holds = result.carriedHolds();

    /// Term 3, the universe seam. `Authoritative` means the round's universe is the catalog's own
    /// `Live`/`Removing` set, so the per-namespace proofs decide on their own; `StageA_Suppressed`
    /// refuses outright, which is the posture a test asserting inertness selects. See `UniversePolicy`.
    const bool universe_authoritative = policy == UniversePolicy::Authoritative;
    /// `frontier_proven == frontier_namespaces` is `0 == 0` -- TRUE --
    /// on an empty universe, which is not a proof of anything BY ITSELF: a fresh pool, a damaged
    /// catalog, or a read that legitimately returns nothing all produce zero entries. `frontier_namespaces
    /// > 0` closes that degenerate case for the ordinary, nonempty pool -- a nonzero count still needs
    /// every namespace PROVEN, which the equality above still checks.
    ///
    /// That floor alone is unsound for a pool whose LAST namespace was removed: the catalog then reads
    /// genuinely empty forever, `frontier_namespaces` can never again exceed 0, and the equality's
    /// vacuous truth can never be licensed -- an emptied pool would stop reclaiming permanently. The
    /// floor's job was never "reject `frontier_namespaces == 0`", it was "reject the UNSUPPORTED case of
    /// it". `catalog_cut_proved_empty` is the supported case: the round's own hot-scan catalog cut,
    /// read once and reused (never a second `GET`), decoded successfully, token-bearing, and holding
    /// zero rows of every lifecycle state including `Creating`. Under this pool's protocol every live or
    /// live-precommit edge belongs to a `Live` catalog row or to a `Removing` row without a terminal
    /// record, so that cut is a positive proof that no namespace anywhere holds one -- not merely an
    /// absence of proof. A catalog holding only `Creating` rows produces the SAME `frontier_namespaces == 0`
    /// but is a birth in progress, not an empty universe, and `catalog_cut_proved_empty` is false for it
    /// (see `entries.empty()` above).
    result.frontier_complete = universe_authoritative
        && result.frontier_proven == result.frontier_namespaces
        && (result.frontier_namespaces > 0 || result.catalog_cut_proved_empty);
    const bool frontier_incomplete = !result.frontier_complete;

    const bool suppressed_by_intake
        = !report.anomalies.empty() || !carried_holds.empty() || frontier_incomplete || stop_seen;

    /// Ref-log cleanup runs after its round commits a newer `gc/state`, so the leased version, still current
    /// after the last intake read, shows that no newer leader removed a record this round read as absent.
    /// Rounds with no `pending` or `standing` parent row skip the read.
    /// That is safe: a delete that acts on a false zero needs such a row, and every other intent delete
    /// follows a `HEAD` that showed another incarnation. The summary decides it before the merge: the reader refuses
    /// a merged shard whose rows disagree with its summary, and every body and intent delete runs after the merge.
    bool parent_has_pending_rows = false;
    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
        parent_has_pending_rows = parent_has_pending_rows || summaryOfParent(shard).pending_total != 0;
    if (!suppressed_by_intake && parent_has_pending_rows)
    {
        String seen;
        try
        {
            const std::optional<Meta> current = op.head(layout.gcStateKey(), Retry::standard());
            if (!current)
                seen = "no object";
            else if (!state_etag || !sameVersion(current->etag, *state_etag))
                seen = "another version";
        }
        catch (const Exception & e)
        {
            /// A read verb that gives up throws `NETWORK_ERROR`; any other failure fails the round, as elsewhere.
            /// A lost fence or a spent lease budget throws the same code.
            if (e.code() != ErrorCodes::NETWORK_ERROR)
                throw;
            seen = "no answer (" + e.message() + ")";
        }
        result.parent_unconfirmed = !seen.empty();
        if (result.parent_unconfirmed)
            LOG_WARNING(logger,
                "CAS GC fold: the `HEAD` of `gc/state` after the intake shows {}, not the version this round "
                "leased (parent_unconfirmed). A newer leader may have removed records this round read as absent; "
                "no body or intent is deleted this pass.",
                seen);
    }

    result.suppress_destructive = suppressed_by_intake || result.parent_unconfirmed;
    const bool suppress_destructive = result.suppress_destructive;
    if (suppress_destructive)
    {
        ProfileEvents::increment(ProfileEvents::CASGCClampSuppressedPasses);
        /// LEVEL SPLIT, deliberately. A pass suppressed by an anomaly, a hold, an unproven namespace or
        /// an empty universe has a per-round cause an operator can chase, and that is a WARNING. A pass
        /// suppressed because the CALLER refused to supply a universe carries no such cause -- nothing on
        /// the pool explains it -- so it is reported at Info with the same numbers rather than raising an
        /// alarm nobody can act on.
        ///
        /// An AUTHORITATIVE-but-EMPTY universe that the catalog cut did NOT prove empty
        /// (`frontier_namespaces == 0 && !catalog_cut_proved_empty` -- e.g. a `Creating`-only catalog)
        /// is a per-round cause and is named explicitly here, because the bare equality
        /// `frontier_proven != frontier_namespaces` reads `0 != 0` (false) and would otherwise miss it.
        /// A catalog the cut DID prove empty is not a suppression cause at all: it satisfies
        /// `frontier_complete`, so a round reaching this block with one is suppressed by an anomaly, a hold
        /// or a stop request, never by the frontier term. A stop request alone is not a pool problem: Info.
        const bool per_round_cause = !report.anomalies.empty() || !carried_holds.empty() || result.parent_unconfirmed
            || result.frontier_proven != result.frontier_namespaces
            || (universe_authoritative && result.frontier_namespaces == 0 && !result.catalog_cut_proved_empty);
        const char * const universe_note =
            universe_authoritative ? "" : "; the caller supplied no universe";
        /// Names the real reason a `Creating`-only (or otherwise unprovable) catalog is not an empty
        /// universe, so the operator does not read a suppressed round with rows on file as "empty".
        String catalog_empty_note;
        if (universe_authoritative && result.frontier_namespaces == 0 && !result.catalog_cut_proved_empty)
        {
            std::map<NsState, size_t> by_state;
            for (const CatalogEntry & entry : catalog_snapshot.catalog.entries)
                ++by_state[entry.state];
            catalog_empty_note = fmt::format(
                "; catalog holds {} row(s), none walkable/provable: {} `Creating`, {} `Live`, {} `Removing`; "
                "`cas-fsck` names the creator of each `Creating` row and what its mount slot says",
                catalog_snapshot.catalog.entries.size(), by_state[NsState::Creating], by_state[NsState::Live],
                by_state[NsState::Removing]);
        }
        /// The per-cause breakdown of the unproven namespaces. Without it "N of M proven" names a
        /// deficit but not its cause, and the causes want opposite operator responses.
        const String deficit_note = result.frontier_deficit.total() == 0
            ? String{}
            : fmt::format("; unproven: {}", result.frontier_deficit.describe());
        const char * const parent_note = result.parent_unconfirmed ? "; the leased `gc/state` is unconfirmed" : "";
        const char * const stop_note = stop_seen ? "; the round was asked to stop" : "";
        if (per_round_cause)
            LOG_WARNING(logger,
                "CAS GC fold: destructive work SUPPRESSED this pass — {} anomaly(ies), {} held "
                "namespace(s), frontier {} ({} of {} namespace(s) proven{}{}{}){}{}. Body deletes, intent removals "
                "and the intent creates of carried candidates wait for a pass that clears every cause; fresh "
                "condemnations and transitions still create intents and write `pending` rows.",
                report.anomalies.size(), carried_holds.size(),
                result.frontier_complete ? "complete" : "INCOMPLETE",
                result.frontier_proven, result.frontier_namespaces, universe_note, catalog_empty_note,
                deficit_note, parent_note, stop_note);
        else
            LOG_INFO(logger,
                "CAS GC fold: destructive work SUPPRESSED this pass — {} anomaly(ies), {} held "
                "namespace(s), frontier {} ({} of {} namespace(s) proven{}{}{}){}. Body deletes, intent removals "
                "and the intent creates of carried candidates wait for a pass that clears every cause; fresh "
                "condemnations and transitions still create intents and write `pending` rows.",
                report.anomalies.size(), carried_holds.size(),
                result.frontier_complete ? "complete" : "INCOMPLETE",
                result.frontier_proven, result.frontier_namespaces, universe_note, catalog_empty_note,
                deficit_note, stop_note);
    }

    if (round_point_hook_for_test)
        runRoundPointHookForTest(RoundPointForTest::AfterIntake);

    /// One page of `blob-intents/` nominates intents that may have no row. It is listed after the cut, so the
    /// adoption `HEAD`s in the merge follow both. A suppressed round lists nothing. A listed key only nominates:
    /// the state comes from the `HEAD`. One nomination per blob per round.
    std::vector<std::vector<Layout::ParsedBlobIntentKey>> intent_nominations(state.gc_shards);
    if (!suppress_destructive)
    {
        GcPhaseTimer t(phase_sink, "intent_sweep_list");
        const ListPage page = op.list(layout.blobIntentsPrefix(), state.intent_sweep_cursor,
                                      store->poolConfig().intent_sweep_list_budget_keys, Retry::standard());
        std::set<BlobRef> nominated;
        uint64_t unparsed = 0;
        for (const ListedKey & listed : page.keys)
        {
            if (layout.isBlobIntentProbeKey(listed.key))
                continue;   /// the capability probe's scratch object, not an intent
            const std::optional<Layout::ParsedBlobIntentKey> parsed = layout.parseBlobIntentKey(listed.key);
            if (!parsed)
            {
                ++unparsed;
                continue;
            }
            if (nominated.insert(parsed->ref).second)
                intent_nominations[blobShard(parsed->ref, state.gc_shards)].push_back(*parsed);
        }
        result.intent_sweep_next_cursor = page.next_cursor;
        t.metric("listed", page.keys.size());
        t.metric("nominated", nominated.size());
        t.metric("unparsed", unparsed);
        t.metric("wrapped", page.next_cursor.empty() ? 1 : 0);
        t.metric("list_budget_keys", store->poolConfig().intent_sweep_list_budget_keys);
    }

    /// Started after the intent sweep, so the sweep's `LIST` is counted in its own phase row only.
    reduce_timer.emplace(phase_sink, "fold_reduce");

    std::vector<BlobSourceRetirement> orphan_source_retirements;
    if (!suppress_destructive && store->poolConfig().manifest_sweep_list_budget_keys > 0)
    {
        result.orphan_sweep = planManifestCursorPage(
            *store,
            state.manifest_sweep_cursor,
            store->poolConfig().manifest_sweep_list_budget_keys,
            store->poolConfig().manifest_sweep_delete_budget_keys,
            /// The sweep may recover a catalog-named namespace's debris only from the same frozen catalog
            /// cut and `_ckpt` frontier the round's own universe came from -- which is exactly what an
            /// authoritative universe means, and is why this is the gate's term and not a separate one.
            universe_authoritative,
            &work_budget, io_pool.get(), store->poolConfig().gc_io_concurrency);
        for (const ManifestSweepResult::Nomination & nomination : result.orphan_sweep.nominations)
            orphan_source_retirements.insert(
                orphan_source_retirements.end(),
                nomination.source_retirements.begin(),
                nomination.source_retirements.end());
    }

    /// The blobs the merge may close at zero: a key whose last verdict is a removal, and none whose last verdict
    /// is an activation. Retirements count after the deltas, as the merge applies them. The set is a superset, and
    /// nothing may depend on it being exact: a blob that keeps an untouched prior edge costs one unused `HEAD`, and
    /// a missed one is decided inline. Built after the sweep decided `orphan_source_retirements`, past the cut.
    ///
    /// The two sets below steer the condemned-row read-ahead and are disjoint. They stay empty when nothing reads
    /// them: on the sequential setting, in a suppressed round, and when no parent row is `pending` or `standing`.
    /// Blobs touched only by removals: their rows may be decided inline at zero.
    std::set<BlobRef> touched_blobs;
    /// Blobs that end this merge with a reference, from an activation that is the last verdict at its key; their
    /// rows are spared.
    std::set<BlobRef> referenced_blobs;
    {
        std::map<std::pair<BlobRef, UInt128>, bool> last_verdict_is_remove;
        for (const BlobDelta & delta : deltas)
            last_verdict_is_remove[{delta.ref, delta.source_id}] = delta.remove;
        for (const BlobSourceRetirement & retirement : orphan_source_retirements)
            last_verdict_is_remove[{retirement.ref, retirement.source_id}] = true;

        std::set<BlobRef> removed;
        std::set<BlobRef> surviving_add;
        for (const auto & [edge, is_remove] : last_verdict_is_remove)
            (is_remove ? removed : surviving_add).insert(edge.first);
        const bool keep_read_ahead_sets = reads.window() > 0 && !suppress_destructive && parent_has_pending_rows;
        for (const BlobRef & ref : removed)
        {
            if (surviving_add.contains(ref))
                continue;
            head_candidates[blobShard(ref, state.gc_shards)].push_back(ref);
            if (keep_read_ahead_sets)
                touched_blobs.insert(ref);
        }
        if (keep_read_ahead_sets)
            referenced_blobs = std::move(surviving_add);
    }

    /// Pending-row read-ahead: hints the body `HEAD` of an untouched pending row while the redelete budget and the
    /// window have room. Hints are issued in the merge, after intake.
    const auto redeleteBudgetFull = [&]
    {
        return work_budget.redeletes.cap != 0
            && work_budget.redeletes.used + pending_head_reserved.size() >= work_budget.redeletes.cap;
    };
    const auto offerPendingHead = [&](const BlobRef & ref)
    {
        /// A touched row may be spared or superseded inline; reserving past it would let later rows take its slot.
        if (touched_blobs.contains(ref) || referenced_blobs.contains(ref))
            return false;
        const String key = layout.blobKey(ref);
        if (key == settling_pending_head || pending_head_reserved.contains(key))
            return true;
        /// A row past the budget is carried, or read inline if a reserved row is spared; it must not stop the scan.
        if (redeleteBudgetFull())
            return true;
        /// A row the rotation skips this round is carried without its `HEAD`.
        if (!work_budget.redeletes.admits(blobShard(ref, state.gc_shards), ref))
            return true;
        if (reads.pendingHeads() >= reads.window())
            return false;
        reads.hintHead(key);
        pending_head_reserved.insert(key);
        return true;
    };
    /// A spared row takes no redelete slot, so the scan goes on past it.
    const auto offerSpareHead = [&](const BlobRef & ref, const CondemnedRow & row)
    {
        if (!row.token.canonical())
            return true;
        const String key = layout.blobKey(ref);
        if (spare_head_reserved.contains(key))
            return true;
        if (reads.pendingHeads() >= reads.window())
            return false;
        reads.hintHead(key);
        spare_head_reserved.insert(key);
        return true;
    };
    const PendingHeadHints pending_head_hints{
        .offer = [&](const BlobRef & ref, const CondemnedRow & row)
        {
            if (suppress_destructive || !row.delete_pending)
                return true;
            if (referenced_blobs.contains(ref))
                return offerSpareHead(ref, row);
            if (row.standing)
                return true;
            return offerPendingHead(ref);
        },
        /// The settling row's `HEAD` goes to the pool before the scan hints the rows behind it: sent inline beside a
        /// full pool, it would be one request over `gc_io_concurrency`.
        .take_pending = [&](const BlobRef & ref)
        {
            const String key = layout.blobKey(ref);
            if (pending_head_reserved.erase(key) == 0)
                reads.hintHead(key);
            settling_pending_head = key;
        },
        .take_spare = [&](const BlobRef & ref)
        {
            const String key = layout.blobKey(ref);
            if (spare_head_reserved.insert(key).second)
                reads.hintHead(key);
        }};
    const PendingHeadHints * pending_hints = reads.window() > 0 ? &pending_head_hints : nullptr;
    /// A reservation left after a shard's merge belongs to no settlement.
    const auto discardReservedReads = [&]
    {
        for (const String & key : pending_head_reserved)
            reads.discardHead(key);
        pending_head_reserved.clear();
        for (const String & key : spare_head_reserved)
            reads.discardHead(key);
        spare_head_reserved.clear();
    };

    /// A nomination alone does not take a shard off the pure carry, or an idle pool with one intent left over a
    /// referenced blob would rewrite that shard's run every round. A merge the shard runs anyway consumes its
    /// nominations; otherwise they wait for a round whose number is a multiple of `gc_fold_max_defer_rounds + 1`,
    /// and the sweep cursor stays at their page until then.
    const uint64_t max_defer_rounds = store->poolConfig().gc_fold_max_defer_rounds;
    const uint64_t nomination_period = max_defer_rounds == std::numeric_limits<uint64_t>::max() ? max_defer_rounds : max_defer_rounds + 1;
    const bool nominations_due = current_round % nomination_period == 0;
    uint64_t nominations_waiting = 0;
    const auto pureCarry = [&](uint64_t shard, bool merges_anyway)
    {
        if (merges_anyway || (nominations_due && !intent_nominations[shard].empty()))
            return false;
        nominations_waiting += intent_nominations[shard].size();
        return true;
    };

    /// Target-sharded reducers: `blobShard(blob_hash, gc_shards)` partitions the delta stream into disjoint
    /// buckets, each folded into `blobTargetRunKey(new_generation, shard, 0)`. A promote that displaces a blob's
    /// owner emits both its `-1` and `+1` at the same source event, so each delta routes independently to the
    /// shard owning its blob and cross-shard displacement needs no fixup.
    std::vector<std::vector<BlobDelta>> buckets(state.gc_shards);
    std::vector<std::vector<BlobSourceRetirement>> retirement_buckets(state.gc_shards);
    if (state.gc_shards == 1)
    {
        buckets[0] = std::move(deltas);
        retirement_buckets[0] = std::move(orphan_source_retirements);
    }
    else
    {
        for (BlobDelta & d : deltas)
            buckets[blobShard(d.ref, state.gc_shards)].push_back(std::move(d));
        for (BlobSourceRetirement & retirement : orphan_source_retirements)
            retirement_buckets[blobShard(retirement.ref, state.gc_shards)].push_back(std::move(retirement));
    }

    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
    {
        /// One shard also merges when intake changed a table but produced no delta.
        const bool has_input = state.gc_shards == 1 ? folded_any : !buckets[shard].empty();
        if (pureCarry(shard, has_input || !retirement_buckets[shard].empty() || summaryOfParent(shard).condemned_total != 0))
        {
            /// Nothing changed and no condemned entries to settle => zero run I/O. Carry the parent refs and
            /// summary into the seal so coverage and resume stay durable.
            carryParentRefs(shard);
            ++shards_pure_carry;
            continue;
        }
        /// Empty deltas still settle the `RunMarker::Condemned` rows riding the parent run. Two replicas may
        /// run reducers for DIFFERENT shards concurrently (CasGcScheduler ownership); their run keys never collide.
        head_hint_shard = shard;
        next_head_hint = 0;
        RunRef shard_run;
        bool absence_classified = false;
        try
        {
            shard_run = foldDeltasIntoGeneration(
                op, layout, priorRunsFor(shard),
                AdoptedRunContext{.gc_shards = state.gc_shards, .parent_round = state.round, .summary = summaryOfParent(shard),
                    .on_absent_run = [&](const String & failure)
                    {
                        absence_classified = true;
                        throwIfStateMoved(op, state_etag, failure);
                    }},
                new_generation, attempt, shard, std::move(buckets[shard]),
                ShardFoldOptions{
                    .current_round = current_round,
                    .condemn_round = condemn_round,
                    .head_blob = head_blob,
                    .peek_head = peek_head,
                    .pending_head = pending_head,
                    .spare_head = spare_head,
                    .create_intent = create_intent,
                    .out_retired = &result.retired_merge[shard],
                    .suppress_destructive = suppress_destructive,
                    .out_applied_by_txn_ordinal = &ledger.applied,
                    .source_retirements = std::move(retirement_buckets[shard]),
                    .work_budget = &work_budget,
                    .pending_hints = pending_hints,
                    .intent_nominations = std::move(intent_nominations[shard])});
        }
        catch (const Exception & e)
        {
            /// The parent runs this merge streams are what a newer leader's retention removes. An absent run was
            /// classified by its own probe; a second probe could contradict it.
            if (e.code() == ErrorCodes::CORRUPTED_DATA && !absence_classified)
                throwIfStateMoved(op, state_etag, e.message());
            throw;
        }
        discardReservedReads();
        result.fold_seal.blob_target_runs.push_back(std::move(shard_run));
        result.fold_seal.condemned_summary[shard] = summarize(result.retired_merge[shard].still_retired);
    }

    if (nominations_waiting > 0)
        result.intent_sweep_next_cursor.reset();

    /// Aggregate the unmatched-remove signal across every gc-shard this pass touched and log ONCE per
    /// round with the total plus one example, rather than from the hot per-edge inner loop that detects
    /// them (see `foldDeltasIntoGeneration`'s comment) — that loop runs over potentially millions of
    /// rows, so it only counts (`ProfileEvents::CASGCUnmatchedRemoveDeltas`, incremented per occurrence)
    /// and hands back one example; this is the bounded, once-per-round operator-visible trail.
    {
        uint64_t total_unmatched_removes = 0;
        std::optional<UnmatchedRemoveExample> example;
        for (const RetiredMergeResult & merge : result.retired_merge)
        {
            total_unmatched_removes += merge.unmatched_removes;
            if (!example && merge.unmatched_remove_example)
                example = merge.unmatched_remove_example;
        }
        if (total_unmatched_removes > 0 && example)
            LOG_WARNING(logger,
                "CAS GC fold: {} unmatched removal delta(s) this pass (matched no existing source edge; "
                "a harmless per-key no-op by design, since the in-degree model is a set, not a counter — "
                "but a persistent nonzero rate means removal deltas are reaching the reducer without "
                "their matching activation, which is a correctness signal) — example: blob {} source {}",
                total_unmatched_removes, blobIdOf(example->ref), u128ToHex(example->source_id));
        reduce_timer->metric("unmatched_removes", total_unmatched_removes);
    }

    /// PROBE B2's ledger verdict, computed inside the reduce phase so the row carries it; the
    /// fail-closed throw it drives stays below, at its original site before the seal write.
    const std::vector<uint32_t> unapplied_txns = ledger.unapplied();
    transactions_unapplied_this_round = unapplied_txns.size();
    {
        uint64_t intent_confirmed = 0;
        uint64_t spared = 0;
        uint64_t body_deletes_decided = 0;
        uint64_t adopted = 0;
        for (const RetiredMergeResult & merge : result.retired_merge)
        {
            intent_confirmed += merge.confirmed.size();
            spared += merge.spared.size();
            body_deletes_decided += merge.body_deletes.size();
            adopted += merge.adopted.size();
        }
        reduce_timer->metric("shards_total", state.gc_shards);
        reduce_timer->metric("shards_pure_carry", shards_pure_carry);
        reduce_timer->metric("shards_reduced", state.gc_shards - shards_pure_carry);
        reduce_timer->metric("nominations_waiting", nominations_waiting);
        reduce_timer->metric("read_ahead_blobs", touched_blobs.size() + referenced_blobs.size());
        reduce_timer->metric("deltas_in", deltas_in);
        reduce_timer->metric("runs_written", result.fold_seal.blob_target_runs.size());
        reduce_timer->metric("condemned", report.condemned - condemned_before);
        reduce_timer->metric("intents_confirmed", intent_confirmed);
        reduce_timer->metric("spared", spared);
        reduce_timer->metric("body_deletes_decided", body_deletes_decided);
        reduce_timer->metric("adopted", adopted);
        reduce_timer->metric("suppress_destructive", suppress_destructive ? 1 : 0);
        /// Published separately from `suppress_destructive` so a reader can tell the frontier term apart
        /// from the anomaly and hold terms without re-deriving the formula from the tally.
        reduce_timer->metric("frontier_complete", result.frontier_complete ? 1 : 0);
        reduce_timer->metric("parent_unconfirmed", result.parent_unconfirmed ? 1 : 0);
        reduce_timer->metric("transactions_unapplied", transactions_unapplied_this_round);
        uint64_t condemned_rows = 0;
        uint64_t pending_rows = 0;
        for (const auto & [shard, summary] : result.fold_seal.condemned_summary)
        {
            condemned_rows += summary.condemned_total;
            pending_rows += summary.pending_total;
        }
        reduce_timer->metric("condemned_rows", condemned_rows);
        reduce_timer->metric("pending_rows", pending_rows);
    }
    reduce_timer.reset();   /// emits the `fold_reduce` row

    /// The part-manifest cleanup RUN + its fold-seal record are removed: the run
    /// object had no reader — the manifest cleanups execute inline from `result.mf_cleanup` (below /
    /// the recheck path), so the durable bundle was pure dead weight. `result.mf_cleanup` is unchanged.

    /// Write-once CasFoldSeal: its existence marks fold complete. The fold seal is DETERMINISTIC (same
    /// fold inputs => byte-identical seal), so it goes through `putDeterministicArtifact`: a byte-equal
    /// occupant is our own crash/deterministic replay (adopt, no-op); divergent bytes are impossible
    /// under correct operation and fail closed with `CORRUPTED_DATA`. A deposed leader writes under its
    /// own unadopted attempt so it never collides with the adopted seal — the occupant here is only ever
    /// our own prior attempt-scoped write.
    /// PROBE B2's verdict. A committed transaction that produced deltas but whose deltas never
    /// reached a reducer means this round LOST a durable record it had already read and decoded.
    /// Unlike a 404 during a fold (missing evidence, which must never wedge the round), this is proof
    /// of loss, so the round fails CLOSED: nothing is adopted, GC reclaims and deletes nothing, and an
    /// operator has to intervene. Thrown before the seal write, and therefore long before the single
    /// `gc/state` CAS, so the whole round evaporates.
    /// `unapplied_txns` was computed in the reduce phase above (so its row could report it); the verdict
    /// itself is unchanged and still fires here, before the seal write.
    if (!unapplied_txns.empty())
    {
        String detail;
        for (size_t i = 0; i < unapplied_txns.size() && i < 8; ++i)
        {
            if (i != 0)
                detail += ", ";
            detail += ledger.namespaces[unapplied_txns[i]] + "@"
                + renderRefTxnId(ledger.txns[unapplied_txns[i]]);
        }
        ProfileEvents::increment(ProfileEvents::CASGCUnappliedFoldedTransactions, unapplied_txns.size());
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS GC fold: {} ref transaction(s) folded and merged into the round buffers but NONE of "
            "their blob deltas reached a shard reducer ({}{}). The round would have advanced its "
            "cursor past a transaction it never applied. {}",
            unapplied_txns.size(), detail, unapplied_txns.size() > 8 ? ", ..." : "", kCorruptBookkeeping);
    }

    /// PROBE B1's comparison. Both terms were derived at the end of the ref intake (see the
    /// recomputation there, which is also what the `fold_ref_intake` row reports) and are 0 on a
    /// ref-folding abort, where the identity does not apply -- so the inequality below can only fire on
    /// a round that actually folded.
    if (logs_accounted_this_round != logs_applied_this_round)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS GC fold: the round sealed coverage over {} ref log(s) but only {} fully folded -- "
            "a cursor advanced past a log this round never applied. {}",
            logs_accounted_this_round, logs_applied_this_round, kCorruptBookkeeping);

    /// Phase `fold_seal_write`: one PUT (or, on a deterministic replay, a byte-compare GET).
    {
        GcPhaseTimer t(phase_sink, "fold_seal_write");
        validateFoldSealForWrite(result.fold_seal, layout, store->poolConfig().gc_shards);
        const String seal_body = encodeFoldSeal(result.fold_seal);
        t.metric("seal_bytes", seal_body.size());
        t.metric("seal_runs", result.fold_seal.blob_target_runs.size());
        t.metric("seal_ref_lives", result.fold_seal.ref_lives.size());
        t.metric("seal_cleanup_evidence", std::count_if(
            result.fold_seal.ref_lives.begin(), result.fold_seal.ref_lives.end(),
            [](const auto & item) { return item.second.cleanup_evidence.has_value(); }));
        putDeterministicArtifact(op, layout.foldSealKey(new_generation, attempt), seal_body);
    }

    /// One-pass round: the fold NO LONGER CASes gc/state. (new_generation, attempt) are adopted
    /// in-memory here and committed — together with the round, the retired refs, and the retention
    /// cursor — by the SINGLE round CAS in runRegularRound. A deposed leader's whole pass therefore
    /// evaporates at that one CAS; its attempt-scoped artifacts are never adopted.
    state.snap_generation = new_generation;
    state.snap_attempt = attempt;
    return result;
}

void Gc::reportSweepRetention(const ManifestSweepResult & result)
{
    const auto top = result.topRetainReason();
    if (top.second == 0)
    {
        /// Nothing retained by the premise. Re-arm, so that the next retention -- however far off --
        /// is reported as the change it is rather than swallowed by a repeat counter.
        last_retain_rollup.reset();
        retain_rollup_passes_since_report = 0;
        return;
    }

    const bool changed = !last_retain_rollup || *last_retain_rollup != top;
    if (!changed && ++retain_rollup_passes_since_report < kRetainRollupRepeatPasses)
        return;

    /// INFO, not WARNING: retention is the CORRECT outcome whenever rule (1) is unsatisfiable (it is
    /// satisfiable only for a closed-and-folded epoch), so warning here would alarm on healthy rounds. It is
    /// still the operator's answer to "why is manifest debris not shrinking?", which is why it is not
    /// left at DEBUG with the per-object sentences.
    /// The "X of Y" denominator is the RETAINED total, not `skipped`. `skipped` is a strictly larger
    /// population -- it also counts malformed keys, ineligible prefixes, protected owners and
    /// budget-deferred candidates -- so measuring the top class against it would understate the class's
    /// share of the very number this sentence just reported.
    const uint64_t retained = result.retained_no_coverage + result.retained_hold
        + result.retained_unconsumed_seal + result.retained_tail_removal + result.retained_work_budget;
    LOG_INFO(logger,
        "CAS gc orphan sweep: retained {} manifest body(ies) this pass, most of them ({} of {}) for "
        "'{}' -- see the fold seal's coverage for that namespace; deleted {}, listed {}, skipped {}",
        retained, top.second, retained, sweepRetainClassName(top.first),
        result.deleted, result.listed, result.skipped);

    last_retain_rollup = top;
    retain_rollup_passes_since_report = 0;
}

namespace
{
/// What the pre-chunk revalidation of `Gc::cleanupRefObjects` allows for the next chunk.
enum class RefCleanupLicence : uint8_t
{
    Delete,
    /// The namespace's own row or life changed; its plan is stale, other namespaces are unaffected.
    SkipNamespace,
    /// `gc/state` was read and its lease owner or sequence differs from the adopted one: no namespace
    /// may delete any more this pass.
    StopLeaseMoved,
    /// A mandatory read failed, `gc/state` is absent, or the catalog is ambiguous: nothing can be
    /// verified this pass.
    StopAuthorityUnverified,
};

struct RefCleanupVerdict
{
    RefCleanupLicence licence;
    /// The catalog read for this verdict has another etag than the fold's cut.
    bool catalog_etag_differs;
};
}

void Gc::cleanupRefObjects(
    const FoldResult & folded, const GcLease & adopted_lease, bool suppress_destructive,
    GcRoundWorkBudget & work_budget, RefCleanupPassStats & stats)
{
    stats = RefCleanupPassStats{};

    /// A clamp / ref-folding abort this round may leave landed-before-cut edges unfolded behind the clamp,
    /// so a covered-log cleanup could delete a log whose delta is not yet durable -- defer to a clean pass.
    if (suppress_destructive)
        return;

    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();
    if (!folded.catalog_cut)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS GC ref cleanup: fold result carries no catalog cut");

    for (const auto & [ns_str, listing] : folded.ref_tables)
    {
        const RootNamespace ns{ns_str};
        /// Look up the SAME complete cut the round's walk resolved, never re-resolve
        /// independently. A fresh read here could see a namespace dropped and recreated since the
        /// walk and delete the successor's objects using predecessor bounds. An absent or `Creating`
        /// row is skipped rather than mapped to a fabricated key.
        const auto entry_it = std::lower_bound(
            folded.catalog_cut->catalog.entries.begin(), folded.catalog_cut->catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        if (entry_it == folded.catalog_cut->catalog.entries.end() || entry_it->ns != ns
            || (entry_it->state != NsState::Live && entry_it->state != NsState::Removing))
            continue;
        const CatalogEntry & observed_entry = *entry_it;
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(entry_it->ns, entry_it->incarnation);

        /// Current-life ref cleanup is not the dead-life janitor: every irreversible delete is licensed by
        /// this namespace's own row and life plus the GC lease, re-read immediately before the chunk. The
        /// unit of licence is one chunk of write-once keys: a `_log` or `_snap` key is published once at a
        /// life-qualified key and never rewritten, so there is nothing to HEAD, and the plan below is
        /// derived from durable state a successor leader derives too. The catalog etag decides nothing; it
        /// is only compared for `CASRefCleanupChunksUnderCatalogChange`. An ambiguous catalog still stops
        /// the pass, whichever rows alias. Churn on other namespaces' rows does not touch this life's keys.
        /// `gc/state` is read last, so the lease check is the observation nearest the delete.
        bool catalog_etag_differs = false;
        const auto licenceForChunk = [&](const String & first_key) -> RefCleanupVerdict
        {
            try
            {
                const CasRefCatalog::Snapshot current_catalog = CasRefCatalog::read(op, layout);
                catalog_etag_differs = !identicalToken(current_catalog.etag, folded.catalog_cut->etag);
                current_catalog.life_index.throwIfAmbiguous("CAS GC ref cleanup revalidation");
                const auto current_entry_it = std::lower_bound(
                    current_catalog.catalog.entries.begin(), current_catalog.catalog.entries.end(), ns,
                    [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
                const std::optional<NamespaceLifeId> current_life
                    = current_catalog.life_index.resolve(life.incarnation);
                if (current_entry_it == current_catalog.catalog.entries.end()
                    || current_entry_it->ns != ns || *current_entry_it != observed_entry
                    || !current_life || *current_life != life)
                {
                    LOG_DEBUG(logger,
                        "CAS GC ref cleanup skips namespace '{}' from the chunk starting at '{}': its catalog row or life moved",
                        ns_str, first_key);
                    return {RefCleanupLicence::SkipNamespace, catalog_etag_differs};
                }

                const auto current_state_object = op.read(layout.gcStateKey(), Retry::standard());
                if (!current_state_object)
                {
                    LOG_WARNING(logger,
                        "CAS GC ref cleanup stopped before the chunk starting at '{}': mandatory gc/state is absent",
                        first_key);
                    return {RefCleanupLicence::StopAuthorityUnverified, catalog_etag_differs};
                }
                const GcState current_state = decodeGcState(current_state_object->bytes);
                if (current_state.lease.owner != adopted_lease.owner
                    || current_state.lease.seq != adopted_lease.seq)
                {
                    LOG_DEBUG(logger,
                        "CAS GC ref cleanup stopped before the chunk starting at '{}': GC fence moved", first_key);
                    return {RefCleanupLicence::StopLeaseMoved, catalog_etag_differs};
                }
            }
            catch (const std::exception & e)
            {
                LOG_WARNING(logger,
                    "CAS GC ref cleanup stopped before the chunk starting at '{}': authority revalidation failed: {}",
                    first_key, e.what());
                return {RefCleanupLicence::StopAuthorityUnverified, catalog_etag_differs};
            }
            return {RefCleanupLicence::Delete, catalog_etag_differs};
        };

        const auto row_it = folded.fold_seal.ref_lives.find(entry_it->incarnation);
        const RefTxnId durable_cursor = row_it != folded.fold_seal.ref_lives.end()
            ? row_it->second.coverage.last_folded_ref_id
            : RefTxnId{};

        /// Only a checkpoint-named RECOVERY TRIPLE licenses deletion. A listed snapshot may have landed
        /// before its publisher's checkpoint CAS, so it must never be promoted into cleanup authority.
        /// Validate the exact same-id non-seal `_log` and `_snap` through recovery's one shared helper;
        /// failure is confined to this namespace and leaks its listed objects for a later round.
        std::optional<RefCkpt> checkpoint;
        if (const auto ckit = folded.checkpoints.find(ns_str); ckit != folded.checkpoints.end())
            checkpoint = ckit->second;
        if (!checkpoint || !checkpoint->checkpoint_snapshot_id)
            continue;
        const RefTxnId checkpoint_snapshot_id = *checkpoint->checkpoint_snapshot_id;

        std::optional<RefTxnId> retained_log_proof;
        try
        {
            retained_log_proof = readCheckpointSnapshotBase(op, layout, life, *checkpoint).predecessor_seal_id;
        }
        catch (const Exception & e)
        {
            LOG_WARNING(logger,
                "CAS GC ref cleanup retained namespace '{}': checkpoint base {} is not a valid recovery triple: {}",
                ns_str, renderRefTxnId(checkpoint_snapshot_id), e.message());
            continue;
        }

        const RefCleanupPlan plan = planRefCleanup(
            listing, durable_cursor, checkpoint_snapshot_id, retained_log_proof);
        std::vector<WriteOnceKey> cohort;
        cohort.reserve(plan.deletable_logs.size() + plan.deletable_snapshots.size());
        for (const RefTxnId & log_id : plan.deletable_logs)
            cohort.push_back(layout.writeOnceRefLogKey(life, log_id));
        for (const RefTxnId & snap_id : plan.deletable_snapshots)
        {
            /// The snapshot the checkpoint names is the one a recovering reader will sample, so it must
            /// survive every cleanup that the same checkpoint authorized.
            chassert(snap_id < checkpoint_snapshot_id);
            cohort.push_back(layout.writeOnceRefSnapshotKey(life, snap_id));
        }
        stats.objects_planned += cohort.size();
        ProfileEvents::increment(ProfileEvents::CASRefCleanupObjectsPlanned, cohort.size());

        const size_t chunk_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        const size_t request_keys = bulkDeleteChunkKeys();
        for (size_t begin = 0; begin < cohort.size(); )
        {
            /// Cumulative per-round cap in KEYS, exactly as before; a chunk is cut to what remains. The
            /// plan recomputes the same remaining candidates from durable state next round, so nothing
            /// here needs its own cursor.
            if (!work_budget.refCleanupAvailable())
                return;
            size_t end = std::min(cohort.size(), begin + chunk_keys);
            if (work_budget.max_ref_cleanup_objects != 0)
                end = std::min<size_t>(end, begin + (work_budget.max_ref_cleanup_objects - work_budget.ref_cleanup_objects_used));
            std::vector<WriteOnceKey> chunk(cohort.begin() + begin, cohort.begin() + end);
            const RefCleanupVerdict verdict = licenceForChunk(chunk.front().str());
            if (verdict.licence == RefCleanupLicence::SkipNamespace)
            {
                ++stats.namespaces_skipped;
                ProfileEvents::increment(ProfileEvents::CASRefCleanupNamespacesSkipped);
                break;
            }
            if (verdict.licence == RefCleanupLicence::StopLeaseMoved)
            {
                stats.stopped_lease_moved = true;
                ProfileEvents::increment(ProfileEvents::CASRefCleanupStoppedLeaseMoved);
                return;
            }
            if (verdict.licence == RefCleanupLicence::StopAuthorityUnverified)
            {
                stats.stopped_authority_unverified = true;
                ProfileEvents::increment(ProfileEvents::CASRefCleanupStoppedAuthorityUnverified);
                return;
            }
            /// A backend without `DeleteObjects` (GCS) falls back to one admitted delete per key here.
            /// The budget and the profile event below count OBJECTS in `chunk`, which is the same
            /// `chunk.size()` whichever way `removeChunkWriteOnceOrOneByOne` actually sent them.
            removeCohortWriteOnce(op, chunk, request_keys, Retry::standard());
            work_budget.ref_cleanup_objects_used += chunk.size();
            ProfileEvents::increment(ProfileEvents::CASRefCleanupObjectsDeleted, chunk.size());   /// cleanup object deletion
            stats.objects_deleted += chunk.size();
            if (verdict.catalog_etag_differs)
            {
                ++stats.chunks_under_catalog_change;
                ProfileEvents::increment(ProfileEvents::CASRefCleanupChunksUnderCatalogChange);
            }
            /// Advance by what was actually sent, not the nominal chunk size: the budget cap above can
            /// truncate a chunk short of `chunk_keys`, and advancing by the full stride would skip the
            /// untried remainder instead of retrying it next iteration.
            begin = end;
        }
    }
}

namespace
{
/// GC-metadata wholesale delete of every object under `prefix`. Returns the number of objects deleted.
/// `bounded_remaining` caps how many objects this call may delete (0 => stop immediately, deleting none).
///
/// Precondition source: the in-memory and S3 backends surface a per-key incarnation through `list`
/// (`supportsListTokens()`), so the removal goes straight from the listed one; otherwise observe first.
///
/// An absence is FAIL-OPEN: an object that vanished between LIST and delete (a concurrent crashed
/// attempt, or a racing prune) is already reclaimed — never throw on a benign missing GC-internal object
/// during a prune (it would only wedge GC). A genuine mismatch is
/// likewise tolerated here: the object was rewritten under us (another attempt is live at this key) — the
/// safe direction during a best-effort prune is to leave it for a later round, never to force-delete.
/// `out_fully_drained`, when set, reports whether the WHOLE prefix was exhausted (every listed key
/// visited) rather than the call stopping early because `bounded_remaining` ran out. A
/// caller advancing a monotone cursor past this prefix must consult this: a `false` here means objects
/// remain, and the cursor must stay put so a later round's fresh budget can finish the same prefix
/// instead of stranding the remainder permanently. `bounded_remaining == 0` conservatively reports
/// `false` (nothing was even examined, so completeness cannot be claimed).
uint64_t deletePrefixWholesale(CasOperation & op, const String & prefix, uint64_t bounded_remaining,
                               bool * out_fully_drained)
{
    if (out_fully_drained)
        *out_fully_drained = false;
    static constexpr size_t kListPageLimit = CasOperation::kCasListPageKeys;
    uint64_t deleted = 0;
    String cursor;
    while (deleted < bounded_remaining)
    {
        ListPage page = op.list(prefix, cursor, kListPageLimit, Retry::standard());
        /// One page fetched, not one increment per listed key below.
        ProfileEvents::increment(ProfileEvents::CASGCEnumerationPages);
        for (const auto & listed : page.keys)
        {
            if (deleted >= bounded_remaining)
                return deleted;
            if (listed.etag.has_value())
            {
                /// `Gone` and `Mismatch` are both benign here (already gone / rewritten by a live
                /// attempt); do not throw.
                op.remove(listed.key, *listed.etag, Retry::standard());
            }
            else if (const auto head = op.head(listed.key, Retry::standard()))
            {
                op.remove(listed.key, head->etag, Retry::standard());
            }
            ++deleted;
        }
        if (page.next_cursor.empty())
        {
            if (out_fully_drained)
                *out_fully_drained = true;
            break;
        }
        cursor = page.next_cursor;
    }
    return deleted;
}
}

void Gc::pruneSupersededGenerations(uint64_t adopted_generation, uint64_t attempt, GcState & next,
                                    const std::set<uint64_t> & referenced_generations,
                                    bool suppress_destructive, GcRoundWorkBudget & work_budget)
{
    /// GATED, and `snap_pruned_through` stays where it is. The cursor is a monotone high-water mark the
    /// wholesale prune never revisits, so advancing it over a generation this round declined to delete
    /// would strand that generation's whole prefix with no reclaimer left (the hand-off only covers
    /// generations a LIVE ref moved off, not ones skipped for suppression).
    if (suppress_destructive)
        return;

    const uint64_t configured_keep = store->poolConfig().gc_snapshot_generations_to_keep;
    if (configured_keep == 0)
        return;   /// keep ALL (debug/forensics — replay GC's in-degree view as-of a past round)
    /// The adopted generation's seal must survive a round that loses, so one generation is never enough.
    const uint64_t keep = std::max<uint64_t>(configured_keep, 2);

    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();

    static constexpr uint64_t kMaxPrunePerRound = 64;   /// bound the per-round prune burst

    /// (1) WHOLESALE generation-retention (correctness). A single generation may hold artifacts under
    /// MULTIPLE attempts: every round mints a fresh `lease.seq` (= attempt), and a deposed leader writes
    /// its fold_seal/runs/cleanup AND its attempt-scoped retired/outcomes sets under its OWN unadopted
    /// attempt before its CAS fails. The old per-key single-attempt prune (keyed on the final
    /// snap_attempt) therefore leaked every non-adopted attempt's debris. Instead, LIST the whole
    /// `gc/gen/<g>/` prefix and delete every listed object — reclaiming ALL attempts of `g`, including
    /// the retired/ and outcomes/ sets that now live under `gc/gen/<g>/attempt/<a>/`. Bounded per round
    /// by generation count (`kMaxPrunePerRound`) AND by the round's shared object-count work budget;
    /// fail-open on 404. `snap_pruned_through` advances over every generation the loop
    /// FULLY processes this round — ref-retained (skipped) generations count as fully processed (there
    /// is nothing left for THIS loop to do to them), but a generation whose delete the work budget cut
    /// short does NOT: the loop stops there, so the cursor never strands a partially-drained prefix
    /// behind it. It is a monotone high-water cursor, NOT a proof that everything below it is gone.
    if (adopted_generation > keep)
    {
        const uint64_t prune_floor = adopted_generation - keep;   /// prune generations <= prune_floor
        uint64_t g = next.snap_pruned_through + 1;
        uint64_t pruned = 0;
        for (; g <= prune_floor && pruned < kMaxPrunePerRound; ++g, ++pruned)
        {
            /// A generation whose run the LIVE adopted seal still
            /// references (reference-parent carry: an idle shard's current run physically lives at an
            /// older generation's key) must NOT be reclaimed — deleting it would strand the live seal's
            /// ref. Skip its prefix delete; the run stays alive as long as it is referenced. NOTE the
            /// cursor still advances past this skipped generation (see the `g - 1` cursor note above), so
            /// the wholesale prune NEVER revisits it once it is behind the cursor. LEAK-FREEDOM therefore
            /// rests on the post-CAS hand-off in `runRegularRound`: the round that finally moves the
            /// ref OFF this generation (a later delta writes a fresh run) wholesale-deletes this whole
            /// prefix right after its CAS. So every formerly-referenced generation is eventually FULLY
            /// reclaimed — either here (if the ref moved off before the cursor reached it, WholesalePrune*
            /// test) or by the hand-off (if the cursor passed it while still referenced, HandOffDeletes*
            /// test). Until the ref moves it persists safely (bounded: one small run per shard).
            if (referenced_generations.contains(g))
            {
                LOG_TRACE(logger,
                    "CAS GC prune: retaining generation {} — still referenced by the live adopted seal",
                    g);
                continue;
            }
            /// `bounded_remaining` is the round's remainder shared across every PRUNE
            /// `deletePrefixWholesale` call this round (never `UINT64_MAX`; the post-CAS hand-off draws
            /// from its own separate reserve, never this one). A generation whose prefix
            /// this call cannot FULLY drain within the remaining budget must not let the cursor advance
            /// past it -- `snap_pruned_through` is a monotone high-water mark this loop never revisits,
            /// so stranding a partially-drained generation behind it would leak the remainder forever.
            /// Stop the loop here; `g - 1` (the previous, fully-processed generation) is what gets
            /// persisted below.
            const uint64_t remaining = work_budget.prefixWholesaleRemaining();
            if (remaining == 0)
                break;
            bool fully_drained = false;
            const uint64_t reclaimed = deletePrefixWholesale(
                op, layout.gcGenPrefix(g), remaining, &fully_drained);
            work_budget.prefix_wholesale_objects_used += reclaimed;
            if (!fully_drained)
                break;
        }
        next.snap_pruned_through = g - 1;   /// highest generation FULLY processed this round
    }

    /// (2) NO per-round current-generation attempt-sweep (KISS). A previous revision LISTed the FOLD
    /// generation's `gc/gen/<G_f>/` prefix EVERY completed round to delete non-adopted attempts with
    /// `a < snap_attempt` — debris a deposed leader of the just-completed round left under its own
    /// (unadopted) `lease.seq`. That per-round LIST was steady-state S3 budget spent for the RARE case
    /// of a concurrent-leader collision (the GC-DISCOVERY-LIST-QUADRATIC concern), so it is removed.
    ///
    /// The wholesale generation-retention prune in (1) is now the SOLE reclaimer of ALL attempt debris,
    /// including a deposed leader's: every artifact of generation `g` — across every attempt — lives
    /// under `gc/gen/<g>/`, and the prefix-delete in (1) reclaims the whole subtree once `g` ages past
    /// `keep`. Deposed-leader current-generation debris is therefore BOUNDED space (one collision leaves
    /// at most a handful of small objects per generation) that waits at most `keep` completion-advances
    /// to be reclaimed. This trades ~`keep` rounds of reclaim latency on (rare) concurrent-leader
    /// collisions for eliminating a per-round LIST on the common (single-leader) path. When `keep == 0`
    /// (keep-all / forensics mode) nothing is reclaimed by design — same as before.
    (void)attempt;
}

std::optional<CasFoldSeal> Gc::readFoldSeal(uint64_t generation, uint64_t attempt)
{
    CasOperation op = store->openRequests().admit();
    if (const auto got = op.read(store->layout().foldSealKey(generation, attempt), Retry::standard()))
        return decodeFoldSeal(
            got->bytes, store->layout(), store->poolConfig().gc_shards, generation);
    return std::nullopt;
}

void Gc::throwIfStateMoved(CasOperation & op, const std::optional<Etag> & state_etag, const String & failure)
{
    if (!state_etag)
        return;
    std::optional<Meta> current;
    try
    {
        current = op.head(store->layout().gcStateKey(), Retry::standard());
    }
    catch (...)
    {
        /// Without an answer nothing shows a newer leader, so the caller's own error stands.
        tryLogCurrentException(logger, "CAS GC: could not read gc/state to tell a newer leader from corruption", LogsLevel::warning);
        return;
    }
    if (current && sameVersion(current->etag, *state_etag))
        return;
    throw Exception(ErrorCodes::ABORTED,
        "CAS gc round: gc/state moved during the round ({}); {}; retry next round",
        current ? "another leader advanced it, observed " + current->etag.render() : String("the object is gone"), failure);
}

namespace
{

/// `Layout::parseRefObjectKey` for a key coming from a global `cas/ns/stream/` enumeration, with the one
/// refusal it can raise absorbed into the ordinary "unrecognized" answer.
///
/// A ref object naming no life (the un-incarnated shape) is the single malformed key the parser
/// REFUSES by name instead of classifying as debris, and both global enumerations run OUTSIDE the
/// fold's catch. Letting the refusal escape one of them would not merely lose a round: GC is the only
/// thing that could ever delete the key, so every future round would die on it too, with nothing able
/// to clear it. Absorbed here, the key stays in the enumeration's raw key list, unindexed, exactly
/// like every other malformed shape, and `groupRefKeys` raises it once inside the fold's catch --
/// louder than before (an anomaly plus `suppress_destructive`), and without the wedge.
///
/// Only `CORRUPTED_DATA` is absorbed: any other exception is a real failure of the enumeration itself.
std::optional<ParsedRefObjectKey> parseRefObjectKeyForEnumeration(const Layout & layout, const String & key)
{
    try
    {
        return layout.parseRefObjectKey(key);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::CORRUPTED_DATA)
            throw;
        return std::nullopt;
    }
}

}

std::vector<NamespaceLifeId> Gc::discoverUniverse()
{
    /// One catalog GET replaces the pool-wide `LIST(cas/ns/stream/)` this used to run to
    /// discover WHICH namespaces exist. `Creating` entries are excluded -- no publication can
    /// exist while a namespace is still being created, so there is nothing here for a discovery path to
    /// walk; `Live` and `Removing` entries are both returned. `fromCatalogEntry` mints each life directly
    /// from the row that is its own authority for both fields, never from a listed key (which could name
    /// a DEAD incarnation of the same namespace name).
    ///
    /// The filter itself lives in `CasRefCatalog::liveUniverse` -- fsck's own
    /// reachability walk needed the identical catalog-authoritative set and is not this class, so the
    /// filter moved to where both can share it rather than grow a second copy that could disagree.
    CasOperation op = store->openRequests().admit();
    return CasRefCatalog::liveUniverse(op, store->layout());
}

bool Gc::rowDecisionDue(const GcState & state, uint64_t current_round)
{
    /// Retired-in-snapshot: the signal is read from the adopted fold seal's per-shard `condemned_summary` — ZERO
    /// backend I/O beyond the single seal read. A summary distilled from this generation's `RunMarker::Condemned`
    /// rows says, per shard, how many entries are `delete_pending` (`pending` or `standing`) and the oldest
    /// condemn round of a `candidate` (due once `condemn_round < current_round`).
    if (state.snap_generation == 0)
        return false;   /// fresh pool: nothing condemned yet, nothing to decide.

    /// FAIL-CLOSED: a missing / undecodable seal, or a summary that is not TOTAL over gc_shards, is
    /// corrupt GC bookkeeping — force a FOLD so the round's own fail-closed path surfaces it, never a
    /// silent defer (matching the fold's throw-on-missing-adopted-seal treatment).
    std::optional<CasFoldSeal> seal;
    try
    {
        seal = readFoldSeal(state.snap_generation, state.snap_attempt);
    }
    catch (...)
    {
        return true;   /// undecodable seal => fail-closed force-fold
    }
    if (!seal)
        return true;
    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
    {
        const auto it = seal->condemned_summary.find(shard);
        if (it == seal->condemned_summary.end())
            return true;   /// summary not total over gc_shards => fail-closed force-fold
        if (it->second.pending_total > 0 || it->second.oldest_nonpending_condemn_round < current_round)
            return true;
    }
    return false;
}

RefScanSummary Gc::enumerateRefPrefix()
{
    /// One full enumeration of `cas/ns/stream/`: the raw keys, plus a lenient per-life index of the
    /// Log-kind ids among them. Lenient is deliberate — a malformed key is kept in `keys` and left
    /// unindexed, so the STRICT validation (and the round-abort it can raise) happens exactly once, in
    /// the fold's `groupRefKeys`. That holds for EVERY malformed shape: the one the parser refuses by
    /// name is absorbed per key by `parseRefObjectKeyForEnumeration`, which is what keeps this
    /// enumeration -- which runs before the fold, outside its catch -- unable to wedge the round.
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();

    RefScanSummary scan;
    static constexpr size_t kListPageLimit = CasOperation::kCasListPageKeys;
    size_t count_in_page = 0;
    op.forEachListedKey(layout.casRefsPrefix(), [&](const ListedKey & lk)
    {
        scan.keys.push_back(lk.key);
        const auto parsed = parseRefObjectKeyForEnumeration(layout, lk.key);
        if (parsed)
        {
            scan.listed_lives.insert(parsed->life_id);
            if (parsed->kind == RefObjectKind::Log)
            {
                scan.logs_by_life[parsed->life_id].insert(parsed->txn_id);
                RefTxnId & g = scan.max_log_by_life[parsed->life_id];
                if (g < parsed->txn_id)
                    g = parsed->txn_id;
            }
        }
        if (++count_in_page == kListPageLimit)
        {
            count_in_page = 0;
            ProfileEvents::increment(ProfileEvents::CASRefGlobalListPages);
        }
        return true;
    }, Retry::standard(), kListPageLimit, onGcEnumerationPage);
    /// The walk's list lands at least once even for an empty/undersized final page --
    /// count it (one increment per physical LIST call).
    if (count_in_page > 0 || scan.keys.empty())
        ProfileEvents::increment(ProfileEvents::CASRefGlobalListPages);
    return scan;
}

RoundInput Gc::listRefPrefix(const GcState & state, const std::optional<Etag> & state_etag)
{
    /// The round's ONE hint enumeration, followed by the parent coverage and authoritative catalog cut
    /// needed to build the DEFER/FOLD walk plan. The caller computes the DEFER signal from that frozen
    /// plan. A listed id absent from the later cut is dead, inert debris: it contributes no work and
    /// cannot force DEFER.
    RefScanSummary scan = enumerateRefPrefix();
    CasOperation op = store->openRequests().admit();
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(op, store->layout());
    /// TEST SEAM: see `setPostHotScanCatalogReadHookForTest`. Moved into a local before invoking (the
    /// same reason `create_namespace_step1_pre_read_hook_for_test` is swapped rather than called
    /// directly): a hook that reassigns the member from inside its own body would otherwise reassign
    /// the very `std::function` executing it.
    if (post_hot_scan_catalog_read_hook_for_test)
    {
        std::function<void()> hook_to_run;
        std::swap(hook_to_run, post_hot_scan_catalog_read_hook_for_test);
        hook_to_run();
    }
    if (round_point_hook_for_test)
        runRoundPointHookForTest(RoundPointForTest::AfterCatalogRead);
    catalog_cut.life_index.throwIfAmbiguous("CAS GC hot scan");
    store->reconcileRefCatalogCut(catalog_cut);

    const std::optional<CasFoldSeal> seal = readFoldSeal(state.snap_generation, state.snap_attempt);
    if (!seal && state.snap_generation > 0)
    {
        const String failure = fmt::format(
            "CAS GC hot scan: adopted fold seal (generation {}, attempt {}) is missing",
            state.snap_generation, state.snap_attempt);
        throwIfStateMoved(op, state_etag, failure);
        throw Exception(ErrorCodes::CORRUPTED_DATA, "{}", failure);
    }
    if (seal)
        scan.parent_ref_lives = seal->ref_lives;

    for (const NamespaceLifePhysicalId life_id : scan.listed_lives)
        if (!catalog_cut.life_index.resolve(life_id))
            ++scan.dead_life_debris;

    return RoundInput{std::move(scan), catalog_cut};
}

RebuildReport Gc::rebuildBaseline(bool force)
{
    /// The `gc/state` disaster-recovery command.
    /// DRY: the engine is the round's own bricks — one catalog cut for the universe,
    /// foldManifestEdges(+1) for edge emission, foldDeltasIntoGeneration with EMPTY priors
    /// (attempt-iterated for O(budget) memory), computeHeartbeatFloor for the round mint.
    /// Writes ONLY the GC plane; namespace streams/state, manifests, and blobs are read-only inputs;
    /// the rebuild never deletes them.
    RebuildReport rep;
    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();
    /// The rebuild reads through the same seam the fold does, so the two share one implementation of
    /// "read this key" rather than growing a second.
    ///
    /// WHAT THAT MEANS HERE, exactly: `readCheckpointWitnesses` hints its own keys and does not ask who
    /// called it, so the rebuild's checkpoint reads ARE fetched ahead, on the same pool and by the same
    /// rule as the fold's. That is a gain and not an accident -- a rebuild reads every namespace's
    /// checkpoint too. Nothing on THIS path hints a manifest key, so `foldManifestEdges` below takes
    /// them one at a time, exactly as it did before: a rebuild walks a plan it already holds rather
    /// than discovering its next key from the body it just read, so a lookahead would have nothing to
    /// hide behind.
    GcReadAhead reads(op, store->openRequests(), *io_pool, store->poolConfig().gc_io_concurrency);

    /// Read bookkeeping health before the lease (the lease acquire on an absent state CREATES a
    /// bootstrap body, which must not make scenario (а) look healthy). A generation-0 ref-baseline
    /// check is deliberately postponed to the sole post-LIST work cut below.
    /// The prior seal, when `gc/state` claims one. It is the ONLY place holds live, so the rebuild
    /// either reads it and carries every hold forward, or refuses (see the refusal below).
    std::optional<CasFoldSeal> prior_seal;
    bool healthy = false;
    bool validate_generation_zero_ref_baseline = false;
    {
        const auto got = op.read(layout.gcStateKey(), Retry::standard());
        /// The state's own decode stays inside its own try: an undecodable `gc/state` IS scenario (а),
        /// the disaster this command exists for. The prior-seal refusal below must NOT be swallowed by
        /// that catch, so the seal is read outside it.
        std::optional<GcState> decoded;
        if (got)
        {
            try
            {
                decoded = decodeGcState(got->bytes);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// undecodable state = scenario (а)
            }
        }
        if (decoded && decoded->header_generation < G_BLOB_INTENTS)
            throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION, "gc/state is at generation {} in a generation 4 pool",
                decoded->header_generation);
        if (decoded)
        {
            const GcState & st = *decoded;
            healthy = true;
            if (st.snap_generation == 0)
            {
                /// This check needs the rebuild universe. Delay it until after lease acquisition, the
                /// zero-mutation generation-0 drain, the completed hot LIST and the sole fresh work cut.
                /// No pre-lease catalog snapshot may become authority for successor construction.
                validate_generation_zero_ref_baseline = true;
            }
            if (st.snap_generation > 0)
            {
                /// THE REFUSAL. The prior seal is where every hold lives, and a rebuild
                /// rewrites coverage from owner state -- so with no readable seal it would hand back a
                /// baseline that LOOKS proven while silently discarding holds it cannot even enumerate.
                /// The alternative, a "pool-wide hold" on the rebuilt baseline, is not representable:
                /// every hold names a position the fold must fold THROUGH, and a pool-wide one would
                /// need an invented position nothing could ever resolve. So the honest branch is the
                /// already-safe one -- refuse, and name pool recreation. FORCE does not buy past this:
                /// force means "rebuild deliberately", never "drop the holds". It THROWS rather than
                /// returning `rep.refusal` because, unlike every other refusal here, no flag and no
                /// retry makes it succeed.
                std::optional<CasFoldSeal> seal;
                try
                {
                    seal = readFoldSeal(st.snap_generation, st.snap_attempt);
                }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS GC rebuild: the prior fold seal (generation {}, attempt {}) is UNDECODABLE "
                        "({}), so the holds it carries cannot be read. A rebuild that dropped them would "
                        "bless a baseline whose frontier is unproven. GC refuses to rebuild; this pool "
                        "must be recreated.",
                        st.snap_generation, st.snap_attempt, e.message());
                }
                if (!seal)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS GC rebuild: the prior fold seal (generation {}, attempt {}) is MISSING under "
                        "a gc/state that claims it, so the holds it carries cannot be read. A rebuild "
                        "that dropped them would bless a baseline whose frontier is unproven. GC refuses "
                        "to rebuild; this pool must be recreated.",
                        st.snap_generation, st.snap_attempt);
                for (const RunRef & r : seal->blob_target_runs)
                    if (!op.head(r.key, Retry::standard()))
                        healthy = false;
                prior_seal = std::move(seal);
                rep.adopted_seal_generation = st.snap_generation;
            }
        }

        /// NO ADOPTED BASELINE IS NAMED: `gc/state` is absent, undecodable, or sits at generation 0.
        /// The holds live in the SEAL, not in the pointer to it, so stopping here would make losing the
        /// pointer -- the LESSER corruption -- produce a hold-free baseline, while an unreadable seal
        /// refuses. That asymmetry is inverted, and it matters because holds are not re-derivable:
        /// `WitnessDisappeared` names a record that is gone, so the next walk reads a clean frontier
        /// and hands the namespace exactly the proof the hold exists to deny.
        ///
        /// So find the newest fold seal OBJECT by enumeration and carry ITS holds. This keeps the
        /// pool's disaster recovery intact -- losing `gc/state` on a lived-in pool is the scenario this
        /// command exists for -- while making a hold-free baseline over a pool that had holds
        /// unreachable.
        ///
        /// An unadopted deposed-leader attempt can be the newest object, and carrying its holds
        /// over-holds RATHER THAN under-holds -- but that claim needs its qualifier, because it is not
        /// universal. It holds outside the lying-store corner. The exception is a deposed attempt that
        /// FOLDED THROUGH a position on a read that was transient: its seal records the hold as
        /// cleared, the adopted leader's seal still holds it, and adopting the deposed one loses that
        /// hold. Bounded by the refusal above (a seal above the listing's maximum refuses outright) and
        /// by the fact that both attempts walked the same cursor state.
        if (!decoded || decoded->snap_generation == 0)
        {
            const auto newest = newestFoldSealRef();
            /// Absent here is the VIRGIN verdict, and it is the one path left on which a durable hold
            /// can still be dropped without anything failing. It is REPORTED on the command's own row,
            /// not merely logged: this command is run by hand during a disaster, and its operator is
            /// exactly the person who needs to know the clean slate was inferred rather than proved.
            rep.virgin_by_enumeration = !newest.has_value();
            if (newest)
            {
                std::optional<CasFoldSeal> seal;
                try
                {
                    seal = readFoldSeal(newest->first, newest->second);
                }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS GC rebuild: gc/state names no adopted baseline and the newest fold seal "
                        "(generation {}, attempt {}) is UNDECODABLE ({}), so the holds this pool carries "
                        "cannot be read. A rebuild that dropped them would bless a baseline whose "
                        "frontier is unproven. GC refuses to rebuild; this pool must be recreated.",
                        newest->first, newest->second, e.message());
                }
                if (!seal)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS GC rebuild: gc/state names no adopted baseline and the newest fold seal "
                        "(generation {}, attempt {}) vanished between the enumeration and the read, so "
                        "the holds this pool carries cannot be read. GC refuses to rebuild; this pool "
                        "must be recreated.",
                        newest->first, newest->second);
                prior_seal = std::move(seal);
                rep.adopted_seal_generation = newest->first;
            }
            /// else: no fold seal object anywhere. That is the ONE proof that dropping nothing is safe
            /// -- a pool that never sealed a baseline has no hold to lose.
        }
    }
    if (healthy && !force && !validate_generation_zero_ref_baseline)
    {
        rep.refusal = "gc/state and every referenced artifact are healthy — a rebuild would discard "
                      "live bookkeeping; re-run with FORCE to rebuild deliberately";
        return rep;
    }

    /// Lease: single leader vs regular rounds and other rebuilds. On an absent state this CREATES
    /// a lease-bearing bootstrap body whose token anchors our final CAS. allow_steal=false: this is a
    /// manual disaster-recovery command, same reasoning as the manual GC round (runRegularRound's doc
    /// comment) — though it is structurally moot here too (a fresh one-shot `this` with
    /// has_observation==false always takes the non-steal branch on its one and only call), pass it
    /// explicitly rather than rely on that invariant.
    GcState state;
    std::optional<Etag> state_etag;
    bool claimed_never_held = false;
    if (!acquireOrRenewLease(state, state_etag, /*allow_steal=*/false, &claimed_never_held))
    {
        rep.refusal = "another GC leader holds the lease";
        return rep;
    }
    /// Gives back a lease this call claimed over a never-held one: a rebuild never steals, so a claim it kept
    /// would make every later rebuild, each with a fresh identity, refuse as a competing leader.
    bool baseline_installed = false;
    SCOPE_EXIT({
        if (claimed_never_held && !baseline_installed)
            giveBackRebuildLease(op, state, *state_etag);
    });

    /// Healthy `FORCE REBUILD` shares the same parent-authorized barrier as an ordinary round. A
    /// damaged-state rebuild has no adopted parent (`snap_generation == 0`) and the barrier performs
    /// zero catalog mutations. Only after that distinction is resolved do we complete the hot LIST and
    /// take the sole fresh work cut.
    const CatalogLifecycleReconcileResult drain_result = drainCompletedRemoving(state, state_etag);
    if (drain_result.authority_status != AuthorityStatus::Authoritative
        || drain_result.catalog_resolution != CatalogResolution::DrainComplete)
        throwCasRequestRetryLater("CAS GC rebuild lost authority before the catalog settled");
    const RefScanSummary rebuild_ref_scan = enumerateRefPrefix();
    const CasRefCatalog::Snapshot rebuild_work_catalog_cut = CasRefCatalog::read(op, layout);

    RefScanSummary rebuild_round_scan = rebuild_ref_scan;
    if (prior_seal)
        rebuild_round_scan.parent_ref_lives = prior_seal->ref_lives;
    const RefPlan rebuild_walk_plan = buildRefWalkPlan(
        RoundInput{std::move(rebuild_round_scan), rebuild_work_catalog_cut});
    const std::vector<NamespaceLifeId> rebuild_walk_universe = rebuild_walk_plan.lives();
    /// The exact checkpoint sample is paired with the same frozen catalog cut that chose the rebuild
    /// universe. `recoverRefTableDetailedFromAuthority` deliberately has no internal catalog or
    /// checkpoint read: a later cut could admit a different life or frontier than the one every other
    /// part of this rebuild is using.
    const CheckpointWitnesses rebuild_checkpoints = readCheckpointWitnesses(reads, {}, rebuild_walk_plan.catalogCut());

    if (validate_generation_zero_ref_baseline)
    {
        /// A generation-0 state is healthy only when no table proves that a now-lost cursor cleaned
        /// covered logs. The check consumes the same catalog-built universe as reconstruction; it does
        /// not own an earlier catalog cut or a second admission rule.
        for (const NamespaceLifeId & life : rebuild_walk_universe)
        {
            std::vector<String> table_keys;
            op.forEachListedKey(layout.namespaceStreamPrefix(life),
                [&](const ListedKey & lk) { table_keys.push_back(lk.key); return true; },
                Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
            std::map<NamespaceLifePhysicalId, RefTableListing> grouped;
            try
            {
                grouped = groupRefKeys(layout, table_keys);
            }
            catch (const Exception & e)
            {
                if (e.code() != ErrorCodes::CORRUPTED_DATA)
                    throw;
                healthy = false;
                break;
            }
            const auto grouped_it = grouped.find(life.incarnation);
            if (grouped_it != grouped.end() && !grouped_it->second.snapshots.empty()
                && (grouped_it->second.logs.empty()
                    || grouped_it->second.snapshots.back() < grouped_it->second.logs.front()))
            {
                healthy = false;
                break;
            }
        }
        if (healthy && !force)
        {
            rep.refusal = "gc/state and every referenced artifact are healthy — a rebuild would discard "
                          "live bookkeeping; re-run with FORCE to rebuild deliberately";
            return rep;
        }
    }

    /// Numbering, part 1: generation above ANY surviving gc/gen prefix (putDeterministicArtifact
    /// must never collide with debris of the lost era).
    uint64_t max_gen = state.snap_generation;
    {
        const String gen_prefix = layout.gcGenPrefix(0);
        const String top = gen_prefix.substr(0, gen_prefix.size() - 2);   /// ".../gc/gen/"
        op.forEachListedKey(top, [&](const ListedKey & k)
        {
            const size_t from = top.size();
            const size_t slash = k.key.find('/', from);
            if (slash == String::npos)
                return true;
            try
            {
                max_gen = std::max(max_gen, static_cast<uint64_t>(std::stoull(k.key.substr(from, slash - from))));
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// Foreign key shape under `gc/gen` is debris, not a numbering input.
            }
            return true;
        }, Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
    }
    const uint64_t generation = max_gen + 1;
    const uint64_t budget = rebuild_edge_budget_override ? rebuild_edge_budget_override
                                                         : store->poolConfig().rebuild_edge_budget;

    /// Per-gc-shard attempt-iterated fold state: batch k folds with attempt k and the previous
    /// attempt's runs as priors; the FINAL attempt's runs go into the seal.
    const uint64_t gc_shards = state.gc_shards ? state.gc_shards : store->poolConfig().gc_shards;
    std::vector<std::vector<BlobDelta>> buckets(gc_shards);
    std::vector<std::vector<RunRef>> prior_runs(gc_shards);
    std::vector<uint64_t> attempt_of(gc_shards, 0);
    /// The fold is EDGE-ONLY here: a rebuild condemns nothing, so no head source and no create hook is
    /// supplied. Its prior runs are its own earlier flushes, which hold no row, so no blob reaches the intent
    /// rules -- this call is `foldDeltasIntoGeneration`'s pure edge form.
    auto flush_shard = [&](uint64_t shard)
    {
        if (buckets[shard].empty())
            return;
        /// No applied-delta ledger either: the rebuild derives edges from owner state, not from ref transactions.
        RunRef run = foldDeltasIntoGeneration(op, layout, prior_runs[shard],
                                              AdoptedRunContext{.gc_shards = gc_shards, .parent_round = state.round,
                                                                .summary = CondemnedSummary{}},
                                              generation, ++attempt_of[shard], shard, std::move(buckets[shard]));
        buckets[shard].clear();
        prior_runs[shard] = {std::move(run)};
    };
    auto route_deltas = [&](std::vector<BlobDelta> & deltas)
    {
        rep.edges += deltas.size();
        for (BlobDelta & d : deltas)
        {
            const uint64_t shard = blobShard(d.ref, gc_shards);
            buckets[shard].push_back(std::move(d));
            if (buckets[shard].size() >= budget)
                flush_shard(shard);
        }
        deltas.clear();
    };

    /// `rebuild_walk_plan` was frozen immediately after the completed hot LIST and sole fresh catalog
    /// cut. Listed ids absent from that later cut are inert dead-life debris and cannot mint work or
    /// refuse reconstruction.
    std::set<String> seen_ns;
    std::set<String> owned_manifest_keys;
    CasFoldSeal seal;
    seal.generation = generation;
    seal.parent_generation = state.snap_generation;
    seal.ref_lives = rebuild_walk_plan.successorFoldStates();
    uint64_t max_fence_round = 0;
    std::map<ManifestId, Etag> mf_cleanup_unused;

    for (const NamespaceLifeId & life : rebuild_walk_universe)
    {
        const RootNamespace & ns = life.ns;
        seen_ns.insert(ns.string());
        ++rep.shards;

        /// Recover from the exact catalog row and exact checkpoint paired with this plan. A visible
        /// log above `committed_through` is not logical history yet, and a Live/Removing row without a
        /// readable checkpoint has no bounded recovery frontier; both cases must refuse rather than
        /// letting a stream LIST decide what this baseline protects.
        const auto entry_it = std::lower_bound(
            rebuild_walk_plan.catalogCut().catalog.entries.begin(), rebuild_walk_plan.catalogCut().catalog.entries.end(), ns,
            [](const CatalogEntry & entry, const RootNamespace & needle) { return entry.ns < needle; });
        chassert(entry_it != rebuild_walk_plan.catalogCut().catalog.entries.end());
        chassert(entry_it->ns == ns);
        chassert(entry_it->incarnation == life.incarnation);
        if (const auto bad_checkpoint = rebuild_checkpoints.undecodable.find(ns.string());
            bad_checkpoint != rebuild_checkpoints.undecodable.end())
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS GC rebuild: catalog life {} has an undecodable checkpoint: {}", ns.string(), bad_checkpoint->second);
        std::optional<CkptSample> checkpoint;
        if (const auto checkpoint_it = rebuild_checkpoints.recovery_checkpoints.find(ns.string());
            checkpoint_it != rebuild_checkpoints.recovery_checkpoints.end())
            checkpoint = checkpoint_it->second;
        /// The plan is frozen, so a moved base cannot be re-read here; the next attempt plans again.
        const std::optional<RecoveredRefTable> recovered = recoverRefTableDetailedFromAuthority(
            op, layout, *entry_it, checkpoint);
        if (!recovered)
            throwCasRequestRetryLater("CAS GC rebuild: the checkpoint of namespace '" + ns.string()
                + "' moved while its ref chain was read");
        const RefTableState & st = recovered->state;

        RefCoverage cov;
        cov.classification = CoverageClass::Folded;   /// unless the prior seal's hold is carried below
        cov.last_folded_ref_id = st.getGreatestApplied();

        std::vector<BlobDelta> deltas;

        /// Committed owners: a missing/invalid body under a committed ref is DATA LOSS the rebuild must
        /// not bless -- refuse.
        for (const auto [ref_name, row] : st.getCommitted())
        {
            const ManifestId id{ns, row.manifest_ref};
            owned_manifest_keys.insert(layout.manifestKey(id));
            if (!foldManifestEdges(reads, id, +1, deltas, mf_cleanup_unused, /*txn_ordinal=*/0))
            {
                rep.refusal = "committed ref '" + ns.string() + "/" + ref_name
                    + "' names a missing or invalid part manifest — that is DATA LOSS the rebuild "
                      "must not bless; run fsck forensics first";
                return rep;
            }
            ++rep.committed_refs;
        }

        /// Live precommits: an owner like a committed ref, so its manifest must be readable. A rebuild
        /// cannot hold the namespace until the body lands: the precommit's log is below the rebuilt cursor,
        /// no round re-reads it, and its promote emits no edge.
        for (const auto & [ref_name, manifest_ref] : st.getPrecommits())
        {
            const ManifestId id{ns, manifest_ref};
            owned_manifest_keys.insert(layout.manifestKey(id));
            if (!foldManifestEdges(reads, id, +1, deltas, mf_cleanup_unused, /*txn_ordinal=*/0))
            {
                rep.refusal = "live precommit '" + ns.string() + "/" + ref_name + "' names part manifest "
                    + layout.manifestKey(id) + ", which cannot be read, so the blobs it references cannot be "
                      "counted. Its writer is still uploading the manifest or stopped before it did; retry after "
                      "the writer finishes, or after its server mounts the table again and its stale-precommit "
                      "sweep removes the precommit";
                return rep;
            }
            ++rep.live_precommits;
        }
        route_deltas(deltas);

        /// HOLDS RIDE THROUGH A REBUILD VERBATIM. The rebuild derives coverage from owner
        /// state, which knows nothing about a ref-log position that would not resolve -- so without
        /// this every held row would be overwritten by a clean one and the rebuilt baseline would claim
        /// a frontier proof it does not have. `retry_count` and `next_retry_round` ride unchanged too:
        /// the rebuild retried nothing, so it must not reset the count that says how long the namespace
        /// has been stuck. The ordinary clearing rule then applies from the next round on.
        if (prior_seal)
        {
            const auto pit = prior_seal->ref_lives.find(life.incarnation);
            if (pit != prior_seal->ref_lives.end() && pit->second.coverage.hold)
            {
                cov.classification = CoverageClass::Clamped;
                cov.hold = pit->second.coverage.hold;
            }
        }

        RefLifeFoldState & row = seal.ref_lives.at(life.incarnation);
        row.coverage = cov;
    }
    rep.namespaces = seen_ns.size();

    /// Trimmed-but-live precommits: a build alive across trim has NO journal
    /// evidence; its manifests look unowned. Include edges of every manifest that is unowned AND
    /// not provably build-dead (the watermark fact) — over-protect. An unowned manifest that later
    /// dies without journal evidence leaks its edges until a future rebuild (documented, bounded,
    /// fsck-visible); provably-dead ones stay excluded (the orphan sweep owns their bodies).
    for (const String & ns_str : seen_ns)
    {
        const RootNamespace ns{ns_str};
        std::vector<BlobDelta> deltas;
        op.forEachListedKey(layout.manifestNamespacePrefix(ns), [&](const ListedKey & k)
        {
            if (owned_manifest_keys.contains(k.key))
                return true;
            /// The one shared manifest-path parser for the canonical hexadecimal manifest identifier,
            /// also used by fsck's parseBuildPrefix and the orphan sweep's parseListedManifestObject.
            const auto parsed = layout.parseManifestKey(k.key);
            if (!parsed)
                return true;   /// foreign key shape — debris
            const ManifestRef & mref = parsed->ref;
            if (buildPrefixEligibleUnderFloor(*store, ns, BuildPrefix{mref.writer_epoch, mref.build_sequence}))
                return true;   /// provably dead — the orphan sweep's territory, never an edge
            const ManifestId id{ns, mref};
            if (foldManifestEdges(reads, id, +1, deltas, mf_cleanup_unused, /*txn_ordinal=*/0))
            {
                ++rep.unowned_alive_manifests;
                route_deltas(deltas);
            }
            /// A missing/invalid UNOWNED body is debris (no owner claims it) — skip, never refuse.
            return true;
        }, Retry::standard(), CasOperation::kCasListPageKeys, onGcEnumerationPage);
    }

    /// A REBUILD CONDEMNS NOTHING.
    ///
    /// It used to end here with a LIST of `blobs/`, condemning every listed body its traversal had not
    /// reached ("pipeline blindness repair": the fold discovers candidates by TRANSITIONS to zero, so a
    /// blob whose edges were already gone by rebuild time would have no row and never be reclaimed).
    /// The premise was that a full traversal knows every live blob. It does not. BOTH legs of the
    /// traversal above are listing-driven -- the owner replay reads the ref prefix, the trimmed-but-live
    /// pass reads the manifest prefix -- so a store that omits a durable key from one enumeration hides
    /// a LIVE owner, and this pass would then condemn the very blob that owner pins: one lying
    /// enumeration, and acked data is scheduled for deletion. Hiding is not
    /// hypothetical here; it is the observed `0x1430c` shape that made every ref walk arithmetic.
    ///
    /// The residual this leaves: a blob whose manifest no
    /// longer exists anywhere is unreclaimable -- nothing can enumerate it safely -- until the
    /// build/upload registry can say which uploads are in flight. It is retention, not loss, and it is
    /// bounded by that registry landing. NO substitute reclamation is added in its place: any cheaper
    /// rule that reclaims from an enumeration is the same vector wearing a different hat, and a
    /// fallback that deletes on incomplete evidence is precisely what "fail closed" forbids.
    ///
    /// Numbering, part 2: the round above every surviving fence/state/generation number.
    const uint64_t round = std::max({max_fence_round, state.round, max_gen}) + 1;

    for (uint64_t shard = 0; shard < gc_shards; ++shard)
        flush_shard(shard);   /// real-edge rows only: a rebuild condemns nothing, so it seeds nothing

    for (uint64_t shard = 0; shard < gc_shards; ++shard)
        for (const RunRef & r : prior_runs[shard])
            seal.blob_target_runs.push_back(r);

    /// Also fence out any dead mounts as part of the disaster-recovery pass (liveness cleanup; the
    /// returned classification counts are not needed for the round mint — row decisions pace on rounds).
    /// Use the same threshold/`mount_obs` as the regular round.
    const uint64_t ttl_ms = static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count());
    const uint64_t renew_period_ms = static_cast<uint64_t>(store->poolConfig().mount_renew_period.count());
    computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn, ttl_ms, renew_period_ms, mount_obs, lease_mismatch_warned);

    /// Retired-in-snapshot: the rebuilt seal's `condemned_summary` must be TOTAL over gc_shards so a
    /// subsequent regular round reads row-decision/carry decisions zero-I/O off it (and its `carryParentRefs`
    /// totality check does not fail closed). Every entry is EMPTY -- a rebuild condemns nothing -- and
    /// the totality is still owed: an ABSENT row and a zero row are different claims, and the round that
    /// reads this seal fails closed on the absent one.
    for (uint64_t shard = 0; shard < gc_shards; ++shard)
        seal.condemned_summary[shard] = CondemnedSummary{};

    /// Seal (deterministic artifact) + the single state CAS. attempt = the max per-shard attempt
    /// (>= 1 so the seal key is stable даже for an empty universe).
    uint64_t seal_attempt = 1;
    for (uint64_t a : attempt_of)
        seal_attempt = std::max(seal_attempt, a);
    validateFoldSealForWrite(seal, layout, gc_shards);
    putDeterministicArtifact(op, layout.foldSealKey(generation, seal_attempt), encodeFoldSeal(seal));

    GcState next = state;
    next.round = round;
    next.snap_generation = generation;
    next.snap_attempt = seal_attempt;
    /// No retired set to publish: a rebuild condemns nothing (the deletion above), so there is nothing
    /// to retire in the first place. Retired-in-snapshot removed the separate `RetiredSet` object
    /// family independently of that, and the two reasons are stated apart on purpose — a future reader
    /// must not take this line as evidence that REBUILD still produces condemnations somewhere.
    next.manifest_sweep_cursor = "";
    /// The rebuilt state has no row, so every intent is an orphan again; the sweep starts over to meet each.
    next.intent_sweep_cursor = "";
    WriteResult commit = op.replace(layout.gcStateKey(), encodeGcState(next), *state_etag,
                                    Retry::standard());
    if (std::holds_alternative<Conflict>(commit))
    {
        rep.refusal = "gc/state changed under the rebuild (a competing writer) — re-run";
        return rep;
    }
    orThrow(std::move(commit), "CAS gc rebuild: gc/state commit");
    baseline_installed = true;

    rep.performed = true;
    rep.round = round;
    rep.generation = generation;
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcRebuild;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = round;
        e.gen = generation;
        e.outcome = "performed";
        e.reason = "raw baseline rebuild from owner state (gc/state disaster recovery)";
        e.detail = {{"namespaces", std::to_string(rep.namespaces)},
                    {"shards", std::to_string(rep.shards)},
                    {"committed_refs", std::to_string(rep.committed_refs)},
                    {"live_precommits", std::to_string(rep.live_precommits)},
                    {"unowned_alive_manifests", std::to_string(rep.unowned_alive_manifests)},
                    {"edges", std::to_string(rep.edges)},
                    {"clamped_shards", std::to_string(rep.clamped_shards)},
                    {"force", force ? "1" : "0"}};
    });
    return rep;
}

Gc::DeletePreview Gc::previewDeletes()
{
    CasOperation op = store->openRequests().admit();
    const Layout & layout = store->layout();

    const auto preview_of = [&](const GcState & state)
    {
        DeletePreview preview;
        /// Resolve the run objects THROUGH the adopted seal's refs, never by
        /// `blobTargetRunKey` construction: with reference-parent carry a shard's current run may physically
        /// live under an older generation's key, and the seal ref is the only authority for the real key.
        /// Group the adopted `blob_target_runs` by the ref's explicit `shard`. No seal at generation 0 => no candidates.
        std::map<uint64_t, std::vector<RunRef>> runs_by_shard;
        std::map<uint64_t, CondemnedSummary> summaries;
        if (const auto adopted = readFoldSeal(state.snap_generation, state.snap_attempt))
        {
            preview.snapshot = SnapshotVerdict::Adopted;
            for (const RunRef & r : adopted->blob_target_runs)
                runs_by_shard[r.shard].push_back(r);
            summaries = adopted->condemned_summary;
        }
        else if (state.snap_generation > 0)
        {
            preview.snapshot = SnapshotVerdict::SealMissing;
            preview.missing_seal = fmt::format(
                "CAS GC: the adopted fold seal (generation {}, attempt {}) is missing under a live gc/state; expected key {}",
                state.snap_generation, state.snap_attempt, layout.foldSealKey(state.snap_generation, state.snap_attempt));
            return preview;
        }

        /// Every shard, read the way a round reads it: a shard the round would refuse adds no row and is named instead.
        for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
        {
            const AdoptedShardPreview rows = previewAdoptedShard(op, runs_by_shard[shard], shard,
                AdoptedRunContext{.gc_shards = state.gc_shards, .parent_round = state.round, .summary = summaries[shard]});
            if (rows.refusal)
            {
                preview.refused_shards.push_back(rows.refusal->message);
                continue;
            }
            for (const BlobRef & ref : rows.zero)
            {
                const std::optional<Meta> observed = op.head(layout.blobKey(ref), Retry::standard());
                if (!observed)
                    continue;
                preview.deletes.push_back(PreviewEntry{.kind = ObjectKind::Blob, .ref = ref, .key = layout.blobKey(ref),
                    .size = observed->size, .reason = "unreachable", .token = {}, .condemn_round = 0});
            }
            /// A condemned row carries the incarnation it condemned, so it needs no observation.
            for (const SourceEdgeRecord & rec : rows.condemned)
            {
                if (!rec.standing)
                {
                    preview.deletes.push_back(PreviewEntry{.kind = ObjectKind::Blob, .ref = rec.ref, .key = layout.blobKey(rec.ref),
                        .size = rec.size, .reason = rec.delete_pending ? "pending" : "candidate", .token = rec.token,
                        .condemn_round = rec.condemn_round});
                    continue;
                }
                /// The next round removes the intent of a standing row's token, if it names one; no `HEAD` is sent here.
                const std::optional<CanonicalIdentity> identity = rec.token.canonical();
                if (!identity)
                    continue;
                preview.deletes.push_back(PreviewEntry{.kind = ObjectKind::Blob, .ref = rec.ref,
                    .key = layout.blobIntentKey(rec.ref, *identity), .size = blobIntentRecord().size(), .reason = "standing",
                    .token = rec.token, .condemn_round = rec.condemn_round});
            }
        }
        return preview;
    };

    const auto unread_result = [](String why)
    {
        DeletePreview unread;
        unread.snapshot = SnapshotVerdict::Unread;
        unread.unread = std::move(why);
        return unread;
    };

    uint64_t highest_generation_seen = 0;
    for (size_t attempt = 1;; ++attempt)
    {
        const auto state_bytes = op.read(layout.gcStateKey(), Retry::standard());
        if (!state_bytes)
        {
            if (attempt == 1)
                return {};
            return unread_result("gc/state vanished while the GC snapshot was being confirmed");
        }
        const GcState state = decodeGenerationFourGcState(state_bytes->bytes);
        /// A round never lowers the generation; a lower one means the snapshot this invocation started from is gone.
        if (state.snap_generation < highest_generation_seen)
            return unread_result(fmt::format(
                "gc/state went from snapshot generation {} back to {} while the GC snapshot was being confirmed",
                highest_generation_seen, state.snap_generation));
        highest_generation_seen = state.snap_generation;
        DeletePreview preview = preview_of(state);
        if (preview.refused_shards.empty() && preview.snapshot != SnapshotVerdict::SealMissing)
            return preview;
        /// A refusal or an absent seal is the round's only under the `gc/state` it was read under: a newer round may have pruned it.
        const std::optional<Meta> state_now = op.head(layout.gcStateKey(), Retry::standard());
        if (!state_now)
            return unread_result("gc/state vanished while the GC snapshot was being confirmed");
        if (sameVersion(state_now->etag, state_bytes->etag))
            return preview;
        if (attempt == kAdoptedSnapshotReadAttempts)
            return unread_result(fmt::format(
                "gc/state moved during each of {} reads of the GC snapshot, so its rows were not read", attempt));
    }
}

void Gc::rememberObservation(const GcLease & lease)
{
    has_observation = true;
    last_seen_owner = lease.owner;
    last_seen_seq = lease.seq;
}

void Gc::refreshAuthority(uint64_t admitted_generation, const Retry & policy)
{
    /// Fail-closed first, so every early exit below leaves this leader deposed.
    authority_held = false;
    try
    {
        CasOperation op = store->openRequests().admit();
        const auto got = op.read(store->layout().gcStateKey(), policy);
        if (!got)
            return;
        const GcState current = decodeGcState(got->bytes);
        authority_held = current.lease.owner == gc_id && current.lease.seq == admitted_generation;
    }
    catch (...)
    {
        tryLogCurrentException(logger,
            "CAS gc: the leader-authority probe failed; this round's destructive operations treat the "
            "lease as lost");
    }
}

void Gc::pulseHeartbeat(Pool & store, UInt128 gc_id)
{
    CasOperation op = store.openRequests().admit();
    const String key = store.layout().gcHbKey();
    const auto got = op.read(key, Retry::standard());
    GcHeartbeat hb;
    if (got)
        hb = decodeGcHeartbeat(got->bytes);
    hb.owner = gc_id;
    ++hb.hb_seq;
    /// ONE attempt, and the outcome is discarded: a pulse that loses its race is replaced by the next
    /// one on cadence, so a deposed leader must never spend a whole retry budget fighting for this key.
    const String body = encodeGcHeartbeat(hb);
    if (got)
        op.replace(key, body, got->etag, Retry::once());
    else
        op.create(key, body, Retry::once());
}

bool Gc::acquireOrRenewLease(GcState & state, std::optional<Etag> & state_etag, bool allow_steal,
                             bool * claimed_never_held)
{
    CasOperation op = store->openRequests().admit();
    const String key = store->layout().gcStateKey();

    /// What the decision that actually landed wrote. `readModifyWrite` re-decides on every conflict
    /// against what the losing write's own resolve read observed, so the last decision is the committed
    /// one, and a competing leader's write makes the next decision see a moved lease tuple.
    GcState decided;
    bool decided_never_held = false;
    const std::optional<Etag> committed = orThrow(op.readModifyWrite(key,
        [&](const std::optional<Object> & current_object) -> std::optional<String>
    {
        decided_never_held = false;
        if (!current_object)
        {
            if (has_observation)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state vanished after being observed (owner {}, seq {})",
                    u128ToHex(last_seen_owner), last_seen_seq);

            GcState fresh;
            fresh.lease = GcLease{gc_id, 1};
            /// Creation-time only: gc_shards is set ONCE on first-ever acquire; subsequent rounds read
            /// the authoritative value from the persisted GcState (pool is authoritative on reopen).
            /// PoolConfig carries the configured value from the disk XML.
            fresh.gc_shards = store->poolConfig().gc_shards;
            decided = fresh;
            decided_never_held = true;
            return encodeGcState(fresh);
        }

        const GcState current = decodeGenerationFourGcState(current_object->bytes);
        if (current.gc_shards != store->poolConfig().gc_shards)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc/state gc_shards {} disagrees with the pool-authoritative _pool_meta value {}",
                current.gc_shards, store->poolConfig().gc_shards);

        if (current.lease.owner == gc_id)
        {
            GcState next = current;
            ++next.lease.seq;
            decided = next;
            return encodeGcState(next);
        }

        /// Owner 0 with seq 0 is a lease nobody holds: the migration tool creates `gc/state` so, and a refused
        /// rebuild gives its claim back to it. No incumbent can be alive, so any caller claims it, and the
        /// conditional write admits one claimant.
        if (current.lease.owner == UInt128(0) && current.lease.seq == 0)
        {
            GcState next = current;
            next.lease = GcLease{gc_id, 1};
            decided = next;
            decided_never_held = true;
            return encodeGcState(next);
        }

        GcHeartbeat hb;
        if (const auto hb_got = op.read(store->layout().gcHbKey(), Retry::standard()))
            hb = decodeGcHeartbeat(hb_got->bytes);
        /// Observation-based heartbeat liveness, symmetric with the frozen-lease-tuple check below:
        /// ANY movement of the observed (owner, hb_seq) pair between this contender's two ticks is
        /// proof of life, and `hb_seq` values are comparable only under the SAME remembered hb owner.
        /// Deliberately NOT compared against `current.lease.owner`: a deposed leader's heartbeat
        /// thread keeps pulsing (with `owner = itself`) until its next round resets `i_am_leader`,
        /// and its writes can race out the live new leader's pulses — an hb pair that keeps moving
        /// under the OLD owner's name must still read as "alive", or a live, pulsing new leader gets
        /// its lease stolen. An hb owner change re-arms the window (this tick's pair is remembered
        /// below); a steal happens only once the lease tuple AND the hb pair are both frozen across
        /// a full window.
        const bool hb_alive = has_observation
            && (hb.owner != last_seen_hb_owner || hb.hb_seq > last_seen_hb_seq);

        const bool incumbent_renewed = !has_observation
            || current.lease.owner != last_seen_owner
            || current.lease.seq != last_seen_seq;
        if (incumbent_renewed || hb_alive || !allow_steal)
        {
            /// Only ARM the steal-decision state (last_seen_owner/seq/hb_*) when this call is itself
            /// allowed to act on a frozen tuple - i.e. the loop path. A caller with allow_steal=false
            /// (manual `SYSTEM ... GC`) reads current state for its own acquire/renew/back-off decision
            /// above, but must NOT record this foreign-incumbent observation: doing so would let the
            /// loop's own very next tick treat THIS snapshot as one half of ITS two-observation window,
            /// without the real wall-time gap (>= H) the window's safety argument requires between the
            /// loop's own ticks (a manual command can land microseconds before a scheduled tick). Leaving
            /// last_seen_* untouched here keeps the frozen-tuple comparison that can trigger a steal
            /// between two LOOP observations only, always >= interval apart.
            if (allow_steal)
            {
                rememberObservation(current.lease);
                last_seen_hb_owner = hb.owner;
                last_seen_hb_seq = hb.hb_seq;
            }
            return std::nullopt;
        }

        GcState next = current;
        next.lease.owner = gc_id;
        ++next.lease.seq;
        decided = next;
        return encodeGcState(next);
    }, Retry::standard()), "CAS gc lease");

    if (!committed)
        return false;   /// declined: a live incumbent holds the lease

    rememberObservation(decided.lease);
    state = std::move(decided);
    state_etag = committed;
    if (claimed_never_held)
        *claimed_never_held = decided_never_held;
    return true;
}

void Gc::giveBackRebuildLease(CasOperation & op, const GcState & claimed, const Etag & claim_etag) noexcept
{
    const auto warn = [&](const String & why)
    {
        LOG_WARNING(logger,
            "CAS GC rebuild refused but could not give back its GC lease (owner {}, seq {}): {}. If gc/state still "
            "holds that lease, the next rebuild reports 'another GC leader holds the lease'",
            u128ToHex(claimed.lease.owner), claimed.lease.seq, why);
    };
    try
    {
        GcState restored = claimed;
        restored.lease = GcLease{};
        const WriteResult written = op.replace(store->layout().gcStateKey(), encodeGcState(restored), claim_etag,
                                               Retry::once());
        if (std::holds_alternative<Conflict>(written))
            warn("gc/state changed after the claim");
        else if (!std::holds_alternative<Committed>(written))
            warn("the conditional write was not committed");
    }
    catch (...)
    {
        /// The warning allocates, and an exception that escapes this `noexcept` function terminates the process.
        try
        {
            warn(getCurrentExceptionMessage(/*with_stacktrace=*/false));
        }
        catch (...)
        {
            tryLogCurrentException(logger, "CAS GC rebuild refused but could not give back its GC lease");
        }
    }
}

void Gc::OutcomeTally::add(OutcomeKind kind)
{
    switch (kind)
    {
        case OutcomeKind::Deleted: ++deleted; break;
        case OutcomeKind::Absent: ++absent; break;
        case OutcomeKind::Replaced: ++replaced; break;
        case OutcomeKind::Spared: ++spared; break;
    }
}

CatalogLifecycleReconcileResult Gc::drainCompletedRemoving(const GcState & leased_state, const std::optional<Etag> & leased_etag)
{
    uint64_t completed_erases = 0;
    uint64_t cancelled_creating = 0;
    return drainCompletedRemoving(leased_state, leased_etag, completed_erases, cancelled_creating);
}

CatalogLifecycleReconcileResult Gc::drainCompletedRemoving(
    const GcState & leased_state, const std::optional<Etag> & leased_etag, uint64_t & completed_erases,
    uint64_t & cancelled_creating)
{
    /// Without an adopted parent this drain does not run, so a stalled `Creating` row also waits for the
    /// first round that has one. It owns no edges, so the wait costs that round and no safety.
    if (leased_state.snap_generation == 0)
        return {
            .authority_status = AuthorityStatus::Authoritative,
            .catalog_resolution = CatalogResolution::DrainComplete,
            .final_catalog_cut = std::nullopt};

    const std::optional<CasFoldSeal> parent = readFoldSeal(
        leased_state.snap_generation, leased_state.snap_attempt);
    if (!parent)
    {
        const String failure = fmt::format(
            "CAS GC pre-fold drain: adopted parent seal (generation {}, attempt {}) is missing",
            leased_state.snap_generation, leased_state.snap_attempt);
        CasOperation probe = store->openRequests().admit();
        throwIfStateMoved(probe, leased_etag, failure);
        throw Exception(ErrorCodes::CORRUPTED_DATA, "{}", failure);
    }

    /// The drain erases catalog rows, so its operation carries this leader's authority as its
    /// liveness: `CatalogLifecycleReconciler` and `deleteCompletedRemovingAtSnapshot` decide
    /// `FencedOut` from `op.admitted()`, and the GC plane's fence is open, so without this the verdict
    /// would be a constant TRUE and a deposed leader would keep erasing. The refresh below runs at the
    /// top of every erase attempt, so a leader deposed between two erases of one drain stops before the
    /// second -- the single reading taken here would otherwise authorise all of them.
    const uint64_t admitted_generation = leased_state.lease.seq;
    refreshAuthority(admitted_generation);
    CasOperation op = store->openRequests().admit([this] { return authority_held; });
    CatalogLifecycleReconciler reconciler(op, store->layout(), *parent,
        [this, &op](const CreatorFence & creator)
        {
            return probeCreatorSlot(op, store->layout(), creator.server_root_id, creator.writer_epoch);
        });
    SCOPE_EXIT({
        completed_erases = reconciler.completedErases();
        cancelled_creating = reconciler.cancelledCreating();
    });
    /// The announcement before the erase refuses admissions already in flight; a presence probe or a
    /// drop that admitted the `Removing` row after it holds a runtime that the second one detaches; without
    /// it only a writer's catalog check in `namespaceLife` would. A cancelled `Creating` life is recorded
    /// only by this, after its cancel returned, because an opener on this server may take it over first.
    const auto invalidate_retired = [&]
    {
        for (const NamespaceLifeId & life : reconciler.retiredLives())
            store->invalidateRemovedCatalogLife(life);
    };
    try
    {
        CatalogLifecycleReconcileResult result = reconciler.reconcile(
            [this, admitted_generation](const Retry & policy) { refreshAuthority(admitted_generation, policy); },
            [this](const NamespaceLifeId & life) { store->invalidateRemovedCatalogLife(life); });
        invalidate_retired();
        return result;
    }
    catch (...)
    {
        invalidate_retired();
        throw;
    }
}

}
