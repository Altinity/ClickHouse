---
description: 'Design for a per-pool write lane above the request engine for hot compare-and-swap objects (the ref catalog first): one FIFO per key shared by the pool''s planes, the queued read-modify-writes combined into one conditional PUT with as-if-serial semantics, an LRU of last known objects so the next write needs no read, a decide''s refusal reported only from a fresh read, and lost races between servers paced by a flat jitter. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 24, 2026-09-04. Brainstormed against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`, whose fix sketch
this document supersedes where they differ. The revision history at the end records twenty-one
earlier revisions and the two structural decisions that shaped this one: the lane sits above the
request engine and returns the engine's own result unchanged; and the GC erase, which the design
means to bring into the lane, is a follow-up with named prerequisites rather than a caller of this
change.

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
  pool's planes that write through the lane.
- N concurrent mutations of one hot object from one pool cost about one conditional `PUT` and at
  most one `GET`, not N of each, and never a 412 against each other; a lone mutation costs one
  `PUT` and no `GET` when the pool wrote the object last.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one, and for a store that answers 429.
- The lane serves any hot compare-and-swap object, not the catalog only: its `decide` sees bytes,
  it returns the engine's own `WriteResult`, and a caller chooses to write through it. The catalog
  is the first caller.
- No change to the catalog's API, to the semantics of any conditional write or of any result or
  verdict a caller sees, to the 90 s windows, to what any read returns, or to a single line of GC.
  A read issued inside a hold gives up earlier than today when the hold's window is shorter than
  its own (the clamp, below); that is the one behavioural change on the read side. In the request
  engine, five changes: a pointer carried by `CasRequests`; `friend class CasHotKeys` on
  `CasOperation`, for its private `gate`, `fits`, `reservedFor` and `Gate`; one optional clamp
  deadline on `CasOperation` honoured at its twelve `policy.bind` sites; the sentence in
  `CasOperation`'s header that declares it single-threaded, amended with the disjointness rule of
  Wait step 1; and Half 2's `pauseForConflict`. Two internal changes in `CasRefCatalog.cpp`, named
  below. Every existing catalog test stays green unchanged; the ones that count requests count
  writes, and a lone call's counts are today's.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- Pacing the lane to a store's documented per-object write rate. This is the owner's decision,
  recorded here after two reviews argued the other way: GCS documents about one mutation per
  second per object and says excess writes may be throttled; a store that finds the pool too fast
  answers 429, which is a transport fault and takes the growing schedule, and combining already
  makes one `PUT` carry every mutation that arrived meanwhile. The `gcs` lane's acceptance run
  measures 429s on the catalog; if they rise, spacing is a BACKLOG item there, designed against the
  measurement rather than the documentation.
- Bringing the GC erase into the lane in this change. It writes the same key today by racing the
  mount plane and keeps doing so; what it costs the lane and what bringing it in requires are in
  [the GC erase](#the-gc-erase).
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). They share a
  shape; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

**Half 1, the lane.** A per-pool component above the request engine, `CasHotKeys`, reachable
from any of the pool's three `CasRequests` planes through the operation that admits a write. A
caller submits a mutation of one key: a `decide` over the key's bytes. The lane takes a FIFO ticket
for the key, and when the ticket is at the front it obtains a base (the cache's last known object,
else a `GET`), runs the `decide`, takes the compatible submissions queued behind it, applies their
`decide`s to its candidate in queue order, and lands everything in one conditional write through
the engine, with the semantics "as if each had committed alone, in that order". The engine's
`WriteResult` is returned to the leader as it is, nothing reclassified, so a caller handles it
exactly as it handles today's, plus one rule, resubmit on `Conflict`. Every member is told, in the
same five alternatives, a class it is safe to act on: the leader's own when the batch's fate is the
member's fate, and `Conflict{NotObserved}`, "nothing applied, submit again", when the leader sent
nothing. A refusal a `decide` renders on a cached base is never reported; the lane reads and
decides again. `CasRefCatalog` writes through the lane.

**Half 2, conflict pacing.** In `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`,
for the sites that keep using them, a clean refused precondition is repaid after
`Retry::conflictBackoff()`, a flat uniform over [0, 200] ms, and no longer advances the
transport-fault counter. The lane's callers use the same pause between a `Conflict` and their next
submission. Two sentences of the backend request contract change.

## Half 1: the lane {#half-1-the-lane}

### Placement and reach {#placement-and-reach}

