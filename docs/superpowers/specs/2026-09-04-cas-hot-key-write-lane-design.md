---
description: 'Design for a per-pool write lane above the request engine for hot compare-and-swap objects (the ref catalog first): one FIFO per key shared by the pool''s planes, the queued read-modify-writes combined into one conditional PUT with as-if-serial semantics, an LRU of last known objects so the next write needs no read, a decide''s refusal reported only from a fresh read, and lost races between servers paced by a flat jitter. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 21, 2026-09-04. Brainstormed against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`, whose fix sketch
this document supersedes where they differ. The revision history at the end records twenty
earlier revisions and why this one is shaped differently: the lane now sits above the request
engine instead of inside its read-modify-write loop.

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

Three facts from the code explain the shape:

1. `CasRefCatalog::casUpdateImpl` calls `CasOperation::readModifyWrite` under `Retry::standard()`.
   That verb always starts with a `GET`, and on a lost race sleeps
   `Retry::backoff(++state.reissues)`: full jitter over a window that doubles from 200 ms to the
   5 s cap, on a counter shared with transport faults. A writer that has lost several races is
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

- Per pool and hot key, at most one conditional write in flight, in arrival order, from any of the
  pool's planes.
- N concurrent mutations of one hot object from one pool cost about one conditional `PUT` and at
  most one `GET`, not N of each, and never a 412 against each other; a lone mutation costs one
  `PUT` and no `GET` when the pool wrote the object last.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one, and for a store that answers 429.
- The lane serves any hot compare-and-swap object, not the catalog only: its `decide` sees bytes,
  and a caller chooses to write through it. The catalog is the first caller.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, to any read path, or to a single line of GC. The request
  engine changes only in Half 2 and in carrying a pointer. Three internal changes in
  `CasRefCatalog.cpp`, named below. Every existing catalog test stays green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- Pacing the lane to a store's documented per-object write rate. A store that finds the pool too
  fast answers 429, which is a transport fault and takes the growing schedule, and combining
  already makes one `PUT` carry every mutation that arrived meanwhile. If the `gcs` lane's
  acceptance shows 429s on the catalog, spacing is a BACKLOG item there.
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). They share a
  shape; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

**Half 1, the lane.** A per-pool component above the request engine, `CasHotKeys`, reachable
from any of the pool's three `CasRequests` planes through the operation that admits a write. A
caller submits a mutation of one key: a `decide` over the key's bytes. The lane takes a FIFO ticket
for the key, and when the ticket is at the front it obtains a base (the LRU's last known object,
else a `GET`), runs the `decide`, takes the compatible submissions queued behind it, applies their
`decide`s to its candidate in queue order, and lands everything in one conditional `PUT` through
the engine, with the semantics "as if each had committed alone, in that order". The engine's one
result maps to the lane's three: `Landed`, `Refused` (nothing of yours landed, decide again),
`Unknown`. A refusal a `decide` renders on a cached base is never reported; the lane reads and
decides again. `CasRefCatalog` writes through the lane; so does the GC erase, whose `decide`
refreshes the leader's authority and checks the exact row.

**Half 2, conflict pacing.** In `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`,
for the sites that keep using them, a clean refused precondition is repaid after
`Retry::conflictBackoff()`, a flat uniform over [0, 200] ms, and no longer advances the
transport-fault counter. The lane uses the same pause after a `Refused`. Two sentences of the
backend request contract change.

## Half 1: the lane {#half-1-the-lane}

### Placement and reach {#placement-and-reach}

`Backend/CasHotKeys.{h,cpp}`, beside the engine and above it: the lane calls the engine's verbs
(`read`, `create`, `replace`) and the engine never calls the lane. One instance per pool, owned by
`Pool` as a member declared before its three `CasRequests`, so it is constructed before them and
destroyed after them. Each `CasRequests` is given a pointer to it at construction and exposes it
through `CasOperation::hotKeys()`; that pointer is the engine's whole involvement. A `CasRequests`
built without one (every existing test, the offline tools, the pool factory's local bootstrap
`CasRequests`) owns a private instance with no cache, so a write through it costs today's `GET`
plus `PUT` and every existing test's request counts are unchanged.

```cpp
class CasHotKeys
{
public:
    explicit CasHotKeys(uint64_t cache_budget_bytes);   /// 0: no cache

    /// The caller's mutation of `key`: the candidate bytes to write over `base`, or a refusal by
    /// exception. Pure in `base` and in the reads it issues; runnable any number of times.
    using Decide = std::function<String(const Object & base)>;
    using DecideOnAbsence = std::function<String(const std::optional<Object> & base)>;   /// for keys that may not exist yet

    struct Landed  { Etag etag; };
    struct Refused { std::optional<Object> seen; };   /// nothing of yours landed; `seen` if a resolve read saw the key
    struct Unknown { };                                /// a write was sent and its fate is not known
    using Outcome = std::variant<Landed, Refused, Unknown>;

    /// One hold: base, decide, combine, one engine write, settle. Refusals by `decide` propagate
    /// to the caller as its exception, never from a cached base.
    Outcome submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide);

private:
    struct Item;   /// one queued submission, see "The ticket"
    struct Lane    /// one key; created on first use, lives as long as this object
    {
        std::deque<std::shared_ptr<Item>> queue;     /// guarded by `mutex`; the first item not `done` is the holder-to-be
        std::optional<uint64_t> holder_since_ms;     /// guarded by `mutex`; for the log line
        std::condition_variable cv;
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
    /// The LRU of last known objects, by key, bounded by bytes; guarded by `mutex`.
    LRUCache<String, Object> cache;
};
```

