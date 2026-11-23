#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <Common/ThreadPool.h>
#include <base/types.h>

#include <array>
#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB::Cas
{

/// The source a tagged read belongs to, for the fold's per-family intake accounting. `Other` is never counted.
enum class ReadFamily : uint8_t
{
    RefCheckpoint,
    RefLog,
    Manifest,
    Other,
};

/// Owner-thread counts and elapsed time of one family's reads; see `GcReadAhead::enableFamilyAccounting`.
struct ReadFamilyStats
{
    UInt64 issued = 0;
    UInt64 takes = 0;
    UInt64 awaited = 0;
    UInt64 wait_ns = 0;
    UInt64 apply_ns = 0;

    bool operator==(const ReadFamilyStats &) const = default;
};

/// `std::chrono::steady_clock` in nanoseconds; noexcept, unlike `clock_gettime_ns`.
UInt64 steadyNowNs() noexcept;

/// Read-ahead in front of ONE admitted operation. A caller HINTS keys the sequential code will read
/// next; workers fetch them on `pool`, each through an operation resumed under the SAME admitted
/// generation as the caller's (no liveness -- exactly the fold's own admission); a TAKE returns the
/// fetched result, rethrows the worker's exception, or -- for a key nobody hinted -- performs the
/// request inline. This is a cache of RESULTS, never of decisions: every decode, counter and event
/// stays at the take site, so a round at concurrency 1 (nothing is ever hinted) and a round at 16
/// read the same keys in the same order and decide the same way; only WHEN the bytes were fetched
/// moves.
///
/// Why a result may be fetched early: the objects the fold reads are write-once (a present body is
/// the same body later), a ref log read absent early is still a key read absent during this round,
/// which is all the fold's cut asks of it, and a manifest body still being uploaded when the early
/// read lands yields the same hold a slightly earlier sequential read yields.
///
/// Only the result TAKEN, in the walk's order, decides anything; a hint never does. So a hint can cost
/// a wasted read, never change a verdict. The walk's ref-log hints are ids the round's listing names,
/// inside the epoch of the key the walk reads next, at or below the greatest key planning read present
/// there, and, while the walk has a read budget, within it. `walkRefChain`, through `ReadAheadKeyReader`, hints inside the
/// current epoch, up to a generation 1 or 2 `committed_through` when it has one. A hinted key the walk
/// never takes -- past a tombstone, past the chain's end, or after a hold -- is discarded and counted
/// wasted.
///
/// Memory is the CALLER's to bound: `pending` counts hinted-but-untaken slots and `window` is how
/// many a hinting site keeps in flight. A key hinted twice is one request. Results never taken are
/// awaited by the destructor and counted as wasted, or discarded explicitly by `discardRead`/
/// `discardHead`, which counts them at once. Only the owning thread touches the maps; a worker
/// touches only its own slot.
class GcReadAhead
{
public:
    GcReadAhead(CasOperation & op_, CasRequests & requests_, ThreadPool & pool_, size_t concurrency_);
    ~GcReadAhead();

    GcReadAhead(const GcReadAhead &) = delete;
    GcReadAhead & operator=(const GcReadAhead &) = delete;

    void hintRead(const String & key, ReadFamily family = ReadFamily::Other);
    void hintHead(const String & key);

    std::optional<Object> takeRead(const String & key, ReadFamily family = ReadFamily::Other);
    std::optional<Meta> takeHead(const String & key);

    /// Starts counting tagged reads on this object; off until called, and then an untagged read is still
    /// not counted, polls nothing and reads no clock. Per family: `issued` counts accepted hints and inline
    /// takes; `takes` every take, absent or throwing included; `awaited` inline takes and hinted takes
    /// whose result was not ready at one poll before `get`; `wait_ns` the time from the take's entry
    /// to its result (the whole read when inline); `apply_ns` the time from a take's return to the next tagged take
    /// or `closeApply`. Owner thread only; `clock` is the seam for a fake clock.
    void enableFamilyAccounting(UInt64 (*clock)() noexcept = &steadyNowNs);
    /// Ends the open apply interval, so time after this point is charged to no family.
    void closeApply();
    const ReadFamilyStats & familyStats(ReadFamily family) const { return stats[static_cast<size_t>(family)]; }

    /// Drops a hinted key the caller will never take. The request is already in flight or done; the
    /// wait is bounded by that one request, its result and its exception are dropped, and the slot is
    /// counted as wasted now rather than in the destructor. The exception is dropped because no
    /// sequential walk would have issued this request, so none could have failed on it; the fence and
    /// the budget are re-observed by the very next request anyway. A key nobody hinted is a no-op.
    void discardRead(const String & key);
    void discardHead(const String & key);

    /// Runs one intent create on the pool, under the same admitted generation, and returns the function that
    /// waits for its answer or rethrows what it threw; call it once, on the owning thread, while this object lives.
    /// Unlike a hint it always runs: at concurrency 1 it runs here, before this returns. Once a create on the pool
    /// throws, a create that starts later sends nothing and rethrows that first exception, so the caller sees the
    /// store's error whichever answer it takes first. The destructor waits for every create whose answer was not taken.
    std::function<IntentCreateResult()> startCreate(std::function<IntentCreateResult(CasOperation &)> request);

    /// Creates started on the pool whose answer is not taken yet; for tests.
    size_t untakenCreates() const { return creates.size(); }

    /// Hinted but not yet taken, both verbs together: what a hinting site throttles itself against.
    size_t pending() const { return reads.size() + heads.size(); }
    /// Hinted HEADs not yet taken: the head hinting site throttles on its own count.
    size_t pendingHeads() const { return heads.size(); }

    /// How many requests a hinting site should keep in flight. Zero at concurrency 1, which is what
    /// makes every `while (pending() < window())` loop hint nothing at all on the sequential setting.
    size_t window() const { return concurrency <= 1 ? 0 : 4 * concurrency; }

private:
    template <typename T>
    struct Slot
    {
        std::promise<std::optional<T>> promise;
        std::future<std::optional<T>> future;
        Slot() : future(promise.get_future()) {}
    };

    template <typename T>
    using Slots = std::unordered_map<String, std::shared_ptr<Slot<T>>>;

    template <typename T, typename Request>
    void hint(Slots<T> & slots, const String & key, Request request, ReadFamily family);

    template <typename T, typename Inline>
    std::optional<T> take(Slots<T> & slots, const String & key, Inline inline_request, ReadFamily family);

    bool accounts(ReadFamily family) const { return clock_ns != nullptr && family != ReadFamily::Other; }
    void closeApplyAt(UInt64 now_ns);

    template <typename T>
    void discard(Slots<T> & slots, const String & key);

    CasOperation & op;
    CasRequests & requests;
    ThreadPool & pool;
    const size_t concurrency;
    /// The generation the caller's operation was admitted under. A worker resumes under exactly this
    /// one, so a fence that moves under the round fails a worker's request the way it fails the
    /// round's own -- never with a fresher admission the round itself would not have had.
    const uint64_t generation;

    /// The first exception of a create on the pool. Shared with the jobs, which read it after the answers are taken.
    struct CreateFailure
    {
        std::mutex mutex;
        std::exception_ptr first;
        std::atomic<bool> failed{false};
    };

    Slots<Object> reads;
    Slots<Meta> heads;
    std::unordered_map<uint64_t, std::future<IntentCreateResult>> creates;
    uint64_t next_create = 0;
    const std::shared_ptr<CreateFailure> create_failure = std::make_shared<CreateFailure>();

    /// Null while accounting is off.
    UInt64 (*clock_ns)() noexcept = nullptr;
    std::array<ReadFamilyStats, 4> stats{};
    std::optional<ReadFamily> apply_family;
    UInt64 apply_since_ns = 0;
};

}