`Backend/CasHotKeys.{h,cpp}`, beside the engine and above it: the lane calls the engine's verbs
(`read`, `create`, `replace`) and the engine never calls the lane. One instance per pool, owned by
`Pool` as a member declared before its three `CasRequests`, so it is constructed before them and
destroyed after them. Each `CasRequests` is given a pointer to it at construction and exposes it
through `CasOperation::hotKeys()`. A `CasRequests` built without one (every existing test, the
offline tools, the pool factory's local bootstrap `CasRequests`) owns a private instance with no
cache, so a write through it costs today's `GET` plus `PUT` and every existing test's request counts
are unchanged.

```cpp
class CasHotKeys
{
public:
    explicit CasHotKeys(uint64_t cache_budget_bytes);   /// 0: no cache

    /// The caller's mutation of `key`: the candidate bytes to write over `base`, or a refusal by
    /// exception. `base` is absent when the key does not exist; a caller that refuses to
    /// bootstrap throws there. See "The contract of a decide".
    using Decide = std::function<String(const std::optional<Object> & base)>;

    /// One hold: base, decide, combine, one engine write, settle. Returns the engine's own result
    /// for that write, unchanged in class and content, and reports a `decide`'s refusal by
    /// propagating its exception, never from a cached or unlanded base.
    WriteResult submit(const String & key, CasOperation & op, const Retry & policy, const Decide & decide);

    /// Test seam: items in the key's queue, holder and settled members included.
    size_t queueDepthForTest(const String & key) const;

private:
    struct Item;   /// one queued submission, see "The ticket"
    struct Lane    /// one key; created on first use, lives as long as this object (one key today; erasure is a prerequisite for per-namespace keys)
    {
        std::deque<std::shared_ptr<Item>> queue;     /// guarded by `mutex`; the first item not `done` is the holder-to-be
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
starts from the cache when it can, lands several callers' mutations in one `PUT`, and returns the
engine's `Conflict` for the caller to retry by submitting again. The retry loop belongs to the
caller (`casUpdateImpl`'s), as the hand-written loops of the backend request contract already do,
under one frozen policy, with `Retry::conflictBackoff()` between submissions.

### The contract of a `decide` {#the-contract-of-a-decide}

A caller may write a key through the lane when every `decide` it will ever submit for that key,
and the operation it submits with, meet six conditions. The catalog's meet them today; they are the
entry condition for any other key.

1. Running it again on the same base yields the same result, and running it on a different base
   has no externally visible effect that a second run would double. The lane runs a `decide` more
   than once (on a cached base and then a fresh one; in a batch that did not land and then again),
   and the `decide` neither observes nor cares which run is reported. The catalog's `decide`s are
   pure functions of `base` plus idempotent reads.
2. It refuses by throwing, and its exception is the verdict serial execution would render on the
   same base. The lane reports a refusal only when it was rendered on a fresh read or in a batch that
   landed; a refusal rendered on a cached base is discarded and the `decide` runs again on a fresh
   read, and a refusal rendered in a batch that did not land is discarded and the caller submits
   again (below).
3. It tolerates being run against a state its own mutation is already in, and resolves that
   without a wrong verdict: a `Conflict{seen}` after an ambiguous attempt means the write may have
   landed and been superseded, and the caller re-decides on `seen`. The catalog's `beginRemoving`
   answers `AlreadyRemoving`, its creation steps answer `Superseded` and resume on their own row.
   This is what today's `readModifyWrite` already demands of every `decide` in its
   precondition-moved arm.
4. It issues no write through the lane to any key. A write from inside a hold would wait for the
   hold.
5. It issues reads only through the operation it was given, and none of those reads observes a
   key another member of the same batch may write: the as-if-serial argument below covers the
   lane's key alone, and a `decide` that reads a second key sees it as of before the batch, not
   between the members' serial commits. It reads `base->bytes` and never `base->etag` as an
   identity of those bytes: in a batch the chained `Object` carries the base's etag as a
   placeholder for bytes the store never held under it. Reads through the given operation are what
   let the lane bound them (below); a `decide` that builds its own operation escapes every bound
   the lane can set. The catalog's `decide`s decode `current->bytes` only and read, at most, a
   creator-fence key no catalog mutation writes.
6. The operation's `Liveness` closure, if it has one, is safe to call from two threads at once.
   For every taken member, not only one whose `decide` reads, the leader calls it through the
   member's gates and the member's own parked caller calls it every slice; `CasOperation::gate`
   calls `liveness()` directly, and the `std::function` object being immutable says nothing about
   what it closes over. The catalog meets it: `resolveNamespaceLife` admits with no closure
   (`mount_requests.resume(admitted_generation)`), and the ledger's other mount-plane closures read
   only atomics with acquire loads. And its answer is consulted, for a member, before the member's
   `decide` and after the landing, not before every physical attempt of the batch's write, which the
   leader's gate covers; a caller whose `Liveness` must be consulted before every attempt of its
   own write does not write through the lane.

For `_ckpt`, `gc/state` or any other key, conditions 3, 5 and 6 are the ones to audit before its
first `submit`; `publishCkpt`'s declines, `Gc::refreshAuthority`'s private operation and
`Gc::authority_held`'s plain `bool` are the known cases.

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
    std::optional<WriteResult> result;       /// guarded by `mutex`; this member's own settlement, written by the leader before `done`
    std::shared_ptr<const BatchOutcome> shared;    /// guarded by `mutex`; what the batch's members share
    std::exception_ptr held;                 /// the member's own `decide` exception, delivered by its owner only if the batch landed
};

/// One per batch, allocated by the leader before its write, shared by every member it settles:
/// the parts of a settlement that are the same for everyone and too large to copy per member.
struct BatchOutcome
{
    std::optional<Object> base;              /// the base the batch was decided from
    Observation seen;                        /// what the write's resolve read saw, for a member's `Conflict`
};
```

A member's `result` is its own because the settlement is per member in three places: a member
whose own fence or lease refuses it before its `decide` is settled at that moment, before any write
exists; a contributor of a landed batch is gated three ways on its own operation after the commit;
and a member's `GaveUp` carries its own bound's `Source`. The leader writes each member's `result`
on its own thread and publishes it with `done` under `mutex`; a member never computes its own
settlement after waking, so "gated once more after the commit" means what it says.

**Enter.** `submit` binds the policy to an absolute deadline on the engine's clock, as every verb
does at entry, so time in the queue spends the caller's window. Under `mutex`: push the item (the
one allocation; a failure leaves the lane untouched and propagates). The moment the push succeeds a
ticket guard is installed: on every exit, normal or by unwinding, it runs the leave step below. It
is `noexcept`, allocates nothing, and is the only thing that removes an item from the queue.

**Wait.** Loop:

1. Outside `mutex`: the engine's own `gate(0)` on the caller's operation (fence, then `Liveness`;
   the engine reports a false `Liveness` as `FenceLost` and deliberately does not tell the two
   apart), then the caller's bound against the engine's clock, in that order so a lost fence or a
   stopped task is not reported as a policy deadline, and an exhausted lease is reported as the
   lease. On any of the three the caller intends to leave, and leaving is decided under `mutex` in
   step 2. What this step touches of the caller's `CasOperation` is disjoint from anything a leader
   running this item's `decide` touches: `admitted_generation` and the `liveness` object are
   immutable, the fence and clock closures are the ones `CasRequests` already requires to be
   thread-safe, the `Liveness` closure is safe to call from two threads by condition 6 of the
   contract, and the leader's nested reads write only `last_read_stop` and the clamp field the
   leader itself owns for that time.