`submit` is what `readModifyWrite` is for a key nobody shares: one logical write. Where the
engine's verb starts from its own `GET` and retries its own conflicts inside, `submit` starts from
the cache when it can, lands several callers' mutations in one `PUT`, and returns a `Refused` the
caller retries by submitting again. The retry loop belongs to the caller (`casUpdateImpl`'s, the
erase's), as the hand-written loops of the backend request contract already do, under one frozen
policy, with `Retry::conflictBackoff()` between submissions.

### The contract of a `decide` {#the-contract-of-a-decide}

A caller may write a key through the lane when every `decide` it will ever submit for that key
meets four conditions, which the catalog's meet today and which are the entry condition for any
other key:

1. It is a pure function of `base` and of the reads it issues; it may be run more than once, on
   different bases, and it neither observes nor cares which run is reported.
2. It refuses by throwing. Its exception is its verdict, and it must be the same verdict serial
   execution would render on the same base. The lane reports a refusal only when it was rendered
   on a fresh read or in a batch that landed; a refusal rendered on a cached base is discarded and
   the `decide` runs again on a fresh read (below).
3. It tolerates being run against a state its own mutation is already in, and resolves that
   without a wrong verdict: a `Refused{seen}` after an ambiguous attempt means the write may have
   landed and been superseded, and the caller re-decides on `seen`. The catalog's `beginRemoving`
   answers `AlreadyRemoving`, its creation steps answer `Superseded` and resume on their own row.
   This is what today's `readModifyWrite` already demands of every `decide` in its precondition-moved
   arm.
4. It issues no write through the lane to any key. Reads are fine, and a `decide` that reads holds
   the lane for those reads, which the lane bounds (below).

For `_ckpt`, `gc/state` or any other key, condition 3 is the one to audit before its first
`submit`; `publishCkpt`'s declines are the known case to check.

### The ticket {#the-ticket}

The lane mirrors `CasRefLedger::appendRefOps` on purpose. Nothing runs under `mutex` except the
steps marked.

```cpp
struct Item
{
    CasOperation & op;                       /// the caller's own admission
    Retry::Bound bound;                      /// bound at entry, so queue time spends the caller's window
    const Decide * decide;
    bool taken = false;                      /// guarded by `mutex`; set by a leader in the critical section that takes it
    bool done = false;                       /// guarded by `mutex`
    std::shared_ptr<const BatchOutcome> outcome;   /// guarded by `mutex`; set with `done` for a combined member
    std::exception_ptr error;                /// guarded by `mutex`; a held `decide` exception, delivered by its owner
};

/// One per batch, allocated by the leader before its write and shared by every member it settles.
struct BatchOutcome
{
    Outcome outcome;                         /// `Landed` carries the combined object's etag
    std::optional<Object> base;              /// the base the batch was decided from, for a held refusal's report
};
```

**Enter.** `submit` binds the policy to an absolute deadline on the engine's clock, as every verb
does at entry, so time in the queue spends the caller's window. Under `mutex`: push the item (the
one allocation; a failure leaves the lane untouched and propagates). The moment the push succeeds a
ticket guard is installed: on every exit, normal or by unwinding, it runs the leave step below. It
is `noexcept`, allocates nothing, and is the only thing that removes an item from the queue.

**Wait.** Loop:

1. Outside `mutex`: the engine's own `gate(0)` on the caller's operation (fence, then `Liveness`),
   then the caller's bound against the engine's clock, in that order so a lost fence, a stopped
   task or an exhausted lease is reported as itself. On any of the three the caller intends to
   leave, and leaving is decided under `mutex` in step 2. What this step touches of the caller's
   `CasOperation` is disjoint from anything a leader running this item's `decide` touches:
   `admitted_generation` and `liveness` are immutable, the fence and clock closures are the ones
   `CasRequests` already requires to be thread-safe, and the leader's nested reads write only
   `last_read_stop`.
2. Under `mutex`: if `done`, leave with the settled outcome. If step 1 decided to leave and the item
   is not `taken`, leave with the corresponding exception (`throwCasTransientUnavailable` for a lost
   fence or stopped task, `throwCasWriteRetryLater` for a deadline or lease, the errors
   `orThrow` renders for the same `GaveUp`s today). If step 1 decided to leave and the item is
   `taken`, fall through to the wait: its `decide` may be running on the leader's thread against
   this caller's stack, so it stays until `done` and returns what the leader settled. If the item is
   the first in the queue not `done`: set `holder_since_ms` and a local `entered_hold`, leave the
   loop as the holder. Otherwise `cv.wait_for(lock, slice)` with a short slice (the pattern of
   `recovery_cv.wait_for(lock, 200ms)` in the ledger), release the mutex, go to step 1. "A leader
   takes this item" and "this item's caller leaves" are decided under one mutex against one flag,
   so they are exclusive.

