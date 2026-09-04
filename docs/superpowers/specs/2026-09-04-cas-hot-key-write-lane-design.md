---
description: 'Design for serializing and combining one process''s conditional writes to a hot CAS control object (the ref catalog first) inside the request engine, remembering the last committed object so consecutive writes need no read, and pacing lost races between servers with a flat jitter instead of the transport-fault backoff. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 1. Brainstormed 2026-09-04 against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the two BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`. This document
supersedes the fix sketch in those items where they differ (the door lives in the engine, not in
`CasRefCatalog`; writes are combined, not only serialized).

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

- Inside one process, at most one conditional write in flight per hot key, in arrival order, with no
  read between two consecutive commits of that process.
- N concurrent catalog mutations of one process cost about one conditional `PUT`, not N, and never
  a 412 against each other.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults.
- No change to the catalog's API, to the semantics of any conditional write, to the 90 s windows,
  or to any read path. Every existing catalog test stays green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint; a wrong hint costs one 412 and one
  resolve read, never correctness.
- A general write cache for every key. Participation is opt-in, per key, by the owner (see
  [the participation contract](#which-keys-and-the-participation-contract)).
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). The two share a
  shape and a vocabulary; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

Two halves, in this order.

**Half 1, the hot-key lane.** A per-pool component of the request engine, `CasHotKeys`, owned by
`Pool` and shared by its three `CasRequests` planes. For a key the owner declares hot, every write
verb queues in a FIFO lane for that key; the lane's leader performs the write; a `readModifyWrite`
leader also carries the queued `readModifyWrite`s behind it, applying their `decide`s in order to one
candidate and landing them in one conditional `PUT`. The lane remembers the last object this process
committed, so a `readModifyWrite` starts from memory instead of a `GET`. `Pool` declares
`refCatalogKey()` hot; nothing in `CasRefCatalog`, `CasRefLedger` or GC changes.

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
    CasHotKeys(IsHot is_hot, uint64_t memory_budget_bytes);
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
    std::unordered_map<String, Lane> lanes;          /// a lane exists while it has a queue or a memory
    /// LRU by last commit over `remembered`, evicted by bytes against `memory_budget_bytes`.
};
```

`CasRequests` takes a `std::shared_ptr<CasHotKeys>` in its constructor. A `CasRequests` built
without one (every existing test, the offline tools) owns a private instance whose predicate is
"nothing is hot", so its behaviour is byte-for-byte today's. `CasRequests` remains a class that
writes no member after construction: the lane has its own mutex.

`Pool` declares `std::shared_ptr<CasHotKeys> hot_keys` before `mount_requests`,
`farewell_requests` and `gc_requests`, so it is constructed before them and destroyed after them,
and hands the same pointer to all three. Its predicate is "`key == Layout(config.pool_prefix).refCatalogKey()`",
computed once at construction. The memory budget comes from a new `PoolConfig` field with a default
(16 MiB; today's only hot object is under 1 MiB, and the field exists so the later `_ckpt` opt-in has
a bound). It is a config-struct field, not a user-facing setting, like `manifest_decode_cache_bytes`.

### Which keys, and the participation contract {#which-keys-and-the-participation-contract}

Participation is by predicate, decided by the owner of the table, never by the call site. This is
deliberate: a `readModifyWrite` on a hot key hands `decide` the last object this process committed,
which is a durable state at some earlier moment, not a fresh observation. The conditional `PUT`
protects against a wrong write, but not against a wrong refusal or a wrong diagnosis, because those
write nothing. Sites whose `decide` judges from the observation must therefore stay cold:

- `allocateWriterEpoch`: `decide` lists a subtree and probes a sentinel and throws
  `CORRUPTED_DATA` on disagreement with the object. A remembered object older than a fresh `LIST`
  would be a false corruption verdict.
- `Gc::acquireOrRenewLease`: steals only when the lease tuple is unchanged across two
  observations. Memory is not an observation.
- `publishCkpt`, `casGcMaintenanceState`: a `Declined` "already so" on memory would mean the store
  was never asked.

A key may be declared hot when every `readModifyWrite` site on it satisfies all of:

1. `decide` renders its verdict only by writing: it never returns `nullopt` (a `Declined` decided
   on memory would never be confirmed against the store) and never throws on a comparison of
   `current` with anything read elsewhere. (The catalog's `decide`s never decline, and their markers
   compare `current` only with a caller-observed row whose 128-bit incarnation cannot recur, so a
   mismatch on a stale snapshot is still a true mismatch.)
2. `decide` does not read `current->etag` as an identity of `current->bytes`. In a batch the
   chained `current` carries the precondition of the pending write, not an incarnation of the chained
   bytes.
3. `decide` issues no write on the same key. A write from inside the leader would wait for the
   leader. Reads are fine, as today.

Today `refCatalogKey()` satisfies all three. The `_ckpt` keys are the next candidate and are not
declared by this design.

### The queue and the leader {#the-queue-and-the-leader}

The lane mirrors `CasRefLedger::appendRefOps` on purpose, so a reader of one recognises the other.

**Item.** One queued write, allocated by its caller and held by `shared_ptr` from the queue:

```cpp
struct Item
{
    CasOperation & op;                       /// the caller's own admission
    Retry::Bound bound;                      /// bound at entry: queue wait spends the caller's window
    std::variant<DecideOnObject *, SingleWrite> work;   /// a batchable decide, or one verb with its own precondition
    bool taken = false;                      /// carved into a batch; may no longer leave
    bool done = false;                       /// guarded by `mutex`
    std::optional<WriteResult> result;       /// guarded by `mutex`
    std::exception_ptr error;                /// guarded by `mutex`
};
```

**Wait.** Under `mutex`: push the item, then loop. If `done`: leave with `result` or rethrow
`error`. If the item is not `taken` and its own `bound.deadline_ms` has passed: erase it from
`pending`, leave with `GaveUp{Deadline}`. If not `taken` and its own gate is lost: erase, leave
with `GaveUp{FenceLost}`. If it is the front of `pending` and no leader is active: become leader.
Otherwise `cv.wait(lock)`. There is no timed wait: every waiter is woken at every leadership
handover, and a tenure is bounded (below), so the delay of a deadline or fence verdict is at most one
tenure. This is `appendRefOps`'s own shape.

**Leader.** Still under `mutex`, the leader carves its batch: its own item, then every following
item in queue order whose `work` is a `decide` and whose `bound.deadline_ms` is not earlier than
the leader's, stopping at the first item that fails either test. A single-verb item is never carved
by another leader: it runs when it becomes leader itself, on its own thread. Every carved item is
marked `taken` and removed from `pending`; the leader records them as its owned items. Then
`leader_active = true`, the mutex is released, and the leader works on a ticket, never under the
mutex.

**Exit.** One guard on every exit, normal or exceptional, exactly `completeOwnedItemsAndReleaseLeadership`:
under `mutex`, every owned item not yet `done` is completed with the leader's exception, or with a
fail-closed `LOGICAL_ERROR` if there is none; `leader_active = false`; `cv.notify_all()`. An item
is never left in `pending` after its caller's stack may be gone: the decide closure lives there.

**Fairness.** One batch per tenure. The leader hands over as soon as its batch is settled, even if
the queue refilled meanwhile; the next front item leads the next batch.

### The batch {#the-batch}

**Base.** `remembered`, if present. Otherwise `observe` under the leader's policy and bound, as
`readModifyWrite` does today. A failed observation ends the batch: every carved item receives the
same `GaveUp` after failed observation; memory is cleared.

**Chain.** In queue order, for each carved item:

1. `gate(0)` on the item's own operation. Lost: the item receives `GaveUp{FenceLost}` with
   `sent_any = false`; its `decide` never runs.
2. `decide(current)`, where `current` is the base for the first contributing item and
   `Object{bytes = previous candidate, etag = base etag}` for each later one; absence stays absence
   until the first contribution.
3. `nullopt`: the item receives `Declined{seen = base observation}`; the candidate is unchanged.
4. A throw: the exception goes to the item's `error`; the candidate is unchanged; the chain
   continues.
5. Bytes: they are the new candidate; the item is a contributor.

No contributor: no write is sent; the batch is settled; memory is unchanged.

**Write.** One `writeLoop` of the final candidate against the base etag (or a create when the base
is absence), under the leader's operation, policy and bound, with `ResolveWith::Body`, exactly the
inner write of today's `readModifyWrite`.

- `Committed`: memory := (candidate, etag). For each contributor, `gate(0)` on its own operation
  once more; lost: `GaveUp{FenceLost}` with `sent_any = true`, although the batch landed, which is
  the engine's own rule for a single write whose fence was lost in flight; otherwise
  `Committed{etag, attempts_sent, resolved_by_read}` with the batch's counts.
- `Conflict`: memory := the object the resolve read saw (cleared if it saw absence or nothing);
  pause `Retry::conflictBackoff()` under the leader's gate and bound (Half 2); re-run the chain over
  the contributors only, from the seen object, with the gate check of step 1 repeated for each; then
  write again. Items already settled by steps 1, 3 or 4 are not revisited: a `Declined` or a thrown
  `decide` ends a `readModifyWrite` today too.
- `Refused`, `GaveUp`: every contributor receives a copy; memory is cleared.
- An exception out of `writeLoop` (a non-`Poco` fault, propagated unclassified today): every
  contributor receives the same `exception_ptr`; memory is cleared; the exit guard runs.

**Memory rule, in one sentence.** After any write verb on a hot key, `Committed` remembers the
bytes written and the etag returned, a `Conflict` whose resolve read saw an `Object` remembers that
object, and every other ending and every exception forgets. Those are the only two sources; an
ordinary `read` never reads or writes the memory, and absence is never remembered.

### Single-verb writes on a hot key {#single-verb-writes-on-a-hot-key}

`create`, `replace`, `remove` and `readModifyWriteOnPresence` on a hot key queue as single-verb
items. Each runs when it becomes leader, on its caller's thread, with its own precondition and
policy, exactly as today; it is never carved into another leader's batch and never starts from
memory. After it the memory rule above applies unchanged: `Committed` remembers `(bytes, etag)`, a
`Conflict` that saw an `Object` remembers it, `Removed` and every other ending forget.

This is what puts the GC erase in the lane without rewriting it: `deleteCompletedRemovingAtSnapshot`
does a `replace` against the round's cut etag and then its mandatory resolution read. The `replace`
waits for the mount plane's writers and lands without an intra-process 412; its win refreshes the
memory; its resolution read is unchanged and authoritative for GC as before.
`initializeEmptyForNewPool`'s `create` seeds the memory of a fresh pool the same way.

`readModifyWriteOnPresence` is serialized only, without memory or batching: its one site
(`casPutObject`) is not hot, and the presence variant would need a `Meta` projection of the memory
that nothing asks for.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait. Every catalog write already runs under `Retry::standard()`, 90 s
from the call, or a lease bound if shorter. The lane binds the policy at entry, before queueing, so
time in the queue spends the same window rather than adding to it, which is what today's backoff
sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter not yet carved | its own deadline and its own gate, checked at every handover | its window plus one tenure |
| the leader | its own deadline; the engine starts no attempt that cannot finish inside it | its window plus one attempt timeout |
| a carved contributor | the leader's write | the leader's deadline, which is not later than its own by the carve rule |

The carve rule (`bound.deadline_ms` of a carved item is not earlier than the leader's) holds by
itself for FIFO callers under one policy shape, because the leader arrived first, and is checked
explicitly so a frozen policy (`op.freeze`) can never violate it. A carved item cannot leave: its
mutation is in flight, and the only truthful answer is the write's real ending.

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

The order is therefore: caller (no locks) → `CasHotKeys::mutex` (a leaf: held for enqueue, carve,
result delivery and memory updates; never across I/O, never across a `decide`) → backend mutex.
The one rule this imposes on future callers, stated where the catalog API is documented: a catalog
mutation is not entered while holding a ledger mutex. A `decide` that blocks on something a waiter
holds is the only deadlock shape, and waiters hold nothing.

### Invariants {#invariants}

- INV-HK1. Per process and hot key, at most one conditional write is in flight at any moment.
- INV-HK2. Writes of one process to a hot key are applied in arrival order.
- INV-HK3. Between two consecutive `Committed`s of one process on a hot key, the lane issues no
  read of the key: the later write starts from memory. (Callers' own reads of the key, such as the
  catalog's pre-check `read`, are outside the lane and unchanged.)
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair that was durable at
  some moment, taken from a `Committed` of this process or from the engine's resolve read. It never
  changes the semantics of a conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. An item leaves the queue only by being completed by a leader or by removing itself
  before it is carved; no item outlives its caller's stack in the queue.
- INV-HK6. A contributor whose own fence was lost before the chain never has its `decide` run; one
  whose fence was lost after the commit is told `GaveUp{FenceLost}` even though the batch landed.

### Effects to name {#effects-to-name}

- A carved `decide` runs on the leader's thread while its caller is parked. The caller's
  `CasOperation` is used by one thread at a time, which keeps its single-threaded contract, but
  profile events and the stack of an exception thrown from that `decide` belong to the leader's
  thread and query.
- `CasRefCatalog::casUpdate` and `casAdmitEntry` return the candidate this call's own `mutate`
  produced, which is included in the committed object but is not the committed object when other
  contributors followed in the batch. No production caller reads the return value; tests that assert
  on it run without a hot predicate and see today's behaviour.
- `Committed::attempts_sent` and `resolved_by_read` reported to a contributor are the batch's.
- A carved item's gate is evaluated on the leader's thread. The fence closures read atomics; a
  `Liveness` closure carried by a carved operation must be safe to call from another thread. Today
  every carvable writer is a mount-plane operation with no liveness closure; the GC's operation,
  which carries one, is a single-verb item and is gated only on its own thread.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item` with `done`, `error`, `result` under `CasHotKeys::mutex` |
