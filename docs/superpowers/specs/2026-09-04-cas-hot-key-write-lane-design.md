---
description: 'Design for serializing and combining one process''s conditional writes to a hot CAS control object (the ref catalog first) inside the request engine, remembering the last committed object so consecutive writes need no read, and pacing lost races between servers with a flat jitter instead of the transport-fault backoff. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 2. Brainstormed 2026-09-04 against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the two BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`. This document
supersedes the fix sketch in those items where they differ (the door lives in the engine, not in
`CasRefCatalog`; writes are combined, not only serialized).

Revision 2 folds in the first review round. Revision 1 claimed that the remembered object is at
least as fresh as anything a caller observed; it is not, because another server can write after this
process's last commit, so a `decide` that refused on memory, or on a chained candidate that never
became durable, could report a false verdict with no write to catch it. The fix is one rule: a
verdict is reported only from a base that is proven, either an observation of this tenure or a
committed batch. The same round removed the eager carve (a decide that reads could hold uncarved
waiters past their deadlines), the copying of a leader's failure onto its members (they return to the
queue instead), cross-plane batching, the LRU and its budget, and two wrong claims about the GC erase
and the bootstrap create.

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

- Inside one process, at most one conditional write in flight per hot key, in arrival order, and
  no read by the lane between two consecutive commits of that process.
- N concurrent catalog mutations of one process cost about one conditional `PUT`, not N, and never
  a 412 against each other's in-flight write.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, or to any read path. Every existing catalog test stays
  green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- A general write cache for every key. Participation is opt-in, per key, by the owner.
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). The two share a
  shape and a vocabulary; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

Two halves, in this order.