2. Under `mutex`: if `done`, leave with the settled result. If step 1 decided to leave and the item
   is not `taken`, leave with the `GaveUp` the engine would have returned for the same condition
   (`FenceLost`; `Deadline` with `Source::Lease`; `Deadline` with the bound's own source), which the
   caller handles as it handles the engine's. If step 1 decided to leave and the item is `taken`,
   fall through to the wait: its `decide` may be running on the leader's thread against this
   caller's stack, so it stays until `done` and returns what the leader settled. If the item is the
   first in the queue not `done`: set `holder_since_ms` and a local `entered_hold`, leave the loop
   as the holder. Otherwise `cv.wait_for(lock, slice)` with a short slice (the pattern of
   `recovery_cv.wait_for(lock, 200ms)` in the ledger), release the mutex, go to step 1. "A leader
   takes this item" and "this item's caller leaves" are decided under one mutex against one flag,
   so they are exclusive.

**Hold.** The batch, below, on the caller's thread and operation. While an operation is inside a
hold, its own or as a taken member whose `decide` is running, every read it issues is bounded by
the smaller of its own policy's bound and the hold's bound: `CasOperation` gains one optional
clamp deadline, set by the lane when the hold begins (and, for a member, by the leader before its
`decide` and cleared after) and taken into account at each of the engine's twelve
`policy.bind(owner.now_ms())` sites as a minimum. The field is written only by the thread that is
about to run the operation's `decide` and read only by that operation's own verbs, which the same
thread issues; the parked caller's step 1 does not read it. A read the clamp refuses gives up as a
read gives up at its own policy deadline, reported with `GaveUp::Source::Policy` (`Bound::lease_bound`
false), because the clamp is the hold's policy and not the member's lease, and surfaces as that
`decide`'s exception. So a `decide` that
reads inside a hold through its own operation, as `reconcileStaleCreator` and
`cancelStalledCreating` do through `isCreatorFenceTerminal`, ends inside the hold's window whatever
policy its caller froze or defaulted, and a hold never outlasts its holder's own deadline plus one
attempt for any reason but a transport that does not answer.

**Leave.** The ticket guard, in this order: under `mutex`, if `entered_hold` and the batch is
unsettled (the leader unwound), settle every taken member from the `BatchOutcome` allocated before
the write, as `GaveUp{Unresolved, sent_any = true}` (below) and reset `holder_since_ms`; erase this
item; if the caller left on its deadline behind a held item, snapshot that item and
`holder_since_ms`; `cv.notify_all()`. After releasing the mutex: record the queue time and emit the
log line if a snapshot was taken, inside a catch-all in the `tryLogCurrentException` shape. The
settlement and the erase are one critical section, so a taken and unsettled member is never the
first item not `done`.

### The batch {#the-batch}

1. **Base.** The cache's object for the key, if present; else `op.read(key, policy)`, which is
   today's initial observation, and which may find the key absent. A cached start is gated as a
   read would be, `gate(reservedFor(0, 1))` and `fits`, so a caller past its deadline, lease or
   fence never runs `decide` on the cache.
2. **The leader's decide** on the base. Bytes: the candidate. An exception: if the base was a fresh
   read, it propagates to the caller as today; if the base was the cache, the cache entry is
   dropped, the key is read, and `decide` runs once more on the read; what that run does is the
   result. Any exception, not a chosen class: a refusal, a decode failure on bytes another writer
   stored, an allocation failure inside the decode, all get the same one re-run on a fresh read,
   because a `decide` is re-runnable by contract, and the only thing a refusal on a hint proves is
   that a hint is not a proof. One exception to the re-run: if the validating read cannot be made
   because the caller's own gate refuses it (its fence, `Liveness` or lease), the exception
   rendered on the cache is delivered as rendered rather than replaced by the read's give-up. The
   caller is refused either way; delivering its own `decide`'s verdict keeps a typed outcome typed
   (a `CatalogFenceMovedMarker` thrown on the cache still becomes `FencedOut`, as today, instead of
   a read's transient exception), and a base-dependent verdict delivered to a fenced-out caller
   costs it nothing it would not have paid.
3. **Combine.** A hold whose base was absence takes no members: it is the bootstrap `create` of a
   key nobody has written, it is never contended, and a chained `Object` needs an etag. Otherwise,
   under `mutex`: walk the queue behind the leader and select every item that is not `done`, was
   submitted on the same `CasRequests` as the leader, is not under a `single_attempt` policy, and
   whose `bound.deadline_ms` is not earlier than the leader's; stop at the first that is not. Same
   plane, because the engine gates every physical attempt of the batch's write on the leader's
   operation alone: on one plane the leader's fence is the member's fence (the same atomics), a
   re-arm between the two admissions is caught by whichever generation is older at its own gate,
   and so the leader's per-attempt gate covers every member for the write's whole duration; a
   leader on another plane would carry a member's bytes past a fence the member's own engine would
   have refused. Not `single_attempt`, because `Retry::once` permits one attempt and the batch's
   write may reissue an ambiguous one; such an item runs alone, as its own holder. Allocation
   before taking, as the ledger's carve does: reserve the vector for the selected items and
   allocate the batch's `BatchOutcome` first; if either throws, nothing is taken and the exception
   is the leader's. Only then set each selected item's `taken`, all in the same critical section;
   the mutex is a leaf that calls nothing. The batch is what is queued at that walk; nothing
   arriving later joins it, and there is no other cap. Release the mutex. For each taken item in
   order: first `fits(reservedFor(0, 2), leader's bound)`, and if the leader's remaining window no
   longer covers a write, stop the chain and go to step 4 with what it has; every taken item the
   chain did not reach is settled, in its own `result`, as `Conflict{NotObserved}` (nothing of
   theirs applied, they submit again), since a taken item may not leave and must be settled by
   its leader. Then the item's own fence, `Liveness` and lease budget through its own operation's
   `gate(reservedFor(0, 2))`, the reservation its own write would make; `FenceLost` or `NoBudget`:
   write the `GaveUp` the engine would have returned, `sent_any = false`, into that item's own
   `result` now, and skip its `decide`. Otherwise `decide(Object{bytes = candidate, etag = base
   etag})`, with the member's operation clamped to the leader's bound. Bytes: the item contributes
   and the candidate is those bytes. An exception: held in the item, the candidate unchanged, the
   chain continues.
4. **Write.** One engine call: `op.replace(key, candidate, base.etag, policy)`, or
   `op.create(key, candidate, policy)` when the base was absence and the `decide` returned bytes
   for it, under the leader's operation and frozen policy. The engine's verb is one logical write:
   it reissues ambiguous attempts, settles every refused precondition by a resolve read, and gates
   every attempt on the leader's fence. Its transport backoffs sleep inside the hold (a named
   tradeoff, below).
5. **Settle.** The engine's result is the leader's, unchanged. `create` and `replace` return four
   of `WriteResult`'s five alternatives: `Declined` is produced only by the two read-modify-write
   verbs (their `return Declined{...}` sites are the only ones in `CasRequests.cpp`), so the lane
   never sees it and the enumeration below is complete. Every taken member is told, in its own
   `result`, a class it is safe to act on, never a stronger one than the leader's; a held exception
   is delivered only when the batch landed, because it was rendered on the chained candidate, a
   state that exists only if the batch lands, and in every other case the member is settled like a
   contributor and decides again on a real base:
   - `Committed`: the cache stores the candidate. Each contributor is gated once more on its own
     operation, `gate(0)`, three ways as `postCommit` does: `FenceLost` → `GaveUp{FenceLost,
     sent_any = true}`; `NoBudget` → `GaveUp{Deadline, Source::Lease, sent_any = true}`; otherwise
     `Committed{etag, attempts_sent = 0, resolved_by_read}` with the combined object's etag and the
     batch's `resolved_by_read`. Each held exception is delivered to its owner: the serial verdict,
     now that the base and the prefix are durable.
   - `Conflict{seen, attempts_sent}`: the cache stores `seen` if it is an `Object`, and is dropped
     otherwise. Every taken member receives `Conflict{seen, attempts_sent = 0}`. The class has the
     engine's two-sided meaning, a clean lost race or an ambiguous attempt that may have landed and
     been superseded; condition 3 of the contract is what makes forwarding it right, and the
     caller's next submission decides again on `seen` when the cache holds it and on a read when the
     resolve read saw nothing.
   - `Refused{store_error, message}`: the store refused the batch's bytes and nothing applied. The
     cache entry is dropped. Every taken member receives the same `Refused` with `attempts_sent = 0`,
     and every caller surfaces it as it surfaces the engine's today: the catalog's
     `throwCatalogWriteFailure` throws the store's own code and message at once, with no resubmit.
   - `GaveUp` with `sent_any == false`: nothing was sent, whatever the reason (the leader's fence,
     lease or deadline); the leader keeps its own `GaveUp`. Every taken member receives
     `Conflict{NotObserved, attempts_sent = 0}`: nothing of theirs applied, and their own loops
     pause and submit again, which is what a member whose leader never wrote should do. The cache
     entry is unchanged.
   - `GaveUp` with `sent_any == true`: the batch's fate is unknown; the leader keeps its own
     `GaveUp`, `FenceLost` after a landed `PUT` included. Every taken member receives
     `GaveUp{Unresolved, sent_any = true, last_seen, attempts_sent = 0}` with its own bound's
     `Source`. The cache entry is dropped.
   - An exception out of the engine's write: the leader's own, propagated. The lane cannot tell
     whether an attempt was sent (`WriteState` is the verb's local), so every taken member receives
     `GaveUp{Unresolved, sent_any = true}`, the conservative class. The one exception the engine
     throws before any attempt, `valueFor`'s `LOGICAL_ERROR` for a token of another key or backend,
     is unreachable from the lane, whose base etag is its own read's or its cache's for this key on
     this pool's backend. The cache entry is dropped.
   The leader writes the `BatchOutcome` and each taken item's `result` on its own thread, then
   updates the cache (outside `mutex`; `CacheBase` is its own leaf) inside a catch-all: `set`
   allocates and may throw, and a failed hint fill after a proven `Committed` must not become the
   leader's result, so the failure is logged, the entry is dropped, and the results already written
   stand. Then under `mutex` it assigns the shared pointer to every taken item, sets `done`, and
   does `cv.notify_all()`: no allocation and no copy under the mutex. A member copies `seen` out of
   the shared outcome on its own thread.

**Why this is the same as writing one at a time, for the lane's key.** Serial execution would apply
mutation 1, land it, apply mutation 2 to what landed, land it. The batch applies mutation 2 to what
mutation 1 produced before it lands, and lands both at once. For every member, its input from the
lane's key is exactly what it would have seen serially in queue order with no external write in
between; and that no external write came in between is what the conditional `PUT` against the base
etag proves, for that key and for nothing else, which is why condition 5 keeps a `decide`'s other
reads off any key a batch member may write. Verdicts depend
only on their input, so a verdict rendered in a landed batch is the serial verdict, admission
included: an insert refused on its prefix for capacity is refused exactly where serial execution
would refuse it, whatever a later member deletes. The one difference is the batch that does not
land, where serial execution would have landed some prefix and the batch landed nothing; that is
why a refusal or a contribution rendered in a batch is delivered only when the batch lands, and
otherwise becomes the class the leader's result belongs to and is decided again. What a member's
`Committed` means is therefore: your mutation is in a landed object, as if you had committed alone
immediately after the members before you. This is the ref-log append lane's semantics, and it is
what every writer already accepts when its own commit is overwritten a millisecond later.

**Read your writes.** A member is told `Committed` only after the combined `PUT` landed, and at
that moment the cache holds the combined object. The caller's next submission starts from an object
that contains its mutation, and a plain `GET` returns it too (S3 and GCS are read-after-write
consistent for overwritten objects). Two things in a member's view are not its own: the etag in its
`Committed` is the combined object's, and the candidate its own `decide` produced (which
`casUpdateImpl` returns as `written`) is a prefix state the store never held once later members
followed. No production caller reads either: the lifecycle functions discard `written`, and
`casUpdate` and `casAdmitEntry`, which return it, are called only from tests.

**Effects to name.** A member's `decide` runs on the leader's thread while its caller is parked;
the two threads touch disjoint parts of the member's operation (Wait, step 1), and the locals a
`decide` captures on its caller's stack are touched by the leader alone until the caller wakes.
Profile events and an exception's stack belong to the leader's thread; the pool's attempt counters
are the leader's alone, since members carry `attempts_sent = 0`. A `decide` that reads holds the
lane for those reads, clamped to the hold's bound. A transport fault inside the hold is repaid by
the engine's growing schedule, up to 5 s per reissue, inside the hold, and every other writer of
the key waits through it: accepted, since a store that throttles the pool's front writer is
throttling the pool, and visible in the keyed log line.

### The cache {#the-cache}

A `CacheBase` by key, constructed with the "LRU" policy name (its convenience constructor defaults
to "SLRU") and the two metrics named in the sketch, weighted by object bytes, bounded by
`cache_budget_bytes`, a `PoolConfig` field with a default in the family of
`manifest_decode_cache_bytes`; 0 disables it, which is what a `CasRequests` without a pool gets.
It is read and written outside the lane's mutex; its synchronization is its own and it calls
nothing. It holds, per key, the last object this pool knows: the
candidate a `Committed` wrote, or the object a resolve read saw after a `Conflict`. It is dropped
on `Refused`, on `GaveUp` after a send, and on an exception out of the write; it is unchanged by a
`GaveUp` that sent nothing. A candidate above the budget is simply not stored. It is never a source
of truth: every write against it is conditional on its etag, and every refusal rendered on it is
discarded and re-rendered on a read (step 2). Those two sentences are the whole safety argument,
and they hold for any bytes, malformed ones included: a `decide` that cannot decode a cached object
throws, the lane reads, and the read either decodes or is the real corruption.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait of its own. The caller binds its policy at entry, so time in the
queue spends the same window today's backoff sleeps spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own fence, `Liveness` and deadline, re-checked every slice | its window plus one slice |
| a taken member | the leader's write's completion, then its own loop | the leader's completion, which is the leader's deadline plus one attempt unless the leader's transport does not answer; then its own |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it, and every read inside the hold through the operation given is clamped to it | its window plus one attempt timeout, unless the transport does not answer |

Nothing copies a deadline verdict from one operation to another: a member that leaves on its own
bound reports its own; a taken member reports what the leader's result implies for it, with its
own `Source`.

**A holder whose transport does not answer.** The engine calls the transport synchronously and
cannot cancel it. A stalled attempt ends at the backend's attempt timeout; an attempt that
trickles, or holder code that never returns for any other reason, has no bound in the engine. Today
that traps only its own caller. In the lane it traps the key on this pool: every waiter leaves at
its own deadline with retry-later, and every taken member waits for the holder's completion, past
its own deadline and past its lease, until the holder returns, the connection is closed, or the
process restarts; nothing lands under an expired lease, because the leader's fence gates the write.
Accepted as a release-level availability tradeoff for that case: nothing lands that should not, no
second write starts under a running first one, the keyed log line names the holder and its hold
time while it happens, and the fix belongs in the transport, a whole-request deadline in the S3
client, which bounds today's single trapped writer too. Evicting the holder was designed three
times and rejected; not to be reopened without the transport deadline first, at which point it is
unnecessary.

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

Twenty-nine acquisition sites (`grep -c "lock(driver_mutex\|lock(runtime.driver_mutex"` on
`CasMountRuntime.cpp` at brainstorm time, one of them `try_to_lock`), every one in the rows above.
None reaches `CasRefLedger`, the catalog key, or any request on any plane, so no `driver_mutex`
holder ever waits for a ticket. The rule for future runtime code follows: nothing under
`driver_mutex` issues a write.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held to push, inspect or erase an item, to
take a batch, and to publish a batch's settlement. The cache is not touched under it (`CacheBase`
has its own internal synchronization, calls nothing, and is accessed with the lane's mutex
released). The fence, the `Liveness` closure, the clock, the engine's verbs and every `decide` run
with it released. The order is therefore: caller (no locks)
→ `CasHotKeys::mutex` → nothing; and, with the ticket logically held, caller → `driver_mutex` →
nothing, caller → backend mutex → nothing. The one rule for future callers, stated where the
catalog API is documented: a submission is not made while holding a ledger mutex, and no callback
that runs while a ticket is held (`decide`, `Liveness`, a backend hook) submits to the lane.

