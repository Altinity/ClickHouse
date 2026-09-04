---
description: 'Design for serializing one process''s mount-plane conditional writes to a hot CAS control object (the ref catalog first) inside the request engine, remembering the last committed object so consecutive writes need no read, reporting a decide''s refusal only from a proven base, and pacing lost races between servers with a flat jitter instead of the transport-fault backoff. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 6. Brainstormed 2026-09-04 against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the two BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`. This document
supersedes the fix sketch in those items where they differ: the door lives in the engine, not in
`CasRefCatalog`.

Revision history, kept because each step removed something a review proved unsound:

- Revision 1 combined the `decide`s queued behind a leader into one `PUT`, and claimed the
  remembered object is at least as fresh as anything a caller observed. Neither held: another server
  can write after this process's last commit, so a `decide` that refused on memory could report a
  false verdict with no write to catch it.
- Revision 2 kept combining and added the verdict rule below. The second review showed that
  combining generic closures is unsound on its own: a later closure can change what an earlier
  closure's contribution or refusal was about (a capacity refusal rendered on a prefix that the
  final candidate would have admitted; a row a later closure rewrites), and the engine cannot know
  which closures compose. Typed catalog deltas with provable composition would fix that, but they
  belong in `CasRefCatalog`, where admission and row exactness are defined, and they are a larger
  design.
- Revision 3 dropped combining: a per-key FIFO ticket with a deadline, one remembered object, one
  rule for verdicts, and a flat conflict jitter, shared by the pool's three planes.
- Revision 4 takes the open plane out of the lane. The GC erase refreshes a cached authority flag
  immediately before its `replace`; a wait in the lane would widen the gap between that refresh and
  the `PUT` from microseconds to the whole wait, and a deposed leader could erase. The lane is now a
  mount-plane facility, whose writers are fenced by atomics the engine re-reads before every
  attempt. The same round fixed the order of the waiter's checks, made the memory start gated like
  the observation it replaces, made the leave guard allocation-free, and kept the growing schedule
  for a conflict that settled a transport fault.
- Revision 5 leaves the GC erase loop's pacing as it is (its `replace` cannot tell a clean lost
  race from one that settled a fault), states the stalled-holder case as accepted fail-stop with
  its recovery, copies rather than moves what the memory keeps, narrows the liveness rule to the
  operations that write the hot key, and stops claiming that 429s alone hold a store's per-object
  write budget.
- Revision 6 keeps the engine's rule that an exception from `decide` propagates unchanged: only a
  `nullopt` rendered on memory is re-rendered on an observation, and the catalog's own verdict
  exceptions travel as `nullopt` plus a captured marker inside `casUpdateImpl`. A ticket guard
  covers every exit from the moment of enqueue, a holder that outlives its own worst case no
  longer wedges the key (waiters escape to un-ticketed writes, which the precondition keeps safe),
  and the memory start reserves one attempt like the read it replaces.

## The problem, as measured {#the-problem-as-measured}

Every `CREATE TABLE` and `DROP TABLE` on a content-addressed disk mutates one pool-wide object,
`cas/ref_catalog`, through a conditional write. Ten parallel stateless jobs produced this on the
CA-s3 lane:

| measurement | value |
|---|---|
| `DROP TABLE` (n = 1436) | p50 2.4 s, p90 11.9 s, max 34.7 s |
| `CREATE TABLE` | p50 184 ms, p90 391 ms, max 10.4 s; one gave up after 78.9 s at the 90 s policy deadline |
| `PreconditionFailed` on `ref_catalog` in the failing window | 113 in 80 s, from 53 threads |
| the losing writer alone | 35 attempts, gaps growing to the 5 s cap |
| one 33.6 s `DROP`: the namespace-removal write | one conditional write that lost eight races in a row, 15.4 s |

Three facts from the code explain the shape:

1. `CasRefCatalog::casUpdateImpl` calls `CasOperation::readModifyWrite` under `Retry::standard()`.
   That verb always starts with a `GET`, and on a lost race sleeps
   `Retry::backoff(++state.reissues)`: full jitter over a window that doubles from 200 ms to the
   5 s cap. The counter is shared with transport faults. A writer that has lost several races is
   asleep for seconds while a fresh writer starts at zero, so the oldest loser is the least likely
   to win the next race. The RCA calls this spec-conformant and unfair by construction.
2. Every writer in one process races every other writer in the same process. Compare-and-swap is
   needed only against other servers; inside one server the races are pure waste, and each costs a
   `GET`, a refused `PUT`, a resolve `GET` and a sleep.
