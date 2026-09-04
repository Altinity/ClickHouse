---
description: 'Design for serializing one process''s conditional writes to a hot CAS control object (the ref catalog first) inside the request engine, remembering the last committed object so consecutive writes need no read, reporting a decide''s refusal only from a proven base, and pacing lost races between servers with a flat jitter instead of the transport-fault backoff. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 3. Brainstormed 2026-09-04 against the measurements in
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
- Revision 3 drops combining. What remains is small: a per-key FIFO ticket with a deadline, one
  remembered object, one rule for verdicts, and a flat conflict jitter. Combining is placed as a
  follow-up with the constraints the reviews established.

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

- Inside one process, at most one conditional write in flight per hot key, in arrival order.
- A `readModifyWrite` that follows a commit of this process starts from the committed object, not
  from a `GET`. N concurrent catalog mutations of one process cost N sequential `PUT`s, at most one
  `GET`, and no 412 against each other.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, or to any read path. Every existing catalog test stays
  green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- A general write cache for every key. Participation is opt-in, per key, by the owner.
- Combining several writers' mutations into one `PUT`. On S3 the lane's throughput is one `PUT`
  latency per write, an order of magnitude above this lane's arrival rate. On a store that budgets
  about one mutation per second per object (GCS), sustained arrivals above that budget queue up to
  their deadlines; combining is the answer there and is placed as a follow-up in
  [what this does not do](#placement-of-what-this-does-not-do).
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`).
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

Two halves, in this order.

**Half 1, the hot-key lane.** A per-pool component of the request engine, `CasHotKeys`, owned by
`Pool` and shared by its three `CasRequests` planes. For a key the owner declares hot, every write
verb takes a FIFO ticket for that key and runs only when its ticket is at the front; a waiter
leaves on its own deadline or fence. The lane remembers the last object this process committed, so
a `readModifyWrite` can start from it instead of a `GET`. A `decide`'s refusal is reported only
from a proven base. `Pool` declares `refCatalogKey()` hot; nothing in `CasRefCatalog`,
`CasRefLedger` or GC changes.

**Half 2, conflict pacing.** In `readModifyWrite`, `readModifyWriteOnPresence` and the GC erase
loop, a `Conflict` is repaid after `Retry::conflictBackoff()`, a flat uniform(0, 200 ms), and no
longer advances the transport-fault counter. Two sentences of the backend request contract change.

## Half 1: the hot-key lane {#half-1-the-hot-key-lane}

### Ownership and placement {#ownership-and-placement}

`Backend/CasHotKeys.{h,cpp}`, in the engine's own directory, because it is a property of how the
engine talks to one key, not of any caller. One instance per pool.

```cpp
class CasHotKeys
{
public:
    using IsHot = std::function<bool(const String & key)>;
    explicit CasHotKeys(IsHot is_hot);
    bool isHot(const String & key) const;
private:
    friend class CasOperation;
    struct Lane    /// one hot key
    {
        std::deque<uint64_t> tickets;                /// guarded by `mutex`; front = the holder or the next holder
        bool holder_active = false;                  /// guarded by `mutex`
        std::condition_variable cv;
        std::optional<Object> remembered;            /// guarded by `mutex`; the last object THIS process committed
        uint64_t next_ticket = 0;                    /// guarded by `mutex`
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
};
```

A lane exists while it has a ticket, an active holder, or a memory, and is erased when the last of
the three goes. There is no eviction and no byte budget: the owner's predicate names the hot keys,
today exactly one per pool, and its object is under 1 MiB. A bound becomes necessary when a
per-namespace key such as `_ckpt` is declared hot; that declaration brings it.

`CasRequests` takes a `std::shared_ptr<CasHotKeys>` in its constructor. A `CasRequests` built
without one (every existing test, the offline tools, the pool factory's local bootstrap
`CasRequests`) owns a private instance whose predicate is "nothing is hot", so its behaviour is
byte-for-byte today's. `CasRequests` remains a class that writes no member after construction: the
lane has its own mutex.

`Pool` declares `std::shared_ptr<CasHotKeys> hot_keys` before `mount_requests`,
`farewell_requests` and `gc_requests`, so it is constructed before them and destroyed after them,
and hands the same pointer to all three. Its predicate is
"`key == Layout(config.pool_prefix).refCatalogKey()`", computed once at construction.

### Which keys, and the participation contract {#which-keys-and-the-participation-contract}

Participation is by predicate, decided by the owner of the table, never by the call site.

What memory is and is not. `remembered` is the last object this process committed, or the last
object the engine's resolve read saw after a lost race. Either was the durable state at some
moment. Neither is an observation of this call, and neither is guaranteed newer than an
observation a caller made outside the lane: `dropNamespaceImpl` reads the catalog fresh, sees a
`Live` row another server created, and calls `beginRemoving` on it; the lane's memory may predate
that row. A `decide` that refuses on such a base (the catalog's markers, or a `nullopt`) would be
reporting a verdict about a state the store has left, and with no write sent nothing would catch
it.

The rule that closes it: **a verdict (a `decide` that returns `nullopt` or throws) is reported to
its caller only when it was rendered on an observation of this call**, a `GET` or the resolve read
after a `Conflict`. A verdict rendered on memory is not a result: the memory is cleared, the key is
observed, and `decide` runs again on the observation; what it renders then is the result. A
contribution (a `decide` that returns bytes) needs no such rule: the conditional `PUT` validates the
base it was decided on, as today, and a refused precondition is settled by the resolve read, as
today.

With that rule the safety of the lane does not depend on what a `decide` does with `current`. The
contract a participating site must meet is mechanical:

1. `decide` tolerates being run again on a different `current`, which `readModifyWrite` already
   requires of it (it re-decides on every `Conflict`), and tolerates a verdict it rendered on memory
   being discarded and re-rendered.
2. `decide` issues no write on the same key. A write from inside the holder would wait for the
   holder. Reads are fine, as today, and a `decide` that reads holds the lane for that read (see
   [effects to name](#effects-to-name)).

Today `refCatalogKey()` satisfies both. The `_ckpt` keys are the next candidate and are not
declared by this design.

### The ticket {#the-ticket}

The lane is a FIFO ticket with a deadline. Every write verb on a hot key does the following around
its existing body; nothing runs under `mutex` except the four steps marked.

**Enter.** The caller binds its policy to an absolute deadline on the engine's clock
(`policy.bind(now)`), as every verb does today at entry, so time spent waiting spends the caller's
own window. Under `mutex`: take `next_ticket++`, push it at the back of `tickets` (the one
allocation of the lane; a failure here leaves the lane untouched and propagates).

**Wait.** Loop:

- Under `mutex`: if the ticket is at the front and `holder_active` is false, set
  `holder_active = true` and leave the loop as the holder. Otherwise `cv.wait_for(lock, slice)`
  with a short slice (the pattern of `recovery_cv.wait_for(lock, 200ms)` in the ledger), then
  release the mutex.
- Outside `mutex`: check the caller's own bound against the engine's clock, and `gate(0)` on the
  caller's own operation. The gate is evaluated outside the lane mutex because it calls the
  operation's `Liveness` closure, and the lane mutex must stay a leaf that calls nothing. If the
  bound has passed: under `mutex`, erase the ticket (no allocation), then return
  `GaveUp{Deadline}` with the bound's own source. If the gate is `FenceLost`: erase, return
  `GaveUp{FenceLost}`; `NoBudget`: erase, return `GaveUp{Deadline, Lease}`, the engine's own
  three-way mapping.

**Hold.** Run the verb's body on the caller's own thread, with the caller's own operation, policy,
bound and result type, exactly as today, plus the memory step below. Every attempt still passes the
operation's own gate and bound before it is sent, as today.

**Leave.** A guard on every exit, normal or exceptional, that under `mutex` pops the front ticket,
sets `holder_active = false`, applies the memory rule, and does `cv.notify_all()`. Nothing in it
allocates or throws.

A holder never waits for another ticket and never runs anyone else's closure: a ticket is settled
only by its own caller. No ticket outlives its caller's stack in the queue: the caller removes it on
every exit path, and nothing else references it.

### The memory {#the-memory}

**Memory rule.** After any write verb on a hot key ends, in the leave guard: `Committed`
remembers the bytes that verb wrote and the etag it got; a `Conflict` whose resolve read saw an
`Object` remembers that object; `Removed`, `Gone`, `Mismatch`, `Refused`, `GaveUp`, `Declined` and
every exception forget. Those are the only two sources; an ordinary `read` never reads or writes
the memory, and absence is never remembered. A `GaveUp{FenceLost}` after a landed `PUT` also
forgets: the memory is a hint, and forgetting is the fail-close direction.

**Where memory is used.** Only `readModifyWrite`. Its body gains one optional input, the
remembered object, used in place of its initial observation:

1. Base := `remembered` if present, else `observe` as today.
2. `decide(base)`. Bytes: proceed to the inner write against the base's etag, as today. A verdict
   on an observed base: return it, as today. A verdict on memory: clear the memory, `observe`,
   `decide` again on the observation, and return what that renders. This restart happens at most
   once per call, and the first verdict is discarded without being seen by anyone: it was rendered
   on a hint.
3. The inner write and the `Conflict` loop are today's, with the pause of Half 2. A `Conflict`'s
   resolve read is an observation, so what `decide` renders on it is a result.

`create`, `replace`, `remove` and `readModifyWriteOnPresence` never start from memory. They take a
ticket, run as today, and feed the memory rule.

What this buys the GC erase, and what it does not. `deleteCompletedRemovingAtSnapshot` does a
`replace` against the etag of the round's catalog cut, read by the reconciler earlier, and then its
mandatory resolution read. In the lane that `replace` no longer overlaps another write of this
process, and its win refreshes the memory. Its precondition is still the cut's etag: a mount
writer that committed after the cut refuses it with a 412 exactly as today, and its resolution read
feeds its next attempt exactly as today. The lane does not rewrite that loop and does not claim to
remove that 412.

`initializeEmptyForNewPool` runs on the pool factory's local bootstrap `CasRequests`, before the
`Pool` exists; it has no hot keys and seeds nothing. The first mount-plane `readModifyWrite` reads.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait. Every catalog write already runs under `Retry::standard()`, 90 s
from the call, or a lease bound if shorter. The lane binds the policy at entry, before queueing, so
time in the queue spends the same window rather than adding to it, which is what today's backoff
sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own deadline and its own gate, re-checked every slice | its window plus one slice |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it | its window, plus one attempt timeout, plus the windows of reads its own `decide` issues |

Nothing copies a verdict from one operation to another: every `GaveUp` is the reporting operation's
own, with its own `Source`. A frozen policy (`op.freeze`) and a lease-bound policy
(`untilLeaseSafe`) behave as today: the bound the caller brought is the bound the wait and the
write are checked against.

A stopping plane or a lost fence reaches waiters through their own gate, checked every slice, and
reaches the holder through its own attempts' gates, as today.

### Lock order {#lock-order}

Verified for the three production writers at brainstorm time:

| caller | locks held on entry to the catalog write |
|---|---|
| `CasRefLedger::namespaceLife` → `resolveNamespaceLife` (CREATE) | none; `ref_queue_mutex` scope closes before, `state_mutex` is taken after |
| `CasRefLedger::dropNamespaceImpl` → `cancelStalledCreating`, `beginRemoving` (DROP) | none; the queue lock scope closes before `removal_op` |
| `Gc` → `CatalogLifecycleReconciler` → erase | none; `authority_held` is a plain flag of the round thread |

Inside a write: the mount fence's `admit`, `generation` and `check_or_throw` read atomics only;
the in-memory backend holds its mutex only for the duration of one operation and runs hooks
without it; the mount plane's sleep is `CasMountRuntime::sleepInterruptibly`, a leaf; `decide`s
call `op.admitted()` (atomics) and issue reads (backend leaf). No `decide` takes a ledger mutex.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held only to push, inspect, pop or erase a
ticket, to flip `holder_active`, and to read or write `remembered`. The gate, the `Liveness`
closure, the verb's body and every `decide` run with it released. The order is therefore: caller
(no locks) → `CasHotKeys::mutex` → nothing; and separately caller (no locks) → backend mutex. The
one rule this imposes on future callers, stated where the catalog API is documented: a catalog
mutation is not entered while holding a ledger mutex, because the holder's `decide` may take
ledger-visible reads and a waiter must not hold what the holder needs. Today's callers hold nothing.

### Invariants {#invariants}

- INV-HK1. Per process and hot key, at most one conditional write is in flight at any moment.
- INV-HK2. Writes of one process to a hot key are applied in arrival order.
- INV-HK3. A `readModifyWrite` that follows a `Committed` of this process on a hot key, with no
  other ending in between, obtains its base from memory and issues no read for it. A lost race's
  resolve read is not a base read and is unchanged.
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair that was durable at
  some moment, taken from a `Committed` of this process or from the engine's resolve read. It never
  changes the semantics of a conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A verdict (a `decide` returning `nullopt` or throwing) reaches its caller only when it
  was rendered on an observation of that call.
- INV-HK6. A ticket is removed only by its own caller, on every exit path; no ticket references a
  stack that may have unwound.
- INV-HK7. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`.

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

- `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`: on `Conflict`, instead of
  `pauseAndReissue` (which sleeps `Retry::backoff(++state.reissues)`), a `pauseForConflict` that
  performs the same `gate` and `fits` checks with `conflictBackoff()` and leaves `state.reissues`
  untouched. `recordReissue` is still counted, so the operator's reissue counter keeps today's
  meaning.
- `CasRefCatalog::deleteCompletedRemovingAtSnapshot`: `op.pause(Retry::conflictBackoff())` instead
  of `op.pause(Retry::backoff(attempt + 1))`.
- Transport faults and unresolved-but-repeatable attempts keep `pauseAndReissue` unchanged.

Why flat is right and growing was wrong: a conflict is a lost race the resolve read has already
settled; the writer holds the fresh object and has nothing to wait for except desynchronisation
from its competitors. Growing the pause with the writer's own loss count makes the oldest loser the
slowest and therefore the likeliest to lose again. The spec's motivation for pacing, GCS's budget
of about one mutation per second per object, is served by the transport path: exceeding it answers
429, which is a transport fault and takes the growing schedule. After Half 1, conflicts arise only
between servers, and there a loss is dearer than before, because the losing server's whole lane
waits behind its holder.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff()`, a flat uniform(0, 200 ms) that does not grow with the writer's loss
   count. The growing schedule belongs to transport faults, which is also how a store's per-object
   rate limit (GCS, about one mutation per second, answered as 429) reaches the loop. Inside one
   process a hot key never conflicts with itself: see the hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition, `backoff(attempt)` after a fault".
3. In the `readModifyWrite` section, one paragraph: on a hot key the initial observation may be
   the lane's remembered object, and a verdict rendered on it is re-rendered on an observation
   before it is reported; pointer to this document.

## Observability {#observability}

Three profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to hold, per ticket.
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a `readModifyWrite` obtained its base.
- `CASHotKeyVerdictRestarts`: verdicts rendered on memory and re-rendered on an observation.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Engine level, in `gtest_cas_requests.cpp`'s harness: fake clock, counting backend, write hooks. A
`CasHotKeys` with a predicate naming the test key is passed to the `CasRequests` under test. Each
`decide` in these tests decodes a list of tickets from `bytes` and appends its own, so order is
visible in the object.

1. Serialization and memory. N threads call `readModifyWrite` on the hot key; a write hook parks
   the first holder until all N are waiting. Assert: reads of the key 1, writes N, no write
   returned `Conflict`, the final object lists the tickets in arrival order (INV-HK1, HK2, HK3).
2. Verdict on stale memory. The process commits once (memory set). A second `CasRequests` over the
   same backend writes a row. A caller reads the key fresh outside the lane and runs a
   `beginRemoving`-shaped `decide` (throw unless the row equals what it observed). Assert: no
   exception, exactly one read by the lane, `Committed`, the row transitioned, and the discarded
   first verdict is visible only as `CASHotKeyVerdictRestarts` (INV-HK5).
3. Verdict on an observation. Same shape, but the row really changed. Assert: the exception is
   delivered, one read, no write.
4. External writer on the write path. A second `CasRequests` writes between two writes of the
   first. Assert the first's next `readModifyWrite` does exactly one resolve read and one retry
   write and ends `Committed`, and the write after that starts from memory with zero reads.
5. Forgetting. `Refused` from a hook, a thrown `writeLoop`, and a `GaveUp{FenceLost}` after a
   landed `PUT` each clear the memory: the next write reads first.
6. Deadlines and fences in the queue. A waiter whose fake-clock deadline passes while the holder
   is parked inside its `decide`'s nested read leaves with `GaveUp{Deadline}` carrying its own
   source and sends nothing; a waiter whose fence is tripped leaves with `GaveUp{FenceLost}`; a
   waiter whose lease budget runs out leaves with `GaveUp{Deadline, Lease}`. The ticket is gone
   from the lane in all three cases (INV-HK6).
7. Cross-plane serialization. A `replace` from a second plane sharing the same `CasHotKeys` (the
   GC erase shape) waits for a parked holder, lands, and refreshes the memory: the next
   `readModifyWrite` starts with zero reads. A `remove` clears it.
8. Half 2. One writer loses K races in a row (a hook mutates the key before every attempt).
   Assert every recorded sleep is at most 200 ms and the sum at most K × 200 ms; a sibling test
   injecting K transport faults sees the unchanged growing schedule.

Pool level, through the ledger:

9. N concurrent `createNamespace` calls on one `Pool`: catalog reads bounded by a constant
   independent of N, and zero catalog writes returned `Conflict`.
10. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
without a hot predicate, and must stay green. That is the check that the catalog API was not
touched.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Combining catalog mutations into one `PUT`: a BACKLOG item, with the constraints the reviews
  established. It must be typed catalog deltas inside `CasRefCatalog`, not closures in the engine;
  each delta conditioned on an exact row so that a later delta cannot silently rewrite an earlier
  one's effect; admission evaluated on the final candidate, never on a prefix; a verdict delivered
  only after the batch that rendered it committed, or re-rendered on an observation; every member
  gated on its own fence before its delta is applied and after the commit; membership frozen before
  the write. Motivation: a store that budgets one mutation per second per object.
- Declaring the `_ckpt` keys hot, with the memory bound that many hot keys need: a BACKLOG item.
- Bounding the reads a catalog `decide` issues (`isCreatorFenceTerminal` under a frozen policy): a
  BACKLOG item.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.