**Hold.** The batch, below, on the caller's thread and operation. While an operation is inside a
hold, its own or as a taken member whose `decide` is running, every read it issues is bounded by
the smaller of its own policy's bound and the hold's bound: the engine clamps `Retry::bind`'s
result by a per-operation deadline the lane sets and clears. A read the clamp refuses gives up as a
read gives up at its own deadline and surfaces as that `decide`'s exception. So a `decide` that
reads inside a hold ends inside the hold's window whatever policy its caller froze or defaulted,
and a hold never outlasts its holder's own deadline plus one attempt for any reason but a transport
that does not answer.

**Leave.** The ticket guard, in this order: under `mutex`, if `entered_hold` and the batch is
unsettled (the leader unwound), settle every taken member (a held error to its owner; every other
taken member `Refused{}` if nothing was sent and `Unknown` if something was, from the
`BatchOutcome` allocated before the write) and reset `holder_since_ms`; erase this item; if the
caller left on its deadline behind a held item, snapshot that item and `holder_since_ms`;
`cv.notify_all()`. After releasing the mutex: record the queue time and emit the log line if a
snapshot was taken, inside a catch-all in the `tryLogCurrentException` shape. The settlement and
the erase are one critical section, so a taken and unsettled member is never the first item not
`done`.

### The batch {#the-batch}

1. **Base.** The cache's object for the key, if present; else `op.read(key, policy)`, which is
   today's initial observation. A cached start is gated as a read would be, `gate(reservedFor(0, 1))`
   and `fits`, so a caller past its deadline, lease or fence never runs `decide` on the cache.
2. **The leader's decide** on the base. Bytes: the candidate. An exception: if the base was a
   fresh read, it propagates to the caller as today; if the base was the cache, the cache entry is
   dropped, the key is read, and `decide` runs once more on the read; what that run does is the
   result. Any exception, not a chosen class: a refusal, a decode failure on bytes another writer
   stored, an allocation failure inside the decode, all get the same one re-run on a fresh read,
   because a `decide` is pure and re-runnable by contract, and the only thing a refusal on a hint
   proves is that a hint is not a proof.
3. **Combine.** Under `mutex`: walk the queue behind the leader and take every item that is not
   `done` and whose `bound.deadline_ms` is not earlier than the leader's; stop at the first that is
   not. Copy the taken `shared_ptr`s into a vector, set each item's `taken`, allocate the batch's
   `BatchOutcome`, all in the same critical section; the mutex is a leaf that calls nothing. The
   batch is what is queued at that walk; nothing arriving later joins it, and there is no other
   cap. Release the mutex. For each taken item in order: first `fits(reservedFor(0, 2), leader's
   bound)`, and if the leader's remaining window no longer covers a write, stop taking and go to
   step 4 with what the chain has (untouched items stay in the queue, never `taken`). Then the
   item's own fence, `Liveness` and lease budget through its own operation's
   `gate(reservedFor(0, 2))`, the reservation its own write would make; lost, stopped or out of
   budget: settle it now with the exception its own submission would have thrown, and skip its
   `decide`. Otherwise `decide(Object{bytes = candidate, etag = base etag})`, with the member's
   operation clamped to the leader's bound. Bytes: the item contributes and the candidate is those
   bytes. An exception: held as that item's error, the candidate unchanged, the chain continues.
4. **Write.** One engine call, `op.replace(key, candidate, base.etag, policy)` (or `create` when the
   base was absence), under the leader's operation and frozen policy. The engine's verb is one
   logical write: it reissues ambiguous attempts, settles every refused precondition by a resolve
   read, and gates every attempt on the leader's fence. Its transport backoffs sleep inside the
   hold (a named tradeoff, below).
5. **Settle.** The engine's one result becomes the batch's one outcome, and every taken member is
   told the class the leader's ending belongs to, never a stronger one:
   - `Committed` → `Landed{etag}`. The cache stores the candidate under the key. Each contributor
     is gated once more on its own operation, `gate(0)`; lost: the exception its own submission
     would have thrown after a landed write (`throwCasTransientUnavailable`); otherwise
     `Landed{etag}` with the combined object's etag. Each held error is delivered to its owner, a
     serial-equivalent verdict now that the base and the prefix are durable.
   - `Conflict{seen}` → `Refused{seen}`. The cache stores `seen` if it is an `Object`. This class
     has the engine's two-sided meaning, a clean lost race or an ambiguous attempt that may have
     landed and been superseded; condition 3 of the contract is what makes forwarding it right.
     Each held error is delivered to its owner (its `decide` threw and contributed nothing, which is
     what its own submission would have done). Every contributor receives `Refused{seen}`.
   - `Refused` from the engine, or `GaveUp` with `sent_any == false` → `Refused{}`: nothing was
     sent; the cache entry is dropped. Held errors delivered; contributors receive `Refused{}` and
     read on their next submission.
   - `GaveUp` with `sent_any == true`, or an exception out of the engine after an attempt was
     sent → `Unknown`. The cache entry is dropped. Held errors delivered; contributors receive
     `Unknown`.
   The leader writes the `BatchOutcome` on its own thread, then under `mutex` assigns the pointer to
   every taken item, sets `done`, updates the cache, and does `cv.notify_all()`: no allocation and
   no copy under the mutex. A member copies `seen` out of the shared outcome on its own thread.