### Invariants {#invariants}

- INV-HK1. Per constructed `Pool` and key written through the lane, at most one conditional write
  is in flight at any moment from the operations that write through it. The pool factory's
  bootstrap `create` of the catalog runs before the `Pool` exists and is outside this; a writer that
  does not use the lane (the GC erase, until its follow-up) races the lane's writes as it races
  everything today.
- INV-HK2. Submissions of one pool to a key are applied in the order their holds were entered, a
  batch counting as its members in queue order; a member re-submitted after a `Conflict` enters a
  new hold behind whoever arrived meanwhile.
- INV-HK3. The cache never changes the semantics of a conditional write: a stale entry costs one
  412 and one resolve read. A refusal a `decide` renders on a cached base is never reported; it is
  re-rendered on a read.
- INV-HK4. Every member of a batch sees, as its input, exactly the object it would have seen had
  the members before it landed one at a time with no external write between; a member's
  contribution or refusal is delivered only from a batch that landed, and otherwise the member is
  told the class the leader's result belongs to and decides again.
- INV-HK5. An item is removed only by its own guard; no item references a stack that may have
  unwound; "a leader takes this item" and "this item's caller leaves" are decided under one mutex
  against one flag and are exclusive; what a parked caller touches of its own operation is
  disjoint from what the leader touches.
