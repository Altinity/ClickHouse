#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <functional>
#include <vector>

namespace DB::Cas
{

struct NamespaceJanitorResult
{
    uint64_t pages = 0;
    uint64_t keys = 0;
    uint64_t deleted = 0;
    uint64_t leaked = 0;
    /// Delete calls the page made: one per exact-token delete and per batch, including a batch the storage
    /// refused before its keys went one by one. Reissues of a call are not counted.
    uint64_t delete_requests = 0;
    /// The page deleted something, published its cursor and did not cover the whole stream by itself, so
    /// the next page is worth taking now. After the last page of the stream the next one starts from its
    /// beginning.
    bool more = false;
    std::vector<String> anomalies;
};

/// Deletes one page's dead `_log`/`_snap` keys under the page's operation and adds to `out` the keys it
/// deleted, the keys it could not confirm deleted with an anomaly for each group of them, and its delete
/// calls. An exception leaves the page unpublished.
using RemoveWriteOnce
    = std::function<void(CasOperation & op, const std::vector<WriteOnceKey> & keys, NamespaceJanitorResult & out)>;

/// Runs one bounded, leak-only page over the physical namespace ownership tree; `Gc` repeats it under a time budget.
class NamespaceJanitor
{
public:
    /// An empty `remove_write_once_` deletes on the caller's thread, one request per `kBulkDeleteMaxKeys` keys.
    NamespaceJanitor(CasRequests & requests_, const Layout & layout_, size_t page_budget_, RemoveWriteOnce remove_write_once_ = {});

    /// `liveness` is admitted once for the whole page (one `CasOperation` covers the read, the list,
    /// every delete and the cursor publication): a fact the fence cannot see, such as "this tenure
    /// still holds the GC round's own lease" -- see `CasRequests::admit`. It is SAMPLED BEFORE EVERY
    /// REQUEST the page makes (and before every reissue of one), not just at the two points this
    /// function itself checks `op.admitted()` -- so it must be cheap and must never throw. A sample
    /// that returns false ends whichever request was about to be sent: a read verb (the maintenance
    /// read, the list, a HEAD) throws out of this call, and a write verb (a delete, the cursor
    /// publication) reports it as `GaveUp` rather than sending anything.
    ///
    /// A dead life's canonical `_log`/`_snap` keys are write-once, so the page deletes them together through
    /// `remove_write_once` with no per-key `HEAD` or token; `_ckpt` and `_files` keep the exact-token delete.
    ///
    /// A failed `LIST` resets the persisted cursor only when `first_page_of_pass`: that cursor comes from an
    /// earlier round and the store may reject it forever, while the one a page of this pass just published
    /// is fresh, so the failure is transient and the next round resumes from it.
    ///
    /// `leaked` is zero for a page that did not publish its cursor: the page is listed again.
    NamespaceJanitorResult runOnePage(bool suppress_deletes, Liveness liveness, bool first_page_of_pass = true);

private:
    CasRequests & requests;
    const Layout & layout;
    size_t page_budget;
    RemoveWriteOnce remove_write_once;
};

}