**Why this is the same as writing one at a time.** Serial execution would apply mutation 1, land
it, apply mutation 2 to what landed, land it. The batch applies mutation 2 to what mutation 1
produced before it lands, and lands both at once. For every member, its input is exactly what it
would have seen serially in queue order with no external write in between; and that no external
write came in between is what the conditional `PUT` against the base etag proves. Verdicts depend
only on their input, so a verdict rendered in a landed batch is the serial verdict, admission
included: an insert refused on its prefix for capacity is refused exactly where serial execution
would refuse it, whatever a later member deletes. The one difference is the batch that does not
land, where serial execution would have landed some prefix and the batch landed nothing; that is
why a refusal or a contribution rendered in a batch is delivered only when the batch lands, and
otherwise becomes `Refused` and is decided again. What a member's `Landed` means is therefore:
your mutation is in a landed object, as if you had committed alone immediately after the members
before you. This is the ref-log append lane's semantics, and it is what every writer already
accepts when its own commit is overwritten a millisecond later.

**Read your writes.** A member is told `Landed` only after the combined `PUT` landed, and at that
moment the cache holds the combined object. The caller's next submission starts from an object that
contains its mutation, and a plain `GET` returns it too (S3 and GCS are read-after-write consistent
for overwritten objects). What is not the member's own in its `Landed` is the etag: it is the
combined object's. No catalog caller uses it.

**Effects to name.** A member's `decide` runs on the leader's thread while its caller is parked;
the two threads touch disjoint parts of the member's operation (Wait, step 1), and the locals a
`decide` captures on its caller's stack are touched by the leader alone until the caller wakes.
Profile events and an exception's stack belong to the leader's thread. A `decide` that reads holds
the lane for those reads, clamped to the hold's bound. A transport fault inside the hold is repaid
by the engine's growing schedule, up to 5 s per reissue, inside the hold, and every other writer of
the key waits through it: accepted, since a store that throttles the pool's front writer is
throttling the pool, and visible in the keyed log line.

### The cache {#the-cache}

An LRU by key, bounded by bytes (`cache_budget_bytes`, a `PoolConfig` field with a default, in
the family of `manifest_decode_cache_bytes`; 0 disables it, which is what a `CasRequests` without a
pool gets). It holds, per key, the last object this pool knows: the candidate a `Landed` wrote, or
the object a resolve read saw after a `Refused`. It is dropped on `Unknown` and on a nothing-sent
`Refused`. It is never a source of truth: every write against it is conditional on its etag, and
every refusal rendered on it is discarded and re-rendered on a read (step 2). Those two sentences
are the whole safety argument, and they hold for any bytes, malformed ones included: a `decide`
that cannot decode a cached object throws, the lane reads, and the read either decodes or is the
real corruption.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait of its own. The caller binds its policy at entry, so time in the
queue spends the same window today's backoff sleeps spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own fence, `Liveness` and deadline, re-checked every slice | its window plus one slice |
| a combined member | the leader's write, then its own loop | the leader's deadline, no later than its own by the combine rule; then its own |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it, and every read inside the hold is clamped to it | its window plus one attempt timeout, unless the transport does not answer |

Nothing copies a deadline verdict from one operation to another: a member that leaves on its own
bound reports its own; a member that is taken is settled inside its own window.

**A holder whose transport does not answer.** The engine calls the transport synchronously and
cannot cancel it. A stalled attempt ends at the backend's attempt timeout; an attempt that
trickles, or holder code that never returns for any other reason, has no bound in the engine. Today
that traps only its own caller. In the lane it traps the key on this pool: every later writer waits
to its own deadline and reports retry-later until the holder returns, the connection is closed, or
the process restarts. Accepted as a release-level availability tradeoff for that case: nothing
lands that should not, no second write starts under a running first one, the keyed log line names
the holder and its hold time while it happens, and the fix belongs in the transport, a
whole-request deadline in the S3 client, which bounds today's single trapped writer too. Evicting
the holder was designed three times and rejected; not to be reopened without the transport
deadline first, at which point it is unnecessary.

### Lock order {#lock-order}

Verified for the production writers at brainstorm time:

| caller | locks held on entry to the catalog write |
|---|---|
| `CasRefLedger::namespaceLife` → `resolveNamespaceLife` (CREATE) | none; `ref_queue_mutex` scope closes before, `state_mutex` is taken after |
| `CasRefLedger::dropNamespaceImpl` → `cancelStalledCreating`, `beginRemoving` (DROP) | none; the queue lock scope closes before `removal_op` |
| `Gc` → `CatalogLifecycleReconciler` → erase | none; `authority_held` is a plain flag of the round thread |

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

Twenty-nine acquisition sites (`grep -c "lock(driver_mutex\|lock(runtime.driver_mutex"` on
`CasMountRuntime.cpp` at brainstorm time, one of them `try_to_lock`), every one in the rows above.
None reaches `CasRefLedger`, the catalog key, or any request on any plane, so no `driver_mutex`
holder ever waits for a ticket. The rule for future runtime code follows: nothing under
`driver_mutex` issues a write.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held to push, inspect or erase an item, to
take a batch, to settle a batch, and to read or update the cache. The fence, the `Liveness`
closure, the clock, the engine's verbs and every `decide` run with it released. The order is
therefore: caller (no locks) → `CasHotKeys::mutex` → nothing; and, with the ticket logically held,
caller → `driver_mutex` → nothing, caller → backend mutex → nothing. The one rule for future
callers, stated where the catalog API is documented: a submission is not made while holding a
ledger mutex, and no callback that runs while a ticket is held (`decide`, `Liveness`, a backend
hook) submits to the lane.

### Invariants {#invariants}

