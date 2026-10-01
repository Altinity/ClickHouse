#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Common/ThreadPool_fwd.h>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace DB::Cas
{

/// Counts of one janitor phase, summed over its pages.
struct NamespaceJanitorResult
{
    uint64_t pages = 0;
    uint64_t keys = 0;
    uint64_t deleted = 0;
    uint64_t leaked = 0;
    uint64_t batches = 0;
    uint64_t batches_leaked = 0;
    uint64_t batches_held = 0;
    uint64_t delete_jobs = 0;
    uint64_t delete_jobs_skipped = 0;
    uint64_t batch_keys = 0;
    bool budget_exhausted = false;
    bool cursor_advanced = false;
    std::vector<String> anomalies;
};

/// What one janitor phase runs with.
struct JanitorRunContext
{
    /// Sampled before every request; cheap and never throws.
    Liveness liveness;
    /// Re-reads authority before each page; `liveness` answers its verdict. Empty: no refresh.
    std::function<void()> refresh_authority;
    /// Keys per delete job, fixed for the phase; clamped to [1, `kBulkDeleteMaxKeys`].
    size_t batch_keys = kBulkDeleteMaxKeys;
    /// No page starts once this much time has passed since the phase began; the page in progress finishes.
    uint64_t budget_ms = std::numeric_limits<uint64_t>::max();
    /// Phase clock; empty reads as a clock that never moves.
    std::function<uint64_t()> now_ms;
    /// Delete jobs run here, so its thread count bounds them; null runs them inline on the caller's thread,
    /// in submit order, which keeps unit tests deterministic. A page's jobs are all enqueued before the next
    /// page is listed.
    ThreadPool * io_pool = nullptr;
    /// TEST SEAM: the enqueue at this index within the phase throws; reset once it fires.
    std::optional<size_t> * schedule_refuse_at_for_test = nullptr;
    /// TEST SEAM: runs in each job before its stop-flag check and its admission.
    std::function<void(size_t page, size_t job)> on_job_start_for_test;
};

/// Reclaims the objects of namespace lives absent from the catalog, page by page from the persisted cursor.
class NamespaceJanitor
{
public:
    NamespaceJanitor(CasRequests & requests_, const Layout & layout_, size_t page_keys_)
        : requests(requests_), layout(layout_), page_keys(page_keys_) {}

    /// One phase. Each page: refresh authority, LIST, one catalog cut after the LIST, exact-token deletes of
    /// dead `_ckpt`/`_files`, then jobs of `batch_keys` token-free deletes of dead canonical `_log`/`_snap`.
    /// Another page starts only within the budget, while no job has held, after a decided page that had a
    /// dead-life candidate and a next cursor. A pass that began mid-stream wraps once: after the last page it
    /// continues from the stream start, skips keys past the start cursor and ends at the page that reaches it;
    /// a wrapped page publishes an empty cursor. At the end one publication of the cursor after the longest
    /// prefix of complete pages. `suppress_deletes` lists one page and parses its keys without classifying them,
    /// deletes nothing and publishes no cursor, but still writes a cursor reset when the state is corrupt or the LIST fails.
    /// A page's jobs run while the next page is listed and are settled before that page is used.
    /// Jobs share a phase-wide stop flag: a job that holds sets it, a job that finds it set sends nothing, and the
    /// caller stops submitting. `liveness` must be monotone within the phase: once it returns `false` it stays false.
    /// Throws when the maintenance read, the first page's LIST (after resetting the cursor) or its catalog
    /// read fails; `out` keeps the counts gathered so far (`pages` and `keys` are added before a page's catalog read,
    /// so a page whose read throws is still counted) and no cursor is published.
    void run(bool suppress_deletes, const JanitorRunContext & context, NamespaceJanitorResult & out);

private:
    CasRequests & requests;
    const Layout & layout;
    size_t page_keys;
};

}
