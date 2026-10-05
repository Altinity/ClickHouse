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
    /// The page deleted something, published its cursor and did not cover the whole stream by itself, so
    /// the next page is worth taking now. After the last page of the stream the next one starts from its
    /// beginning.
    bool more = false;
    std::vector<String> anomalies;
};

/// Deletes one page's dead `_log`/`_snap` keys under the page's operation. Returns how many keys it could
/// not confirm deleted and adds one anomaly per failed request; throws only when nothing may be published.
using RemoveWriteOnce
    = std::function<uint64_t(CasOperation & op, const std::vector<WriteOnceKey> & keys, std::vector<String> & anomalies)>;

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
    NamespaceJanitorResult runOnePage(bool suppress_deletes, Liveness liveness);

private:
    CasRequests & requests;
    const Layout & layout;
    size_t page_budget;
    RemoveWriteOnce remove_write_once;
};

}