- INV-HK1. Per constructed `Pool` and key written through the lane, at most one conditional
  write is in flight at any moment, from any of its planes. The pool factory's bootstrap `create`
  of the catalog runs before the `Pool` exists and is outside this; it already resolves its one race
  by reading the winner's object.
- INV-HK2. Submissions of one pool to a key are applied in the order their holds were entered, a
  batch counting as its members in queue order; a member re-submitted after a `Refused` enters a
  new hold behind whoever arrived meanwhile.
- INV-HK3. The cache never changes the semantics of a conditional write: a stale entry costs one
  412 and one resolve read. A refusal a `decide` renders on a cached base is never reported; it is
  re-rendered on a read.
- INV-HK4. Every member of a batch sees, as its input, exactly the object it would have seen had
  the members before it landed one at a time with no external write between; a member's
  contribution or refusal is delivered only from a batch that landed, and otherwise becomes the
  class the leader's ending belongs to; a member's exception is delivered on every ending.
- INV-HK5. An item is removed only by its own guard; no item references a stack that may have
  unwound; "a leader takes this item" and "this item's caller leaves" are decided under one mutex
  against one flag and are exclusive; what a parked caller touches of its own operation is
  disjoint from what the leader touches.
- INV-HK6. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`; the guard
  neither allocates nor throws.
- INV-HK7. A member's own fence, `Liveness` and lease budget for one write are checked before its
  `decide` runs, and its fence again after the write it took part in landed; an item with an
  earlier deadline than the leader's is never combined; every read inside a hold is clamped to the
  hold's bound.
- INV-HK8. A member is told the class the leader's ending belongs to and never a stronger one.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item` with `done`, `error`, `outcome` under `CasHotKeys::mutex` |
| leader | first to find `leader_active` false; serves until its own item is done | the first item not `done`; one batch per hold |
| combine | compatible items into one ref-log transaction, `build_ops` run by the leader | queued submissions into one conditional `PUT`, `decide` run by the leader on the chained candidate |
| failure | a rejection is final per item | every member gets `Refused` and decides again in its own loop |
| exit | `completeOwnedItemsAndReleaseLeadership` | the ticket guard; settlement and erase in one critical section |
| wait | `cv.wait`, woken at every handover | `cv.wait_for` in slices, own fence and deadline re-checked |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror | replace-style object: ambiguity settled by the engine's resolve read |

The shared shape is deliberate. Extracting a common flat-combining component is possible and is not
part of this design.

## The callers {#the-callers}

### The catalog {#the-catalog}

`CasRefCatalog::casUpdateImpl` submits instead of calling `readModifyWrite`. Its `decide` is what
its `decide` for the engine is today: decode `base.bytes`, `throwIfAmbiguous`, `mutate`, `encode`,
capturing the encoded candidate as `written`; the four markers and the admission refusal are
thrown as they are today and reach their catchers as they do today. Around `submit` the loop is a
hand-written one under a policy frozen at entry, as the backend request contract prescribes for
such loops: `Landed` returns `written`; `Refused{seen}` pauses `Retry::conflictBackoff()` and
submits again (the cache holds `seen`, so the next hold starts from it); `Refused{}` submits again
(the next hold reads); `Unknown` is `throwCasWriteRetryLater`, what a `GaveUp{Unresolved}` is
mapped to today; the loop ends at the frozen deadline with the same retry-later. `casUpdate`,
`casAdmitEntry` and the five lifecycle functions do not change: their `mutate`s, markers and
outcomes are the same. `casAdmitEntry`'s duplicate-row `LOGICAL_ERROR`, thrown from the grammar
check on a cached base, is re-rendered on a read like every other exception and is the same
`LOGICAL_ERROR` on a read that shows the row; both functions are called only from tests. This is
the first change in `CasRefCatalog.cpp`.

The second is a catch that is missing today and that the queue wait would widen: `casUpdateImpl`
turns the engine's lost fence into `CatalogFenceMovedMarker`, which `casUpdate` translates and the
lifecycle functions catch, but `createNamespace` catches only `CatalogEntryAlreadyPresentMarker`
around step 1, so a creator whose fence moves during step 1 throws the bare marker, a
`std::exception` no caller names. Step 1 catches `CatalogFenceMovedMarker` and returns
`NamespaceCreationOutcome::FencedOut`, which `resolveNamespaceLife` already handles; `casAdmitEntry`
gets the translation `casUpdate` performs.

### The GC erase {#the-gc-erase}

`deleteCompletedRemovingAtSnapshot` is today a hand-written loop: refresh the leader's authority
(one `gc/state` read into the cached flag the operation's `Liveness` returns), check the exact row
on the snapshot, `replace` against the snapshot's etag, then a mandatory resolution read that is the
authority for what happened and the next attempt's base, then a growing pause. Its precondition is
the etag it last read; the row's exactness is what it protects, and on a refused precondition it
already moves to a newer etag and tries the same exact row again.

Put into a FIFO as it is, that loop would be refused at every hold under sustained load: its etag
is fixed before it enqueues and moved by every write queued ahead, and its resolution read and pause
run outside any ticket. So its attempt becomes a submission, the third change in
`CasRefCatalog.cpp`: the loop's body from "refresh authority" through the `replace` is one `submit`
whose `decide`, on whatever base the hold gives it, first runs `refresh_authority` and then
`op.admitted()`, throwing `CatalogFenceMovedMarker` if the leader's authority is gone (mapped to
`FencedOut` as today); then finds the exact observed row and throws `CatalogEntryMismatchMarker`
if it is absent or differs (the resolution read that follows tells `Deleted` from `EntryChanged`,
as today); otherwise returns the candidate with the row erased. After a `Landed` the mandatory
resolution read runs as today, remains the authority the reconciler feeds into its next selection,
and a `Landed` the read contradicts is retry-later as today. The reconciler,
`CatalogLifecycleReconciler`, and everything in `Gc/` are untouched: the outcomes, the result
struct and the `refresh_authority` closure are the same.

