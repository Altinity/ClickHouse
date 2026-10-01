#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>
#include <Common/scope_guard_safe.h>
#include <Common/setThreadName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <base/scope_guard.h>
#include <fmt/format.h>
#include <Poco/Exception.h>
#include <algorithm>
#include <atomic>
#include <deque>

namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
}

namespace DB::Cas
{

namespace
{

/// The legacy `casPut` this write replaces reported a definite conflict as a value (never a failure to
/// this caller) and reported a store failure -- a refusal, an exhausted policy -- by throwing. Only
/// `Refused`/`GaveUp` are the alternatives a thrown exception used to carry, so only those propagate;
/// `Committed`/`Declined`/`Conflict` stay silent exactly as they did before.
void throwOnRefusedOrGaveUp(WriteResult && result, std::string_view what)
{
    if (std::holds_alternative<Refused>(result) || std::holds_alternative<GaveUp>(result))
        (void)orThrow(std::move(result), what);
}

enum class JobOutcome : uint8_t
{
    Deleted,
    Leaked,
    Held,
    Skipped,
    /// The enqueue was refused, so the job never ran; held like a lost authority.
    EnqueueRefused,
};

/// One delete job: written by whoever runs it, read by the round thread after every job finished.
struct JobSlot
{
    std::vector<WriteOnceKey> keys;
    JobOutcome outcome = JobOutcome::Skipped;
    std::exception_ptr error;
};

/// The page whose jobs are in flight. It joins the published prefix once its jobs settle without a hold.
struct PendingPage
{
    String next_cursor;
    bool decided = false;
};

String describeKeys(const std::vector<WriteOnceKey> & keys)
{
    if (keys.size() == 1)
        return "'" + keys.front().str() + "'";
    return fmt::format("'{}' .. '{}' ({} keys)", keys.front().str(), keys.back().str(), keys.size());
}

/// Holding the cursor on an ordinary store failure would let one key that always fails starve every page
/// behind it; leaking costs one pass and never deletes wrongly. So only lost authority, `NOT_IMPLEMENTED`
/// and a deterministic local failure hold the page. Every other failure leaks after its last attempt, whether the
/// policy refused it at once or ran out of retries.
JobOutcome classifyFailure(const std::exception_ptr & error, const CasOperation & job_op)
{
    /// A lost-authority refusal reads false when `admitted` re-samples, whatever the exception says.
    /// A budget refusal can read true again after a renewal, so it falls through to the exception.
    if (!job_op.admitted())
        return JobOutcome::Held;
    try
    {
        std::rethrow_exception(error);
    }
    catch (const DB::Exception & e)
    {
        return isDeterministicLocalFailure(e.code()) ? JobOutcome::Held : JobOutcome::Leaked;
    }
    catch (const Poco::Exception &)
    {
        return JobOutcome::Leaked;
    }
    catch (...)
    {
        return JobOutcome::Held;
    }
}

/// Runs one job on its own operation, resumed from the phase's generation, so the mount fence and the
/// liveness predicate admit it on its own. A failed job leaks or holds by `classifyFailure`.
void runJob(JobSlot & slot, CasRequests & requests, uint64_t generation, const Liveness & liveness, std::atomic<bool> & stop)
{
    if (stop.load())
        return;
    CasOperation job_op = requests.resume(generation, liveness);
    try
    {
        job_op.removeManyWriteOnce(slot.keys, Retry::standard());
        slot.outcome = JobOutcome::Deleted;
    }
    catch (...)
    {
        slot.error = std::current_exception();
        slot.outcome = classifyFailure(slot.error, job_op);
        if (slot.outcome == JobOutcome::Held)
            stop.store(true);
    }
}

/// The exact-token delete of one listed `_ckpt`/`_files` key; false when admission was lost.
bool removeExactly(CasOperation & op, const ListedKey & listed, NamespaceJanitorResult & out)
{
    std::optional<Etag> etag = listed.etag;
    if (!etag)
    {
        try
        {
            const std::optional<Meta> current = op.head(listed.key, Retry::standard());
            if (!current)
                return true;
            etag = current->etag;
        }
        catch (const std::exception & e)
        {
            ++out.leaked;
            out.anomalies.push_back("leaked dead-life object '" + listed.key + "': exact HEAD failed: " + e.what());
            return true;
        }
    }
    if (!op.admitted())
        return false;
    try
    {
        if (op.remove(listed.key, *etag, Retry::standard()) == Removal::Removed)
            ++out.deleted;
    }
    catch (const std::exception & e)
    {
        ++out.leaked;
        out.anomalies.push_back("leaked dead-life object '" + listed.key + "': exact delete failed: " + e.what());
    }
    return true;
}

}

void NamespaceJanitor::run(bool suppress_deletes, const JanitorRunContext & context, NamespaceJanitorResult & out)
{
    const auto now = [&] { return context.now_ms ? context.now_ms() : uint64_t{0}; };
    const uint64_t phase_start = now();
    /// Saturates: a clock read below the start is no time spent, never a wrapped huge one.
    const auto elapsed = [&]
    {
        const uint64_t current = now();
        return current > phase_start ? current - phase_start : uint64_t{0};
    };
    const size_t batch_keys = std::clamp<size_t>(context.batch_keys, 1, kBulkDeleteMaxKeys);
    out.batch_keys = batch_keys;

    if (context.refresh_authority)
        context.refresh_authority();
    CasOperation op = requests.admit(context.liveness);
    const GcMaintenanceReadResult progress = readGcMaintenanceState(op, layout);
    if (progress.status == GcMaintenanceReadStatus::Corrupt)
    {
        out.anomalies.push_back(progress.diagnostic);
        throwOnRefusedOrGaveUp(
            casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::standard()),
            "CAS namespace janitor: corrupt maintenance-state reset");
        return;
    }
    const String start_cursor = progress.state ? progress.state->janitor_cursor : String{};

