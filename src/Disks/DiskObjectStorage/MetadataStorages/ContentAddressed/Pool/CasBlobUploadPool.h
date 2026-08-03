#pragma once

#include <Common/ThreadPool_fwd.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace DB::Cas
{

/// Server-wide pool for the parallel intra-part blob upload fan-out (stage-1 design §1,
/// "Parallel blob upload within a part"). Deliberately disjoint from
/// `IObjectStorage::getThreadPoolWriter`: an upload task may itself submit to the writer pool (S3
/// multipart), so nesting the fan-out on that same pool would risk the classic same-pool
/// wait-on-self deadlock. The calling thread only submits tasks and joins them -- it never
/// occupies a pool slot itself -- so pool size 1 is a valid (fully serial) configuration, never a
/// deadlock risk.
///
/// Fail-loud lifecycle: the server (or a test) must call `initializeBlobUploadPool` before any
/// `blobUploadPool` use. There is no lazy self-initialization on the production path.

/// Throws `BAD_ARGUMENTS` if `size == 0`. Throws `LOGICAL_ERROR` if already initialized.
void initializeBlobUploadPool(size_t size);

/// Throws `LOGICAL_ERROR` if the pool has not been initialized. The returned reference is only
/// valid while the pool stays initialized: callers must not race this against
/// `shutdownBlobUploadPool` (in the server, shutdown runs after query drain; tests own the order).
ThreadPool & blobUploadPool();

/// Idempotent: safe to call multiple times, and safe to call even if never initialized. Joins all
/// outstanding tasks before returning.
void shutdownBlobUploadPool() noexcept;

/// For tests only: true once `initializeBlobUploadPool` has run, false before that call and after
/// `shutdownBlobUploadPool`.
bool blobUploadPoolInitializedForTest();

/// Byte-weighted admission for the condemned-LOCAL resurrection branch (stage-1 design §1,
/// "Condemned-local resurrection memory cap"). That branch is the one blob-upload path that
/// materializes a COMPLETE header+payload body in memory for `putOverwrite` (`putOverwrite` has no
/// streaming variant), so under Task 5's fan-out N concurrent condemned-local displacements would hold
/// N full bodies where the old serial path held one. This semaphore caps the AGGREGATE materialized
/// bytes; a plain thread-count limit would not (bodies differ in size). The permit's weight is the
/// checked `header + payload` size, known before materialization.
///
/// A single blob heavier than the whole capacity cannot ever satisfy `in_flight + weight <= capacity`,
/// so it would wait forever under a naive counting rule. Instead such an OVERWEIGHT blob acquires
/// EXCLUSIVE access: it waits only for the semaphore to fully drain (a reachable condition, since new
/// normal admissions are held off while it waits), then runs alone.
///
/// Fairness rule: overweight (exclusive) acquirers take PRIORITY over normal acquirers. While any
/// overweight acquirer is queued or running, normal acquires block. This is what guarantees the
/// overweight drains (a steady stream of normal admissions can never starve it into an infinite wait --
/// the spec's only hard requirement). The converse -- a normal acquirer delayed behind queued overweight
/// blobs -- is bounded by the number of queued overweight blobs times one materialization each, and
/// overweight blobs are pathologically rare (a single blob larger than the ENTIRE admission capacity),
/// so normal traffic is not starved in practice.
class ByteWeightedSemaphore
{
public:
    /// `capacity_bytes` is the aggregate materialized-byte budget for NORMAL admissions. Must be > 0
    /// (the derived default is `pool_size * 64 MiB`, always positive); a 0 capacity is rejected by
    /// `initializeCondemnedUploadAdmission`, so it never reaches here on the production path.
    explicit ByteWeightedSemaphore(uint64_t capacity_bytes_);

    /// Blocks until `weight` bytes fit. `weight <= capacity`: waits for room AND for no overweight
    /// acquirer to be queued/running (fairness). `weight > capacity` (overweight): waits for full drain,
    /// then holds EXCLUSIVELY (all other acquires block until it releases). Never waits forever.
    void acquire(uint64_t weight);

    /// Releases a permit taken with the SAME `weight`. `noexcept`: called from a scope-exit path where a
    /// throw would terminate. Wakes any waiters.
    void release(uint64_t weight) noexcept;

    /// Immutable after construction, so no lock is needed.
    uint64_t capacity() const { return capacity_bytes; }

    /// Test observability (no production reader). `peak_in_flight` is the high-water mark of concurrently
    /// admitted NORMAL weight -- the conservative bound on peak materialized bytes, since a permit is held
    /// across the whole materialize+putOverwrite window. `peak_holders` is the high-water mark of
    /// concurrent permit holders (proves a cap test is not vacuously serial). `exclusive_grants` counts
    /// overweight (exclusive) admissions. `co_hold_violation` latches true if an exclusive holder was ever
    /// observed co-active with any other holder -- the semaphore guarantees this never happens, so a test
    /// asserts it stays false under real concurrent load.
    struct StatsForTest
    {
        uint64_t peak_in_flight = 0;
        uint32_t peak_holders = 0;
        uint32_t exclusive_grants = 0;
        bool co_hold_violation = false;
    };
    StatsForTest statsForTest() const;
    void resetStatsForTest();

    /// Fired AFTER a permit is granted and OUTSIDE the internal mutex, while the permit is held, with the
    /// granted `weight`. A test rendezvous here pins several permits co-resident so the peak is actually
    /// reached (and, for an overweight permit, holds the exclusive window open for a probe). Pre-traffic
    /// setter only (like the other CAS sink/hook setters). Default empty: zero production overhead.
    void setHeldHookForTest(std::function<void(uint64_t weight)> hook);

    /// Fired UNDER the internal mutex the first time an acquire cannot immediately admit and is about to
    /// block. Lets a test prove deterministically that a probe acquire was blocked (e.g. by an exclusive
    /// holder) instead of racing a timed negative check. The hook MUST NOT re-enter the semaphore.
    /// Pre-traffic setter only. Default empty.
    void setWaitHookForTest(std::function<void()> hook);

private:
    const uint64_t capacity_bytes;

    mutable std::mutex mutex;
    std::condition_variable cv;

    uint64_t in_flight = 0;          /// sum of admitted NORMAL weights (overweight bodies are not counted -- they run alone)
    size_t waiting_exclusive = 0;    /// overweight acquirers QUEUED (a running one is tracked by `exclusive_active`); drives the fairness gate
    bool exclusive_active = false;   /// an overweight acquirer is currently holding
    size_t active_holders = 0;       /// all permit holders (normal + the one exclusive)

    StatsForTest stats;
    std::function<void(uint64_t)> held_hook;
    std::function<void()> wait_hook;
};

/// Fail-loud lifecycle mirroring the blob upload pool above. `configured_capacity_bytes` is the raw
/// `cas_condemned_upload_memory_bytes` server setting; `0` means "derive from the pool
/// size": the derived capacity is `pool_size * 64 MiB`. 64 MiB is the CHOSEN default per-task budget,
/// NOT a cap on blob bodies -- blob bodies have no size cap; only `RefLog`/`RefSnapshot` objects are
/// capped at 64 MiB (`ref_removal_max_bytes`). Budgeting one per-task budget per pool slot neither
/// over-subscribes memory nor throttles a normal fan-out; a condemned body heavier than the whole
/// capacity is admitted exclusively (alone) rather than deadlocking. Throws `BAD_ARGUMENTS` if the
/// resolved capacity is 0; throws `LOGICAL_ERROR` if already initialized.
void initializeCondemnedUploadAdmission(uint64_t configured_capacity_bytes, size_t pool_size);

/// Throws `LOGICAL_ERROR` if not initialized.
ByteWeightedSemaphore & condemnedUploadAdmission();

/// Idempotent; safe even if never initialized.
void shutdownCondemnedUploadAdmission() noexcept;

/// For tests only: true once `initializeCondemnedUploadAdmission` has run.
bool condemnedUploadAdmissionInitializedForTest();

/// RAII: acquires `weight` on construction, releases it on destruction. Declare it BEFORE the body
/// buffer it protects, so on scope exit the buffer's destructor runs FIRST and the permit is released
/// only after the materialized bytes are freed (the exact spec ordering: release after `putOverwrite`
/// returns AND the body is destroyed, before event/meta work). Also releases on an exception thrown
/// while the body is materialized or `putOverwrite` runs.
class ByteWeightedSemaphoreLock
{
public:
    ByteWeightedSemaphoreLock(ByteWeightedSemaphore & sem_, uint64_t weight_)
        : sem(sem_), weight(weight_)
    {
        sem.acquire(weight);
    }

    ~ByteWeightedSemaphoreLock() { sem.release(weight); }

    ByteWeightedSemaphoreLock(const ByteWeightedSemaphoreLock &) = delete;
    ByteWeightedSemaphoreLock & operator=(const ByteWeightedSemaphoreLock &) = delete;

private:
    ByteWeightedSemaphore & sem;
    const uint64_t weight;
};

}