What this buys and costs. The erase queues, combines and starts from the cache like any other
writer, and its precondition is the pool's latest known object rather than a cut FIFO guarantees is
stale. Its authority refresh runs inside the hold, immediately before the `PUT`, a narrower window
than today's, where local work and the `PUT`'s flight sit between them; its `gc/state` read inside
`decide` is clamped to the hold's bound. What the flag protected and did not: the `PUT` is
conditional on the base's etag, so any catalog write by a successor moves it and a late erase is
refused; and the row is one a parent seal proved clean, so if nothing moved, the late erase is the
erase the successor would perform (a `Removing` life admits no new publication, so no hold can
reappear). A GC erase combined into a mount-plane leader's batch, or leading one with mount-plane
members, is gated per member like any other.

One gap exists today and stays: after an ambiguous attempt the engine may sleep and reissue the
same bytes gated only by the cached flag, without running `decide` again, so the refresh is per
decision and not per physical reissue. It is a BACKLOG item; the principled closure is a TTL on
the GC lease, which would make GC's writes time-fenced like the mount plane's.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff()` returns `backoff(1)`, uniform over [0, 200] ms inclusive (`backoff`
draws `thread_local_rng() % (ceiling + 1)` and doubles the ceiling from attempt 2 on). It is flat:
it does not grow with the writer's loss count, and it knows no dialect.

- `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`, for the sites that keep using
  them: on a `Conflict` whose inner write had no ambiguous attempt (a clean refused precondition,
  settled by the resolve read), a `pauseForConflict` that performs the same `gate` and `fits`
  checks as `pauseAndReissue` with `conflictBackoff()` and leaves `state.reissues` untouched. It
  records a new profile event, `CASRequestConflictPause`, and not `CASRequestReissue`; in these two
  loops the reissue counter then counts transport reissues only (`removeCurrent` and the read loops
  keep recording it as they do today).
- A `Conflict` whose inner write had an ambiguous attempt (`state.any_ambiguous`: a transport
  fault, a 429 among them, that the resolve read then settled as a lost race because the key had
  moved) keeps `pauseAndReissue` and its growing schedule: the fault is the signal that must pace
  the loop, and `writeLoop` already carries the distinction.
- The lane's callers pause `conflictBackoff()` between a `Refused{seen}` and their next submission,
  and nothing after a `Refused{}`, which follows an ending that sent nothing.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. On a key written through
the lane, conflicts arise only between pools and servers.

Half 2 is engine-wide and the lane covers the keys that write through it. The seven other
`readModifyWrite` sites keep their in-process contention (`publishCkpt` on `_ckpt` above all), and
on S3 their writers' retry pace goes from a schedule saturating at 5 s to a flat 200 ms. This is the
same fairness fix for the same starvation shape at smaller scale, and it raises the aggregate
attempt rate on a contended non-lane key by up to M × 5 per second for M writers: a refused
precondition is a 412 with no body, S3 has no per-object write limit and its per-prefix request
budget is thousands per second, and sustained pressure past it answers `SlowDown`, a transport
fault that takes the growing schedule. On GCS the store answers 429 the same way. Go/no-go for
`_ckpt`, alongside the `ref_catalog` one: the aggregate attempt rate on `_ckpt` keys over the
acceptance run must not exceed today's by more than the number of writers per key; if it does,
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
   `CasRefCatalog::casUpdateImpl` row becomes "the hot-key lane's `submit`, under one frozen
   `standard`, `conflictBackoff` between submissions", and the `deleteCompletedRemoving` row
   becomes "`submit`, decide = authority refresh plus exact-row erase; the post-write resolution
   read stays and stays authoritative".

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to departure, per item, on every exit.
- `CASHotKeyBatches`: landed batches with at least one member besides the leader.
- `CASHotKeyBatchMembers`: members per landed batch (sum; divide by `CASHotKeyBatches` for the mean).
- `CASHotKeyCacheStarts` and `CASHotKeyReadStarts`: how a hold obtained its base.
- `CASHotKeyCacheRefusalsReread`: refusals rendered on the cache and re-rendered on a read.
- `CASHotKeyBatchRefused` and `CASHotKeyBatchUnknown`: members told `Refused` or `Unknown` by a
  batch that did not land.

One log line, naming the key: a waiter that left on its deadline while a held item was at the
front, with that item's ticket and how long it has held. It is what makes a stuck holder visible
while it is stuck.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Lane level, in a new `gtest_cas_hot_keys.cpp` on `gtest_cas_requests.cpp`'s harness: counting
backend, write hooks, and a synchronized clock (a mutex-guarded `now` advanced by the test thread,
sleeps recorded under the same mutex), since the harness's `FakeClock` is single-threaded. The
`CasRequests` under test share one `CasHotKeys` with a cache. Each `decide` decodes a list of
tickets from `bytes` and appends its own, so order and membership are visible in the object.

1. Serialization, combining, cache. N threads submit; a write hook parks the first holder, and each
   further thread is released only after the test observes its item queued (`queueDepthForTest`),
   so arrival order is the release order. Assert: reads 1, writes 2 (the parked one and one
   combined batch), no `Refused`, the final object lists all tickets in arrival order, every caller
   received `Landed` with the batch's etag, and a following submission starts from the cache with
   zero reads (INV-HK1, HK2, HK4). A sibling installs a hook asserting `CasHotKeys::mutex` is not
   held whenever `decide`, a `Liveness` closure or a backend hook runs (INV-HK6).
2. Batch endings. A hook makes the combined `PUT` `Refused` by the store once: the leader receives
   `Refused{}`, every member `Refused{}`, and through their own loops they read, re-submit and land
   in the next batch; the final object carries every ticket once. A sibling makes the `PUT` lose to
   an external `CasRequests`: members receive `Refused{seen}` with the external object and their
   next hold starts from it (the cache holds `seen`). A sibling makes the `PUT` ambiguous and
   refuses the resolve read by the bound: the leader and every member receive `Unknown`, no member
   re-decides, and the object carries every ticket (INV-HK8). A sibling makes the `PUT` ambiguous
   and then has an external `CasRequests` overwrite the landed object before the resolve read: all
   receive `Refused{seen}`, and each member's re-decide through the real `beginRemoving` resolves
   `AlreadyRemoving`, never `EntryChanged`. A sibling makes the leader's engine call throw before
   any attempt (a token of another key) with members taken: every member receives `Refused{}` from
   the leader's guard, a member whose `decide` had thrown receives its exception, and a hook
   between the guard's settle and its erase asserts `queueDepthForTest` unchanged and no second
   `decide` ran (INV-HK4, HK5).
3. Refusals in a batch. A member's `decide` throws because the leader's chained change made its
   row differ from what it observed. Batch lands: the member receives that exception after the
   commit, the one a serial `EntryChanged` gives. Batch does not land: the member receives its
   exception anyway, contributed nothing, and its `decide` ran exactly once (a counter).
4. Member fences, and the take/leave race. A member resumed under a stale generation receives its
   fence exception with its `decide` never run; a member whose fence is tripped between the `PUT`
   and the post-commit check receives it while the object carries its ticket (INV-HK7). The race: a
   hook parks the leader between taking the members and running their `decide`s; the test trips one
   member's fence and advances another member's deadline past its bound; both stay parked
   (`queueDepthForTest` unchanged), the leader skips the first's `decide` and runs the second's,
   and each returns only after `done` (INV-HK5). Under ASan the same test with the `taken` check
   removed is the use-after-free the check exists to prevent.
5. Refusal on the cache, at the catalog level in `gtest_cas_ref_catalog.cpp`. The pool lands once.
   An external `CasRequests` changes a row. A caller reads fresh outside the lane and runs the real
   `beginRemoving` through a `CasRequests` with a cache: `Transitioned`, not `EntryChanged`, one
   lane read, `CASHotKeyCacheRefusalsReread` 1 (INV-HK3). The matrix: for each of
   `createNamespaceStep1`, `completeCreation`, `beginRemoving`, `reconcileStaleCreator`,
   `cancelStalledCreating` (their markers) and the admission refusal: one case where the cache is
   stale and the refusal would be false (positive outcome, as without a cache), and one where it is
   true on the read (the same outcome and error class as without a cache, one lane read, no write,
   plus whatever reads the function itself makes after a mismatch, as `beginRemoving` does).
   `casAdmitEntry` of a namespace whose cached row an external erase removed admits it, and of one
   that is present raises its `LOGICAL_ERROR`. A `decide` that throws decode corruption on a cached
   object reads once and, when the read decodes, lands; when the read is corrupt too, throws.
6. Fence during step 1. A creator queued behind a parked holder has its fence tripped before its
   turn: `createNamespace` returns `FencedOut`, nothing was written, `resolveNamespaceLife` reports
   retry-later. Tripped between its landed step-1 `PUT` and the post-commit check: `FencedOut`
   again, with the `Creating` row durable for a later reconciler. `casAdmitEntry` under a tripped
   fence throws `throwCasTransientUnavailable`'s class, never a bare marker.
7. External writer. A second `CasRequests` writes between two submissions of the first. Assert
   exactly one resolve read and one retry write, `Landed`, the landed body is the external
   writer's bytes plus this call's ticket, and the submission after that starts from the cache. A
   sibling has the external writer store malformed bytes, then repair them, between two
   submissions: the second re-reads and lands, no corruption verdict.
8. Cache rules. A store `Refused`, an `Unknown`, and a candidate above the budget each leave no
   entry: the next submission reads. Eviction: two keys whose objects exceed the budget together
   evict the older; the next submission on it reads. A `submit` refused at its gate before any read
   reports its own fence or deadline exception.
9. Waiters leave on their own. While the holder is parked inside its `decide`'s nested read: a
   waiter whose deadline passes leaves with retry-later carrying its own source and the keyed log
   line naming the holder; a waiter whose fence is tripped or whose `Liveness` flips leaves with
   `throwCasTransientUnavailable`'s class; a waiter whose lease budget runs out leaves with
   retry-later. The item is gone in every case. A waiter whose deadline passes in the same slice the
   holder leaves never enters hold, never runs `decide`, reads nothing.
10. The GC erase. The real `deleteCompletedRemovingAtSnapshot`, on an open-plane `CasRequests`
    sharing the lane, with a cached-flag `Liveness` and a counting `refresh_authority`, is called
    while a mount-plane holder is parked with one more mount writer queued: it waits, is combined
    into or led after that batch, its `decide` ran `refresh_authority` once per hold and inside it,
    the erase lands in one `PUT`, the mandatory resolution read follows, and the call returns
    `Deleted`; the mount plane's next submission starts from the cache. A sibling keeps N mount
    writers arriving for the erase's whole window and asserts `Deleted` at its first or second
    hold, never exhausting `kMaxCatalogCasAttempts`. A sibling flips the flag to false inside
    `refresh_authority`: the `decide` throws the fence marker, nothing is sent, the reconciler sees
    `FencedOut`. A sibling has an external `CasRequests` change the row between enqueue and hold:
    the `decide` sees the mismatch on the fresh base, nothing is sent, and the resolution read tells
    `Deleted` from `EntryChanged` as today.
11. Stalled and throttled holders. The holder is parked inside a `PUT` that does not return; the
    clock is advanced past every waiter's deadline: each waiter leaves with its own retry-later
    and the log line, no second write started, only the holder's item remains, and when the `PUT`
    is released the holder lands, the cache holds its candidate, and the next writer starts from
    it. A hook throttles the leader's `PUT` with a transport fault K times: the waiters stay queued
    through the growing backoff and the batch then lands.
12. Half 2, parameterized over `readModifyWrite` and `readModifyWriteOnPresence` on a key not
    written through the lane. K clean lost races: `CASRequestConflictPause` K, `CASRequestReissue`
    0, every sleep within [0, 200] ms. K races each preceded by a 429-class fault:
    `CASRequestReissue` K, `CASRequestConflictPause` 0, the growing schedule.

Pool level, through the ledger:

13. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, catalog `PUT`s fewer than 2N (steps 1 and 3 combine across callers), and
    exactly one lane base read (`CASHotKeyReadStarts == 1`; the callers' own pre-check reads are
    outside the lane and not counted).
14. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
on a `CasRequests` without a pool and therefore without a cache, and must stay green with the same
request counts. That is the check that the catalog API and its request shape were not touched.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Writing `_ckpt` and `gc/state` through the lane: a BACKLOG item, gated on condition 3 of the
  `decide` contract being checked for `publishCkpt`'s declines and the lease machine.
- Spacing the lane to a store's documented per-object rate: a BACKLOG item in the `gcs` lane,
  gated on its acceptance run showing 429s on `ref_catalog`.
- The GC erase's authority gap across the engine's internal reissue, and its principled closure, a
  TTL on the GC lease: a BACKLOG item, existing today.
- A whole-request deadline in the S3 transport, which makes a non-answering attempt end on its own
  and closes the accepted availability tradeoff: a BACKLOG item, and the only planned answer to
  that case.
- Remembering the resolve read's object after a `Refused` is done; remembering an `Unknown`'s
  is not, and a `decide`-side budget for a nested read the clamp refuses is a BACKLOG note if the
  acceptance run shows reconciliations failing that way.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.

## Revision history {#revision-history}

Revision 1 was brainstormed in chat. Revisions 2 to 15 were fourteen rounds of review by `codex`
(`gpt-5.6-sol`, high) that removed combining, the GC erase and memory for most sites, each on a
finding the author accepted instead of arguing; revision 16 put the three back with the argument
the reviews lacked (as-if-serial in queue order; a GC erase whose late landing is what the successor
would do; memory as a hint the `PUT` validates). Revisions 17 to 20 were four rounds of review by
`opus`, which found the safety core sound and stable from revision 18 on and kept finding one more
per-call field of `readModifyWrite`'s `WriteState` whose meaning broke once a call spanned several
holds and one write served several callers: `last_seen` older than a member's own read, `sent_any`
false for a member, `any_ambiguous` across holds, "`Committed` means your bytes", a spacing helper
with three call sites and two result families.

Revision 21 moves the lane above the engine, which is where a queue of writers belongs:
`readModifyWrite` is the right abstraction for one writer and the wrong seam for many. The lane
makes one engine call per hold and maps its one result to three of its own, so no engine state
spans holds; the cache is an LRU with two rules (conditional on its etag; a refusal on it is
re-rendered on a read) instead of seven; the `decide` contract is four conditions any hot object's
callers can be checked against; the `Generation` spacing is gone, since a store that finds the pool
too fast says so and the growing schedule already answers; and the catalog's markers travel as the
exceptions they are, because the lane re-runs any exception from a cached base rather than
classifying it. What the twenty revisions established and this one keeps: the FIFO ticket with the
`taken` handshake and the settle-and-erase critical section; combining as-if-serial with the
three-way settlement and refusals held until the batch lands; a member's fence checked before its
`decide` and after the landing; reads inside a hold clamped to the hold's bound; the GC erase as a
submission whose `decide` refreshes authority inside the hold; the flat conflict pause and the
growing schedule for a conflict that settled a fault; the `driver_mutex` audit; the stalled holder
as an accepted tradeoff with its fix in the transport; and the step-1 fence marker caught where it
was not.
