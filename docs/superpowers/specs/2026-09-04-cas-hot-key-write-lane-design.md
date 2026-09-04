---
description: 'Design for a per-pool write lane above the request engine for hot compare-and-swap objects (the ref catalog first), phase A: one FIFO per key shared by the pool''s planes, one conditional write in flight per key, an LRU of last known objects so the next write needs no read, a decide''s verdict delivered only from a fresh read, and lost races between servers paced by a flat jitter. Combining, GCS spacing and the hold clamp are phase B, designed and deferred. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 29, 2026-09-04. Phase A. Brainstormed against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`, whose fix sketch
this document supersedes where they differ. Revisions 1 to 26 designed a larger change (combining
of queued mutations into one `PUT`, spacing to GCS's per-object rate, a clamp on reads inside a
hold, the GC erase inside the lane); after twenty-two review rounds its core was judged sound and
its periphery kept producing findings at a constant rate. This revision lands the part the
measurement needs and keeps the seams the rest will use. The deferred part, with the rules those
rounds established, is BACKLOG item `{#hot-key-lane-phase-b}`; its full text is revision 26,
commit `26bde9f9604`.

## The problem, as measured {#the-problem-as-measured}

Every `CREATE TABLE` and `DROP TABLE` on a content-addressed disk mutates one pool-wide object,
`cas/ref_catalog`, through a conditional write. Ten parallel stateless jobs produced this on the
CA-s3 lane:

| measurement | value |
|---|---|
| `DROP TABLE` (n = 1436) | p50 2.4 s, p90 11.9 s, max 34.7 s |
| `CREATE TABLE` | over the ten-minute `query_log` window: p50 184 ms, p90 391 ms; in the RCA's 80 s failing window (n = 109): p50 203 ms, p90 457 ms, max 10.4 s; one gave up after 78.9 s at the 90 s policy deadline |
| `PreconditionFailed` on `ref_catalog` in the failing window | 113 in 80 s, from 53 threads |
| the losing writer alone | 35 attempts, gaps growing to the 5 s cap |
| one 33.6 s `DROP`: the namespace-removal write | one conditional write that lost eight races in a row, 15.4 s |

Two facts from the code explain the shape:

1. `CasRefCatalog::casUpdateImpl` calls `CasOperation::readModifyWrite` under `Retry::standard()`.
   That verb always starts with a `GET`, and on a lost race sleeps
   `Retry::backoff(++state.reissues)`: full jitter over a window that doubles from 200 ms to the
   5 s cap, on a counter shared with transport faults. A writer that has lost several races is
   asleep for seconds while a fresh writer starts at zero, so the oldest loser is the least likely
   to win the next race. The RCA calls this spec-conformant and unfair by construction.
2. Every writer in one process races every other writer in the same process. Compare-and-swap is
   needed only against other servers; inside one server the races are pure waste, and each costs a
   `GET`, a refused `PUT`, a resolve `GET` and a sleep.

What the lane must deliver, in the suite's numbers: the catalog key sees about seven mutations per
second on average (2.4 `DROP`s and two catalog steps per `CREATE`), in bursts of up to sixteen
drop workers. One serialized writer on S3 lands about twelve mutations per second with a `GET`
before each `PUT` and about twenty without, so the queue behind a burst of sixteen drains in
under a second where today it takes twelve. That is the phase A target; the throughput that
combining adds is phase B's, gated on measuring the lane's queue after this lands.

## Goals and non-goals {#goals-and-non-goals}

Goals:

- Per pool and key written through the lane, at most one conditional write in flight, in arrival
  order, from any of the pool's planes.
- No two writers of one pool ever race each other on such a key: N concurrent mutations cost N
  conditional `PUT`s and at most one `GET`, never a 412 against each other, and a lone mutation
  costs one `PUT` and no `GET` when the pool wrote the object last.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one.
- The lane serves any hot compare-and-swap object, not the catalog only: its `decide` is the
  engine's own `DecideOnObject`, it returns the engine's own `WriteResult`, and a caller moves to
  it by changing one call. The catalog is the first caller.
- No change to the catalog's API, to the semantics of any conditional write or of any result or
  verdict a caller sees, to the 90 s windows, to what any read returns, or to a single line of GC.
  In the request engine, four changes: a pointer carried by `CasRequests` and exposed as
  `CasOperation::hotKeys()`; `friend class CasHotKeys` on `CasOperation`, for its private `gate`,
  `fits`, `reservedFor`, `observe` and `gaveUpAfterFailedObservation`; one field on `Conflict`,
  `any_ambiguous`, set where `writeLoop` returns it from the `WriteState` it already keeps and
  carried where `readModifyWriteOnPresence` rebuilds a bodyless `Conflict` under `single_attempt`
  (`CasRequests.cpp:1034`); and Half 2's `conflictBackoff` and `pauseForConflict`. Two internal changes in `CasRefCatalog.cpp`,
  named below. Every existing catalog test stays green unchanged; the ones that count requests
  count writes, and a lone call's counts are today's.