**Half 1, the hot-key lane.** A per-pool component of the request engine, `CasHotKeys`, owned by
`Pool` and shared by its three `CasRequests` planes. For a key the owner declares hot, every write
verb queues in a FIFO lane for that key; the lane's leader performs its write; a `readModifyWrite`
leader that contributed bytes also takes the compatible `readModifyWrite`s queued behind it, one at
a time, applying their `decide`s to its candidate and landing them in the same conditional `PUT`.
The lane remembers the last object this process committed, so a `readModifyWrite` can start from
memory instead of a `GET`. A `decide`'s refusal is reported only from a proven base. `Pool`
declares `refCatalogKey()` hot; nothing in `CasRefCatalog`, `CasRefLedger` or GC changes.

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
    struct Item;   /// one queued write, see below
    struct Lane    /// one hot key
    {
        std::deque<std::shared_ptr<Item>> pending;   /// guarded by `mutex`
        bool leader_active = false;                  /// guarded by `mutex`
        std::condition_variable cv;
        std::optional<Object> remembered;            /// guarded by `mutex`; the last object THIS process committed
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
};
```

A lane exists while it has a pending item, an active leader, or a memory, and is erased when the
last of the three goes. There is no eviction and no byte budget: the owner's predicate names the
hot keys, today exactly one per pool, and its object is under 1 MiB. A bound becomes necessary when
a per-namespace key such as `_ckpt` is declared hot; that declaration brings it.

`CasRequests` takes a `std::shared_ptr<CasHotKeys>` in its constructor. A `CasRequests` built
without one (every existing test, the offline tools, the pool's local bootstrap `CasRequests`) owns
a private instance whose predicate is "nothing is hot", so its behaviour is byte-for-byte today's.
`CasRequests` remains a class that writes no member after construction: the lane has its own mutex.

`Pool` declares `std::shared_ptr<CasHotKeys> hot_keys` before `mount_requests`,
`farewell_requests` and `gc_requests`, so it is constructed before them and destroyed after them,
and hands the same pointer to all three. Its predicate is
"`key == Layout(config.pool_prefix).refCatalogKey()`", computed once at construction.

### Which keys, and the participation contract {#which-keys-and-the-participation-contract}

Participation is by predicate, decided by the owner of the table, never by the call site.

What memory is and is not. `remembered` is the last object this process committed, or the last
object the engine's resolve read saw after a lost race. Either was the durable state at some
moment. Neither is an observation of this tenure, and neither is guaranteed newer than an
observation a caller made outside the lane: `dropNamespaceImpl` reads the catalog fresh, sees a
`Live` row another server created, and calls `beginRemoving` on it; the lane's memory may predate
that row. A `decide` that refuses on such a base (the catalog's markers, or a `nullopt`) would be
reporting a verdict about a state the store has left, and with no write sent nothing would catch
it. The same holds for a verdict rendered on a chained candidate whose batch then fails to commit:
the candidate never became durable, so a row the `decide` saw as changed may never have changed.

The rule that closes both: **a verdict (a `decide` that returns `nullopt` or throws) is reported to
its caller only from a proven base**, meaning either the base was an observation this tenure made
(a `GET`, or the resolve read after a `Conflict`) and the verdict was rendered directly on it, or
the batch the `decide` took part in committed, which proves the base was the durable state throughout
and that the chained candidate became durable as written. A verdict on memory, or on a chained
candidate whose batch did not commit, is not a result: the memory is cleared and the `decide` runs
again on a fresh observation. A contribution (a `decide` that returns bytes) needs no such rule: the
conditional `PUT` validates the base it was decided on, as today.

With that rule the safety of the lane does not depend on what a `decide` does with `current`. The
contract a participating site must meet is mechanical:

1. `decide` tolerates being run again on a different `current`, which `readModifyWrite` already
   requires of it (it re-decides on every `Conflict`).
2. `decide` does not read `current->etag` as an identity of `current->bytes`. In a batch the
   chained `current` carries the precondition of the pending write, not an incarnation of the chained
   bytes. Every catalog `decide` decodes `bytes` only.
3. `decide` issues no write on the same key. A write from inside the leader would wait for the
   leader. Reads are fine, as today, and a `decide` that reads holds the chain for that read (see
   [effects to name](#effects-to-name)).
4. A `Liveness` closure carried by an operation whose `decide` may be run by another thread must be
   safe to call from that thread. Today every writer on the catalog key that can be batched is a
   mount-plane operation with no liveness closure.

Today `refCatalogKey()` satisfies all four. The `_ckpt` keys are the next candidate and are not
declared by this design.

### The queue and the leader {#the-queue-and-the-leader}

The lane mirrors `CasRefLedger::appendRefOps` on purpose, so a reader of one recognises the other.

**Item.** One queued write, allocated by its caller and held by `shared_ptr` from the queue. A
single-verb write (`create`, `replace`, `remove`, `readModifyWriteOnPresence`) queues a bare
ticket: when it becomes leader it runs its own verb on its own thread and needs no result slot. A
`readModifyWrite` item carries what a leader needs to run it:

```cpp
struct Item
{
    CasRequests * plane;                     /// which `CasRequests` admitted it: a batch never crosses planes
    CasOperation & op;                       /// the caller's own admission
    Retry policy;                            /// as passed; `single_attempt` items are never carved
    Retry::Bound bound;                      /// bound at entry: queue wait spends the caller's window
    const DecideOnObject * decide = nullptr; /// null for a single-verb ticket
    bool done = false;                       /// guarded by `mutex`
    std::optional<WriteResult> result;       /// guarded by `mutex`; only a carved `readModifyWrite` is settled here
    std::exception_ptr error;                /// guarded by `mutex`
};
```

**Wait.** Under `mutex`: push the item, then loop. If `done`: leave with `result` or rethrow
`error`. If the item is in `pending` (not carved) and its own `bound.deadline_ms` has passed on the
engine's clock: erase it, leave with `GaveUp{Deadline}`. If in `pending` and its own gate is lost:
erase, leave with `GaveUp{FenceLost}`. If it is the front of `pending` and no leader is active:
become leader. Otherwise `cv.wait_for(lock, slice)` with a short slice (the pattern of
`recovery_cv.wait_for(lock, 200ms)` in the ledger), so a waiter re-checks its own deadline against
the engine's clock even when the leader is inside a long `decide`, and so an injected clock in a
test moves it. Every handover also does `cv.notify_all()`.

**Leader.** Under `mutex`: install the exit guard, then `leader_active = true`, then release the
mutex. Nothing between the two can throw. The leader's own item stays at the front of `pending`
until the guard settles it. There is no eager carve.

**Carve, lazily.** Only after the leader's own `decide` has contributed bytes (below), and one item
at a time: under `mutex`, look at the front of `pending` behind the leader; it is compatible when it
is a `readModifyWrite` item on the same `plane` with `policy.single_attempt == false`. Push it onto
the leader's owned list first (the only allocation), then pop it from `pending`; an allocation
failure leaves the queue untouched and ends the carve. Release the mutex and run its `decide`. Stop
carving at the first incompatible item or at an empty queue. An item still in `pending` is not owned
by anyone but its caller and can leave on its own deadline.

**Exit.** One guard on every exit, normal or exceptional, the shape of
`completeOwnedItemsAndReleaseLeadership`: under `mutex`, every owned item not yet `done` is either
settled or put back; `leader_active = false`; `cv.notify_all()`. Which items are settled and which
are put back is decided by the batch's ending (below). An item is never left owned by no one after
its caller's stack may be gone: the decide closure lives there.

**Fairness.** One batch per tenure. The leader hands over as soon as its batch is settled, even if
the queue refilled meanwhile; the next front item leads the next batch.

### The batch {#the-batch}

Every step runs on the leader's thread, on a ticket, never under `mutex` except where stated.

1. **Base.** `remembered`, if present; otherwise `observe` under the leader's policy and bound, as
   `readModifyWrite` does today. A failed observation ends the tenure with the leader's own
   `GaveUp` after failed observation; nothing was carved.
2. **The leader's decide** on the base. Bytes: the leader is the first contributor and the
   candidate is those bytes. A verdict on an observed base: final; the tenure ends and the leader
   reports it; nothing was carved. A verdict on memory: not a result; clear the memory and return to
   step 1, which now observes. This restart happens at most once per tenure.
3. **Carve and chain.** For each item carved as above: `gate(0)` on its own operation; lost: settle
   it now with `GaveUp{FenceLost}`, `sent_any = false`; its `decide` never runs. Otherwise
   `decide(Object{bytes = candidate, etag = base etag})`. Bytes: the item is a contributor and the
   candidate is those bytes. A verdict: held provisionally, the candidate unchanged, the chain
   continues.
4. **Write.** One `writeLoop` of the candidate against the base etag (a create when the base was
   absence), under the leader's operation, policy and bound, with `ResolveWith::Body`: the inner
   write of today's `readModifyWrite`.
   - `Committed`: `remembered := (candidate, etag)`. For each contributor, `gate(0)` on its own
     operation once more; lost: `GaveUp{FenceLost}` with `sent_any = true`, although the batch
     landed, which is the engine's own rule for a single write whose fence was lost in flight;
     otherwise `Committed{etag, attempts_sent, resolved_by_read}` with the batch's counts. Each
     provisional verdict is now proven and is delivered: `Declined{seen = base}` or the rethrown
     exception.
   - `Conflict`: `remembered := seen` if the resolve read saw an `Object`, cleared otherwise;
     pause `Retry::conflictBackoff()` under the leader's gate and bound (Half 2); base := seen, which
     is an observation; re-run step 2 for the leader on it (a verdict there is final: the leader
     reports it and the tenure ends with every owned item put back), then step 3 over every owned
     item in order, contributors and provisional ones alike, each re-gated; then step 4 again.
   - `Refused`, `GaveUp`, or an exception out of `writeLoop`: the leader alone receives it.
     `remembered` is cleared. Every other owned item is put back at the front of `pending` in its
     original order, not settled and not `done`; when it wakes it applies its own deadline and gate,
     and the front one leads the next tenure, which observes because the memory is gone. A tenure's
     failure is therefore never attributed to a member, and a member's provisional verdict is never
     delivered from a base that did not commit.

**Memory rule, in one sentence.** After any write verb on a hot key, `Committed` remembers the
bytes written and the etag returned, a `Conflict` whose resolve read saw an `Object` remembers that
object, and every other ending and every exception forgets. Those are the only two sources; an
ordinary `read` never reads or writes the memory, and absence is never remembered.

### Single-verb writes on a hot key {#single-verb-writes-on-a-hot-key}

`create`, `replace`, `remove` and `readModifyWriteOnPresence` on a hot key queue as bare tickets.
Each runs when it becomes leader, on its caller's thread, with its own precondition, policy and
result type, exactly as today; it is never carved into another leader's batch and never starts from
memory. After it the memory rule applies unchanged: `Committed` remembers the bytes it wrote and the
etag it got; a `Conflict` that saw an `Object` remembers it; `Removed`, `Gone`, `Mismatch` and every
other ending forget.

What this buys the GC erase, and what it does not. `deleteCompletedRemovingAtSnapshot` does a
`replace` against the etag of the round's catalog cut, read by the reconciler earlier, and then its
mandatory resolution read. In the lane that `replace` no longer overlaps another write of this
process, and its win refreshes the memory. Its precondition is still the cut's etag: a mount
writer that committed after the cut refuses it with a 412 exactly as today, and its resolution read
feeds its next attempt exactly as today. The lane does not rewrite that loop and does not claim to
remove that 412.

`initializeEmptyForNewPool` runs on the pool factory's local bootstrap `CasRequests`, before the
`Pool` exists; it has no hot keys and seeds nothing. The first mount-plane write reads.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait. Every catalog write already runs under `Retry::standard()`, 90 s
from the call, or a lease bound if shorter. The lane binds the policy at entry, before queueing, so
time in the queue spends the same window rather than adding to it, which is what today's backoff
sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter in `pending` | its own deadline and its own gate, re-checked every slice | its window plus one slice |
| the leader | its own deadline; the engine starts no attempt that cannot finish inside it | its window, plus one attempt timeout, plus the windows of reads its own `decide` issues |
| a carved item | the rest of the chain and the leader's write | the leader's worst case, less what already elapsed; after any non-commit it is back in `pending` under its own deadline |

Nothing copies a deadline verdict from one operation to another: `GaveUp::Source` is always the
reporting operation's own bound. A carved item receives exactly one of: `Committed` from a committed
batch, its own proven verdict, or its own gate's `FenceLost`; everything else returns it to the
queue.

A stopping plane or a lost fence reaches waiters through the same route as today: the leader's
operation gives up at its gate, the tenure ends, every waiter wakes and applies its own gate.

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

The order is therefore: caller (no locks) → `CasHotKeys::mutex` (a leaf: held for enqueue, one
carve step, result delivery and memory updates; never across I/O, never across a `decide`) →
backend mutex. The one rule this imposes on future callers, stated where the catalog API is
documented: a catalog mutation is not entered while holding a ledger mutex. A `decide` that blocks
on something a waiter holds is the only deadlock shape, and waiters hold nothing.

### Invariants {#invariants}

- INV-HK1. Per process and hot key, at most one conditional write is in flight at any moment.
- INV-HK2. Writes of one process to a hot key are applied in arrival order.
- INV-HK3. Between two consecutive `Committed`s of one process on a hot key, the lane issues no
  read of the key: the later write starts from memory. (Callers' own reads of the key, such as the
  catalog's pre-check `read`, are outside the lane and unchanged.)
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair that was durable at
  some moment, taken from a `Committed` of this process or from the engine's resolve read. It never
  changes the semantics of a conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A verdict (a `decide` returning `nullopt` or throwing) reaches its caller only from a
  proven base: rendered directly on an observation of this tenure, or proven by the commit of the
  batch it was rendered in.
- INV-HK6. An item leaves the queue only by being settled or put back by a leader, or by removing
  itself while it is in `pending`; no item is owned by no one while its caller may have unwound.
- INV-HK7. A carved item's own fence is checked before its `decide` runs and again after the commit
  it took part in; a lost fence before means its `decide` never ran, a lost fence after means
  `GaveUp{FenceLost}` even though the batch landed.
- INV-HK8. A batch never crosses `CasRequests` planes and never carries a `single_attempt` item.

### Effects to name {#effects-to-name}

- A carved `decide` runs on the leader's thread while its caller is parked. The caller's
  `CasOperation` is used by one thread at a time, which keeps its single-threaded contract, but
  profile events and the stack of an exception thrown from that `decide` belong to the leader's
  thread and query.
- A `decide` that issues reads holds the chain for those reads. `reconcileStaleCreator` and
  `cancelStalledCreating` call `isCreatorFenceTerminal` inside `decide`, and that reads the mount
  key under a fresh `Retry::standard()` on the member's operation. Today that window is the
  caller's own; in a batch it is every later member's too. Uncarved waiters are unaffected (they hold
  their own deadline); a carved member waits. Passing a frozen policy into `isCreatorFenceTerminal`
  would bound it and is a follow-up outside this design.
- `CasRefCatalog::casUpdate` and `casAdmitEntry` return the candidate this call's own `mutate`
  produced, which is included in the committed object but is not the committed object when other
  contributors followed in the batch. No production caller reads the return value; tests that assert
  on it run without a hot predicate and see today's behaviour.
- `Committed::attempts_sent` and `resolved_by_read` reported to a contributor are the batch's.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item` with `done`, `error`, `result` under `CasHotKeys::mutex`; a bare ticket for single verbs |
| leader | first to find `leader_active` false; serves until its own item is done | first to find it false; one batch per tenure |
| carve | compatible items into one ref-log transaction, at flush time | compatible items one at a time, as their `decide` is applied, into one conditional `PUT` |
| exit | `completeOwnedItemsAndReleaseLeadership` | the same guard; settles on commit, puts back on anything else |
| wait | `cv.wait`, woken at every handover | `cv.wait_for` in slices, woken at every handover, own deadline re-checked |
| work per item | `build_ops` runs at most once; a rejection is final | `decide` re-runs on the fresh object after a conflict, as `readModifyWrite` already promises; a refusal is final only from a proven base |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror for confirms | replace-style control object: ambiguity is settled by the engine's resolve read |
| home | inside `RefTableRuntime`, per namespace | inside the engine, per key |

