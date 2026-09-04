---
description: 'Design for serializing one process''s mount-plane conditional writes to a hot CAS control object (the ref catalog first) inside the request engine, remembering the last committed object so consecutive writes need no read, reporting a decide''s refusal only from a proven base, and pacing lost races between servers with a flat jitter instead of the transport-fault backoff. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 15. Brainstormed 2026-09-04 against the measurements in
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
- Revision 7 makes the memory start opt-in per call site, not per key: `casAdmitEntry` reports a
  duplicate row as `LOGICAL_ERROR` through the grammar check, which stale memory could raise
  falsely, so only the five audited lifecycle decisions start from memory and the generic
  `casUpdate`/`casAdmitEntry` stay read-first. The wedge escape now detects a stuck transport
  attempt (longer than the backend's attempt timeout) rather than a long hold, since a `decide`'s
  nested reads legitimately run for their own windows; it never fires without an active holder.
  The `Generation` dialect gets per-key spacing of write attempts to the store's documented one
  mutation per second. Guarantees are stated per plane, and the tests cover the marker matrix.
- Revision 8 removes the wedge escape: its lifetime and spacing semantics kept growing, and the
  honest statement of the stalled-holder case (fail-stop, recovered by the attempt's own end)
  costs nothing. The memory has one source, an opted-in call's own committed candidate; a resolve
  read's bytes are never remembered, so stale memory can never carry another writer's malformed
  object into a corruption verdict. `reconcileStaleCreator` and `cancelStalledCreating`, which read
  inside `decide` after matching the row, do not opt in; the three hot decisions do. The
  `Generation` spacing sleep is budgeted like every engine sleep, removal verbs keep their
  exception-based refusals, and the GC authority window is described as it is.
- Revision 9 pins four details: a failure to copy the memory value after a commit is swallowed
  and the caller still receives its `Committed`; a memory start leaves `WriteState::last_seen` as
  `NotObserved` until a real read, so no `GaveUp` reports remembered bytes as observed; on the
  `Generation` dialect the conflict jitter is added on top of the one-second boundary rather than
  absorbed by it; and the GC erase's own internal reissue after an ambiguous attempt is named as an
  existing authority gap, outside this design and a prerequisite for ever ticketing the open plane.
  The memory has a size cap, since the catalog's enforced cap is the 256 MiB object cap, and three
  test expectations were corrected.
- Revision 10 makes the conflict pause dialect-aware: on `Generation` it is a flat one second plus
  the jitter, for every key, so a non-hot writer (`_ckpt`, `gc/state`) retries a lost race no
  faster than the store's documented rate without depending on 429s; on the other dialects it stays
  uniform(0, 200 ms). A declared hot key's lane lives as long as its `CasRequests`, so the spacing
  timestamp survives every forgetful ending, and `holder_active` is gone: the holder is the front
  ticket. INV-HK3 is qualified by the memory cap, callbacks run during a ticket's tenure may not
  write a hot key, and the two-plane jitter test asserts a range, not distinct instants.
- Revision 11 replaces the accepted stalled-holder outage with eviction. A holder's whole tenure
  is bounded by its own budget plus one attempt once every read it issues is under a policy frozen
  before its window (reconciliation already does this; cancellation gets the same one line), so a
  holder found past that bound can only be a trickling transport attempt, and a waiter evicts its
  ticket and takes over while the stuck request finishes on its own operation, untouched. One
  pre-attempt wait helper composes transport backoff, conflict pause and the `Generation` boundary
  into one sleep; queue time is recorded on every departure; eviction and expiry behind a holder
  are logged by key.
- Revision 12 removes eviction for good. Three reviews found three different holes in it (a
  promoted-but-unstarted front, a late byte-equality `Committed` after eviction, invariants that
  contradicted the overlap it permits), and each fix grew the state machine. The stalled-holder
  case is recorded instead as an explicit release-level availability tradeoff, with what bounds
  it in practice, how it is observed, and the fix that belongs in the transport. The bounded
  tenure, the one-sleep helper and the keyed observability stay.
- Revision 13 states the acceptance outright rather than pending a confirmation, and drops the
  invariant that contradicted it. It also closes a leak the lane would widen: `casUpdateImpl`
  turns a lost fence into a private marker that `createNamespace` never caught around step 1, so
  a creator fenced while queued would have thrown a bare `std::exception` instead of returning
  `FencedOut`; the same translation `casUpdate` already performs is applied there and in
  `casAdmitEntry`. The ticket guard is the sole remover of a ticket, and the spacing timestamp
  is an `optional`.
- Revision 14: a single-attempt policy never starts from memory (it cannot re-decide after the
  conflict a stale base would cause, so it observes as today); the pre-attempt sleep reserves
  the verb's own envelope count, not always two; the moment a ticket enters its hold is a local
  fact that decides whether its ending touches the memory; and the deadline and conflict tests
  are stated so they can run.
- Revision 15 corrects the lock-order section with an audit instead of a claim: the mount
  plane's interruptible sleep takes `CasMountRuntime::driver_mutex`, and the mount fence's
  `admit` calls the clock function, so a ticket is logically held across both; every holder of
  that mutex was read, and none reaches the ledger, the catalog key or a mount-plane write. Also:
  deterministic argument validation runs before the ticket is taken, the stalled-holder
  acceptance covers any non-returning holder code, the cache-fill test names its seam, the FIFO
  test stages its threads, and the holder timestamp is an `optional`.

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

- Per `CasRequests` (per `Pool`, on its mount plane), at most one conditional write in flight per
  hot key, in arrival order. A process with two pools over one bucket has two lanes, and they are
  external writers to each other.
- An opted-in `readModifyWrite` that follows a commit of the same plane starts from the committed
  object, not from a `GET`. N concurrent catalog mutations of one pool cost N sequential `PUT`s,
  one `GET`, and no 412 against each other.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, to any read path, or to the GC erase's authority window.
  Two internal changes in `CasRefCatalog.cpp` (how `casUpdateImpl` carries its markers, and the
  fence marker caught where it was not, both below) and one line in
  `CasRefLedger::dropNamespaceImpl` (the cancellation's terminality read under a frozen policy, as
  the reconciliation's already is). Every existing catalog test stays green unchanged.

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
lane remembers the last object this `CasRequests` committed, so a `readModifyWrite` whose call
site opted in can start from it instead of a `GET`. A `decide`'s refusal is reported only from a
proven base. `Pool` gives the predicate to `mount_requests` only, naming `refCatalogKey()`; the
catalog's API and GC do not change, `casUpdateImpl` changes only how it carries its own markers
and which of its callers opt into memory, and the ledger changes one line.

**Half 2, conflict pacing.** In `readModifyWrite` and `readModifyWriteOnPresence`, a clean
refused precondition is repaid after `Retry::conflictBackoff(dialect)`, a flat pause that does not
grow with the loss count (uniform(0, 200 ms); one second plus that on `Generation`), and no longer
advances the transport-fault counter. The GC erase loop keeps its pacing. Two sentences of the
backend request contract change.

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
    struct Lane    /// one hot key; created on first use, lives as long as this object
    {
        std::deque<uint64_t> tickets;                /// guarded by `mutex`; the front ticket is the holder
        std::optional<uint64_t> holder_since_ms;     /// guarded by `mutex`; when the front ticket entered its hold, for the log line; absent until then
        std::optional<uint64_t> last_write_attempt_end_ms;   /// guarded by `mutex`; for the Generation dialect's spacing; absent before the first attempt
        std::condition_variable cv;
        std::optional<Object> remembered;            /// guarded by `mutex`; the last candidate this plane committed
        uint64_t next_ticket = 0;                    /// guarded by `mutex`
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
};
```

The front ticket is the holder-to-be; there is no separate holder flag. A ticket reaches the
front only when every earlier ticket's own guard has erased it, and a ticket stays at the front
until its own guard runs, so "my ticket is the front" is the whole exclusion, and nothing but a
ticket's own caller ever removes it. Whether the front ticket has actually entered its hold (run
its checks and started the verb's body) is a fact its caller keeps in a local, `entered_hold`,
and reports to its guard; between the previous guard's erase and that moment the front ticket
holds nothing, and `holder_since_ms` is absent.

`CasRequests`'s constructor gains an optional `CasHotKeys::IsHot`. Every existing constructor call
(every test, the offline tools, the pool factory's local bootstrap `CasRequests`) passes none and
behaves byte-for-byte as today. `CasRequests` remains a class that writes no member after
construction: the lane has its own mutex, and `CasOperation` reaches it through its owner.

A lane is created the first time a write touches its key and lives as long as the `CasHotKeys`:
the predicate names the hot keys, today exactly one, and the lane's spacing timestamp must
survive endings that forget the memory. There is no eviction. The memory has a size cap, an
engine constant of 16 MiB (about a hundred thousand catalog rows at today's row size): a
committed candidate above it is not remembered, and the next opted-in call reads, which is
today's cost. The catalog's only enforced bound is the 256 MiB object cap, so without this cap one
remembered catalog could be that large. A per-key budget across many hot keys, and lane erasure,
become necessary when a per-namespace key such as `_ckpt` is declared hot; that declaration
brings them.

`Pool` passes the predicate "`key == Layout(config.pool_prefix).refCatalogKey()`" to
`mount_requests` and to no other plane.

### The open plane stays out {#the-open-plane-stays-out}

The GC erase runs on `openRequests()` under a `Liveness` closure that returns a cached flag, and
the reconciler refreshes that flag by one `gc/state` read at the top of every erase attempt of its
own loop. That refresh is the authority argument of the GC drain. Between it and the `replace`
today there is only local work, the exactness check, the copy, the erase and the encode of the
catalog, and no I/O and no wait; the engine's gate before the `PUT` consults the cached flag, not
the store. A ticket would put an unbounded wait in that gap, and a leader deposed during the wait
would still erase when its turn came. Mount-plane writers have no such flag: their fence is three
atomics the engine re-reads before every attempt, so a wait cannot stale it.

One gap in that argument exists today and is not this design's: the `replace` runs under
`Retry::standard()`, and after an ambiguous attempt its `writeLoop` may resolve, sleep and
reissue the same bytes under the same precondition, gated only by the cached flag. A leader
deposed between the refresh and that reissue erases under stale authority. The window is one
ambiguous transport outcome wide, the erase is one a parent seal already proved safe, and closing
it (a refresh before every physical attempt, which needs an engine hook the reconciler's
`refresh_authority` could fill) is a BACKLOG item and a prerequisite for ever ticketing the open
plane. This design neither widens nor narrows it.

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

What memory is and is not. `remembered` is the candidate an opted-in `readModifyWrite` of this
plane last committed: bytes this plane encoded, and the etag the store returned for them. It was
the durable state at the moment of that commit. It is not an observation of this call, and it is
not guaranteed newer than an observation a caller made outside the lane: `dropNamespaceImpl` reads
the catalog fresh, sees a `Live` row another server created, and calls `beginRemoving` on it; the
lane's memory may predate that row. A `decide` that refuses on such a base (the catalog's markers,
or a `nullopt`) would be reporting a verdict about a state the store has left, and with no write
sent nothing would catch it.

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
it is thrown. No signature moves.

One catch that is missing today is added, because the lane widens the window it covers.
`casUpdateImpl` turns the engine's `GaveUp{FenceLost}`, before an attempt or after a landed
`PUT`, into `CatalogFenceMovedMarker`; `casUpdate` translates that marker into
`throwCasTransientUnavailable`, and the lifecycle functions catch it and return `FencedOut`. But
`createNamespace` catches only `CatalogEntryAlreadyPresentMarker` around step 1, so a creator
whose fence moves during step 1 throws the bare marker, a `std::exception` no caller names, up
through `resolveNamespaceLife`. Today that needs the fence to move inside one short call; in the
lane it can move during the queue wait. Step 1 therefore catches `CatalogFenceMovedMarker` and
returns `NamespaceCreationOutcome::FencedOut`, which `resolveNamespaceLife` already handles, and
the public `casAdmitEntry` translates it exactly as `casUpdate` does. This is the second change
inside `CasRefCatalog.cpp`.

**Memory is opt-in per call site, not per key.** The ticket applies to every write on a hot key;
the memory start applies only where the caller asks for it, `readModifyWrite(key, decide, policy,
MemoryStart::Allowed)`, with `Never` the default. A site may opt in only after an audit shows that
every exception its `decide` can throw is either independent of `current` or carried as `nullopt`,
and that `decide` does no I/O after consulting `current`. The audit for the catalog:

- `createNamespaceStep1`, `completeCreation` and `beginRemoving` opt in. Each refuses only through
  a marker or the admission refusal, all captured, and does no I/O inside `decide` beyond
  `op.admitted()`, which reads atomics. These are the writes behind `CREATE TABLE` (steps 1 and 3)
  and `DROP TABLE`.
- `reconcileStaleCreator` and `cancelStalledCreating` do not opt in. After matching the row they
  call `isCreatorFenceTerminal`, a backend read, inside `decide`; on stale memory that read would
  run where today's fresh observation returns `EntryChanged` without it, and its transport failure
  would become the caller's result. They are rare (a stalled creation) and stay read-first.
- `casUpdate` and `casAdmitEntry` do not opt in. `casAdmitEntry` reports a duplicate row as the
  `LOGICAL_ERROR` of `encodeRefCatalog`'s grammar check, by design, and a row that GC has since
  erased would raise it falsely from memory where today's `GET` sees absence and admits. Both have
  no production caller.

Every one of them still takes a ticket. `casUpdateImpl` carries the choice as an internal
parameter its public callers set.

With that rule the safety of the lane does not depend on what a `decide` does with `current`. The
contract a participating site must meet is mechanical:

1. `decide` tolerates being run again on a different `current`, which `readModifyWrite` already
   requires of it (it re-decides on every `Conflict`), and tolerates a `nullopt` it rendered on
   memory being discarded and re-rendered. A verdict it wants re-rendered on stale memory is a
   `nullopt`, never a throw; a site that cannot promise that for every throw does not opt in.
2. No callback that runs while a ticket is held, `decide`, the `Liveness` closure, a backend hook,
   issues a write on any hot key of the same plane. A write from inside the holder would wait for
   the holder, and two holders entering each other's keys would wait forever. Reads are fine, as
   today, and a `decide` that reads holds the lane for that read (see
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
around its existing body. Nothing runs under `mutex` except the steps marked. A refusal while
waiting is reported in the verb's own result family: the `WriteResult` verbs return the `GaveUp`
named below; `remove` and `removeCurrent`, which return `Removal`, throw the same fence or
deadline exception their read-class loops throw today for the same condition.

**Enter.** Everything a verb validates deterministically today before touching the store runs
first, before any ticket: `valueFor` on a `replace`'s or `remove`'s etag (a `LOGICAL_ERROR` for a
token of another key or backend), and whatever else a verb refuses without a request. A caller
that would fail those checks today fails them identically, at once, and never waits. Then the
caller binds its policy to an absolute deadline on the engine's clock (`policy.bind(now)`), as
every verb does today at entry, so time spent waiting spends the caller's own window. Under
`mutex`: push `next_ticket` at the back of `tickets` (the one allocation of the
lane), then increment `next_ticket`. If the push throws, the number is not consumed, a lane the
lookup created for this call is erased again, and the exception propagates. The moment the push
succeeds, a ticket guard is installed: on every exit from here on, normal or by unwinding (the gate
calls `std::function`s, `wait_for` may throw), it erases exactly this ticket from `tickets` (a
`std::deque::erase` by value, no allocation) and does `cv.notify_all()`. It is `noexcept`. The
leave step below is this guard running on the normal path.

**Wait.** Loop, in this order:

1. Outside `mutex`: `gate(0)` on the caller's own operation, then the caller's own bound against
   the engine's clock, in that order so a lost fence or an exhausted lease is reported as itself
   and not as a policy deadline. `FenceLost`: return `GaveUp{FenceLost}`. `NoBudget`: return
   `GaveUp{Deadline, Lease}`. Bound passed: return `GaveUp{Deadline}` with the bound's own
   source. Returning runs the ticket guard, which is the only thing that ever removes the ticket.
   The gate runs outside the lane mutex because it may call the operation's `Liveness` closure,
   and the lane mutex must stay a leaf that calls nothing.
2. Under `mutex`: if the ticket is at the front, set `holder_since_ms` to the clock value read in
   step 1, set the local `entered_hold`, and leave the loop as the holder. Otherwise
   `cv.wait_for(lock, slice)` with a short slice (the pattern of
   `recovery_cv.wait_for(lock, 200ms)` in the ledger), release the mutex, and go to step 1. The
   clock is read in step 1, never under the mutex. A waiter that leaves on its deadline in step 1
   records that fact for its guard, which snapshots the front ticket and `holder_since_ms` under
   the mutex and logs them after releasing it (an absent `holder_since_ms` means the front has
   not entered its hold yet).

A ticket is promoted only after its checks passed in the same iteration. The engine's own
per-attempt gate and `fits` still run before anything is sent, as today.

**Hold.** Run the verb's body on the caller's own thread, with the caller's own operation, policy,
bound and result type, exactly as today, plus the memory step below. Every sleep the body takes
before a write attempt goes through one helper, `pauseBeforeAttempt(delay_owed, envelopes)`: it
takes the larger of the pause the loop owes (the growing transport backoff after a fault or an
ambiguous attempt, the flat conflict pause after a clean lost race, or nothing before a first
attempt) and, on the `Generation` dialect and a hot key, the time to the one-second boundary
after `last_write_attempt_end_ms` (snapshotted under `mutex`, computed outside it); runs the
engine's pre-sleep check, `gate(reservedFor(delay, envelopes))` and `fits`, with the envelope
count the verb reserves today, two for `writeLoop` (the attempt and its resolve read), one for
`remove`'s single request, and what `removeCurrent`'s own iteration reserves for its head plus
remove, so the sleep never starts unless the sleep and what follows it fit the operation's bound
and a verb that fits today still fits; sleeps once through the plane's interruptible sleep; and
re-checks the gate after. A refusal there is the engine's usual `GaveUp`. After every write
attempt on a hot key ends, successful or not, its end is recorded in
`last_write_attempt_end_ms`. Credential-refresh reissues and ambiguous-attempt reissues take the
same helper, so a hot `Generation` key never sleeps twice before one attempt and never issues one
inside the boundary.

**Leave.** The ticket guard, on every exit, holder or waiter, normal or unwinding, in this
order: under `mutex`, if `entered_hold` is set, move the prepared memory value into
`remembered` or clear it (a holder that unwinds clears) and reset `holder_since_ms`; if it is
not set, the memory is untouched, because a ticket that never held wrote nothing and learned
nothing; erase this ticket; if the caller left on its deadline behind a front ticket, snapshot
that ticket and `holder_since_ms`; `cv.notify_all()`; after releasing the mutex, record the
queue time to the profile event and emit the log line if a snapshot was taken. It is the sole
remover of a ticket and the sole notifier. It is `noexcept` and allocates nothing: the memory
value is prepared before the guard runs (below), and erasing from a `std::deque` does not
allocate.

A holder never waits for another ticket and never runs anyone else's closure: a ticket is settled
only by its own caller. No ticket outlives its caller's stack in the queue: the guard removes it
on every exit path, and nothing else references it.

### The memory {#the-memory}

**Memory rule.** After any write verb on a hot key ends its hold: a `Committed` of an opted-in
`readModifyWrite` remembers the candidate that call encoded and the etag the store returned for
it; every other ending of every verb that held forgets; a ticket that never entered its hold
leaves the memory as it found it. That is the only source. Bytes that came from the
store, a resolve read's `Conflict::seen` among them, are never remembered: another writer's
malformed object, remembered and then repaired in the store, would otherwise reach an opted-in
`decide` as a stale base and become a corruption verdict where today's observation sees the
repair. A `create` or `replace` that commits forgets too: its bytes are the caller's, not a
candidate this loop encoded, and the next opted-in call pays one read for it. An ordinary `read`
never reads or writes the memory, and absence is never remembered. A `GaveUp{FenceLost}` after a
landed `PUT` forgets: the memory is a hint, and forgetting is the fail-close direction.

**Preparing the value.** The `(bytes, etag)` pair is assembled outside the mutex, after
`postCommit` produced the `Committed` and before the leave guard: the bytes by moving the engine's
own candidate string, which the engine owns once `decide` returned it, and the etag by copying
from the `Committed` the caller will receive, which stays intact. The copy runs inside its own
`try`: if it throws, the exception is swallowed, the guard forgets, and the caller still receives
the original `Committed`, because its write landed and a failed cache fill is not an ending of the
write. A candidate above the memory size cap is not copied at all. The guard only moves.

**What a memory start is not.** It is not an observation. `WriteState::last_seen` stays
`NotObserved` until this call performs a real read (the `observe` of a verdict restart, or the
resolve read of a `Conflict`), so a `GaveUp` produced before any read, at the gate before the
inner write or at its `fits`, reports `NotObserved` and never the remembered bytes as something
this call saw.

**Where memory is used.** Only `readModifyWrite`, only for a call that passed
`MemoryStart::Allowed`, and only under a policy that may reissue: a `single_attempt` policy
(`Retry::once`) observes as today, because after the conflict a stale base would cause it may
not re-decide, and its one attempt must be decided on the current object. Its body gains one
optional input, the remembered object, used in place of its initial observation:

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
   resolve read is an observation, so what `decide` renders on it is a result; the object it saw
   drives the re-decide, as today, and is not remembered.

`create`, `replace`, `remove`, `removeCurrent`, `readModifyWriteOnPresence` and a
`readModifyWrite` that did not opt in never start from memory. They take a ticket, run as today,
and feed the memory rule.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait. Every catalog write already runs under `Retry::standard()`, 90 s
from the call, or a lease bound if shorter. The lane binds the policy at entry, before queueing, so
time in the queue spends the same window rather than adding to it, which is what today's backoff
sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own gate and its own deadline, re-checked every slice | its window plus one slice |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it, and its `decide`'s reads run under a policy frozen before its window | its window plus one attempt timeout, unless the transport trickles (below) |

Nothing copies a verdict from one operation to another: every `GaveUp` is the reporting operation's
own, with its own `Source`. A frozen policy (`op.freeze`) and a lease-bound policy
(`untilLeaseSafe`) behave as today: the bound the caller brought is the bound the wait and the
write are checked against.

A stopping plane or a lost fence reaches waiters through their own gate, checked every slice, and
reaches the holder through its own attempts' gates, as today.

**A holder's tenure is bounded by its own budget.** Everything a holder does through the engine
is under its own bound: no attempt and no sleep starts unless it fits, an attempt is reserved at
the backend's attempt timeout, and every read a catalog `decide` issues runs under a policy
frozen before the holder's window began (`resolveNamespaceLife` freezes one and passes it through
`reconcileStaleCreator` into `isCreatorFenceTerminal`; `dropNamespaceImpl` does the same for
`cancelStalledCreating` with the one line this design adds). So a holder whose transport answers
leaves by its deadline plus one attempt timeout, and so does every waiter behind it.

**A stalled holder: an accepted availability tradeoff.** What can outlive that bound is holder
code that does not return: a transport attempt that trickles rather than stalls, delivering
bytes slowly enough that no socket timeout fires (the engine cannot cancel it, and the S3
transport's timeouts are per socket operation, not per request); a `decide`, a backend hook or a
`Liveness` closure that never returns; or a lock one of them cannot get. Today any of those traps
only its own caller and its query. In the lane it traps the hot key on this plane: every later
catalog writer waits to its own deadline and reports the same typed retry-later the trapped
writer would, so `CREATE` and `DROP` on this pool fail with that error until the holder returns,
which for a transport attempt happens when the transport answers, when the connection is closed
by either side, or when the process restarts. Nothing in the lane ends it, because nothing in the
engine can, and the lane does not try to distinguish the causes: the keyed log line names the
holder and its hold time, and the cause is read from that holder's own stack.

This design accepts that, as a release-level availability tradeoff: for a trickling transport
attempt, and for that case only, the lane turns one trapped writer into a trapped catalog key on
this pool until the attempt ends. The reasons:

- Nothing lands that should not, and no second write is started under a running first one; the
  failure is retry-later, the same error the trapped writer alone reports today.
- It is observable while it happens: the keyed log line a waiter emits when it expires behind a
  holder names the holder's ticket and how long it has held.
- Eviction, a handover of the ticket to a waiter while the stuck request keeps running, was
  designed in three revisions and found incomplete in each (a promoted-but-unstarted front, a
  late byte-equality `Committed` after eviction, invariants contradicted by the overlap it
  permits). A complete eviction state machine costs more than a trickling transport is worth.
- The fix belongs in the transport: a whole-request deadline in the S3 client makes every attempt
  end on its own, bounds today's single trapped writer too, and is placed in
  [what this does not do](#placement-of-what-this-does-not-do) as the item that closes this
  tradeoff.

### Lock order {#lock-order}

Verified for the two mount-plane production writers at brainstorm time:

| caller | locks held on entry to the catalog write |
|---|---|
| `CasRefLedger::namespaceLife` → `resolveNamespaceLife` (CREATE) | none; `ref_queue_mutex` scope closes before, `state_mutex` is taken after |
| `CasRefLedger::dropNamespaceImpl` → `cancelStalledCreating`, `beginRemoving` (DROP) | none; the queue lock scope closes before `removal_op` |

Inside a write, while the ticket is logically held: the mount fence's `generation` and
`check_or_throw` read atomics; `admit` reads atomics and calls the clock function
(`config.boot_ms_fn`, `clock_gettime` in production, the synchronized test clock's own leaf mutex
in the lane tests); the in-memory backend holds its mutex only for the duration of one operation
and runs hooks without it; `decide`s call `op.admitted()` and issue reads (backend leaf). No
`decide` takes a ledger mutex. The mount plane's sleep, `CasMountRuntime::sleepInterruptibly`,
takes `driver_mutex` for a condition-variable wait, so the ticket is held across that mutex, and
the question is whether any holder of `driver_mutex` can ever enter the lane. Audit of every
acquisition in `CasMountRuntime.cpp` at brainstorm time:

| `driver_mutex` holder | what runs under it |
|---|---|
| `renewalWorkerMayRenew`, `renewalLive`, `renewalCancelled`, the `*ForTest` accessors | reads of driver state |
| `DriverLease::finish`, `admitRenewerCall`, `installRenewer`, `startRenewer`, `renewerReset` | driver state transitions, no I/O |
| `startBackgroundWorkers`, `stopBackgroundWorkers` | worker handles; joins happen with the lock released |
| `renewalLoop`, `remountLoop` | condition-variable waits and state reads; `remount_attempt()` runs after the lock is released |
| `lockTerminalPublication` and its three callers | atomic stores of terminal state; no backend request |
| `scheduleRemount` | a generation bump and a notify |

None reaches `CasRefLedger`, the catalog key, or any request on the mount plane, so no
`driver_mutex` holder ever waits for a ticket, and the ticket may be held across `driver_mutex`.
The rule for future runtime code follows: nothing under `driver_mutex` issues a mount-plane
write.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held only to push, inspect or erase a
ticket, to read or set the spacing timestamp, and to move or clear `remembered`. The gate, the
`Liveness` closure, the clock, the verb's body and every `decide` run with it released. The order
is therefore: caller (no locks) → `CasHotKeys::mutex` → nothing; and, with the ticket logically
held, caller (no locks) → `driver_mutex` (a condition-variable wait whose other holders never
enter the lane) → nothing, and caller (no locks) → backend mutex → nothing. The one rule this
imposes on future callers, stated where the catalog API is documented: a catalog
mutation is not entered while holding a ledger mutex, because the holder's `decide` may take
ledger-visible reads and a waiter must not hold what the holder needs. Today's callers hold nothing.

### Invariants {#invariants}

- INV-HK1. Per `CasRequests` and hot key, at most one conditional write is in flight at any
  moment. The mount plane is the only plane with hot keys, so per pool at most one mount-plane
  write is in flight per hot key; an open-plane write or another pool's write may overlap it, and
  the precondition orders those, as today.
- INV-HK2. Writes of one plane to a hot key are applied in arrival order.
- INV-HK3. An opted-in `readModifyWrite` that follows a `Committed` of an opted-in
  `readModifyWrite` of this plane on a hot key, with no other ending in between and a committed
  candidate within the memory cap, obtains its base from memory and issues no read for it. A lost
  race's resolve read is not a base read and is unchanged.
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair this plane encoded
  and committed, durable at the moment of that commit. It never changes the semantics of a
  conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A `nullopt` from `decide` reaches its caller as `Declined` only when it was rendered on
  an observation of that call; an exception from `decide` reaches its caller unchanged, whatever
  the base, and no write is sent after it.
- INV-HK6. A ticket is removed only by its own caller, on every exit path; no ticket references a
  stack that may have unwound.
- INV-HK7. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`, and the leave
  guard neither allocates nor throws.
- INV-HK8. A waiter is promoted to holder only in an iteration whose gate and bound checks passed,
  and a memory start passes the same one-attempt gate and `fits` an observation would.
- INV-HK9. On the `Generation` dialect, write attempts on a hot key from one plane are at least
  one second apart, and the spacing sleep starts only when it and the attempt after it fit the
  operation's bound.

A holder's tenure is not an invariant: it is bounded by its deadline plus one attempt timeout
whenever the transport answers within that timeout, and unbounded otherwise, which is the
accepted tradeoff above.

### Effects to name {#effects-to-name}

- A `decide` that issues reads holds the lane for those reads. `reconcileStaleCreator` and
  `cancelStalledCreating` call `isCreatorFenceTerminal` inside `decide`. `resolveNamespaceLife`
  already freezes one policy before its loop and passes it through `reconcileStaleCreator` into
  that read; `dropNamespaceImpl` today lets the cancellation's read take a fresh
  `Retry::standard()`, and this design gives it the same frozen policy (one line: the closure
  captures `cancel_op.freeze(Retry::standard())` and passes it). With both bounded, a holder's
  reads end inside its own window, which is what bounds every waiter's wait behind it by that
  same window; each waiter still leaves on its own deadline.
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

`Retry::conflictBackoff(Dialect)` returns, on the `ETag` and `Emulated` dialects, `backoff(1)`,
uniform(0, 200 ms) (`backoff` doubles from attempt 2 on); on the `Generation` dialect,
1000 ms plus `backoff(1)`. Both are flat: they do not grow with the writer's loss count. The
`Generation` value is the store's documented rate for one writer, applied to every key the loop
serves, hot or not: `_ckpt` and `gc/state` writers on GCS that lose a clean race retry no faster
than once a second each, without relying on a 429 the store only might send. The number has one
home next to the schedule it departs from, and the dialect comes from `Backend::dialect()`, which
the engine already consults.

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

**The `Generation` dialect's write budget.** GCS documents about one mutation per second per
object name, says excess writes may be throttled, and asks applications not to exceed the rate. A
429 is a transport fault and takes the growing schedule, but a lane that drains holders at `PUT`
latency is not paced by 429s it does not receive, and today's code is not either. So on the
`Generation` dialect the lane spaces write attempts on a hot key itself, across different holders:
no conditional write attempt on a hot key starts sooner than one second after the previous write
attempt on that key from this plane ended, successful or not. The wait is one sleep, not two: its
length is the larger of the time to that boundary and, when the attempt follows a clean
`Conflict`, the dialect's conflict pause of one second plus jitter, so the jitter is never absorbed
by the boundary and two servers whose boundaries coincide do not retry at the same instant
forever. The sleep goes through the plane's interruptible sleep after the engine's pre-sleep check
and with the attempt's own gate and `fits` checked again after it. The S3 and emulated dialects
get no spacing. This holds the documented budget by construction for one plane; two pools or two
servers on one object still share it, as they do today. Go/no-go for the `gcs` lane: the count of 429s on `ref_catalog` in `system.text_log` over
the acceptance run must not exceed today's, and the count of 412s must fall; both recorded in
`docs/superpowers/cas/BACKLOG/gcs.md`. Under sustained arrivals above one per second the queue
grows to the deadline on that dialect, which is the store's limit and not the lane's; combining is
the answer there and stays a follow-up.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a clean lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff(dialect)`, flat: uniform(0, 200 ms) on the `ETag` and `Emulated`
   dialects, one second plus that jitter on `Generation`, the store's documented rate for one
   writer; neither grows with the writer's loss count. The growing schedule belongs to transport
   faults, and to a conflict that settled one. Inside one plane a hot key never conflicts with
   itself: see the hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a
   settled fault, `backoff(attempt)` otherwise and after a fault; `deleteCompletedRemoving`'s
   `replace` cannot tell and keeps `backoff(attempt)`".
3. In the `readModifyWrite` section, one paragraph: on a hot key the initial observation may be
   the lane's remembered object, gated like an observation, and a verdict rendered on it is
   re-rendered on an observation before it is reported; pointer to this document.

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to departure, per ticket, recorded by the
  guard on every exit, including a waiter that left on its deadline or fence without holding.
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a `readModifyWrite` obtained its base.
- `CASHotKeyVerdictRestarts`: verdicts rendered on memory and re-rendered on an observation.

One log line, naming the key: a waiter that left on its deadline while a holder was at the front,
with the holder's ticket and how long it has held (`holder_since_ms`, snapshotted under the mutex
and logged after it). It is what makes a stuck holder visible while it is stuck, since the profile
event only counts departures.

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
   the first holder, and each further thread is released only after the test observes its ticket
   queued (a `queueDepthForTest(key)` accessor on `CasHotKeys`), so the arrival order is the
   release order and not the scheduler's. Assert: reads of the key 1, writes N, no write returned
   `Conflict`, the final object lists the tickets in that order (INV-HK1, HK2, HK3).
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
   `seen` intact, one read by the lane, no write; and at the catalog level, `EntryChanged` with no
   write and exactly the reads today's path makes (the lane's one, plus the read `beginRemoving`
   itself performs after a mismatch to tell `AlreadyRemoving` from `EntryChanged`).
4. External writer on the write path. A second `CasRequests` writes between two writes of the
   first. Assert the first's next `readModifyWrite` does exactly one resolve read and one retry
   write and ends `Committed`, that the committed body is the external writer's bytes plus this
   call's ticket (the fresh bytes and their etag stayed paired through the re-decide), and that
   the write after that starts from memory with zero reads. A sibling runs an opted-in call under
   `Retry::once` with memory present: it observes first (`CASHotKeyReadStarts` advances), its one
   attempt is decided on the observation, and a conflict forced after that observation returns
   `Conflict` with `seen` intact, after which the next call reads, because a resolve read's bytes
   are never remembered (INV-HK4). A second sibling has the external writer store malformed
   bytes, then repair them, between two opted-in calls of the first plane: the second call reads
   and commits, and no corruption verdict is raised.
4a. The marker matrix, in `gtest_cas_ref_catalog.cpp` through a hot `CasRequests`. For each of
   the opted-in decisions, `createNamespaceStep1` (already present), `completeCreation` (entry
   mismatch, fence moved) and `beginRemoving` (entry mismatch, fence moved), and the admission
   refusal: one case where memory is stale and the refusal would be false (an external
   `CasRequests` changed the row, or an open-plane erase freed capacity) asserting the same positive
   outcome a cold key gives, and one case where the refusal is true on the observation asserting
   the same outcome and error class a cold key gives, with one read and no write. Plus:
   `reconcileStaleCreator`, `cancelStalledCreating` and `casAdmitEntry` each read first on a hot
   key with memory present (one read, `CASHotKeyMemoryStarts` unchanged), and a `decide` that
   throws decode corruption on memory propagates that exception with no second read and no write
   (INV-HK5).
5. Forgetting. `Refused` from a hook, a thrown `writeLoop`, a `remove`, a committed `replace`,
   and a `GaveUp{FenceLost}` after a landed `PUT` each clear the memory: the next opted-in write
   reads first. A `remove` and a `removeCurrent` refused while waiting throw the same exception
   class their loops throw today for a lost fence or a passed deadline.
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
7b. Stalled holder. The holder is parked inside a `PUT` that does not return; the clock is
   advanced past every waiter's deadline. Assert: each waiter leaves with its own `GaveUp`, emits
   the keyed log line naming the holder's ticket and hold time, no second write was started, only
   the holder's ticket remains in the lane, and when the `PUT` is finally released the holder
   commits and the next writer proceeds normally. Sibling: `cancelStalledCreating` through
   `dropNamespaceImpl` issues its terminality read under the frozen policy (its deadline is not
   later than the outer one); with that read answering retryable transport failures and the clock
   advanced, the holder gives up at the outer deadline rather than a fresh 90 s later. A read that
   never returns is the accepted stalled-holder case, not this test.
7g. Fence during step 1. A creator queued behind a parked holder has its fence tripped before its
   turn: `createNamespace` returns `FencedOut`, nothing was written, and `resolveNamespaceLife`
   reports retry-later. Its fence tripped between its landed step-1 `PUT` and the post-commit
   check: `FencedOut` again, with the `Creating` row durable for a later reconciler, as today's
   `completeCreation` fence case leaves it. `casAdmitEntry` under a tripped fence throws
   `throwCasTransientUnavailable`'s class, as `casUpdate` does, never a bare marker.
7d. Cache fill failure. `CasHotKeys` carries a `memory_prepare_hook_for_test`, empty in
   production and invoked immediately before the post-commit etag copy, in the pattern of the
   runtime's `*_hook_for_test` seams; the test installs one that throws. Assert: the caller
   receives its `Committed` unchanged, the memory is empty, and the next opted-in call reads. A
   memory start whose inner write is refused at the gate before sending reports `GaveUp` with
   `last_seen == NotObserved`.
7c. Generation spacing. On a backend whose dialect is `Generation`, N writes through the lane:
   assert consecutive write attempts on the key are at least one second apart on the clock, that
   the sleeps went through the plane's sleep function, that a fence tripped during the spacing
   sleep ends the holder with `GaveUp{FenceLost}` and no attempt, and that a holder whose bound
   cannot fit the sleep plus one attempt (plain, frozen and lease-bound policies) gives up before
   sleeping (INV-HK9). On the `ETag` dialect the same test records no spacing sleep. A two-plane
   sibling on `Generation` has both planes lose a race at the same boundary and asserts each
   recorded sleep is one second plus a jitter within [0, 200 ms], drawn independently per plane (a
   range, since two draws may legitimately coincide). Siblings for the lane's lifetime: after a
   committed `replace`, a `Refused`, a `Declined` and an oversized commit, an immediately following
   write on the same key still waits to the one-second boundary. A non-hot key on `Generation`
   that loses K clean races records K sleeps of one second plus jitter each.
7e. Memory cap. A committed candidate of exactly the cap is remembered; one byte above is not,
   and the next opted-in call reads.
7f. One sleep per attempt on a hot `Generation` key. A credential-refresh reissue and an
   ambiguous-attempt reissue each record exactly one sleep before the next attempt, of length
   `max(transport backoff, boundary)`, attributed to the operation's own bound; a clean conflict
   records one sleep of `max(conflict pause, boundary)`. A `remove` whose bound fits the spacing
   delay plus one attempt but not two is admitted, as today; a `readModifyWrite` in the same
   position is refused, as today.
8. Open plane overlaps. A second `CasRequests` with no predicate (the open-plane shape) does a
   `replace` while the hot plane's holder is parked; it does not wait; whichever write is refused is
   settled by its resolve read; after an open-plane win the hot plane's next opted-in
   `readModifyWrite` pays exactly one resolve read and one retry write, commits, and the one after
   that starts from memory, because the committed retry candidate is this plane's own.
9. Half 2, clean conflict, parameterized over `readModifyWrite` and
   `readModifyWriteOnPresence`. One writer loses K clean races in a row (a hook mutates the key
   before every attempt). Assert `CASRequestConflictPause` advanced K times, `CASRequestReissue`
   not at all, and every recorded sleep is at most 200 ms on the `Emulated` dialect and within
   [1000, 1200] ms on a backend reporting `Generation`.
10. Half 2, conflict after a fault, parameterized over both verbs. A hook makes the attempt fail
    with a 429-class transport error and then moves the key before the resolve read, K times.
    Assert `CASRequestReissue` advanced K times and `CASRequestConflictPause` not at all. A
    sibling with K plain transport faults and no external write sees the same counters.

Pool level, through the ledger:

11. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, and exactly one lane base read (`CASHotKeyReadStarts == 1`). The callers'
    own pre-check reads are outside the lane and are not counted.
12. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Not tested: a genuine allocation failure inside the etag copy; the hook of test 7d exercises the
same path by throwing where the copy would, and the prepare-before-guard order is the argument
for the rest.

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
  the write. Motivation: the `Generation` dialect, where this design's one-second spacing bounds
  the lane to one mutation per second and only combining makes that second carry every mutation
  that arrived meanwhile.
- The GC erase's authority gap across its own internal reissue (see
  [the open plane](#the-open-plane-stays-out)): a BACKLOG item, existing today, closed by a refresh
  before every physical attempt through an engine hook.
- Ticketing the GC erase: a BACKLOG note, gated on the item above; the benefit is one avoided 412
  per GC erase.
- Remembering a resolve read's object, so that one external conflict costs one read rather than
  two: a BACKLOG note, gated on validating the bytes (a decode) before remembering them.
- A whole-request deadline in the S3 transport, which makes a trickling attempt end on its own
  and closes the accepted availability tradeoff above: a BACKLOG item, and the only planned
  answer to that case.
- Eviction of a stalled holder: rejected in revision 12 after three incomplete designs; not to be
  reopened without a whole-request transport deadline first, at which point it is unnecessary.
- Declaring the `_ckpt` keys hot, with the memory bound that many hot keys need: a BACKLOG item.
- Bounding the reads a catalog `decide` issues: done for both sites by this design and the
  existing reconciliation code; nothing remains.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.
