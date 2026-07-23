#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Common/ThreadPool.h>
#include <base/types.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

namespace DB::Cas
{

/// The decoupled, pure-data record the Disks-layer GC scheduler emits per round. It carries NO
/// Interpreters dependency: the metadata storage converts it into a
/// ContentAddressedGarbageCollectionLogElement and forwards it to the SystemLog. Keeping it a plain
/// POD lets the scheduler (and its unit tests) stay free of the system-log machinery.
struct GcRoundLogRecord
{
    enum class EventType { Start, Finish };
    enum class Outcome { Unknown, Success, NotALeader, Failed };
    enum class Trigger { Scheduled, Manual };

    EventType event_type = EventType::Start;
    Outcome outcome = Outcome::Unknown;   /// Unknown until a round finishes
    Trigger trigger = Trigger::Scheduled;
    String disk_name;
    String srid;          /// server_root_id of the mount that ran this round (this pool's poolConfig())
    String gc_id;        /// hex of the scheduler's gc_id
    UInt64 round = 0;
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 manifests_deleted = 0;   /// owner-removed manifest bodies deleted, distinct from blob deletes
    /// Counts for the three-stage deletion pipeline reported by `Cas::RoundReport`.
    UInt64 entries_condemned = 0;   /// entries newly condemned into the retired list this round
    UInt64 entries_graduated = 0;   /// entries newly floor-passed (published delete_pending) this round
    UInt64 entries_redeleted = 0;   /// pending exact-token blob deletes executed this round
    UInt64 fence_outs = 0;          /// expired mounts fenced-out by the round's heartbeat floor
    UInt64 anomalies = 0;           /// fold clamps surfaced (never wedging) this round
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;
};

using GcRoundLogger = std::function<void(const GcRoundLogRecord &)>;

/// Paces regular content-addressed garbage-collection rounds for one pool. The scheduler does not
/// implement the GC protocol: `Cas::Gc` owns lease acquisition, work deduplication, and the
/// split-brain-safe round operations, so schedulers on different mounters may run independently.
/// This class waits for a tick, runs one round, records its result, and retries after exceptions;
/// round operations are idempotent, while `LOGICAL_ERROR` exceptions remain visible in the log.
///
/// The background and heartbeat workers are `ThreadFromGlobalPool` instances. The background worker
/// therefore has the attached `ThreadStatus` required by `ProfileEventsScope` for per-round
/// `ProfileEvents` deltas. A manual round runs on the caller's thread; if that thread has no
/// `ThreadStatus`, the per-round delta is omitted rather than making the GC round fail.
class CasGcScheduler
{
public:
    CasGcScheduler(
        Cas::PoolPtr store_,
        std::chrono::seconds interval_,
        const String & log_name,
        String disk_name_,
        GcRoundLogger logger_ = {});
    ~CasGcScheduler();

    /// Starts the periodic round and heartbeat workers. Calling `start` more than once while the
    /// scheduler is running is a no-op; after `stop`, it may be started again.
    void start();

    /// Stops both workers, wakes them if they are waiting, and joins them before returning. It is
    /// safe to call `stop` when the scheduler is not running and from the destructor.
    void stop();

    /// Test/diagnostics hook: run ONE round synchronously on the caller's thread. Returns the round
    /// report so the SYSTEM command / tests can inspect it. Emits a Start + Finish record.
    Cas::RoundReport runOneRoundNow(GcRoundLogRecord::Trigger trigger = GcRoundLogRecord::Trigger::Manual);

    /// Returns per-disk GC health for `system.content_addressed_mounts`. The fields describing
    /// rounds snapshot this scheduler's state, while `wedged_namespace_count` is read live from the
    /// store's ref lanes; keeping the state here avoids process-global gauges colliding across disks.
    struct GcHealth
    {
        bool is_leader = false;
        bool ever_succeeded = false;
        Int64 pending_reclaim = 0;             /// cumulative condemned - executed deletes (this process)
        UInt64 last_success_age_seconds = 0;   /// seconds since the last led round (0 if never)
        UInt64 wedged_namespace_count = 0;
    };
    /// Takes a consistent-enough atomic snapshot for diagnostics. The returned counters are local
    /// health indicators rather than durable GC state, and the wedged-namespace count is queried
    /// directly from the store.
    GcHealth gcHealth() const;