The shared shape is the point; the differences are why they are two components. Extracting a common
flat-combining lane is possible and is not part of this design.

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
- The hot-key batch loop uses the same pause on its `Conflict` path.
- Transport faults and unresolved-but-repeatable attempts keep `pauseAndReissue` unchanged.

Why flat is right and growing was wrong: a conflict is a lost race the resolve read has already
settled; the writer holds the fresh object and has nothing to wait for except desynchronisation
from its competitors. Growing the pause with the writer's own loss count makes the oldest loser the
slowest and therefore the likeliest to lose again. The spec's motivation for pacing, GCS's budget
of about one mutation per second per object, is served by the transport path: exceeding it answers
429, which is a transport fault and takes the growing schedule. After Half 1, conflicts arise only
between servers, and there a loss is dearer than before, because the losing server's whole lane
waits behind its leader.

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
3. A new short section pointing to this document for the lane's invariants and the participation
   contract.

## Observability {#observability}

Three profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enqueue to completion, per item.
- `CASHotKeyBatchMembers`: contributors per committed batch (sum; divide by
  `CASHotKeyBatches` for the mean).
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a tenure obtained its base.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Engine level, in `gtest_cas_requests.cpp`'s harness: fake clock, counting backend, write hooks. A
`CasHotKeys` with a predicate naming the test key is passed to the `CasRequests` under test. Each
`decide` in these tests decodes a list of tickets from `bytes` and appends its own, so order and
membership are visible in the object.