    /// The jobs of the page in progress. A deque, because running jobs hold pointers to their slots.
    std::deque<JobSlot> slots;
    std::atomic<bool> stop{false};
    const uint64_t generation = op.generation();

    std::optional<ThreadPoolCallbackRunnerLocal<void>> runner;
    if (context.io_pool)
        runner.emplace(*context.io_pool, ThreadName::CAS_GC_JANITOR);
    /// Handle `i` belongs to `slots[i]`: a refused enqueue is the last slot of its page and has no handle.
    std::vector<std::shared_ptr<ThreadPoolCallbackRunnerLocal<void>::Task>> handles;
    /// Every exit waits for every job: jobs reference `slots` and `stop`.
    SCOPE_EXIT_SAFE({ ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles); });
    size_t enqueued = 0;

    /// An incomplete page (undecided, or with a held or skipped job) always ends the loop, so the complete
    /// pages are a prefix and the last complete page holds the cursor to publish.
    std::optional<PendingPage> pending;
    std::optional<String> publish_cursor;
    uint64_t publish_leaked = 0;

    /// Waits for the pending page's jobs and folds them into `out`.
    const auto settlePage = [&]
    {
        ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles);
        for (size_t i = 0; i < handles.size(); ++i)
        {
            try
            {
                handles[i]->future.get();
            }
            catch (...)
            {
                slots[i].outcome = JobOutcome::Held;
                slots[i].error = std::current_exception();
                stop.store(true);
            }
        }
        handles.clear();

        bool page_held = pending && !pending->decided;
        uint64_t page_leaked = 0;
        for (const JobSlot & slot : slots)
        {
            if (slot.outcome != JobOutcome::Skipped && slot.outcome != JobOutcome::EnqueueRefused)
                ++out.batches;
            switch (slot.outcome)
            {
                case JobOutcome::Deleted:
                    out.deleted += slot.keys.size();
                    break;
                case JobOutcome::Leaked:
                    page_leaked += slot.keys.size();
                    ++out.batches_leaked;
                    out.anomalies.push_back("leaked dead-life objects " + describeKeys(slot.keys) + ": " + getExceptionMessage(slot.error, false));
                    break;
                case JobOutcome::Held:
                case JobOutcome::EnqueueRefused:
                    ++out.batches_held;
                    page_held = true;
                    out.anomalies.push_back("held dead-life objects " + describeKeys(slot.keys) + ": " + getExceptionMessage(slot.error, false));
                    break;
                case JobOutcome::Skipped:
                    ++out.delete_jobs_skipped;
                    page_held = true;
                    break;
            }
        }
        slots.clear();
        if (pending && !page_held)
        {
            publish_cursor = std::move(pending->next_cursor);
            publish_leaked += page_leaked;
        }
        pending.reset();
    };

    const auto submit = [&](size_t page_index, size_t job_index, std::vector<WriteOnceKey> keys)
    {
        if (stop.load())
            return false;
        JobSlot & slot = slots.emplace_back();
        slot.keys = std::move(keys);
        const auto job = [&context, &job_requests = requests, &stop, generation, slot_ptr = &slot, page_index, job_index]
        {
            if (context.on_job_start_for_test)
                context.on_job_start_for_test(page_index, job_index);
            runJob(*slot_ptr, job_requests, generation, context.liveness, stop);
        };
        if (!runner)
        {
            ++out.delete_jobs;
            job();
            return true;
        }
        try
        {
            if (context.schedule_refuse_at_for_test && *context.schedule_refuse_at_for_test == enqueued)
            {
                context.schedule_refuse_at_for_test->reset();
                throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "Injected CAS namespace janitor enqueue refusal at job {}", enqueued);
            }
            handles.emplace_back(runner->enqueueAndGiveOwnership(job));
        }
        catch (...)
        {
            slot.outcome = JobOutcome::EnqueueRefused;
            slot.error = std::current_exception();
            stop.store(true);
            return false;
        }
        ++enqueued;
        ++out.delete_jobs;
        return true;
    };

    String cursor = start_cursor;
    bool wrapped = false;
    for (size_t page_index = 0;; ++page_index)
    {
        if (page_index > 0)
        {
            /// Authority is monotone within the phase: the loop breaks on the first loss a refresh reports and never
            /// refreshes after it, so a request that saw `false` is never contradicted by a later `true`.
            /// A gate-refused exact `HEAD` also sees a loss, but it records a leak and the page continues.
            if (context.refresh_authority)
                context.refresh_authority();
            if (!op.admitted())
                break;
        }

        ListPage page;
        try
        {
            page = op.list(layout.namespaceRootPrefix(), cursor, page_keys, Retry::standard());
        }
        catch (...)
        {
            if (page_index > 0)
            {
                out.anomalies.push_back("namespace page LIST failed: " + getCurrentExceptionMessage(false));
                break;
            }
            /// Only the persisted cursor is reset: a cursor the store rejects would fail every round.
            (void)casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::once());
            throw;
        }
        /// The previous page's jobs ran during the LIST; a hold among them ends the phase before this page is used.
        settlePage();
        if (stop.load())
            break;
        ++out.pages;
        /// The pass already handled everything past the start cursor; do not classify or delete it twice.
        if (wrapped)
            std::erase_if(page.keys, [&](const ListedKey & listed) { return listed.key > start_cursor; });
        out.keys += page.keys.size();

        std::optional<CasRefCatalog::Snapshot> catalog_cut;
        try
        {
            catalog_cut.emplace(CasRefCatalog::read(op, layout));
        }
        catch (...)
        {
            if (page_index == 0)
                throw;
            out.anomalies.push_back("namespace page catalog read failed: " + getCurrentExceptionMessage(false));
            break;
        }

        bool ambiguous = false;
        try
        {
            catalog_cut->life_index.throwIfAmbiguous("CAS namespace janitor");
        }
        catch (const DB::Exception & e)
        {
            out.anomalies.push_back(e.message());
            ambiguous = true;
        }
        /// A page is decided only when the round had deletion authority for every dead-life candidate on it.
        /// Advancing while the global gate is closed could phase-lock a dead page onto every suppressed round.
        /// Malformed keys, absent objects and token mismatches are final per-key outcomes.
        bool decided = !ambiguous && !suppress_deletes;
        bool dead_candidate = false;
        std::vector<const ListedKey *> exact_keys;
        std::vector<WriteOnceKey> stream_keys;
        for (const ListedKey & listed : page.keys)
        {
            std::optional<NamespaceLifePhysicalId> life_id;
            std::optional<ParsedRefObjectKey> stream_key;
            try
            {
                if (listed.key.starts_with(layout.namespaceStreamRootPrefix()))
                {
                    stream_key = layout.parseRefObjectKey(listed.key);
                    if (stream_key)
                        life_id = stream_key->life_id;
                }
                else if (listed.key.starts_with(layout.namespaceStateRootPrefix()))
                {
                    if (const auto parsed = layout.parseRefCkptKey(listed.key))
                        life_id = *parsed;
                    else if (const auto file_parsed = layout.parseNamespaceFileKey(listed.key))
                        life_id = file_parsed->life_id;
                }
            }
            catch (const DB::Exception & e)
            {
                out.anomalies.push_back(listed.key + ": " + e.message());
                continue;
            }
            if (!life_id)
            {
                out.anomalies.push_back(listed.key + ": unrecognized namespace object key");
                continue;
            }
            if (ambiguous || suppress_deletes || catalog_cut->life_index.resolve(*life_id))
                continue;
            dead_candidate = true;
            /// A dead life's `_log`/`_snap` are write-once and never reborn under its prefix, so they need no token.
            if (stream_key)
            {
                if (std::optional<WriteOnceKey> write_once = layout.writeOnceStreamKey(*stream_key, listed.key))
                {
                    stream_keys.push_back(std::move(*write_once));
                    continue;
                }
            }
            exact_keys.push_back(&listed);
        }

        for (const ListedKey * listed : exact_keys)
        {
            if (!removeExactly(op, *listed, out))
            {
                decided = false;
                break;
            }
        }
        size_t job_index = 0;
        /// Reserved up front so no push after an enqueue can throw and orphan the scheduled job.
        if (runner && decided)
            handles.reserve((stream_keys.size() + batch_keys - 1) / batch_keys);
        for (size_t begin = 0; decided && begin < stream_keys.size(); begin += batch_keys)
        {
            if (!op.admitted())
            {
                decided = false;
                break;
            }
            const size_t end = std::min(stream_keys.size(), begin + batch_keys);
            if (!submit(page_index, job_index++, std::vector<WriteOnceKey>(stream_keys.begin() + begin, stream_keys.begin() + end)))
            {
                break;
            }
        }
        /// A wrapped page publishes an empty cursor: the pass has covered the whole stream.
        pending = PendingPage{.next_cursor = wrapped ? String{} : page.next_cursor, .decided = decided};

        if (!decided || !dead_candidate || stop.load())
            break;
        const bool wrap_here = page.next_cursor.empty() && !wrapped && !start_cursor.empty();
        /// A wrapped pass ends at the first page that reaches the start cursor.
        if ((page.next_cursor.empty() && !wrap_here) || (wrapped && page.next_cursor >= start_cursor))
            break;
        if (elapsed() >= context.budget_ms)
        {
            out.budget_exhausted = true;
            break;
        }
        /// Wrap once: without it a dropped table costs an extra round, because the fold round's pass
        /// advances one live page past the debris and the keys before that page wait for the next round.
        wrapped = wrapped || wrap_here;
        cursor = wrap_here ? String{} : page.next_cursor;
    }

    settlePage();
    /// Re-checked even when no page had a dead candidate: a tenure that lost its fence must not publish.
    if (!publish_cursor || !op.admitted())
        return;
    try
    {
        const WriteResult published = casGcMaintenanceState(
            op, layout, progress.etag, GcMaintenanceState{.janitor_cursor = *publish_cursor}, Retry::standard());
        if (std::holds_alternative<Refused>(published) || std::holds_alternative<GaveUp>(published))
            out.anomalies.push_back("cursor publication did not commit");
        else if (std::holds_alternative<Committed>(published))
        {
            out.cursor_advanced = true;
            /// Only a published page leaves its failed keys behind; an unpublished one is listed again next round.
            out.leaked += publish_leaked;
        }
    }
    catch (const std::exception & e)
    {
        out.anomalies.push_back("cursor publication failed: " + String(e.what()));
    }
}

}