| leader | first to find `leader_active` false; serves until its own item is done | first to find it false; one batch per tenure |
| carve | compatible items into one ref-log transaction | following `decide` items into one conditional `PUT` |
| exit | `completeOwnedItemsAndReleaseLeadership` | the same guard, same name |
| wait | `cv.wait`, woken at every handover | the same |
| work per item | `build_ops` runs at most once; a rejection is final | `decide` re-runs on the fresh object after a conflict, as `readModifyWrite` already promises |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror for confirms | replace-style control object: ambiguity is settled by the engine's resolve read |
| home | inside `RefTableRuntime`, per namespace | inside the engine, per key |

The shared shape is the point; the differences are why they are two components. Extracting a common
flat-combining lane is possible and is not part of this design.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff()` returns `backoff(2)`, uniform(0, 200 ms), so the number has one home
next to the schedule it departs from.

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
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a batch obtained its base.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Engine level, in `gtest_cas_requests.cpp`'s harness: fake clock, counting backend, write hooks. A
`CasHotKeys` with a predicate naming the test key is passed to the `CasRequests` under test.

1. Serialization and combining. N threads call `readModifyWrite` on the hot key; a write hook parks
   the first leader until all N are queued; each `decide` appends its ticket to the object. Assert:
   reads of the key 1, writes 2, the final object lists the tickets in arrival order, no read between
   the two commits.
2. A carved item whose operation was resumed under a stale generation: it receives
   `GaveUp{FenceLost}`, its `decide` never ran (a counter), the others `Committed`.
3. A carved `decide` throws: only that caller sees the exception; the batch lands without its
   contribution. A carved `decide` returns `nullopt`: `Declined`, no contribution.
4. External writer. A second `CasRequests` over the same backend writes the key between two writes
   of the first. Assert the first's next `readModifyWrite` does exactly one resolve read and one
   retry write and ends `Committed`, and the write after that starts from memory with zero reads.
5. `Refused` from a hook clears the memory: the next write reads first.
6. Deadline in the queue, both orders. A waiter not yet carved whose fake-clock deadline passes
   leaves with `GaveUp{Deadline}` and sends nothing; a waiter already carved receives the write's real
   ending.
7. The leader's `writeLoop` throws a non-`Poco` exception: every carved item receives the same
   `exception_ptr`, memory is cleared, the queue holds no stranded item, and the next writer
   proceeds after a read.
8. A `replace` from a second plane sharing the same `CasHotKeys` (the GC erase shape) waits for a
   parked leader, lands, and its win refreshes the memory: the next `readModifyWrite` starts with
   zero reads.
9. Half 2. One writer loses K races in a row (a hook mutates the key before every attempt). Assert
   every recorded sleep is at most 200 ms and the sum at most K × 200 ms; the transport-fault path's
   sleeps are unchanged by a sibling test that injects K faults.

Pool level, through the ledger:

10. N concurrent `createNamespace` calls on one `Pool`: catalog writes fewer than N, catalog reads
    bounded by a constant independent of N.
11. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
without a hot predicate, and must stay green. That is the check that the catalog API was not
touched.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Declaring the `_ckpt` keys hot: a BACKLOG item, gated on the participation contract being
  checked for `publishCkpt`'s `IdenticalSkip` and epoch-decrease declines (they decline, so today
  they fail rule 1; the rule for a `Declined` decided on memory would have to be "confirm by a read
  before reporting").
- Unifying the two lanes: a BACKLOG note, not planned.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.