1. Serialization and combining. N threads call `readModifyWrite` on the hot key; a write hook parks
   the first leader until all N are queued. Assert: reads of the key 1, writes 2, the final object
   lists the tickets in arrival order, no read between the two commits (INV-HK1, HK2, HK3).
2. Fence before and after. A carved item whose operation was resumed under a stale generation
   receives `GaveUp{FenceLost}` and its `decide` never ran (a counter); a carved item whose fence is
   tripped by a hook between the `PUT` and the post-commit check receives `GaveUp{FenceLost}` with
   `sent_any = true` while the object carries its ticket (INV-HK7).
3. Verdict on stale memory. The process commits once (memory set). A second `CasRequests` over the
   same backend writes a row. A caller reads the key fresh outside the lane and runs a
   `beginRemoving`-shaped `decide` (throw unless the row equals what it observed). Assert: no
   exception, one read by the lane, `Committed`, the row transitioned (INV-HK5, the first half).
4. Provisional verdict, failed batch. Leader's `decide` changes row R; a carved member's `decide`
   throws because R no longer equals what it observed; a hook makes the batch `PUT` `Refused`.
   Assert: the leader receives `Refused`, the member receives nothing yet, memory is cleared, the
   member leads the next tenure from a fresh read, sees R unchanged, contributes, `Committed`
   (INV-HK5, the second half; INV-HK6).