    /// Whether GC is quiescent for the `Vanished(erased)` erasure proof ([D1], spec §2): TRUE iff NO
    /// round is currently in flight on this scheduler. This is the per-sample "no GC round can land a
    /// durable `gc/state`/heartbeat write between this LIST sample and the next" signal the metadata
    /// storage feeds into `PoolConfig::gc_quiescent_fn`.
    ///
    /// It deliberately does NOT also require the scheduler THREAD to have exited: the erasure proof is
    /// reached from `IdentityLost`, where the pool's own writers are stopped but the GC scheduler is STILL
    /// ALIVE (it exits only once the pool is fully `Vanished` — the proof's OUTPUT, not a precondition), so
    /// requiring "thread exited" here would make the natural proof unreachable. GC's `gc/state`/heartbeat
    /// writes are NOT fence-generation / `mayMutate`-checked (they bypass `CasPlainObjects`; Task 4
    /// excluded them), so a live scheduler is safe against the proof for three CONCRETE reasons — not a
    /// write fence:
    ///   (a) `gcStateKey`/`gcHbKey` live under `poolPrefix`, which the proof's per-tick full-prefix
    ///       emptiness LIST covers — any GC write that lands is seen and RESETS the empty-sample streak;
    ///   (b) `round_in_flight` is held for the WHOLE round body (`SCOPE_EXIT` in `runRoundLogged`), so a
    ///       sample taken while a round could still write reads not-quiescent — closing the
    ///       landed-just-after-LIST window;
    ///   (c) a round in the proof window has no valid `gc/state` to advance and its `has_observation` guard
    ///       throws `CORRUPTED_DATA` rather than recreate an absent `gc/state`, so it lands nothing.
    /// The scheduler-exit half of [D1] is thus covered by (a)+(b)+(c) plus the proof's minimum-grace gate
    /// (which by spec §3 exceeds GC's bounded round + next-tick exit latency); this predicate surfaces the
    /// sharp per-sample "no round mid-flight" half — (b) — that a pure timing argument cannot itself
    /// guarantee. An in-flight round resets the empty-sample streak, so the proof concludes only across
    /// samples with no round in flight.
    bool isQuiescent() const { return !round_in_flight.load(std::memory_order_acquire); }

    /// Test seam: force the in-flight-round flag `isQuiescent` reads, so a test can drive
    /// "running round => not quiescent" without spinning up a real round against a live backend.
    void setRoundInFlightForTest(bool v) { round_in_flight.store(v, std::memory_order_release); }

private:
    /// Waits for the configured interval, runs scheduled rounds while the scheduler is active, and
    /// logs exceptions before continuing with the next tick. The round lock serializes this worker
    /// with `runOneRoundNow` because the persistent `gc` object is not thread-safe.
    void loop();

    /// While this scheduler believes it owns the lease, periodically advances the advisory
    /// heartbeat independently of round progress. The cadence is shorter than the lease
    /// observation window, so a long round does not look like a dead leader to another scheduler.
    /// Heartbeat failures are advisory and are retried on the next cadence.
    void heartbeatLoop();

    /// Runs synchronously when `Cas::Gc` acquires or renews the lease, before the round's potentially
    /// long fold begins. It marks this scheduler as the heartbeat owner and sends the first pulse
    /// immediately; otherwise a new leader's first round could appear inactive until it returned.
    /// The same hook is used by scheduled and manual rounds so both acquisition paths are protected.
    void onLeaseAcquired();

    /// Run one round through the full logging path (Start record, ProfileEventsScope, Finish
    /// record). Used by BOTH loop() and runOneRoundNow. Logging is best-effort - the logger sink
    /// never throws into the round. Rethrows a round exception (after emitting an Aborted Finish).
    /// `allow_steal` is forwarded to `Cas::Gc::runRegularRound` verbatim (see its doc comment).
    Cas::RoundReport runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                     std::function<void()> on_lease_acquired = {}, bool allow_steal = true);

    const Cas::PoolPtr store;
    const std::chrono::seconds interval;
    const std::chrono::milliseconds hb_interval;   /// advisory heartbeat cadence, interval / 4 with a 50 ms minimum
    const LoggerPtr log;
    const UInt128 gc_id;
    const String disk_name;
    const GcRoundLogger logger;

    /// One persistent Gc for BOTH loop() and runOneRoundNow: the lease's observation-window steal
    /// protocol REQUIRES a stable observer (it compares the lease across consecutive runRegularRound
    /// calls of the same instance). A throwaway per call could never recover a dead-incumbent lease.
    Cas::Gc gc;
    /// Serializes the manual round against the background round so the two never touch the single
    /// (not-thread-safe) `gc` concurrently. Distinct from `mutex`: the loop releases `mutex` before
    /// the round so stop()/heartbeatLoop are not blocked, so the round cannot hold `mutex`.
    std::mutex gc_round_mutex;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    ThreadFromGlobalPool thread;
    /// Set by the round worker and read by the heartbeat worker. It is only an in-process hint: the
    /// durable lease remains the authority, and a failed round clears the hint before retrying.
    std::atomic<bool> i_am_leader{false};
    ThreadFromGlobalPool hb_thread;

    /// Set true for the whole body of one round (`runRoundLogged`, held across the `gc_round_mutex`
    /// critical section a scheduled or manual round runs under) and cleared when it returns, on the
    /// success AND exception paths. Read by `isQuiescent` for the `Vanished(erased)` erasure proof.
    std::atomic<bool> round_in_flight{false};

    /// Cumulative condemned entries minus exact-token deletes completed by this scheduler while it
    /// led. It is an approximate health gauge, not durable GC state.
    std::atomic<Int64> pending_reclaim{0};
    /// Steady-clock timestamp of the last round that acquired the lease; zero means never.
    std::atomic<UInt64> last_success_ms{0};
};

}