3. The GC erase (`deleteCompletedRemovingAtSnapshot`) is a hand-written loop over `replace` and a
   resolution read that paces its lost races with the same growing schedule.

The server is not CPU-bound during this: query threads wait in the drop queue and in the catalog
write; 21% of the CPU samples are stack capture for expected exceptions.

## Goals and non-goals {#goals-and-non-goals}

Goals:

- Inside one process, at most one mount-plane conditional write in flight per hot key, in arrival
  order.
- A `readModifyWrite` that follows a commit of this process starts from the committed object, not
  from a `GET`. N concurrent catalog mutations of one process cost N sequential `PUT`s, one `GET`,
  and no 412 against each other.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, to any read path, or to the GC erase's authority window.
  One internal change in `CasRefCatalog.cpp` (how `casUpdateImpl` carries its markers, below).
  Every existing catalog test stays green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- A general write cache for every key. Participation is opt-in, per key, by the owner.
- Combining several writers' mutations into one `PUT`. On S3 the lane's throughput is one `PUT`
  latency per write, an order of magnitude above this lane's arrival rate. On a store that budgets
  about one mutation per second per object (GCS), sustained arrivals above that budget queue up to
  their deadlines; combining is the answer there and is placed as a follow-up in
  [what this does not do](#placement-of-what-this-does-not-do).
- Serializing the open plane's writes with the mount plane's. See
  [the open plane](#the-open-plane-stays-out).
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`).
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

Two halves, in this order.

**Half 1, the hot-key lane.** A facility of `CasRequests`: a `CasRequests` may be given a
predicate naming its hot keys, and every write verb it admits on such a key takes a FIFO ticket
and runs only when its ticket is at the front; a waiter leaves on its own fence or deadline. The
lane remembers the last object this `CasRequests` committed, so a `readModifyWrite` can start from
it instead of a `GET`. A `decide`'s refusal is reported only from a proven base. `Pool` gives the
predicate to `mount_requests` only, naming `refCatalogKey()`; the catalog's API, `CasRefLedger`
and GC do not change, and `casUpdateImpl` changes only how it carries its own markers.

**Half 2, conflict pacing.** In `readModifyWrite` and `readModifyWriteOnPresence`, a clean
refused precondition is repaid after `Retry::conflictBackoff()`, a flat uniform(0, 200 ms), and no
longer advances the transport-fault counter. The GC erase loop keeps its pacing. Two sentences of
the backend request contract change.

## Half 1: the hot-key lane {#half-1-the-hot-key-lane}

### Ownership and placement {#ownership-and-placement}

`Backend/CasHotKeys.{h,cpp}`, in the engine's own directory, owned by value by `CasRequests`:

```cpp
class CasHotKeys
{
public:
    using IsHot = std::function<bool(const String & key)>;
    explicit CasHotKeys(IsHot is_hot);   /// an empty predicate: nothing is hot
    bool isHot(const String & key) const;
private:
    friend class CasOperation;
    struct Lane    /// one hot key
    {
        std::deque<uint64_t> tickets;                /// guarded by `mutex`; front = the holder or the next holder
        bool holder_active = false;                  /// guarded by `mutex`
        uint64_t holder_worst_case_ms = 0;           /// guarded by `mutex`; the holder's deadline plus one attempt
        std::condition_variable cv;
        std::optional<Object> remembered;            /// guarded by `mutex`; the last object this plane committed
        uint64_t next_ticket = 0;                    /// guarded by `mutex`
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
};
```

`CasRequests`'s constructor gains an optional `CasHotKeys::IsHot`. Every existing constructor call
(every test, the offline tools, the pool factory's local bootstrap `CasRequests`) passes none and
behaves byte-for-byte as today. `CasRequests` remains a class that writes no member after
construction: the lane has its own mutex, and `CasOperation` reaches it through its owner.

A lane exists while it has a ticket, an active holder, or a memory, and is erased when the last of
the three goes. There is no eviction and no byte budget: the predicate names the hot keys, today
exactly one, and its object is under 1 MiB. A bound becomes necessary when a per-namespace key such
as `_ckpt` is declared hot; that declaration brings it.

`Pool` passes the predicate "`key == Layout(config.pool_prefix).refCatalogKey()`" to
`mount_requests` and to no other plane.

### The open plane stays out {#the-open-plane-stays-out}

The GC erase runs on `openRequests()` under a `Liveness` closure that returns a cached flag, and
the reconciler refreshes that flag by one `gc/state` read at the top of every erase attempt,
immediately before the `replace`. That refresh is the authority argument of the GC drain: the gap
between it and the `PUT` is a few instructions. A ticket would put a wait in that gap, and a leader
deposed during the wait would still erase when its turn came, because the engine's gate consults
the cached flag, not the store. Mount-plane writers have no such gap: their fence is three atomics
the engine re-reads before every attempt, so a wait cannot stale it.

So the open plane has no lane. Consequences, all of them today's behaviour:

- A GC erase can overlap one mount-plane write; the loser's precondition is refused and settled by
  its resolve read, as today.
- A GC erase that wins leaves the mount lane's memory stale; the next mount-plane
  `readModifyWrite` pays one 412 and one resolve read for it, then proceeds. GC erases are rare
  (one per removed namespace per round).
- The farewell plane writes no hot key.

`initializeEmptyForNewPool` runs on the pool factory's local bootstrap `CasRequests`, before the
`Pool` exists; it has no hot keys and seeds nothing. The first mount-plane `readModifyWrite` reads.

### Which keys, and the participation contract {#which-keys-and-the-participation-contract}

Participation is by predicate, decided by the owner of the `CasRequests`, never by the call site.

What memory is and is not. `remembered` is the last object this plane committed, or the last
object the engine's resolve read saw after a lost race. Either was the durable state at some
moment. Neither is an observation of this call, and neither is guaranteed newer than an
observation a caller made outside the lane: `dropNamespaceImpl` reads the catalog fresh, sees a
`Live` row another server created, and calls `beginRemoving` on it; the lane's memory may predate
that row. A `decide` that refuses on such a base (the catalog's markers, or a `nullopt`) would be
reporting a verdict about a state the store has left, and with no write sent nothing would catch
it.

The rule that closes it: **a `nullopt` from `decide` is reported as `Declined` only when it was
rendered on an observation of this call**, a `GET` or the resolve read after a `Conflict`. A
`nullopt` rendered on memory is not a result: the memory is cleared, the key is observed, and
`decide` runs again on the observation; what it renders then is the result. A contribution (a
`decide` that returns bytes) needs no such rule: the conditional `PUT` validates the base it was
decided on, as today, and a refused precondition is settled by the resolve read, as today.

**An exception from `decide` propagates unchanged, whatever the base, exactly as today.** The
engine cannot tell a verdict thrown as control flow from a fault (an allocation failure, a
cancellation, a nested read's error), and re-running `decide` after a fault could land a write
where today the exception ends the call with nothing sent. So the engine discards only `nullopt`,
and a site whose verdicts are exceptions must carry them as `nullopt` itself. The catalog does:
`casUpdateImpl`'s `decide` catches its own verdict exceptions, the four markers
(`CatalogFenceMovedMarker`, `CatalogEntryMismatchMarker`, `CatalogCreatorStillLiveMarker`,
`CatalogEntryAlreadyPresentMarker`) and the admission refusal (`LIMIT_EXCEEDED` from
`checkCatalogAdmission`), stores the `exception_ptr` in a local that every run of `decide` first
clears, and returns `nullopt`. After `readModifyWrite` returns `Declined`, `casUpdateImpl` rethrows
the captured exception, and its callers catch the same types at the same places as today; a
`Declined` with nothing captured stays the `LOGICAL_ERROR` it is today. Every other exception,
decode corruption and `identityPreserving`'s `LOGICAL_ERROR` among them, propagates from wherever
it is thrown. This is the one change inside `CasRefCatalog.cpp`, and no signature moves.

With that rule the safety of the lane does not depend on what a `decide` does with `current`. The
contract a participating site must meet is mechanical:

1. `decide` tolerates being run again on a different `current`, which `readModifyWrite` already
   requires of it (it re-decides on every `Conflict`), and tolerates a `nullopt` it rendered on
   memory being discarded and re-rendered. A verdict it wants re-rendered on stale memory is a
   `nullopt`, never a throw.
2. `decide` issues no write on any hot key of the same plane. A write from inside the holder
   would wait for the holder, and two holders entering each other's keys would wait forever. Reads
   are fine, as today, and a `decide` that reads holds the lane for that read (see
   [effects to name](#effects-to-name)).
3. No operation that writes the hot key carries a `Liveness` closure whose truth a wait could
   stale. The catalog writers (`resolveNamespaceLife`'s and `dropNamespaceImpl`'s operations) carry
   none. Other mount-plane operations do: the ref-log append lane resumes under closures that read
   its runtime (`CasRefLedger.cpp`, the `mount_requests.resume(..., [&rt] ...)` sites), so the rule
   is per hot key, not per plane, and declaring a key hot means auditing the closures of every
   operation that writes it.

Today `refCatalogKey()` on the mount plane satisfies all three. The `_ckpt` keys are the next
candidate, are written by operations that do carry closures, and are not declared by this design.

### The ticket {#the-ticket}

The lane is a FIFO ticket with a deadline. Every write verb on a hot key, `create`, `replace`,
`remove`, `removeCurrent`, `readModifyWrite` and `readModifyWriteOnPresence`, does the following
around its existing body. Nothing runs under `mutex` except the steps marked.

**Enter.** The caller binds its policy to an absolute deadline on the engine's clock
(`policy.bind(now)`), as every verb does today at entry, so time spent waiting spends the caller's
own window. Under `mutex`: push `next_ticket` at the back of `tickets` (the one allocation of the
lane), then increment `next_ticket`. If the push throws, the number is not consumed, a lane the
lookup created for this call is erased again, and the exception propagates. The moment the push
succeeds, a ticket guard is installed: on every exit from here on, normal or by unwinding (the gate
calls `std::function`s, `wait_for` may throw), it erases exactly this ticket from `tickets` (a
`std::deque::erase` by value, no allocation), clears `holder_active` if this ticket was the holder,
and does `cv.notify_all()`. It is `noexcept`. The leave step below is this guard running on the
normal path.

**Wait.** Loop, in this order:

1. Outside `mutex`: `gate(0)` on the caller's own operation, then the caller's own bound against
   the engine's clock, in that order so a lost fence or an exhausted lease is reported as itself
   and not as a policy deadline. `FenceLost`: under `mutex`, erase the ticket (no allocation),
   return `GaveUp{FenceLost}`. `NoBudget`: erase, return `GaveUp{Deadline, Lease}`. Bound passed:
   erase, return `GaveUp{Deadline}` with the bound's own source. The gate runs outside the lane
   mutex because it may call the operation's `Liveness` closure, and the lane mutex must stay a
   leaf that calls nothing.
2. Under `mutex`: if the ticket is at the front and `holder_active` is false, set
   `holder_active = true`, record the holder's own worst case (its bound's deadline plus one
   attempt reservation) as `holder_worst_case_ms`, and leave the loop as the holder. Otherwise, if
   the engine's clock is past `holder_worst_case_ms`, the lane is wedged (below): erase this
   ticket, clear the memory, and leave the loop un-ticketed. Otherwise `cv.wait_for(lock, slice)`
   with a short slice (the pattern of `recovery_cv.wait_for(lock, 200ms)` in the ledger), release
   the mutex, and go to step 1.

A ticket is promoted only after its checks passed in the same iteration. The engine's own
per-attempt gate and `fits` still run before anything is sent, as today.

**Hold.** Run the verb's body on the caller's own thread, with the caller's own operation, policy,
bound and result type, exactly as today, plus the memory step below.

**Leave.** The ticket guard, on every exit: under `mutex`, move the prepared memory value into
`remembered` or clear it (a holder that unwinds clears), erase this ticket, set
`holder_active = false` if it was the holder, `cv.notify_all()`. It is `noexcept` and allocates
nothing: the memory value is prepared before the guard runs (below), and erasing from a
`std::deque` does not allocate.

A holder never waits for another ticket and never runs anyone else's closure: a ticket is settled
only by its own caller. No ticket outlives its caller's stack in the queue: the guard removes it on
every exit path, and nothing else references it.

### The memory {#the-memory}

**Memory rule.** After any write verb on a hot key ends: `Committed` remembers the bytes that verb
wrote and the etag it got; a `Conflict` whose resolve read saw an `Object` remembers that object;
`Removed`, `Gone`, `Mismatch`, `Refused`, `GaveUp`, `Declined` and every exception forget. Those are
the only two sources; an ordinary `read` never reads or writes the memory, and absence is never
remembered. A `GaveUp{FenceLost}` after a landed `PUT` also forgets: the memory is a hint, and
forgetting is the fail-close direction.

**Preparing the value.** The `(bytes, etag)` pair is assembled outside the mutex, before the leave
guard, and never by moving from the `WriteResult` the caller receives: a `Committed`'s etag is
copied, a `Conflict`'s memory is copied from the engine's own `WriteState::last_seen` and the
returned `Conflict::seen` stays intact, `GaveUp::last_seen` is never sourced from memory. The bytes
are the engine's own candidate string for `readModifyWrite` (moved, since the engine owns it) and a
copy of the caller's bytes for `create` and `replace`, taken before the attempt is sent (a copy that
fails is a plain exception before any attempt, as an allocation in the verb's body is today). If the
value cannot be assembled, the guard forgets. The guard only moves.

**Where memory is used.** Only `readModifyWrite`. Its body gains one optional input, the
remembered object, used in place of its initial observation:

1. Base := `remembered` if present, else `observe` as today. A memory start is gated exactly like
   the observation it replaces: `gate(reservedFor(0, 1))`, one attempt envelope, and
   `fits(reservedFor(0, 1), bound)`, the same two checks the read loop makes before its first
   attempt, with the same three-way outcome. A caller past its deadline, its lease or its fence
   never runs `decide` on memory, exactly as it would not have been allowed to observe.
2. `decide(base)`. Bytes: proceed to the inner write against the base's etag, as today. A
   `nullopt` on an observed base: `Declined`, as today. A `nullopt` on memory: clear the memory,
   `observe`, `decide` again on the observation, and return what that renders. This restart happens
   at most once per call, and the first `nullopt` is discarded without being seen by anyone: it was
   rendered on a hint. An exception from `decide` propagates, whatever the base.
3. The inner write and the `Conflict` loop are today's, with the pause of Half 2. A `Conflict`'s
   resolve read is an observation, so what `decide` renders on it is a result.

`create`, `replace`, `remove`, `removeCurrent` and `readModifyWriteOnPresence` never start from
memory. They take a ticket, run as today, and feed the memory rule.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait. Every catalog write already runs under `Retry::standard()`, 90 s
from the call, or a lease bound if shorter. The lane binds the policy at entry, before queueing, so
time in the queue spends the same window rather than adding to it, which is what today's backoff
sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own gate and its own deadline, re-checked every slice | its window plus one slice |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it | its window, plus one attempt timeout, plus the windows of reads its own `decide` issues |

Nothing copies a verdict from one operation to another: every `GaveUp` is the reporting operation's
own, with its own `Source`. A frozen policy (`op.freeze`) and a lease-bound policy
(`untilLeaseSafe`) behave as today: the bound the caller brought is the bound the wait and the
write are checked against.

A stopping plane or a lost fence reaches waiters through their own gate, checked every slice, and
reaches the holder through its own attempts' gates, as today.

**A stalled holder, and the wedge escape.** The engine calls the transport synchronously and
cannot cancel it. A stalled attempt ends at the backend's attempt timeout, which the engine reserves
per attempt; an attempt that trickles rather than stalls has no bound, today or after the backend
request contract, and today it traps only its own caller. In the lane it would trap the key. So the
lane gives exclusivity up before it gives liveness up: the holder's own worst case, its bound's
deadline plus one attempt reservation, is recorded when it takes the ticket, and a waiter that finds
the engine's clock past it treats the lane as wedged. It erases its ticket, clears the memory, and
runs its verb un-ticketed, exactly as on a cold key: a fresh observation, its own conditional
`PUT`. This is safe because exclusivity was never the safety argument; the precondition is. The
stuck write may still land later, or first, and whichever of the two loses its precondition is
settled by its resolve read, as any two servers' writes are. When the stuck holder finally returns,
its guard erases its ticket and clears `holder_active`, and the lane is a lane again. Nothing is
forced and nothing is cancelled; the degraded mode is today's behaviour, entered only after a
holder has exceeded the longest time the engine could have let it run.

### Lock order {#lock-order}

Verified for the two mount-plane production writers at brainstorm time:

| caller | locks held on entry to the catalog write |
|---|---|
| `CasRefLedger::namespaceLife` → `resolveNamespaceLife` (CREATE) | none; `ref_queue_mutex` scope closes before, `state_mutex` is taken after |
| `CasRefLedger::dropNamespaceImpl` → `cancelStalledCreating`, `beginRemoving` (DROP) | none; the queue lock scope closes before `removal_op` |

Inside a write: the mount fence's `admit`, `generation` and `check_or_throw` read atomics only;
the in-memory backend holds its mutex only for the duration of one operation and runs hooks
without it; the mount plane's sleep is `CasMountRuntime::sleepInterruptibly`, a leaf; `decide`s
call `op.admitted()` (atomics) and issue reads (backend leaf). No `decide` takes a ledger mutex.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held only to push, inspect, pop or erase a
ticket, to flip `holder_active`, and to move or clear `remembered`. The gate, the `Liveness`
closure, the verb's body and every `decide` run with it released. The order is therefore: caller
(no locks) → `CasHotKeys::mutex` → nothing; and separately caller (no locks) → backend mutex. The
one rule this imposes on future callers, stated where the catalog API is documented: a catalog
mutation is not entered while holding a ledger mutex, because the holder's `decide` may take
ledger-visible reads and a waiter must not hold what the holder needs. Today's callers hold nothing.

### Invariants {#invariants}

- INV-HK1. Per `CasRequests` and hot key, at most one conditional write is in flight at any
  moment. The mount plane is the only plane with hot keys, so per process at most one mount-plane
  write is in flight per hot key; an open-plane write may overlap it, as today.
- INV-HK2. Writes of one plane to a hot key are applied in arrival order.
- INV-HK3. A `readModifyWrite` that follows a `Committed` of this plane on a hot key, with no
  other ending in between, obtains its base from memory and issues no read for it. A lost race's
  resolve read is not a base read and is unchanged.
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair that was durable at
  some moment, taken from a `Committed` of this plane or from the engine's resolve read. It never
  changes the semantics of a conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A `nullopt` from `decide` reaches its caller as `Declined` only when it was rendered on
  an observation of that call; an exception from `decide` reaches its caller unchanged, whatever
  the base, and no write is sent after it.
- INV-HK6. A ticket is removed only by its own caller, on every exit path; no ticket references a
  stack that may have unwound.
- INV-HK7. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`, and the leave
  guard neither allocates nor throws.
- INV-HK8. A waiter is promoted to holder only in an iteration whose gate and bound checks passed,
  and a memory start passes the same one-attempt gate and `fits` an observation would.
- INV-HK9. A holder holds exclusivity for at most its own worst case (deadline plus one attempt
  reservation); past it, waiters write un-ticketed and the precondition alone orders the writes.

### Effects to name {#effects-to-name}

- A `decide` that issues reads holds the lane for those reads. `reconcileStaleCreator` and
  `cancelStalledCreating` call `isCreatorFenceTerminal` inside `decide`, and that reads the mount
  key under a fresh `Retry::standard()` on the caller's operation. Today that window is the
  caller's own; in the lane it is also every waiter's, each of which still leaves on its own
  deadline. Passing a frozen policy into `isCreatorFenceTerminal` would bound it and is a follow-up
  outside this design.
- A verdict rendered on memory costs one `GET` before the real verdict, which is what every
  `readModifyWrite` costs today.
- The `Committed` a caller receives is its own write's, with its own `attempts_sent`.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

The ref-log append lane is flat combining: items with `done`/`error`, a leader that carves
compatible items into one transaction, an exit guard that completes owned items. The hot-key lane
is deliberately less: a ticket lock with a deadline and one remembered object. It shares the mutex
discipline (never held across the durable write) and the exit-guard discipline (every exit path
releases), and nothing else. Combining is what the reviews showed cannot be done generically over
closures; if it returns, it returns as typed catalog deltas and will resemble `appendRefOps` more
closely.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff()` returns `backoff(1)`, uniform(0, 200 ms) (`backoff` doubles from
attempt 2 on), so the number has one home next to the schedule it departs from.

- `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`: on a `Conflict` whose inner
  write had no ambiguous attempt (a clean refused precondition, settled by the resolve read),
  instead of `pauseAndReissue` (which sleeps `Retry::backoff(++state.reissues)`), a
  `pauseForConflict` that performs the same `gate` and `fits` checks with `conflictBackoff()` and
  leaves `state.reissues` untouched. It records a new profile event, `CASRequestConflictPause`,
  and not `CASRequestReissue`; in these two loops the reissue counter then counts transport
  reissues only (`removeCurrent` and the read loops keep recording it as they do today, so its
  global description stays "reissues, contention among them"). A test tells the two schedules
  apart by the counters, not by sampling the jitter.
- A `Conflict` whose inner write had an ambiguous attempt (`state.any_ambiguous`: a transport
  fault, a 429 among them, that the resolve read then settled as a lost race because the key had
  moved) keeps `pauseAndReissue` and its growing schedule. The race was lost, but the fault is the
  signal that must pace the loop, and `writeLoop` already carries the distinction.
- `CasRefCatalog::deleteCompletedRemovingAtSnapshot` keeps `op.pause(Retry::backoff(attempt + 1))`.
  Its `replace` returns a `Conflict` that carries no provenance, so the loop cannot tell a clean lost
  race from one that settled a fault; the loop is rare (one erase per removed namespace per round)
  and the growing schedule is the safe choice there.
- Transport faults and unresolved-but-repeatable attempts keep `pauseAndReissue` unchanged.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. After Half 1, conflicts
arise only between servers and against the open plane's rare erases, and there a loss is dearer
than before, because the losing plane's whole lane waits behind its holder.

What this does not claim about a store's per-object write budget. GCS documents about one
mutation per second per object name, says excess writes may be throttled, and asks applications
not to exceed the rate. A 429 is a transport fault and takes the growing schedule, but a lane that
drains successful holders at `PUT` latency is not paced by 429s it does not receive, and today's
code is not either. The flat jitter changes nothing about the drain rate; the drain rate is a
property of serialization. Holding the budget by design on the `Generation` dialect needs per-key
pacing of successive holds, which belongs with combining (one `PUT` per second is enough only when
it carries every mutation that arrived meanwhile) and is placed with it below. Until then the
lane's acceptance on GCS is a measurement, not a claim: the `gcs` lane's 412 and 429 counts on
`ref_catalog` before and after, recorded in `docs/superpowers/cas/BACKLOG/gcs.md`.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a clean lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff()`, a flat uniform(0, 200 ms) that does not grow with the writer's loss
   count. The growing schedule belongs to transport faults, and to a conflict that settled one; a
   store's per-object rate limit (GCS, about one mutation per second, answered as 429 when it
   throttles) reaches the loop that way, and is not otherwise held by this loop. Inside one plane a
   hot key never conflicts with itself: see the hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a
   settled fault, `backoff(attempt)` otherwise and after a fault; `deleteCompletedRemoving`'s
   `replace` cannot tell and keeps `backoff(attempt)`".
3. In the `readModifyWrite` section, one paragraph: on a hot key the initial observation may be
   the lane's remembered object, gated like an observation, and a verdict rendered on it is
   re-rendered on an observation before it is reported; pointer to this document.

## Observability {#observability}

Three profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to hold, per ticket.
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a `readModifyWrite` obtained its base.
- `CASHotKeyVerdictRestarts`: verdicts rendered on memory and re-rendered on an observation.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Engine level, in `gtest_cas_requests.cpp`'s harness: counting backend, write hooks, and a clock.
The harness's `FakeClock` is single-threaded; the multi-threaded cases below use a synchronized
clock (a mutex-guarded `now` advanced by the test thread, sleeps recorded under the same mutex)
added for them, and a case that asserts nothing about time may use the real clock. The
`CasRequests` under test is built with a predicate naming the test key. Each `decide` in these
tests decodes a list of tickets from `bytes` and appends its own, so order is visible in the object.

1. Serialization and memory. N threads call `readModifyWrite` on the hot key; a write hook parks
   the first holder until all N are waiting. Assert: reads of the key 1, writes N, no write
   returned `Conflict`, the final object lists the tickets in arrival order (INV-HK1, HK2, HK3).
   A sibling mixes the six verbs (`create` on a fresh key, `replace`, `remove`, `removeCurrent`,
   `readModifyWriteOnPresence`, `readModifyWrite`) from N threads and asserts, through a backend
   hook that records overlapping calls, that no two writes on the key were ever in flight together.
2. Verdict on stale memory. The plane commits once (memory set). A second `CasRequests` over the
   same backend writes a row. A caller reads the key fresh outside the lane and runs a
   `beginRemoving`-shaped `decide` (`nullopt` unless the row equals what it observed, the way
   `casUpdateImpl` carries its marker). Assert: `Committed`, exactly one read by the lane, the row
   transitioned, and the discarded first `nullopt` is visible only as `CASHotKeyVerdictRestarts`
   (INV-HK5). A catalog-level sibling in `gtest_cas_ref_catalog.cpp` drives the real
   `beginRemoving` through a hot `CasRequests` and asserts `Transitioned`, not `EntryChanged`.
3. Verdict on an observation. Same shape, but the row really changed. Assert: `Declined` with
   `seen` intact, one read, no write; and at the catalog level, `EntryChanged` with one read and no
   write, the same as today.
4. External writer on the write path. A second `CasRequests` writes between two writes of the
   first. Assert the first's next `readModifyWrite` does exactly one resolve read and one retry
   write and ends `Committed`, and the write after that starts from memory with zero reads. A
   sibling makes the resolve read the call's last act (a `once` policy): the returned
   `Conflict::seen` carries the object, and the next `readModifyWrite` starts from memory with zero
   reads (INV-HK4, the conflict source, with the caller's observation intact).
5. Forgetting. `Refused` from a hook, a thrown `writeLoop`, a `remove`, and a `GaveUp{FenceLost}`
   after a landed `PUT` each clear the memory: the next write reads first.
6. Waiters leave on their own. While the holder is parked inside its `decide`'s nested read: a
   waiter whose fake-clock deadline passes leaves with `GaveUp{Deadline}` carrying its own source
   and sends nothing; a waiter whose fence is tripped leaves with `GaveUp{FenceLost}`; a waiter
   whose lease budget runs out leaves with `GaveUp{Deadline, Lease}`; a waiter whose fence and
   deadline both fail reports `FenceLost`. The ticket is gone from the lane in all four cases
   (INV-HK6).
7. Expiry at handover. A waiter's deadline passes in the same slice in which the holder leaves.
   Assert the waiter never becomes holder, never runs `decide`, reads nothing, and leaves with
   `GaveUp{Deadline}` (INV-HK8, the first half). A holder-to-be whose remaining window is smaller
   than one attempt reservation, with memory present, never runs `decide` on it and reports the
   same outcome an observation would (INV-HK8, the second half), under a plain, a frozen and a
   lease-bound policy.
7a. Exceptional exits. A `Liveness` closure that throws from the gate, and a `decide` that throws a
   non-verdict exception on memory. Assert: the exception reaches the caller unchanged, no write was
   sent for the second, the ticket is gone, and the next waiter is promoted (INV-HK6).
7b. Wedge escape. The holder is parked in a `PUT` that never returns; the clock is advanced past
   the holder's worst case. Assert: the next waiter proceeds un-ticketed with a fresh read and lands;
   when the parked `PUT` is released it is refused and settled by its resolve read; the lane is
   exclusive again afterwards (a third writer waits for a fourth).
8. Open plane overlaps. A second `CasRequests` with no predicate (the open-plane shape) does a
   `replace` while the hot plane's holder is parked; it does not wait; whichever write is refused is
   settled by its resolve read; after an open-plane win the hot plane's next `readModifyWrite` pays
   exactly one resolve read and one retry write.
9. Half 2, clean conflict. One writer loses K clean races in a row (a hook mutates the key before
   every attempt). Assert `CASRequestConflictPause` advanced K times, `CASRequestReissue` not at
   all, and every recorded sleep is at most 200 ms.
10. Half 2, conflict after a fault. A hook makes the attempt fail with a 429-class transport error
    and then moves the key before the resolve read, K times. Assert `CASRequestReissue` advanced
    K times and `CASRequestConflictPause` not at all. A sibling with K plain transport faults and
    no external write sees the same counters.

Pool level, through the ledger:

11. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, and exactly one lane base read (`CASHotKeyReadStarts == 1`). The callers'
    own pre-check reads are outside the lane and are not counted.
12. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Not tested: an allocation failure while preparing the memory value (no injection point; the
design's prepare-before-guard order and forget-on-failure are the argument).

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
without a hot predicate, and must stay green. That is the check that the catalog API was not
touched and that the marker transport inside `casUpdateImpl` preserves every outcome: the existing
tests already cover each marker and the admission refusal against a cold key.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Combining catalog mutations into one `PUT`: a BACKLOG item, with the constraints the reviews
  established. It must be typed catalog deltas inside `CasRefCatalog`, not closures in the engine;
  each delta conditioned on an exact row so that a later delta cannot silently rewrite an earlier
  one's effect; admission evaluated on the final candidate, never on a prefix; a verdict delivered
  only after the batch that rendered it committed, or re-rendered on an observation; every member
  gated on its own fence before its delta is applied and after the commit; membership frozen before
  the write; and, on the `Generation` dialect, per-key pacing of successive holds to the store's
  documented one mutation per second per object, which is sufficient only once one `PUT` carries
  every mutation that arrived meanwhile. Motivation: exactly that store.
- Ticketing the GC erase: a BACKLOG note. It needs the reconciler's authority refresh to run after
  the ticket is held and before the `PUT`, which is a hook the engine does not offer; the benefit
  is one avoided 412 per GC erase.
- Declaring the `_ckpt` keys hot, with the memory bound that many hot keys need: a BACKLOG item.
- Bounding the reads a catalog `decide` issues (`isCreatorFenceTerminal` under a frozen policy): a
  BACKLOG item.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.