5. Provisional verdict, committed batch. Same setup, the `PUT` commits. Assert: the member's
   exception is delivered after the commit and the object carries the leader's change.
6. Declined and thrown contributions. A carved `decide` returning `nullopt` receives `Declined`
   after the commit, with no ticket in the object; a carved `decide` that throws receives its
   exception after the commit.
7. External writer on the write path. A second `CasRequests` writes between two writes of the
   first. Assert the first's next `readModifyWrite` does exactly one resolve read and one retry
   write and ends `Committed`, and the write after that starts from memory with zero reads.
8. `Refused` and a thrown `writeLoop` clear the memory and put members back: the next tenure
   reads, no item is left owned by no one, the next writer proceeds.
9. Deadlines. A waiter in `pending` whose fake-clock deadline passes while the leader is parked
   inside its `decide`'s nested read leaves with `GaveUp{Deadline}` and sends nothing; a carved item
   whose own deadline passes receives the batch's `Committed`.
10. Compatibility. A `Retry::once` item and an item from a second plane sharing the same
    `CasHotKeys` are never carved: each runs as its own leader after the batch (INV-HK8). The
    second-plane `replace` (the GC erase shape) lands and refreshes the memory.
11. Half 2. One writer loses K races in a row (a hook mutates the key before every attempt).
    Assert every recorded sleep is at most 200 ms and the sum at most K × 200 ms; a sibling test
    injecting K transport faults sees the unchanged growing schedule.

Pool level, through the ledger:

12. N concurrent `createNamespace` calls on one `Pool`: catalog writes fewer than N, catalog reads
    bounded by a constant independent of N.
13. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Not tested: an allocation failure during the carve step (no injection point; the design's
allocation-before-removal order is the argument).

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
without a hot predicate, and must stay green. That is the check that the catalog API was not
touched.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Declaring the `_ckpt` keys hot, with the memory bound that many hot keys need: a BACKLOG item.
- Bounding the reads a catalog `decide` issues (`isCreatorFenceTerminal` under a frozen policy): a
  BACKLOG item.
- Unifying the two lanes: a BACKLOG note, not planned.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.