- Phase B adds combining, spacing and the clamp without changing `submit`'s signature, the
  `decide` type, the caller's loop, or the cache's rules ([Designed for phase B](#designed-for-phase-b)).

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- Combining queued mutations into one `PUT`. Phase B, designed; its gate is the measured queue
  wait after this lands.
- Pacing writes to a store's documented per-object rate. Phase B, for the `gcs` lane, on its own
  429 measurement. Without it the lane is already strictly better than today on GCS: one `PUT`
  per mutation instead of a race, and a 429 is a transport fault on the growing schedule.
- Bounding a holder's reads to the hold. A `decide` that reads does so under the policy its caller
  chose, as today; the two catalog paths are named in [Deadlines and fences](#deadlines-and-fences).
- Bringing the GC erase into the lane. It writes the same key today by racing the mount plane and
  keeps doing so; what it costs the lane is one stale cache entry per erase.
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). They share a
  shape; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

**Half 1, the lane.** A per-pool component above the request engine, `CasHotKeys`, reachable
from any of the pool's three `CasRequests` planes through the operation that admits a write. A
caller submits a mutation of one key: a `decide` over the key's bytes. The lane takes a FIFO ticket
for the key, and when the ticket is at the front the caller obtains a base (the cache's last known
object, else a `GET`), runs its `decide`, and lands the candidate in one conditional write through
the engine, on its own operation and thread. The engine's `WriteResult` is returned as it is,
nothing reclassified, so a caller handles it exactly as it handles today's, plus one rule,
resubmit on `Conflict`. A verdict a `decide` renders on a cached base, a refusal by exception or
"nothing to write", is never reported; the lane reads and decides again. `CasRefCatalog` writes
through the lane.

**Half 2, conflict pacing.** In `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`,
for the sites that keep using them, a clean refused precondition is repaid after
`Retry::conflictBackoff()`, a flat uniform over [0, 200] ms, and no longer advances the
transport-fault counter. The lane's callers pause by the same rule between a `Conflict` and their
next submission. Three sentences of the backend request contract change.

## Half 1: the lane {#half-1-the-lane}

### Placement and reach {#placement-and-reach}

`Backend/CasHotKeys.{h,cpp}`, beside the engine and above it: the lane calls the engine's verbs
and the engine never calls the lane. One instance per pool, owned by `Pool` as a member declared
before its three `CasRequests` (`mount_requests`, `farewell_requests`, `gc_requests`), so it is
constructed before them and destroyed after them. Each `CasRequests` is given a pointer to it at
construction and exposes it through `CasOperation::hotKeys()`. A `CasRequests` built without one
(every existing test, the offline tools, the pool factory's local bootstrap `CasRequests`) owns a
private instance with no cache, so a write through it costs today's `GET` plus `PUT` and every
existing test's request counts are unchanged.

```cpp
class CasHotKeys
{
public:
    explicit CasHotKeys(uint64_t cache_budget_bytes);   /// 0: no cache

    /// The caller's mutation of `key`, the engine's own decide type: the candidate bytes to write
    /// over `base`, nothing (`Declined`), or a refusal by exception. `base` is absent when the key
    /// does not exist; a caller that refuses to bootstrap throws there.
    using Decide = DecideOnObject;

    /// One hold on `key`: wait for the turn, obtain a base, run `decide`, one engine write,
    /// remember. Returns what `readModifyWrite` would return for the same call, in class and
    /// content, and propagates a `decide`'s exception as that verb does, never from a cached base.
    WriteResult submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide);

    /// Test seams: items in the key's queue, the holder included (0 for a key with no lane);
    /// lanes in existence.
    size_t queueDepthForTest(const String & key) const;
    size_t laneCountForTest() const;

private:
    struct Item { uint64_t ticket; };     /// one queued submission, on its caller's stack; a struct so phase B adds to it
    struct Lane                           /// one key; created on first use, erased when its queue empties
    {
        std::deque<Item *> queue;                    /// guarded by `mutex`; the front is the holder
        std::optional<uint64_t> holder_since_ms;     /// guarded by `mutex`; for the log line
        std::condition_variable cv;
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
    /// The last known object per key: `CacheBase<String, Object>` with a byte weight, constructed
    /// with the "LRU" policy and two metrics (`CASHotKeyCacheBytes`, `CASHotKeyCacheEntries`),
    /// bounded by `cache_budget_bytes`, null when the budget is 0: the shape `CasManifestReader`
    /// already uses for its decode cache. Its synchronization is its own, and it is read and
    /// written outside `mutex`.
    std::unique_ptr<CacheBase<String, Object, std::hash<String>, ObjectWeight>> cache;
};
```

`submit` is to a shared key what `readModifyWrite` is to a key nobody shares: one logical write.
Where the engine's verb starts from its own `GET` and retries its own conflicts inside, `submit`
waits its turn, starts from the cache when it can, and returns the engine's `Conflict` for the
caller to retry by submitting again. The retry loop belongs to the caller (`casUpdateImpl`'s), as
the hand-written loops of the backend request contract already do, under one frozen policy.

### The contract of a `decide` {#the-contract-of-a-decide}

A caller may write a key through the lane when every `decide` it will ever submit for that key
meets three conditions. The catalog's meet them today; they are the entry condition for any other
key.

1. It may be run more than once, and a later run on a fresh read is the decision that counts.
   Running it has no externally visible effect that a second run would double, and its verdict on
   a fresh read is a valid serial verdict at that moment whatever an earlier run on a hint said.
   Its reads of other keys may answer differently between two runs; then the later run decides on
   the later state, as a caller that retried after the earlier verdict would. The lane runs a
   `decide` twice only when the first run was on a cached base and rendered a verdict. The
   catalog's `decide`s are functions of `base` plus reads that are idempotent but not frozen: the
   creator-fence check of `cancelStalledCreating` and `reconcileStaleCreator` (`CasRefCatalog.cpp:539`,
   `:689`), whose answer can change from "still live" to "terminal" between two runs and never
   back (the three certificates `classifyFenceCertificate` recognises, `GcFenced`, `CleanFarewell`
   and `SupersededEpoch`, are states a lease does not leave).
2. It refuses by throwing and declines by returning nothing, and either is the verdict
   `readModifyWrite` would render on the same base. The lane reports a verdict only when it was
   rendered on a fresh read; one rendered on a cached base is discarded and the `decide` runs
   again on a read. A decline is a verdict like a refusal because a stale hint can say "nothing to
   write" about an object the store no longer holds.
3. It issues no write through the lane to any key (a write from inside a hold would wait for the
   hold), and its reads go through the operation it was given, under a policy its caller chose
   knowing they run inside the hold.

For `_ckpt`, `gc/state` or any other key, condition 1 is the one to audit before its first
`submit`: `publishCkptContribution`'s `decide` counts its runs and records its decline's reason in
captured locals that the caller's outcome then reads, so a re-run on a fresh base doubles what it
reports.

### The ticket {#the-ticket}

The lane mirrors `CasRefLedger::appendRefOps` on purpose. Nothing runs under `mutex` except the
steps marked.

**Enter.** `submit` binds the policy to an absolute deadline on the engine's clock, as every verb
does at entry, so time in the queue spends the caller's window. Under `mutex`: find or create the
key's lane, push the item. Two allocations can fail there, the lane's map node when the key has no
lane and the deque's growth; if the push throws after the lane was created, the empty lane is
erased in the same critical section and the exception propagates, so a failure leaves the map as
it was. The moment the push succeeds a ticket guard is installed: on every exit, normal or by
unwinding, it runs the leave step below. It is `noexcept`, allocates nothing, and is the only
thing that removes an item from the queue.

**Wait.** Loop:

1. Outside `mutex`: the engine's own `gate(0)` on the caller's operation, in the engine's own
   order (`CasRequests.cpp:361`, `CasMountRuntime::admit`): the fence generation, then the lease
   budget, `NoBudget`, then `Liveness`, which the engine reports as `FenceLost` and deliberately
   does not tell apart from the fence; then the caller's bound against the engine's clock. In that
   order so a lost fence or a stopped task is not reported as a policy deadline, an exhausted
   lease is reported as the lease, and a stopped task whose lease is also exhausted is reported as
   the lease, as the engine reports it before every attempt today. Any of the three means the
   caller leaves.
2. Under `mutex`: if step 1 decided to leave, leave with the `GaveUp` the engine would have
   returned for the same condition (`FenceLost`; `Deadline` with `Source::Lease`; `Deadline` with
   the bound's own source), which the caller handles as it handles the engine's; a waiter that has
   reached the front leaves the same way, since the write it would issue would be refused at the
   same gate. Otherwise, if the item is the front of the queue: set `holder_since_ms`, leave the
   loop as the holder. Otherwise `cv.wait_for(lock, 200ms)` (the ledger's
   `recovery_cv.wait_for(lock, 200ms)` pattern), release the mutex, go to step 1. The slice only
   bounds how late a waiter notices its own fence or deadline; a handover wakes it at once.

**Hold.** [The hold](#the-hold), on the caller's thread and operation. Nothing in it is touched by
any other thread: `CasOperation` stays what its header says, single-threaded.

**Leave.** The ticket guard, under `mutex`: erase this item; if it held, reset `holder_since_ms`;
if it left on its own fence, lease or deadline while another item was at the front, snapshot that
item's ticket and `holder_since_ms` for the log line; if the queue is now empty, erase the lane,
else `cv.notify_all()`. A `Lane` is referenced only by threads whose item is in its queue (a
waiter between slices included), so an empty lane has no referent and the erase invalidates only
what nobody holds. After releasing the mutex: record the queue time and emit the log line if a
snapshot was taken, inside a catch-all in the `tryLogCurrentException` shape.

### The hold {#the-hold}

1. **Base.** The cache's object for the key, if present; else the engine's own `observe(key,
   policy, bound)`, the read `readModifyWrite` starts with, which may find the key absent. A read
   it refuses (the caller's fence, `Liveness`, lease or deadline) is converted exactly as
   `readModifyWrite` converts it, through `gaveUpAfterFailedObservation` with a fresh `WriteState`,
   so a refused base read is the `GaveUp` today's verb returns, with its `Source` and stop, and
   never an exception; the public `read` throws there and the lane does not use it. A cached start
   is not gated separately: the write's own gate refuses a caller past its fence, lease or
   deadline before anything is sent, as it does today after the verb's read, and a verdict such a
   caller renders on the cache is discarded by step 2's rule.
2. **Decide** on the base. Bytes: the candidate. Nothing: `Declined{seen}` with the base as the
   engine's observation (the `Object`, or `ProvenAbsent`); no write; a read base is stored in the
   cache. An exception: if the base was a fresh read, it propagates to the caller as today. Either
   verdict on a cached base: the cache entry is dropped and the key is read through `observe` as
   in step 1; a refused read is that `GaveUp` and the result, `FenceLost` included, which
   `casUpdateImpl` already turns into `CatalogFenceMovedMarker`; otherwise `decide` runs once more
   on the read and what that run does is the result. A verdict rendered on the cache is never
   delivered, whatever refused its validation: a hint can carry a corruption verdict about bytes
   another writer has since repaired, or a catalog marker about a row that has since moved, and a
   caller that cannot read is told what the engine tells a caller that cannot read. Any exception,
   not a chosen class: a refusal, a decode failure on bytes another writer stored, an allocation
   failure inside the decode, all get the same one re-run on a fresh read, because a `decide` may
   be run again by contract, and the only thing a verdict on a hint proves is that a hint is not a
   proof. What the re-run relies on is not that the two runs agree; it is that the second run is
   a serial execution at the time of the fresh read: its base is that read, its other reads are
   made then, and its write is conditional on that read's etag. When a `decide`'s other reads
   changed in between, as the creator-fence check can from "still live" to "terminal", the second
   run renders the later verdict, exactly what a caller that retried after the first would get,
   and the first verdict, rendered on a hint, was never delivered and never written; the fence's
   certificates do not revert, so no run says "terminal" and a later one "live". A transient
   exception in the first run (an allocation failure) followed by a landing second run is the
   landing today's caller gets on its retry. Delivering the hint's verdict whenever the fresh
   bytes equal the cached ones would be no safer (the two runs can still differ when the bytes
   differ) and would reopen the one rule this design has: a verdict rendered on a hint is never
   delivered.
3. **Write.** One engine call: `op.replace(key, candidate, base->etag, policy)`, or
   `op.create(key, candidate, policy)` when the base was absence and the `decide` returned bytes
   for it, under the caller's operation and frozen policy. The engine's verb is one logical write:
   it reissues ambiguous attempts, settles every refused precondition by a resolve read, and gates
   every attempt on the caller's fence, `Liveness` and budget as it does today. Its transport
   backoffs sleep inside the hold (a named tradeoff, below).
4. **Settle.** The engine's result is returned unchanged; only the cache moves, and every fill
   goes through one no-throw `remember` step: `CacheBase::set` allocates and may throw, and a
   failed hint fill must never replace a result the engine already produced, so the failure is
   logged, the entry is dropped, and the result stands. The same step fills the cache in step 2
   for a read base the `decide` declined to write.
   - `Committed`: `remember(Object{candidate, etag})`.
   - `Conflict{seen, attempts_sent, any_ambiguous}`: `remember(seen)` if it is an `Object`; the
     entry is dropped otherwise. The class keeps the engine's two-sided meaning, a clean lost race
     or an ambiguous attempt that may have landed and been superseded, and the caller's next
     submission decides again on `seen` when the cache holds it and on a read when the resolve
     read saw nothing; this is what today's `readModifyWrite` already demands of every `decide` in
     its precondition-moved arm.
   - `Refused`: the store refused the bytes and nothing applied; the entry is dropped.
   - `GaveUp` with `sent_any == false`: nothing was sent; the entry is unchanged.
   - `GaveUp` with `sent_any == true`: the write's fate is unknown, `FenceLost` after a landed
     `PUT` included; the entry is dropped.
   - An exception out of the engine's write: the entry is dropped and the exception propagates;
     the guard hands the key over.

**Read your writes.** A `Committed` names the caller's own candidate under its own etag, and
when `remember` admitted it (it fits the budget and the fill did not throw) the cache holds that
object and the caller's next submission starts from it; otherwise the next submission reads, and
a plain `GET` returns the same object either way (S3 and GCS are read-after-write consistent for
overwritten objects). The goal's "no `GET` when the pool wrote the object last" is stated for an
admitted object.

**Effects to name.** A `decide` that reads holds the lane for those reads, under the policy its
caller chose. A transport fault inside the hold is repaid by the engine's growing schedule, up to
5 s per reissue, inside the hold, and every other writer of the key waits through it: accepted,
since a store that throttles the pool's front writer is throttling the pool, and visible in the
keyed log line. Profile events and an exception's stack belong to the caller's own thread, as
today.

### The cache {#the-cache}

A `CacheBase` by key, constructed with the "LRU" policy name (its convenience constructor defaults
to "SLRU") and the two metrics named in the sketch, weighted by object bytes, bounded by
`cache_budget_bytes`, a `PoolConfig` field with a default in the family of
`manifest_decode_cache_bytes`; 0 disables it, which is what a `CasRequests` without a pool gets.
It is read and written outside the lane's mutex; its synchronization is its own and it calls
nothing. It holds, per key, the last object this pool knows: the candidate a `Committed` wrote,
the object a resolve read saw after a `Conflict`, or the base a hold read and then declined to
write. It is dropped on `Refused`, on `GaveUp` after a send, on an exception out of the write and
on a verdict rendered on it; it is unchanged by a `GaveUp` that sent nothing. A candidate above
the budget is simply not stored, and a fill that throws is logged and dropped, never a result. It is never a source of truth: every write against it is
conditional on its etag, and every verdict rendered on it is discarded and re-rendered on a read.
Those two sentences are the whole safety argument, and they hold for any bytes, malformed ones
included: a `decide` that cannot decode a cached object throws, the lane reads, and the read
either decodes or is the real corruption.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait of its own. The caller binds its policy at entry, so time in the
queue spends the same window today's backoff sleeps spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own fence, `Liveness`, lease and deadline, re-checked every slice | its window plus one slice |
| the holder | its own deadline for the base read and the write (the engine starts no attempt that cannot finish inside it), plus whatever reads its `decide` issues under their own policies | its window plus one attempt timeout, plus those reads' windows, unless the transport does not answer |

The reads a catalog `decide` issues today: `reconcileStaleCreator`'s creator-fence check runs
under the policy `resolveNamespaceLife` froze and passed down
(`isCreatorFenceTerminal(op, layout, ..., policy)`, `CasRefLedger.cpp:1384`), so it ends inside
the caller's window; `cancelStalledCreating`'s, in `dropNamespaceImpl`, runs under a default
`Retry::standard()` of its own (`CasRefLedger.cpp:5121`), so under a misbehaving transport it can
hold the key for up to its own 90 s after its caller's window. Today that traps its own caller;
in the lane it traps the key. The lane adds no bound of its own, by the non-goal above; the
caller-side fix, freezing once in `dropNamespaceImpl` and passing down as `resolveNamespaceLife`
does, is a BACKLOG line under `{#hot-key-lane-phase-b}`, and phase B's clamp is the lane-side
answer.

**A holder whose transport does not answer.** The engine calls the transport synchronously and
cannot cancel it. A stalled attempt ends at the backend's attempt timeout; an attempt that
trickles, or holder code that never returns for any other reason, has no bound in the engine.
Today that traps only its own caller. In the lane it traps the key on this pool: every waiter
leaves at its own deadline with retry-later until the holder returns, the connection is closed, or
the process restarts; nothing lands under an expired lease, because the holder's fence gates the
write. Accepted as a release-level availability tradeoff for that case: nothing lands that should
not, no second write starts under a running first one, the keyed log line names the holder and
its hold time while it happens, and the fix belongs in the transport, a whole-request deadline in
the S3 client, which bounds today's single trapped writer too. Evicting the holder was designed
and rejected three times in revisions 10 to 12 (an evicted request can be told `Committed` for
the survivor's identical write); not to be reopened without the transport deadline first, at
which point it is unnecessary.

### Lock order {#lock-order}

Verified for the production writers at brainstorm time:

| caller | locks held on entry to the catalog write |
|---|---|
| `CasRefLedger::namespaceLife` → `resolveNamespaceLife` (CREATE) | none; `ref_queue_mutex` scope closes before, `state_mutex` is taken after |
| `CasRefLedger::dropNamespaceImpl` → `cancelStalledCreating`, `beginRemoving` (DROP) | none; the queue lock scope closes before `removal_op` |

While a ticket is logically held: the mount fence's `generation` and `check_or_throw` read atomics;
`admit` reads atomics and calls the clock function (`clock_gettime` in production, the synchronized
test clock's own leaf mutex in the lane tests); the in-memory backend holds its mutex only for the
duration of one operation and runs hooks without it; `decide`s call `op.admitted()` and issue reads
(backend leaf); no `decide` takes a ledger mutex. The mount plane's sleep,
`CasMountRuntime::sleepInterruptibly`, takes `driver_mutex` for a condition-variable wait, so the
ticket is held across that mutex. Audit of every acquisition of `driver_mutex` in
`CasMountRuntime.cpp`:

| `driver_mutex` acquirer | what runs under it |
|---|---|
| `renewalLive`, `renewalCancelled`, the `*ForTest` accessors and setters (`renewalWorkerMayRenew` is a helper called with the lock already held) | reads and test-only writes of driver state |
| `DriverLease::finish`, `DriverLease::~DriverLease`, `admitRenewerCall`, `installRenewer`, `startRenewer`, `renewerReset` | driver state transitions and notifies, no I/O |
| `startBackgroundWorkers`, `stopBackgroundWorkers` | worker handles; joins happen with the lock released |
| `renewalLoop`, `remountLoop`, `sleepInterruptibly` | condition-variable waits and state reads; `remount_attempt()` runs after the lock is released |
| `lockTerminalPublication` and its three callers | atomic stores of terminal state; no backend request |
| `scheduleRemount` | a generation bump and a notify |
| three test-only hooks: `renewal_parked_predicate_false_hook_for_test` (inside the renewal wait's predicate, `CasMountRuntime.cpp:672`), `remount_parked_hook_for_test` (`:776`), `terminal_publication_driver_lock_acquired_hook_for_test` (`:929`) | whatever a test installs; their contract, to be stated at their `PoolConfig` declarations: no backend request and no wait on the lane, or the test deadlocks itself. Empty in production; outside the proof below |

Twenty-nine acquisition sites (`grep -c "lock(driver_mutex\|lock(runtime.driver_mutex"` on
`CasMountRuntime.cpp` at brainstorm time, one of them `try_to_lock`), every one in the rows above.
None reaches `CasRefLedger`, the catalog key, or any request on any plane, so no `driver_mutex`
holder ever waits for a ticket. The rule for future runtime code follows: nothing under
`driver_mutex` issues a write.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held to push, inspect or erase an item,
and to erase a lane. The cache is not touched under it. The fence, the `Liveness` closure, the
clock, the engine's verbs and every `decide` run with it released. The order is therefore: caller
(no locks) → `CasHotKeys::mutex` → nothing; and, with the ticket logically held, caller →
`driver_mutex` → nothing, caller → backend mutex → nothing. The one rule for future callers,
stated where the catalog API is documented: a submission is not made while holding a ledger
mutex, and no callback that runs while a ticket is held (`decide`, `Liveness`, a backend hook)
submits to the lane.

### Invariants {#invariants}

- INV-1. Per constructed `Pool` and key written through the lane, at most one conditional write
  is in flight at any moment from the operations that write through it. The pool factory's
  bootstrap `create` of the catalog runs before the `Pool` exists and is outside this; a writer
  that does not use the lane (the GC erase) races the lane's writes as it races everything today.
- INV-2. Holds on a key are entered in the order their submissions were queued; a submission
  resubmitted after a `Conflict` queues behind whoever arrived meanwhile.
- INV-3. The cache never changes the semantics of a conditional write: a stale entry costs one
  412 and one resolve read. A verdict a `decide` renders on a cached base, by exception or by
  returning nothing, is never delivered; it is re-rendered on a read, or replaced by that read's
  own `GaveUp`.
- INV-4. `submit` returns for its caller a result the engine produced for that submission,
  through the paths `readModifyWrite` uses (`observe` and `gaveUpAfterFailedObservation` for the
  base, one `create` or `replace` for the write); the lane invents no result, reclassifies none,
  and lets no exception of its own replace one. A `decide`'s exception from a fresh read
  propagates unchanged. What one submission is not is one `readModifyWrite` call: each
  submission has its own `WriteState`, and the caller's loop, not the engine, decides whether a
  `Conflict` is followed by another; the two consequences are named in [The catalog](#the-catalog).
- INV-5. An item is removed only by its own guard; no item outlives its caller's stack; a lane is
  erased only while its queue is empty; `CasHotKeys::mutex` is held across no callback, no I/O
  and no `decide`; the guard neither allocates nor throws.
- INV-6 (Half 2). A clean lost race is repaid after `conflictBackoff()` and does not advance the
  reissue counter; a conflict that settled an ambiguous attempt is repaid on the growing schedule,
  in the engine's two loops on their call-wide counter and in the lane's callers on their own
  count of such conflicts, the engine's internal reissues restarting per submission.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item`, a ticket; the caller's own stack holds the rest |
| leader | first to find `leader_active` false; serves until its own item is done | the front of the queue; serves its own item only (phase B: and the items behind it) |
| exit | `completeOwnedItemsAndReleaseLeadership` | the ticket guard; erase and hand over in one critical section |
| wait | `cv.wait`, woken at every handover | `cv.wait_for` in slices, own fence and deadline re-checked |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror | replace-style object: ambiguity settled by the engine's resolve read |

The shared shape is deliberate. Extracting a common flat-combining component is possible and is not
part of this design.

### Designed for phase B {#designed-for-phase-b}

Phase B (BACKLOG `{#hot-key-lane-phase-b}`) adds combining, the `Generation` spacing and the hold
clamp. What this revision fixes so that none of them reopens the callers:

- `submit`'s signature, `Decide = DecideOnObject`, and "returns the engine's `WriteResult`" do not
  change. Combining is internal to the lane: whether the holder also lands the items queued
  behind it. A caller's loop, its `Conflict` handling and its pause rule are the same in both
  phases.
- The queue holds `Item`s, one per submission, and the guard is the single remover. Phase B adds
  to `Item` the leader's view of a member (`op`, `bound`, `decide`, `taken`, `result`) and adds
  settlement of taken members to the guard's critical section; the FIFO, the slice wait and the
  leave rule stay.
- The hold is base, decide, write, settle, in that order. Combining inserts "take and chain"
  between decide and write, and "tell each member" into settle. The cache stores the candidate
  the write landed, which is what a combined candidate is.
- `Conflict::any_ambiguous` is what a member is told with its `Conflict`.
- The lane is erased when its queue empties. Spacing changes only that rule (an emptied lane
  outlives its last write by the interval) and adds one timestamp to `Lane`.
- The stalled-holder tradeoff and the `driver_mutex` audit are the same for a batch as for a
  single hold.

## The callers {#the-callers}

### The catalog {#the-catalog}

`CasRefCatalog::casUpdateImpl` submits instead of calling `readModifyWrite`. Its `decide` is
today's: absent base → `throwMandatoryCatalogAbsent`; decode `base->bytes`, `throwIfAmbiguous`,
`mutate`, `encode`, capturing the encoded candidate as `written`. The four markers and the
admission refusal are thrown as they are today and reach their catchers as they do today. Around
`submit` the loop is a hand-written one under a policy frozen at entry, as the backend request
contract prescribes for such loops:

```cpp
const Retry frozen = op.freeze(policy);   /// the caller's policy, frozen once; a nested mutation spends the outer window
uint32_t faults = 0;
for (;;)
{
    WriteResult result = op.hotKeys().submit(key, op, frozen, decide);
    if (const auto * conflict = std::get_if<Conflict>(&result); conflict && !frozen.single_attempt)
    {
        /// flat after a clean lost race; the growing schedule after a conflict that settled a fault
        op.pause(conflict->any_ambiguous ? Retry::backoff(++faults) : Retry::conflictBackoff());
        continue;   /// the next hold starts from `seen` when the cache holds it
    }
    /// unchanged from today: FenceLost -> CatalogFenceMovedMarker; anything else not Committed -> throwCatalogWriteFailure
    ...
    return std::move(*written);
}
```

`policy` is `casUpdateImpl`'s existing parameter, `Retry::standard()` by default and the policy
`resolveNamespaceLife` froze once and passes down, so a catalog mutation nested in a lifecycle step
spends that step's window and never a fresh one. Under a `single_attempt` policy the first
`Conflict` is the result, as `readModifyWrite` returns it today (`CasRequests.cpp:980`), and
`throwCatalogWriteFailure` throws it as today; the loop never sends a second attempt under
`Retry::once`, which permits one. No production catalog caller passes `once` today; the rule is
the contract's. The loop ends at the frozen deadline with today's retry-later, through the same
`GaveUp{Deadline}` `submit` returns when the wait or the write runs out of window, and has no
attempt cap: today's `readModifyWrite` has none either, and a cap of `kMaxCatalogCasAttempts`
(100) at a flat mean pause of 100 ms would end a writer after about ten seconds of cross-pool
conflicts, well inside the window this change exists to make fair. The bare `op.pause` can
overshoot the deadline by at most one pause: 200 ms after a clean conflict, and up to 5 s after a
conflict that settled a transport fault, exactly what the erase's loop already accepts for its
own growing pause (`CasRefCatalog.cpp:456`); a fence lost during the pause is caught by the next
submission's first gate before anything is sent. Accepted, on a path that is already failing.

Three things differ from one `readModifyWrite` call, all the semantics every hand-written loop of
the contract already has, the GC erase's `replace` per attempt above all: each submission is one
engine call with its own `WriteState`, so a credential refresh may happen once per submission
rather than once per catalog mutation (a `Conflict` between two submissions means the store
answered in between, so the credentials the last refresh installed worked); `attempts_sent` in a
result counts that submission's attempts; and the engine's internal reissue counter restarts per
submission, so a submission whose write suffered K transport faults before a settled `Conflict`
slept the growing schedule up to `backoff(K)` inside the engine, and the caller's pause after it
is `backoff(++faults)` on the caller's own count of such conflicts, not `backoff(K + 1)`. The
erase loop paces its attempts on its own counter the same way. `casUpdate`, `casAdmitEntry` and the five lifecycle
functions do not change: their `mutate`s, markers and outcomes are the same. `casAdmitEntry`'s
duplicate-row `LOGICAL_ERROR`, thrown from the grammar check on a cached base, is re-rendered on a
read like every other exception and is the same `LOGICAL_ERROR` on a read that shows the row;
both functions are called only from tests. This is the first change in `CasRefCatalog.cpp`.

The second is a catch that is missing today and that the queue wait would widen: `casUpdateImpl`
turns the engine's lost fence into `CatalogFenceMovedMarker`, which `casUpdate` translates and the
lifecycle functions catch, but `createNamespace` catches only `CatalogEntryAlreadyPresentMarker`
around step 1, so a creator whose fence moves during step 1 throws the bare marker, a
`std::exception` no caller names. Step 1 catches `CatalogFenceMovedMarker` and returns
`NamespaceCreationOutcome::FencedOut`, which `resolveNamespaceLife` already handles; `casAdmitEntry`
gets the translation `casUpdate` performs.

### The GC erase {#the-gc-erase}

`deleteCompletedRemovingAtSnapshot` does not write through the lane. It stays the hand-written
loop it is today: refresh the leader's authority, check the exact row on the snapshot, `replace`
against the snapshot's etag, a mandatory resolution read, a growing pause. It races the lane's
writes as it races every writer today: the loser's precondition is refused and settled by its
resolve read, and a GC erase that wins leaves the lane's cache stale, so the pool's next catalog
submission pays one 412 and one resolve read for it, then proceeds. GC erases are rare, one per
removed namespace per round. Bringing it in, with its four prerequisites, is in
`{#hot-key-lane-phase-b}`.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff()` returns `backoff(1)`, uniform over [0, 200] ms inclusive (`backoff`
draws `thread_local_rng() % (ceiling + 1)` and doubles the ceiling from attempt 2 on). It is flat:
it does not grow with the writer's loss count, and it knows no dialect.

- `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`, for the sites that keep using
  them: on a `Conflict` whose inner write had no ambiguous attempt (a clean refused precondition,
  settled by the resolve read), a `pauseForConflict` that performs the same `gate` and `fits`
  checks as `pauseAndReissue` with `conflictBackoff()` and leaves `state.reissues` untouched
  (nothing reads it; `pauseAndReissue` alone writes it). It records a new profile event,
  `CASRequestConflictPause`, and not `CASRequestReissue`; in these two loops the reissue counter
  then counts transport reissues only (`removeCurrent` and the read loops keep recording it as
  they do today).
- A `Conflict` whose inner write had an ambiguous attempt (`state.any_ambiguous`: a transport
  fault, a 429 among them, that the resolve read then settled as a lost race because the key had
  moved) keeps `pauseAndReissue` and its growing schedule: the fault is the signal that must pace
  the loop, and `writeLoop` already carries the distinction.
- The lane's callers pause between a `Conflict` from `submit` and their next submission by the
  same rule, and for that `Conflict` gains `bool any_ambiguous`, set where `writeLoop` returns it
  from the `WriteState` it already keeps and carried where `readModifyWriteOnPresence` rebuilds a
  bodyless `Conflict` under `single_attempt` (`CasRequests.cpp:1034`): `conflictBackoff()` when
  false, `Retry::backoff` on the caller's own fault count when true, whether or not the resolve
  read saw a body. `attempts_sent`
  cannot stand in for the flag: a throttled first attempt whose resolve read finds the key moved is
  `Conflict{attempts_sent = 1, any_ambiguous = true}`.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. On a key written through
the lane, conflicts arise only between pools and servers, and against the GC erase.

Half 2 is engine-wide and the lane covers the keys that write through it. The seven other
`readModifyWrite` sites keep their in-process contention (`publishCkpt` on `_ckpt` above all), and
on S3 their writers' retry pace goes from a schedule saturating at 5 s to a flat mean of 100 ms.
This is the same fairness fix for the same starvation shape at smaller scale. Its cost, as one
number: a writer whose round trip is negligible attempts about ten times a second on average,
each attempt with its resolve `GET`, so a contended non-lane key sees about 20 M requests per
second for M writers in expectation (draws near 0 give back-to-back attempts, so a one-second
window can exceed it) where today the growing schedule caps a writer near one attempt per 5 s. A
refused precondition is a 412 with no body, S3 has no per-object write limit and its per-prefix
request budget is thousands per second, and sustained pressure past it answers `SlowDown`, a
transport fault that takes the growing schedule; GCS answers 429 the same way. Go/no-go for
`_ckpt`, alongside the `ref_catalog` one: the predicted request rate on a contended `_ckpt` key is
about 20 M per second for M writers in expectation, so the gate is on the measured peak, not the
expectation: no one-second window on a contended key above 40 M requests for the M writers
observed in it, and 429/`SlowDown` counts on those keys not above today's; if either fails,
writing `_ckpt` through the lane is the answer, not a slower jitter.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a clean lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff()`, a flat uniform over [0, 200] ms that does not grow with the writer's
   loss count. The growing schedule belongs to transport faults, and to a conflict that settled
   one. A key several writers of one pool share is written through the hot-key lane, above this
   engine, and never conflicts with itself: see the hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a
   settled fault, `backoff(attempt)` otherwise and after a fault". In the site table, the
   `CasRefCatalog::casUpdateImpl` row becomes "the hot-key lane's `submit`, under the caller's
   policy frozen once, `conflictBackoff` between submissions and `backoff` after a conflict that
   settled a fault; every other result handled as today".
3. In the result vocabulary, `Conflict` carries `any_ambiguous`: whether an attempt of the inner
   write was ambiguous before the resolve read settled it, so a caller outside the engine paces a
   settled fault as the engine does.

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to hold or departure, per item, on every exit.
- `CASHotKeyCacheStarts` and `CASHotKeyReadStarts`: how a hold obtained its base.
- `CASHotKeyCacheVerdictsReread`: verdicts rendered on the cache and re-rendered on a read.

One log line, naming the key: a waiter that left on its own fence, lease or deadline while
another item was at the front, with that item's ticket and how long it has held. It is what makes
a stuck holder visible while it is stuck.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after. The same run records
`CASHotKeyQueueWaitMicroseconds` per submission and the `PUT` rate on the key: phase B's
combining pays when the queue wait, not the write, dominates a submission.

## Tests {#tests}

Lane level, in a new `gtest_cas_hot_keys.cpp` on `gtest_cas_requests.cpp`'s harness: counting
backend, write hooks, and a synchronized clock (a mutex-guarded `now` advanced by the test thread,
sleeps recorded under the same mutex), since the harness's `FakeClock` is single-threaded. The
`CasRequests` under test share one `CasHotKeys` with a cache. Each `decide` decodes a list of
tickets from `bytes` and appends its own, so order is visible in the object.

1. Serialization, order, cache (INV-1, INV-2, INV-3). N threads submit; a write hook parks the
   first holder, and each further thread is released only after the test observes its item queued
   (`queueDepthForTest`), so arrival order is the release order. Assert: reads 1, writes N, no
   `Conflict`, the final object lists all tickets in arrival order, every caller received
   `Committed` with its own candidate's etag, and a following submission starts from the cache
   with zero reads. A sibling installs a hook asserting `CasHotKeys::mutex` is not held whenever
   `decide`, a `Liveness` closure or a backend hook runs (INV-5). Another fails each of Enter's two
   allocations in turn, the lane's map node and the deque's push after the lane was created:
   `laneCountForTest` and the key's `queueDepthForTest` are as before and the exception reaches
   the caller (INV-5).
2. Results are the engine's (INV-4). A hook makes the write `Refused` by the store: the caller
   receives that `Refused` with the store's code and message, and the catalog-level caller throws
   it at once with no resubmission. A hook makes the write lose to an external `CasRequests`: the
   caller receives `Conflict{seen}`, its next hold starts from `seen` (the cache holds it), and the
   real `beginRemoving` re-decides and resolves `AlreadyRemoving`, never `EntryChanged`. A hook
   fails the resolve read at the transport after a clean refusal: `Conflict{NotObserved}`, the
   caller pauses `conflictBackoff` and its next hold reads. A hook makes the write ambiguous and
   refuses the resolve read by the bound: `GaveUp{Unresolved}`, the cache entry dropped. The
   fence is tripped before the write: `GaveUp{FenceLost, sent_any = false}`, and the catalog caller
   returns `FencedOut`. The fence is tripped after the landed `PUT`: `GaveUp{FenceLost, sent_any =
   true}`, the object carries the ticket, the entry dropped. The engine call throws: the exception
   reaches the caller, the entry is dropped, and the next waiter holds (the guard handed over).
   Under `Retry::once`: a first `Conflict` ends the catalog caller with `throwCatalogWriteFailure`'s
   class and exactly one attempt sent, as today. Credential refresh: a hook fails the first
   submission's attempt with a refreshable credential error, lets the refreshed reissue lose a
   race, and fails the second submission's attempt the same way; assert the second submission
   refreshed once more (per-submission `WriteState`, the named difference from one
   `readModifyWrite` call) and that the store's credential calls are exactly two.
3. Verdicts on the cache (INV-3), at the catalog level in `gtest_cas_ref_catalog.cpp`. The pool
   lands once. An external `CasRequests` changes a row. A caller reads fresh outside the lane and
   runs the real `beginRemoving` through a `CasRequests` with a cache: `Transitioned`, not
   `EntryChanged`, one lane read, `CASHotKeyCacheVerdictsReread` 1. The matrix: for each of
   `createNamespaceStep1`, `completeCreation`, `beginRemoving`, `reconcileStaleCreator`,
   `cancelStalledCreating` (their markers) and the admission refusal: one case where the cache is
   stale and the refusal would be false (positive outcome, as without a cache), and one where it is
   true on the read (the same outcome and error class as without a cache, one lane read, no write,
   plus whatever reads the function itself makes after a mismatch, as `beginRemoving` does).
   `casAdmitEntry` of a namespace whose cached row an external erase removed admits it, and of one
   that is present raises its `LOGICAL_ERROR`. A `decide` that throws decode corruption on a cached
   object reads once and, when the read decodes, lands; when the read is corrupt too, throws. A
   cached base whose key the store has since deleted: the read returns absence and the catalog's
   `decide` throws `throwMandatoryCatalogAbsent`, as today. The fence is tripped between a
   cached-base `decide` that threw `CatalogFenceMovedMarker` and the validating read: the read is
   refused at its gate, `submit` returns `GaveUp{FenceLost}`, `beginRemoving` returns `FencedOut`,
   the marker rendered on the cache was not delivered and no read was made; a sibling caches
   malformed catalog bytes through a `Conflict`, has the external writer repair them, trips the
   fence, and asserts `FencedOut` and no `CORRUPTED_DATA`. Declines: a `decide` that returns
   nothing on a cached base an external writer has since replaced: the entry is dropped, one read,
   the `decide` runs again and this time writes, `Committed`, the decline never reported; on a
   fresh read: `Declined{Object}` with that read, no write, the next submission starts from the
   cache with that object; on an absent key, `Declined{ProvenAbsent}`. Reads that change between
   the two runs: the real `cancelStalledCreating` on a cached base whose creator fence is live at
   the first run and terminal at the second (the test flips the certificate between them): the
   second run cancels and lands, `Cancelled`, one lane read, and the same two calls of today's
   `cancelStalledCreating` around the same flip end the same way; a transient exception thrown by
   the first run only (a hook) and a landing second run: `Committed`, and today's caller retrying
   after that exception lands the same object.
4. Base read failures (INV-4). A base read that exhausts its transport retries returns the
   `GaveUp{Deadline}` `readModifyWrite` returns for the same read on the same harness, equal in
   class, `Source`, `sent_any` and `last_seen`; one whose fence is lost during it returns
   `GaveUp{FenceLost}`; neither is an exception. A cached start by a caller past its deadline
   returns `GaveUp{Deadline, sent_any = false}` from the write's own gate, with no request sent
   and the `decide` having run once (INV-3: its verdict, if any, not delivered).
5. Fence during step 1. A creator queued behind a parked holder has its fence tripped before its
   turn: `createNamespace` returns `FencedOut`, nothing was written, `resolveNamespaceLife` reports
   retry-later. Tripped between its landed step-1 `PUT` and the post-commit check: `FencedOut`
   again, with the `Creating` row durable for a later reconciler. `casAdmitEntry` under a tripped
   fence throws `throwCasTransientUnavailable`'s class, never a bare marker.
6. External writer. A second `CasRequests` writes between two submissions of the first. Assert
   exactly one resolve read and one retry write, `Committed`, the landed body is the external
   writer's bytes plus this call's ticket, and the submission after that starts from the cache. A
   sibling has the external writer store malformed bytes, then repair them, between two
   submissions: the second re-reads and lands, no corruption verdict.
7. Cache rules. A store `Refused`, an `Unresolved`, an exception out of the write, and a candidate
   above the budget each leave no entry: the next submission reads. A `GaveUp` that sent nothing
   leaves the entry as it was. Eviction: two keys whose objects exceed the budget together evict the
   older; the next submission on it reads. A hook makes `CacheBase::set` throw after a landed
   `PUT`, after a `Conflict` whose resolve read saw an `Object`, and after a fresh read the
   `decide` declined: the caller receives `Committed`, that `Conflict` and that `Declined`
   respectively, never the exception, and the next submission reads (INV-4).
8. Waiters leave on their own (INV-5). While the holder is parked inside its `decide`'s nested
   read: a waiter whose deadline passes leaves with `GaveUp{Deadline}` carrying its own source and
   the keyed log line naming the holder; a waiter whose fence is tripped or whose `Liveness` flips
   leaves with `GaveUp{FenceLost}`; a waiter whose lease budget runs out leaves with
   `GaveUp{Deadline, Lease}`; a waiter whose `Liveness` is false and whose lease budget is exhausted
   at the same slice leaves with `GaveUp{Deadline, Lease}`, the engine's order. The item is gone in
   every case. A waiter at the front whose deadline
   passed in the same slice the holder left never holds, never runs `decide`, reads nothing. After
   the last item of a key leaves, `laneCountForTest` is 0 and `queueDepthForTest` of the key is 0;
   a hook parks a waiter between its wait slices while the holder leaves, and the waiter's next
   slice finds its item at the front and holds.
9. Stalled and throttled holders (INV-1). The holder is parked inside a `PUT` that does not
   return; the clock is advanced past every waiter's deadline: each waiter leaves with its own
   `GaveUp` and the log line, no second write started, only the holder's item remains, and when the
   `PUT` is released the holder lands, the cache holds its candidate, and the next writer starts
   from it. A hook throttles the holder's `PUT` with a transport fault K times: the waiters stay
   queued through the growing backoff and the write then lands.
10. The GC erase outside the lane. The real `deleteCompletedRemovingAtSnapshot` on an open-plane
    `CasRequests` of the same pool runs while a lane holder is parked: it does not wait; whichever
    write is refused is settled by its resolve read; after an erase that wins, the pool's next
    submission pays exactly one resolve read and one retry write, lands, and the one after starts
    from the cache.
11. Half 2 (INV-6), parameterized over `readModifyWrite` and `readModifyWriteOnPresence` on a key
    not written through the lane. K clean lost races: `CASRequestConflictPause` K,
    `CASRequestReissue` 0, every sleep within [0, 200] ms. K races each preceded by a 429-class
    fault: `CASRequestReissue` K, `CASRequestConflictPause` 0, the growing schedule. Through
    `replace`: a throttled first attempt whose resolve read finds the key replaced returns
    `Conflict{attempts_sent = 1, any_ambiguous = true}`, a clean 412 `any_ambiguous = false`;
    through `readModifyWriteOnPresence` under `Retry::once`, the same ambiguous-then-moved case
    returns `any_ambiguous = true` from the rebuilt bodyless `Conflict`; and the catalog loop, fed
    K of the former, pauses on the growing schedule, and fed K of the latter within [0, 200] ms
    each. The overshoot: a deadline that passes during the loop's pause is noticed at the next
    submission's first gate, at most one `conflictBackoff` late after a clean conflict and at most
    one `backoff(K)` late after K settled faults, with nothing sent.

Pool level, through the ledger:

12. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, exactly 2N catalog `PUT`s (steps 1 and 3, one write each), and exactly one
    lane base read (`CASHotKeyReadStarts == 1`; the callers' own pre-check reads are outside the
    lane and not counted). A second `Pool` over the same backend as the external writer: the first
    `Pool`'s next catalog mutation costs exactly one extra read and one retry write.

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
on a `CasRequests` without a pool and therefore without a cache, and must stay green. What they
count: writes, through `WriteCountingBackend`; none of them counts reads of the catalog key. A lone
`casUpdateImpl` call costs one read and one write, as today. A contended call costs one more read
per conflict than today: today `readModifyWrite` reuses its resolve read as the next iteration's
base, while a resubmission on a cache-less `CasRequests` reads again. The four tests that induce
a catalog conflict (`CasUpdateRetriesOnConflictAgainstFreshState`,
`BeginRemovingRechecksAdmissionAfterACatalogConflict`,
`CasUpdateThrowsOnVanishMidRetryInsteadOfReplacingTheCatalog`, and its death-test twin) assert
outcomes and write counts, not read counts, and are the ones to re-run first. That is the check
that the catalog API and its write shape were not touched.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Combining, the `Generation` spacing, the hold clamp, the GC erase inside the lane, and `_ckpt`
  through the lane: BACKLOG `{#hot-key-lane-phase-b}`, with the rules twenty-two review rounds
  established for them and the measurement that gates each.
- Freezing `dropNamespaceImpl`'s policy once and passing it to `cancelStalledCreating`'s
  creator-fence read, as `resolveNamespaceLife` already does: a line under the same item; a
  caller-side change.
- A whole-request deadline in the S3 transport, which makes a non-answering attempt end on its own
  and closes the accepted availability tradeoff: a BACKLOG item, and the only planned answer to
  that case.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.

## Revision history {#revision-history}

Revisions 1 to 26, all on 2026-09-04, designed the lane with combining, an in-lane GC erase,
memory, `Generation` spacing and, at times, eviction of a stalled holder, through sixteen `codex`
(`gpt-5.6-sol`, high) and six `opus` review rounds. The rounds found the real things: the
`taken` handshake between a leader and a leaving waiter, the cross-plane fence gap, the
`WriteState` fields whose meaning broke once one write served several callers (which moved the
lane above the engine in revision 21), the false verdict a cached base can render, the `driver_mutex`
audit, the step-1 fence marker that leaked. They also never converged: findings per round stayed
between eight and twenty-two, the document grew from 4.4k to 13.5k words with each rewrite
dropping a third and the loop adding it back, and the same ten classes of finding recurred in new
forms, most of them at the periphery of combining, spacing and erasure. Revision 26 is the last
of that line, commit `26bde9f9604`.

Revision 27 cuts the landing to what the measurement needs, phase A: the FIFO ticket, the cache
under one absolute rule, the engine's result unchanged, the flat conflict pause with the growing
schedule for a conflict that settled a fault, the step-1 marker catch, the `driver_mutex` audit,
and the stalled holder as an accepted tradeoff. With no member of a hold but the holder, the
`taken` handshake, per-member gates, the as-if-serial argument, the `Liveness` two-thread and
same-plane rules, the `single_attempt` barriers, the decline that cuts a chain, the settlement
table and the clamp have nothing to protect and are gone from this document; each is recorded in
`{#hot-key-lane-phase-b}` as a rule phase B keeps. What this revision kept from the rounds, as the
checklist a review verifies against: a verdict on a cached base is never delivered, whatever
refused its validation (rounds 1, 5, 6, 7, 25); the base read is the engine's `observe` with
`readModifyWrite`'s conversion, never the throwing `read` (25); the caller's policy is frozen
once, not a fresh `standard` (25); `Conflict::any_ambiguous`, because `attempts_sent` cannot
stand in (opus 17, codex 25); a cache fill that throws after a landed `PUT` is never the result
(9, 23); the guard is the single remover and neither allocates nor throws (2, 3, 5, 13); a waiter
re-checks its own fence, `Liveness`, lease and deadline every slice, in that order (3, 4, opus
16); the lane is erased only when empty (9, 25); no attempt cap on the catalog loop (23); the
200 ms pause overshoot accepted (24); eviction of a stalled holder rejected (11, 12); the
`driver_mutex` audit (14); the step-1 marker catch (12).

Revision 28 folds in the first `codex` review of the phase A document: one critical and four
minors, the critical a one-line fix. The catalog loop honours `single_attempt`: a first `Conflict` under `Retry::once`
is the result, as `readModifyWrite` returns it today, and never a second attempt. The two ways a
submission differs from one `readModifyWrite` call, a per-submission `WriteState` (one credential
refresh per submission) and per-submission `attempts_sent`, are named as the hand-written-loop
semantics they are, with a test for the refresh. Minors: every cache fill goes through one
no-throw `remember`, tested after `Committed`, `Conflict` and `Declined`; the pause overshoot is
stated as up to 5 s after a settled fault, the erase loop's own; `any_ambiguous` is carried where
`readModifyWriteOnPresence` rebuilds a bodyless `Conflict`; the Half 2 rate is an expectation and
the `_ckpt` gate is on the measured one-second peak.

Revision 29 folds in the second `codex` review of the phase A document: one critical, argued
against, and five minors, folded. The critical: `cancelStalledCreating` and `reconcileStaleCreator`
read the creator fence inside their `decide`, so a run on a cached base can say "still live" and
the re-run on a fresh read can say "terminal" and land the cancel, where today's single run would
have reported "still live". The document now says what the re-run relies on: not that two runs
agree, but that the second is a serial execution at the fresh read, with its other reads made
then and its write conditional on that read's etag; the later verdict is the one a caller
retrying after the first would get, the certificates do not revert, and the hint's verdict was
never delivered. The reviewer's alternative, delivering the hint's verdict when the fresh bytes
equal the cached ones, is refused: it does not remove the flip when the bytes differ and it
reopens the one rule the cache has. Condition 1 of the contract says "may be run again" instead
of "yields the same result", and test 3 pins the flip and the transient exception against two
calls of today's function. Minors: the waiter's gate order is the engine's (fence generation,
lease budget, `Liveness`), tested with both refusing at once; the engine's internal reissue
counter restarting per submission is the third named difference from one `readModifyWrite` call,
with the erase loop as precedent, and INV-6 says so; three test-only hooks that run under
`driver_mutex` are in the audit with the contract they must keep; read-your-writes is stated for
an admitted object; Enter's two allocations and the rollback of an empty lane are stated and
tested.