- INV-HK6. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`; the guard
  neither allocates nor throws.
- INV-HK7. A member's own fence, `Liveness` and lease budget for one write are checked before its
  `decide` runs, and its fence and lease again after the write it took part in landed; an item with
  an earlier deadline than the leader's is never combined; every read inside a hold through the
  operation given is clamped to the hold's bound.
- INV-HK8. Nothing the engine said is reclassified for the leader. A member is told, in the
  engine's own alternatives, a class it is safe to act on and never a stronger one than the
  leader's: the leader's class when the batch's fate is the member's, and `Conflict{NotObserved}`,
  "nothing applied, submit again", when the leader sent nothing.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item` with `done`, `held`, `outcome` under `CasHotKeys::mutex` |
| leader | first to find `leader_active` false; serves until its own item is done | the first item not `done`; one batch per hold |
| combine | compatible items into one ref-log transaction, `build_ops` run by the leader | queued submissions into one conditional `PUT`, `decide` run by the leader on the chained candidate |
| failure | a rejection is final per item | every member is told the leader's class and decides again in its own loop |
| exit | `completeOwnedItemsAndReleaseLeadership` | the ticket guard; settlement and erase in one critical section |
| wait | `cv.wait`, woken at every handover | `cv.wait_for` in slices, own fence and deadline re-checked |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror | replace-style object: ambiguity settled by the engine's resolve read |

The shared shape is deliberate. Extracting a common flat-combining component is possible and is not
part of this design.

## The callers {#the-callers}

### The catalog {#the-catalog}

`CasRefCatalog::casUpdateImpl` submits instead of calling `readModifyWrite`. Its `decide` is
today's: absent base → `throwMandatoryCatalogAbsent`; decode `base->bytes`, `throwIfAmbiguous`,
`mutate`, `encode`, capturing the encoded candidate as `written`. The four markers and the
admission refusal are thrown as they are today and reach their catchers as they do today. Around
`submit` the loop is a hand-written one under a policy frozen at entry, as the backend request
contract prescribes for such loops:

```cpp
const Retry policy = op.freeze(Retry::standard());
for (;;)
{
    WriteResult result = op.hotKeys().submit(key, op, policy, decide);
    if (std::holds_alternative<Conflict>(result))
    {
        op.pause(Retry::conflictBackoff());   /// the flat pause; the next hold starts from `seen` when the cache holds it
        continue;
    }
    /// unchanged from today: FenceLost -> CatalogFenceMovedMarker; anything else not Committed -> throwCatalogWriteFailure
    ...
    return std::move(*written);
}
```

The loop ends at the frozen deadline with today's retry-later, through the same `GaveUp{Deadline}`
`submit` returns when the wait or the write runs out of window, and has no attempt cap: today's
`readModifyWrite` has none either, and a cap of `kMaxCatalogCasAttempts` (100) at a flat mean pause
of 100 ms would end a writer after about ten seconds of cross-pool conflicts, well inside the
window this change exists to make fair. The bare `op.pause` can overshoot the deadline by at most
one pause, 200 ms, as the erase's loop already overshoots by one of its own; accepted. `casUpdate`,
`casAdmitEntry` and the five lifecycle functions do not change: their `mutate`s, markers and
outcomes are the same.
`casAdmitEntry`'s duplicate-row `LOGICAL_ERROR`, thrown from the grammar check on a cached base, is
re-rendered on a read like every other exception and is the same `LOGICAL_ERROR` on a read that
shows the row; both functions are called only from tests. This is the first change in
`CasRefCatalog.cpp`.

The second is a catch that is missing today and that the queue wait would widen: `casUpdateImpl`
turns the engine's lost fence into `CatalogFenceMovedMarker`, which `casUpdate` translates and the
lifecycle functions catch, but `createNamespace` catches only `CatalogEntryAlreadyPresentMarker`
around step 1, so a creator whose fence moves during step 1 throws the bare marker, a
`std::exception` no caller names. Step 1 catches `CatalogFenceMovedMarker` and returns
`NamespaceCreationOutcome::FencedOut`, which `resolveNamespaceLife` already handles; `casAdmitEntry`
gets the translation `casUpdate` performs.

### The GC erase {#the-gc-erase}

`deleteCompletedRemovingAtSnapshot` does not write through the lane in this change. It stays the
hand-written loop it is today: refresh the leader's authority (one `gc/state` read, through an
operation of its own, into a cached flag the erase operation's `Liveness` returns), check the exact
row on the snapshot, `replace` against the snapshot's etag, a mandatory resolution read that is the
authority for what happened and the next attempt's base, a growing pause. It races the lane's
writes as it races every writer today: the loser's precondition is refused and settled by its
resolve read, and a GC erase that wins leaves the lane's cache stale, so the pool's next catalog
submission pays one 412 and one resolve read for it, then proceeds. GC erases are rare, one per
removed namespace per round.

Bringing it in is the right follow-up, and its shape is known: the loop's body from "refresh
authority" through the `replace` becomes one `submit` whose `decide` refreshes authority, checks
`op.admitted()`, checks `throwIfAmbiguous` and the exact row, and returns the erased candidate; the
mandatory resolution read after a `Committed` stays and stays authoritative for the reconciler's
next selection. Five prerequisites, each a finding of the review that separated it from this
change: `Gc::authority_held` becomes `std::atomic<bool>`, because a `decide` that refreshes it on
a leader's thread would race the parked GC caller's `Liveness` read (condition 1); the refresh
reads `gc/state` through the erase's own operation, not a private one, so the clamp reaches it
(condition 5); the `decide` keeps `throwIfAmbiguous` and the absent-catalog refusal the loop has
today; `CompletedRemovingDeleteResult` needs the catalog cut the call ends on for `FencedOut` and
`EntryChanged`, which a `decide` exception carries nowhere, so the erase captures the base it was
shown as `casUpdateImpl` captures `written`, or re-reads; and the double refresh a cached-base
re-run causes is one extra `gc/state` `GET` inside the hold, to be named as its cost. Until then the
erase's authority window is today's, and one gap stays with it: after an ambiguous attempt the
engine may reissue the same bytes gated only by the cached flag, a BACKLOG item whose principled
closure is a TTL on the GC lease.

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
- The lane's callers pause `conflictBackoff()` between any `Conflict` from `submit` and their next
  submission, whether the resolve read saw a body or not.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. On a key written through
the lane, conflicts arise only between pools and servers, and against the GC erase until its
follow-up.

Half 2 is engine-wide and the lane covers the keys that write through it. The seven other
`readModifyWrite` sites keep their in-process contention (`publishCkpt` on `_ckpt` above all), and
on S3 their writers' retry pace goes from a schedule saturating at 5 s to a flat mean of 100 ms.
This is the same fairness fix for the same starvation shape at smaller scale. Its cost, as one
number: a writer whose round trip is negligible attempts up to about ten times a second, each
attempt with its resolve `GET`, so a contended non-lane key sees at most about 20 M requests per
second for M writers where today the growing schedule caps a writer near one attempt per 5 s. A
refused precondition is a 412 with no body, S3 has no per-object write limit and its per-prefix
request budget is thousands per second, and sustained pressure past it answers `SlowDown`, a
transport fault that takes the growing schedule; GCS answers 429 the same way. Go/no-go for
`_ckpt`, alongside the `ref_catalog` one: the predicted request rate on a contended `_ckpt` key is
about 20 M per second for M writers (an expectation, not a bound: a draw of 0 gives back-to-back
attempts), so the gate is twice that, 40 M per second per key for the M writers observed, and
429/`SlowDown` counts on those keys must not exceed today's; if either fails, writing `_ckpt`
through the lane is the answer, not a slower jitter.

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
   `standard`, `conflictBackoff` between submissions; every other result handled as today".

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to departure, per item, on every exit.
- `CASHotKeyBatches`: landed batches with at least one member besides the leader.
- `CASHotKeyBatchMembers`: members per landed batch (sum; divide by `CASHotKeyBatches` for the mean).
- `CASHotKeyCacheStarts` and `CASHotKeyReadStarts`: how a hold obtained its base.
- `CASHotKeyCacheRefusalsReread`: refusals rendered on the cache and re-rendered on a read.
- `CASHotKeyBatchNotLanded`: members told a non-`Committed` class by a batch that did not land.

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
`queueDepthForTest` counts every item in the key's queue, the holder and settled members included.

1. Serialization, combining, cache. N threads submit; a write hook parks the first holder, and each
   further thread is released only after the test observes its item queued (`queueDepthForTest`),
   so arrival order is the release order. Assert: reads 1, writes 2 (the parked one and one
   combined batch), no `Conflict`, the final object lists all tickets in arrival order, every caller
   received `Committed` with the batch's etag and `attempts_sent = 0` for members, and a following
   submission starts from the cache with zero reads (INV-HK2, HK4; INV-HK1 is test 11's). A sibling installs a hook
   asserting `CasHotKeys::mutex` is not held whenever `decide`, a `Liveness` closure or a backend
   hook runs (INV-HK6).
2. Settlement classes (INV-HK8). A hook makes the batch's write `Refused` by the store: the leader
   and every member receive that `Refused` with the store's code and message, and the catalog-level
   caller throws it at once with no resubmission. A hook makes the write lose to an external
   `CasRequests`: all receive `Conflict{seen}`, members' next holds start from `seen` (the cache
   holds it), each re-decides through the real `beginRemoving` and resolves `AlreadyRemoving`, never
   `EntryChanged`. A hook fails the resolve read at the transport after a clean refusal: all receive
   `Conflict{NotObserved}`, the callers pause `conflictBackoff` and their next holds read. A hook
   makes the write ambiguous and refuses the resolve read by the bound: the leader receives
   `GaveUp{Unresolved}`, every member `GaveUp{Unresolved, sent_any = true}` with its own `Source`,
   no member re-decides, and the object carries every ticket. The leader's fence is tripped before
   its write: the leader receives `GaveUp{FenceLost, sent_any = false}` and its catalog caller
   returns `FencedOut`; members receive `Conflict{NotObserved}` and land in the next batch. The
   leader's fence is tripped after its landed `PUT`: the leader receives
   `GaveUp{FenceLost, sent_any = true}`, members `GaveUp{Unresolved}`. The leader's engine call
   throws with members taken: every member receives `GaveUp{Unresolved}` from the leader's guard,
   and a hook between the guard's settle and its erase asserts `queueDepthForTest` unchanged and no
   second `decide` ran (INV-HK4, HK5).
3. Refusals in a batch. A member's `decide` throws because the leader's chained change made its
   row differ from what it observed. Batch lands: the member receives that exception after the
   commit, the one a serial `EntryChanged` gives. Batch does not land (an external `Conflict`): the
   exception is not delivered, the member receives `Conflict{seen}`, re-decides on `seen`, sees its
   row unchanged, and lands; its `decide` ran exactly twice (a counter) (INV-HK4).
4. Member fences, and the take/leave race. A member resumed under a stale generation receives
   `GaveUp{FenceLost}` with its `decide` never run; a member whose fence is tripped between the
   `PUT` and the post-commit check receives `GaveUp{FenceLost, sent_any = true}` while the object
   carries its ticket; a member whose lease budget is exhausted at that check receives
   `GaveUp{Deadline, Lease}` (INV-HK7). The race: a hook parks the leader between taking the
   members and running their `decide`s; the test trips one member's fence and advances another
   member's deadline past its bound; both stay parked (`queueDepthForTest` unchanged), the leader
   skips the first's `decide` and runs the second's, and each returns only after `done` (INV-HK5).
   Under ASan the same test with the `taken` check removed is the use-after-free the check exists
   to prevent. Siblings: a `Retry::once` submission behind a standard leader is not taken and runs
   as its own holder with exactly one attempt; a submission from a second `CasRequests` of the same
   pool is not taken by a leader of the first and waits its turn; a hook fails the `BatchOutcome`
   allocation at the take and asserts nothing was taken and the leader received the exception; a
   hook slows the first member's `decide` past the leader's remaining window and asserts the second
   member, taken but never reached, receives `Conflict{NotObserved}` and lands in the next batch; a
   hook makes `CacheBase::set` throw after a landed `PUT` and asserts the leader still receives
   `Committed` and the next submission reads.
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
   object reads once and, when the read decodes, lands; when the read is corrupt too, throws. A
   cached base whose key the store has since deleted: the read returns absence and the catalog's
   `decide` throws `throwMandatoryCatalogAbsent`, as today.
6. Fence during step 1. A creator queued behind a parked holder has its fence tripped before its
   turn: `createNamespace` returns `FencedOut`, nothing was written, `resolveNamespaceLife` reports
   retry-later. Tripped between its landed step-1 `PUT` and the post-commit check: `FencedOut`
   again, with the `Creating` row durable for a later reconciler. `casAdmitEntry` under a tripped
   fence throws `throwCasTransientUnavailable`'s class, never a bare marker. Tripped between a
   cached-base `decide` that threw `CatalogFenceMovedMarker` and the validating read: the marker is
   delivered, `beginRemoving` returns `FencedOut`, and no read was made.
7. External writer. A second `CasRequests` writes between two submissions of the first. Assert
   exactly one resolve read and one retry write, `Committed`, the landed body is the external
   writer's bytes plus this call's ticket, and the submission after that starts from the cache. A
   sibling has the external writer store malformed bytes, then repair them, between two
   submissions: the second re-reads and lands, no corruption verdict.
8. Cache rules. A store `Refused`, an `Unresolved`, an exception out of the write, and a candidate
   above the budget each leave no entry: the next submission reads. A `GaveUp` that sent nothing
   leaves the entry as it was. Eviction: two keys whose objects exceed the budget together evict the
   older; the next submission on it reads. A `submit` refused at its gate before any read reports
   its own `GaveUp`.
9. Waiters leave on their own. While the holder is parked inside its `decide`'s nested read: a
   waiter whose deadline passes leaves with `GaveUp{Deadline}` carrying its own source and the
   keyed log line naming the holder; a waiter whose fence is tripped or whose `Liveness` flips
   leaves with `GaveUp{FenceLost}`; a waiter whose lease budget runs out leaves with
   `GaveUp{Deadline, Lease}`. The item is gone in every case. A waiter whose deadline passes in the
   same slice the holder leaves never enters hold, never runs `decide`, reads nothing.
10. The GC erase outside the lane. The real `deleteCompletedRemovingAtSnapshot` on an open-plane
    `CasRequests` of the same pool runs while a lane holder is parked: it does not wait; whichever
    write is refused is settled by its resolve read; after an erase that wins, the pool's next
    submission pays exactly one resolve read and one retry write, lands, and the one after starts
    from the cache.
11. Stalled and throttled holders (INV-HK1). The holder is parked inside a `PUT` that does not
    return; the clock is advanced past every waiter's deadline: each waiter leaves with its own
    `GaveUp` and the log line, no second write started, only the holder's item remains, and when the `PUT` is
    released the holder lands, the cache holds its candidate, and the next writer starts from it. A
    taken member in the same setup stays parked past its own deadline and receives the batch's
    result when the `PUT` is released. A hook throttles the leader's `PUT` with a transport fault K
    times: the waiters stay queued through the growing backoff and the batch then lands.
12. The clamp. A member's `decide` issues a read through its own operation under a policy frozen
    later than the leader's; the read stalls with retryable failures; assert it gives up at the
    leader's deadline, not its own, and the member receives that exception only if the batch lands
    (INV-HK7).
13. Half 2, parameterized over `readModifyWrite` and `readModifyWriteOnPresence` on a key not
    written through the lane. K clean lost races: `CASRequestConflictPause` K, `CASRequestReissue`
    0, every sleep within [0, 200] ms. K races each preceded by a 429-class fault:
    `CASRequestReissue` K, `CASRequestConflictPause` 0, the growing schedule.

Pool level, through the ledger:

14. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, catalog `PUT`s fewer than 2N (steps 1 and 3 combine across callers), and
    exactly one lane base read (`CASHotKeyReadStarts == 1`; the callers' own pre-check reads are
    outside the lane and not counted).
15. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

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

- Bringing the GC erase into the lane, with the five prerequisites in [the GC erase](#the-gc-erase):
  a BACKLOG item, the first follow-up.
- Writing `_ckpt` and `gc/state` through the lane: a BACKLOG item, gated on conditions 3 and 5 of
  the `decide` contract being checked for `publishCkpt`'s declines and the lease machine.
- Spacing the lane to a store's documented per-object rate: a BACKLOG item in the `gcs` lane,
  gated on its acceptance run showing 429s on `ref_catalog`.
- The GC erase's authority gap across the engine's internal reissue, and its principled closure, a
  TTL on the GC lease: a BACKLOG item, existing today.
- A whole-request deadline in the S3 transport, which makes a non-answering attempt end on its own
  and closes the accepted availability tradeoff: a BACKLOG item, and the only planned answer to
  that case.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.

## Revision history {#revision-history}

Revision 1 was brainstormed in chat. Revisions 2 to 15 were fourteen rounds of review by `codex`
(`gpt-5.6-sol`, high) that removed combining, the GC erase and memory for most sites, each on a
finding the author accepted instead of arguing; revision 16 put the three back with the argument
the reviews lacked (as-if-serial in queue order; a GC erase whose late landing is what the successor
would do; memory as a hint the `PUT` validates). Revisions 17 to 20 were four rounds of review by
`opus`, which found the safety core sound and stable from revision 18 on and kept finding one more
per-call field of `readModifyWrite`'s `WriteState` whose meaning broke once a call spanned several
holds and one write served several callers. Revision 21 moved the lane above the engine, gave it
its own three-way result, and made the GC erase a caller.

Revision 22 folds in the fifth `opus` review, of revision 21, which confirmed that the structural
move removed the `WriteState` class of findings and found the remaining ones at the boundary. Going
down, the lane's three-way result was not a partition of the engine's five: `GaveUp{FenceLost}`
had no home and became an unpaced 90 s loop, the store's `Refused` became one too, a `Conflict`
whose resolve read saw nothing lost its pause, and the exception path asked for a fact the engine
does not export. `submit` now returns the engine's `WriteResult` unchanged, so `casUpdateImpl`
handles every result exactly as today plus one rule, resubmit on `Conflict`; members are told the
leader's class, never a stronger one, with `Conflict{NotObserved}` for a leader that sent nothing
and `Unresolved` for anything the lane cannot classify. Going up, the GC erase as a caller raced
its parked caller on a plain `bool`, escaped the clamp through a private operation, broke the
purity condition, and could not build the result struct the reconciler consumes; it is a follow-up
with those as its named prerequisites, and until then it races the lane as it races everything
today. The document also stated its core rule twice in incompatible forms; a held refusal is now
delivered only from a batch that landed, as the as-if-serial argument requires, and INV-HK4 and
test 3 say so. Minors: a `Decide` sees an absent base and the lane calls `create` for it; the
contract's purity condition is stated as what is needed and a fifth condition names the operation
reads must go through; the post-commit member gate is three-way; a taken member's bound is the
leader's completion and it is named in the stalled-holder tradeoff; `written` is a prefix state,
stated beside the etag; the engine changes are the four listed in Goals; the cache is a
`CacheBase`; the `_ckpt` go/no-go is one number with its derivation; `queueDepthForTest` is
declared with what it counts. What every revision since 16 kept: the FIFO ticket with the `taken`
handshake and the settle-and-erase critical section; combining as-if-serial; a member's fence
checked before its `decide` and after the landing; reads inside a hold clamped to the hold's
bound; the flat conflict pause and the growing schedule for a conflict that settled a fault; the
`driver_mutex` audit; the stalled holder as an accepted tradeoff with its fix in the transport; and
the step-1 fence marker caught where it was not.

Revision 23 folds in the sixth `opus` review, of revision 22, whose verdict was that the design is
sound enough to write an implementation plan from, with no finding against a conditional write, a
`Committed` report, a catalog verdict or memory safety, and every one of the previous round's
eighteen findings resolved or moot. Its majors, all in the text: INV-HK8 and the Overview claimed
nothing is reclassified while a member of a leader that sent nothing is told `Conflict{NotObserved}`,
so both now say what the design does; `Item` gets its own `result`, since three settlements are per
member and one of them precedes the write; the `Liveness` closure's concurrent-call requirement is
condition 6 of the contract with the catalog's evidence, not a corollary of purity; the
as-if-serial argument is scoped to the lane's key and condition 5 keeps a `decide`'s other reads
off keys a batch member may write; the chained `Object`'s etag is a placeholder no `decide` may
read, and a hold on an absent base takes no members. Minors: `Declined` is unreachable from
`create` and `replace`, said; the catalog's loop carries `kMaxCatalogCasAttempts`; the request-count
claim is scoped to writes and lone calls, with the four contended tests named; the cache is
accessed outside the lane's mutex, is constructed "LRU" with two metrics; `friend` goes on
`CasOperation` and its single-threaded sentence is amended; the clamp reports
`Source::Policy`; the read-side change is stated; the `_ckpt` gate is twice its prediction; a
stopped task is reported as `FenceLost`; INV-HK1 is test 11's.

Revision 24 folds in a `codex` review of revision 23, the first since the account's spend cap
lifted. Its critical was real and had been lost in the revision-21 rewrite: the engine gates every
physical attempt of the batch's write on the leader's operation alone, so a leader on another
plane could carry a member's bytes past a fence the member's own engine would refuse; a batch is
again confined to one `CasRequests`, with the argument why the leader's gate then covers every
member. Also lost in that rewrite and restored: a `single_attempt` submission is never combined.
New and fixed: allocation before taking, so a failed allocation takes nothing; a taken item the
chain never reaches is settled `Conflict{NotObserved}`; a cache fill that throws after a landed
`PUT` is logged and dropped, never the leader's result; the attempt cap the sixth `opus` round had
suggested for the catalog's loop is removed, since at a flat 100 ms mean it would end a writer in
about ten seconds and today's `readModifyWrite` has none; a cached-base exception whose validating
read the caller's own gate refuses is delivered as rendered, so `FencedOut` stays typed. Recorded
as decisions rather than fixes: no pacing to GCS's documented rate (the owner's), and the bare
conflict pause's overshoot of at most 200 ms. Lanes live as long as the object; erasure is a
prerequisite for per-namespace keys.
