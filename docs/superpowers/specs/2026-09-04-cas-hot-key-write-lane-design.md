---
description: 'Design for a per-pool write lane on a hot CAS control object (the ref catalog first) inside the request engine: one FIFO per key shared by the pool''s planes, the queued read-modify-writes combined into one conditional PUT with as-if-serial semantics, the last committed object remembered so the next write needs no read, a decide''s refusal reported only from a proven base, and lost races between servers paced by a flat jitter. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 16, 2026-09-04. Brainstormed against the measurements in
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` and the BACKLOG items
`{#stateless-lane-wall-time-is-drop-table}` and `{#ref-catalog-cas-starvation}`, whose fix sketch
this document supersedes where they differ. The revision history is at the end; it matters,
because revisions 2 to 15 were fourteen review rounds that removed three things this revision puts
back with the argument the reviews lacked.

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

- Per pool and hot key, at most one conditional write in flight, in arrival order, from any of
  the pool's planes.
- N concurrent catalog mutations of one pool cost about one conditional `PUT` and one `GET`, not
  N of each, and never a 412 against each other. On S3 the win is latency; on a store that budgets
  one mutation per second per object (GCS) it is the difference between a queue that drains and
  one that grows to the deadline.
- A lost race against another server is repaid after a flat jitter that does not grow with the
  writer's personal loss count. The growing backoff stays for transport faults, including a
  conflict that settled one.
- No change to the catalog's API, to the semantics of any conditional write or of any verdict a
  `decide` renders, to the 90 s windows, to any read path, or to a single line of GC. Three small
  internal changes in `CasRefCatalog.cpp` and one line in `CasRefLedger::dropNamespaceImpl`, all
  named below. Every existing catalog test stays green unchanged.

Non-goals:

- Making the remembered object a source of truth. It is a hint: a stale hint costs one 412 and one
  resolve read on the write path, and one read on the verdict path, never correctness.
- A general write cache for every key. Participation is by a predicate the pool owns.
- Unifying this lane with the ref-log append lane (`CasRefLedger::appendRefOps`). They share a
  shape; extracting a common component is a separate decision.
- The other two `DROP TABLE` costs named in the BACKLOG item (part-commit round trips, `StackTrace`
  capture for expected 412s). They keep their placement there.

## Overview {#overview}

**Half 1, the lane.** A per-pool component of the request engine, `CasHotKeys`, shared by the
pool's three `CasRequests` planes. For a key the pool declares hot, every write verb takes a FIFO
ticket and runs only when its ticket is at the front. A `readModifyWrite` at the front also takes
the compatible `readModifyWrite`s queued behind it: it applies their `decide`s to its own candidate
in queue order and lands everything in one conditional `PUT`, with the semantics "as if each had
committed alone, in that order". If that `PUT` does not land, every member receives `Conflict`, which
is what losers receive today, and their own loops re-decide and re-queue. The lane remembers the
last object the pool committed, so the next `readModifyWrite` starts from it instead of a `GET`. A
`decide`'s refusal is reported only from a proven base. The GC erase takes a ticket like any other
write and changes nothing about how it decides.

**Half 2, conflict pacing.** In `readModifyWrite` and `readModifyWriteOnPresence`, a clean refused
precondition is repaid after `Retry::conflictBackoff(dialect)`, flat: uniform(0, 200 ms), or one
second plus that on the `Generation` dialect, and no longer advances the transport-fault counter.
The GC erase loop keeps its pacing. Two sentences of the backend request contract change.

## Half 1: the lane {#half-1-the-lane}

### Ownership and placement {#ownership-and-placement}

`Backend/CasHotKeys.{h,cpp}`, in the engine's own directory, because it is a property of how the
engine talks to one key. One instance per pool.

```cpp
class CasHotKeys
{
public:
    using IsHot = std::function<bool(const String & key)>;
    explicit CasHotKeys(IsHot is_hot);   /// an empty predicate: nothing is hot
    bool isHot(const String & key) const;
private:
    friend class CasOperation;
    struct Item;   /// one queued write, see "The ticket"
    struct Lane    /// one hot key; created on first use, lives as long as this object
    {
        std::deque<std::shared_ptr<Item>> queue;     /// guarded by `mutex`; the front item is the holder-to-be
        std::optional<uint64_t> holder_since_ms;     /// guarded by `mutex`; when the front entered its hold; for the log line
        std::optional<uint64_t> last_write_attempt_end_ms;   /// guarded by `mutex`; for the Generation dialect's spacing
        std::condition_variable cv;
        std::optional<Object> remembered;            /// guarded by `mutex`; the last candidate this pool committed
    };
    std::mutex mutex;
    std::unordered_map<String, Lane> lanes;
};
```

`CasRequests` takes a `std::shared_ptr<CasHotKeys>` in its constructor; without one (every existing
test, the offline tools, the pool factory's local bootstrap `CasRequests`) it owns a private
instance whose predicate is "nothing is hot", so its behaviour is byte-for-byte today's.
`CasRequests` remains a class that writes no member after construction: the lane has its own mutex.

`Pool` declares `std::shared_ptr<CasHotKeys> hot_keys` before `mount_requests`, `farewell_requests`
and `gc_requests`, so it is constructed before them and destroyed after them, and hands the same
pointer to all three. Its predicate is "`key == Layout(config.pool_prefix).refCatalogKey()`",
computed once. One lane per pool is the point: the GC erase on the open plane and the ledger's
writes on the mount plane are the writers that race today, and they must stand in one queue.

A lane is created the first time a write touches its key and lives as long as the `CasHotKeys`.
There is no eviction: the predicate names exactly one key, and the lane's spacing timestamp must
survive endings that forget the memory. The memory has a size cap, an engine constant of 16 MiB
(about a hundred thousand catalog rows at today's row size): a committed candidate above it is not
remembered and the next write reads, which is today's cost. The catalog's only enforced bound is the
256 MiB object cap. A per-key budget and lane erasure become necessary when a per-namespace key such
as `_ckpt` is declared hot; that declaration brings them.

### The ticket {#the-ticket}

Every write verb on a hot key, `create`, `replace`, `remove`, `removeCurrent`, `readModifyWrite`
and `readModifyWriteOnPresence`, does the following around one write attempt. Nothing runs under
`mutex` except the steps marked. A refusal while waiting is reported in the verb's own result
family: the `WriteResult` verbs return the `GaveUp` named below; `remove` and `removeCurrent`, which
return `Removal`, throw the fence or deadline exception their loops throw today for the same
condition.

**Item.** One queued write:

```cpp
struct Item
{
    CasOperation & op;                       /// the caller's own admission; its plane is `&op.owner`
    Retry policy;                            /// as passed; a `single_attempt` item is never combined
    Retry::Bound bound;                      /// bound at entry, so queue time spends the caller's window
    const DecideOnObject * decide = nullptr; /// a `readModifyWrite`'s decide; null for a single-verb ticket
    bool done = false;                       /// guarded by `mutex`
    std::optional<WriteResult> result;       /// guarded by `mutex`; set only for a combined member
};
```

**Enter.** Everything a verb validates deterministically today before touching the store runs
first, before any ticket (`valueFor` on a `replace`'s or `remove`'s etag, a `LOGICAL_ERROR` for a
token of another key or backend), so a caller that would fail those checks today fails them
identically, at once, and never waits. Then the caller binds its policy to an absolute deadline on
the engine's clock, as every verb does today at entry. Under `mutex`: push the item at the back of
`queue` (the one allocation; a failure leaves the lane untouched and propagates). The moment the
push succeeds, a ticket guard is installed: on every exit from here on, normal or by unwinding, it
runs the leave step below. It is `noexcept`, allocates nothing, and is the only thing that ever
removes an item from the queue.

**Wait.** Loop, in this order:

1. Outside `mutex`: the caller's own fence (`Fence::admit(generation, 0)`, atomics on the mount
   plane, always open on the other two), then the caller's own bound against the engine's clock,
   in that order so a lost fence or an exhausted lease is reported as itself and not as a policy
   deadline. `LostOrRearmed`: return `GaveUp{FenceLost}`. `NoBudget`: return
   `GaveUp{Deadline, Lease}`. Bound passed: return `GaveUp{Deadline}` with the bound's own source.
   The `Liveness` closure is not consulted while waiting; the engine's full gate, which does
   consult it, runs before every attempt as today.
2. Under `mutex`: if `done` (a leader combined this item, below): leave the loop with `result`. If
   the item is at the front: set `holder_since_ms` from the clock value read in step 1, set a local
   `entered_hold`, and leave the loop as the holder. Otherwise `cv.wait_for(lock, slice)` with a
   short slice (the pattern of `recovery_cv.wait_for(lock, 200ms)` in the ledger), release the
   mutex, and go to step 1. The clock is read in step 1, never under the mutex.

**Hold.** Run the verb's body on the caller's own thread, with the caller's own operation, policy,
bound and result type, exactly as today, plus the batch and memory steps below. Every sleep the body
takes before a write attempt goes through one helper, `pauseBeforeAttempt(delay_owed, envelopes)`:
the larger of the pause the loop owes (the growing transport backoff after a fault or an ambiguous
attempt, the flat conflict pause after a clean lost race, nothing before a first attempt) and, on
the `Generation` dialect, the time to the one-second boundary after `last_write_attempt_end_ms`;
then the engine's pre-sleep check, `gate(reservedFor(delay, envelopes))` and `fits`, with the
envelope count the verb reserves today (two for `writeLoop`, one for `remove`, what `removeCurrent`
reserves for its head plus remove); one sleep through the plane's interruptible sleep; the gate
again after. After every write attempt on the key ends, successful or not, its end is recorded in
`last_write_attempt_end_ms`.

**Leave.** The ticket guard, in this order: under `mutex`, if `entered_hold`, apply the memory rule
(below) and reset `holder_since_ms`; erase this item; if the caller left on its deadline behind a
front item, snapshot that item and `holder_since_ms`; `cv.notify_all()`; after releasing the mutex,
record the queue time and emit the log line if a snapshot was taken.

A holder never waits for another ticket. An item is settled only by its own caller or, for a
combined member, by the leader that took it; it is removed only by its own guard, and its `decide`
lives on its caller's stack, which is alive as long as the item is in the queue.

### The batch {#the-batch}

A `readModifyWrite` holder is a leader. Its one write attempt is:

1. **Base.** `remembered`, if present and the policy may reissue; otherwise `observe`, as today.
   A memory start is gated exactly like the observation it replaces: `gate(reservedFor(0, 1))` and
   `fits`, the same checks the read loop makes before its first attempt, so a caller past its
   deadline, lease or fence never runs `decide` on memory. `WriteState::last_seen` stays
   `NotObserved` until this call performs a real read.
2. **The leader's decide** on the base. Bytes: the candidate. A `nullopt` on an observed base:
   `Declined`, as today. A `nullopt` on memory: not a result; clear the memory, `observe`, `decide`
   again on the observation, and continue with what that renders (at most one restart per attempt).
   An exception from `decide` propagates unchanged, whatever the base.
3. **Combine.** Under `mutex`, walk the queue behind the leader and take every item that is
   compatible: a `readModifyWrite` (`decide != nullptr`), `policy.single_attempt == false`, and
   `bound.deadline_ms` not earlier than the leader's; stop at the first item that is not. Taking is
   no allocation (the items are already `shared_ptr`s held by the queue; the leader records them in
   a vector it reserved before walking, and stops if the reservation is short). Release the mutex.
   For each taken item in order: the item's own fence and `Liveness`, through its own operation's
   `gate(0)`; lost or exhausted: settle it now with the corresponding `GaveUp` and skip its `decide`.
   Otherwise `decide(Object{bytes = candidate, etag = base etag})`. Bytes: the item contributes and
   the candidate is those bytes. `nullopt`: held as a verdict, the candidate unchanged. An
   exception: held as that item's error, the candidate unchanged. The chain continues.
4. **Write.** One `writeLoop` of the candidate against the base etag (a create when the base was
   absence), under the leader's operation, policy and bound, with the pause helper above.
5. **Settle.** `Committed`: the memory rule remembers the candidate; each contributor is gated
   once more on its own operation, `gate(0)`; lost: `GaveUp{FenceLost}` with `sent_any = true`,
   the engine's own rule for a single write whose fence was lost in flight; otherwise
   `Committed{etag, attempts_sent, resolved_by_read}` with the batch's values; each held verdict is
   delivered as `Declined{seen = base}`, each held error is rethrown in its owner's thread. Any
   other ending, `Conflict`, `Refused`, `GaveUp`, an exception out of `writeLoop`: the leader
   receives it as its own; every taken item receives `Conflict{seen = the batch's last_seen}`,
   which is the resolve read's object after a lost race and `NotObserved` otherwise, and is not
   `done` with anything else. All settlement is under `mutex` in one pass, followed by
   `cv.notify_all()`.

A member that received `Conflict` is back in its own `readModifyWrite` loop, exactly where a loser
of a race lands today: the loop re-decides on `seen` (or observes first when nothing was seen),
pauses `conflictBackoff`, and takes a new ticket. The queue re-forms by itself; the members of a
failed batch wake together and stand in roughly their old order behind whoever arrived meanwhile.

**Why this is the same as writing one at a time.** Serial execution would apply mutation 1, land
it, apply mutation 2 to what landed, land it. The batch applies mutation 2 to what mutation 1
produced before it lands, and lands both at once. For every member, its input is exactly what it
would have seen serially in queue order with no external write in between; and that no external
write came in between is what the conditional `PUT` against the base etag proves. Verdicts depend
only on their input, so a verdict rendered in a committed batch is the serial verdict, admission
included: an insert refused on its prefix for capacity is refused exactly where serial execution
would refuse it, whatever a later member deletes. The one difference is the failed batch, where
serial execution would have landed some prefix and the batch landed nothing; that is why a verdict
or a contribution rendered in a batch is delivered only when the batch commits, and everything else
becomes `Conflict` and is re-decided. What a member's `Committed` means is therefore: your mutation
is in a committed object, as if you had committed alone immediately after the members before you.
This is the ref-log append lane's semantics (`committed_id` names the transaction that carries
your ops), and it is what every writer already accepts when its own commit is overwritten a
millisecond later.

**Read your writes.** A member is told `Committed` only after the combined `PUT` landed, and at
that moment the pool's memory holds the combined object. The caller's next write on this lane
starts from an object that contains its mutation; a plain `GET` returns it too (S3 and GCS are
read-after-write consistent for overwritten objects). The sequence inside one `createNamespace`,
step 1 then `_ckpt` then step 3, therefore works from memory. What is not the member's own in its
`Committed` is the etag and `attempts_sent`: they are the combined object's. No catalog caller uses
the returned etag, and `casUpdate`'s and `casAdmitEntry`'s return value is the candidate this
call's own `mutate` produced, which is contained in the committed object but is not it; no
production caller reads that value.

**Effects to name.** A member's `decide` runs on the leader's thread while its caller is parked;
the caller's `CasOperation` is used by one thread at a time, which keeps its single-threaded
contract, but profile events and an exception's stack belong to the leader's thread. A `decide`
that issues reads holds the lane for those reads (`reconcileStaleCreator` and
`cancelStalledCreating` read the mount key inside `decide`; the reconciliation's read already runs
under a policy frozen before its window, the cancellation's gets the same one line in
`dropNamespaceImpl`, so those reads end inside the holder's window).

### The memory {#the-memory}

**Memory rule.** After a hold ends: a `Committed` of a `readModifyWrite` remembers the candidate
that write landed, combined or not, and the etag the store returned for it, if the candidate is
within the size cap; every other ending of every verb forgets. That is the only source. Bytes that
came from the store, a resolve read's `Conflict::seen` among them, are never remembered: another
writer's malformed object, remembered and then repaired in the store, would otherwise reach a
`decide` as a stale base and become a corruption verdict where today's observation sees the repair.
A `create` or `replace` that commits forgets too: its bytes are the caller's, not a candidate this
loop encoded. A ticket that never entered its hold leaves the memory as it found it. Absence is
never remembered. A `GaveUp{FenceLost}` after a landed `PUT` forgets: forgetting is the fail-close
direction for a hint.

**Preparing the value.** The `(bytes, etag)` pair is assembled outside the mutex, after
`postCommit` produced the `Committed`: the bytes by moving the engine's own candidate string, the
etag by copying from the `Committed` the caller receives, which stays intact. The copy runs inside
its own `try` (a `memory_prepare_hook_for_test`, empty in production, runs just before it): if it
throws, the guard forgets and the caller still receives its `Committed`, because its write landed
and a failed cache fill is not an ending of the write.

**What memory is and is not.** It is the durable state at the moment of the pool's last commit.
It is not an observation of this call, and it is not guaranteed newer than an observation a caller
made outside the lane: `dropNamespaceImpl` reads the catalog fresh, sees a `Live` row another
server created, and calls `beginRemoving` on it; the memory may predate that row. A `decide` that
refused on such a base would be reporting a verdict about a state the store has left, with no write
sent to catch it. Hence the rule in step 2: **a `nullopt` is reported as `Declined` only when it was
rendered on an observation of this call** (a `GET`, or the resolve read after a lost race) or in a
batch that committed (which proves the base). A contribution needs no such rule: the `PUT`
validates its base.

**Exceptions from `decide` propagate unchanged, whatever the base, as today.** The engine cannot
tell a verdict thrown as control flow from a fault (an allocation failure, a cancellation, a
nested read's error), and re-running `decide` after a fault could land a write where today the
exception ends the call with nothing sent. So the engine discards only `nullopt`, and the catalog
carries its own verdict exceptions as `nullopt`. This is the first internal change in
`CasRefCatalog.cpp`: `casUpdateImpl`'s `decide` catches the four markers (`CatalogFenceMovedMarker`,
`CatalogEntryMismatchMarker`, `CatalogCreatorStillLiveMarker`, `CatalogEntryAlreadyPresentMarker`)
and the admission refusal (`LIMIT_EXCEEDED` from `checkCatalogAdmission`), stores the
`exception_ptr` in a local that every run of `decide` first clears, and returns `nullopt`; after
`readModifyWrite` returns `Declined`, it rethrows the captured exception, and its callers catch the
same types at the same places as today. A `Declined` with nothing captured stays the
`LOGICAL_ERROR` it is today. Decode corruption and `identityPreserving`'s `LOGICAL_ERROR` propagate
from wherever they are thrown.

The second internal change: `casAdmitEntry` checks presence itself before inserting, like
`createNamespaceStep1`, and carries the duplicate as the same marker, translating it back to its
documented `LOGICAL_ERROR` after the loop. Today it lets `encodeRefCatalog`'s grammar check report
the duplicate, which on a stale memory base would raise a false `LOGICAL_ERROR` for a row the store
no longer has. The third: `createNamespace` catches `CatalogFenceMovedMarker` around step 1 and
returns `NamespaceCreationOutcome::FencedOut`, which `resolveNamespaceLife` already handles; today
only `CatalogEntryAlreadyPresentMarker` is caught there, and a creator whose fence moves during
step 1 throws the bare marker, a `std::exception` no caller names. The lane's queue wait widens
that window from one short call to the wait, so the catch is not optional. `casAdmitEntry` gets the
translation `casUpdate` already performs for the same marker.

With those, every `readModifyWrite` on the catalog key may start from memory; there is no opt-in
parameter and no audit list. Two sites pay a cost on a stale base, not a risk: `reconcileStaleCreator`
and `cancelStalledCreating` read the mount key after matching the row, and on a stale base that read
runs where today's fresh observation would have returned `EntryChanged` without it. A
`single_attempt` policy (`Retry::once`) observes as today and never starts from memory, because
after the conflict a stale base would cause it may not re-decide.

### GC in the lane {#gc-in-the-lane}

The GC erase, `deleteCompletedRemovingAtSnapshot`, is a `replace` against the etag of the round's
catalog cut, followed by a mandatory resolution read, on `openRequests()` under a `Liveness`
closure that returns a cached leadership flag which the reconciler refreshes by one `gc/state` read
at the top of each of its own attempts. Not one line of it changes. Its `replace` takes a ticket
like any other write on the key, waits for the mount plane's writers, and lands without overlapping
any of them; the mount plane's next write pays one 412 and one resolve read for the memory the erase
made stale, then proceeds.

What the wait changes about GC's authority, stated so it is a decision and not an omission. Today
the flag is refreshed, local work follows, and the `PUT` flies; the `PUT` is in flight and processed
for some time during which leadership can change, and nothing checks `gc/state` meanwhile. The
model tolerates that because the flag is not what protects the erase. Two things do: the `PUT` is
conditional on the cut's etag, so any catalog write by a successor moves it and a late erase is
refused; and the row is one a parent seal proved clean, so if nothing moved, the late erase is the
erase the successor would perform (a `Removing` life admits no new publication, so no hold can
reappear). The queue wait is the same class of window as the `PUT` in flight: an erase landing with
leadership information N seconds old, where N is now bounded by the erase's own policy deadline (a
frozen `Retry::standard()`, 90 s) instead of by a transport attempt. A shorter policy is GC's to
pass. The "deposed leader touches nothing" rule stays what it is today: exact to within one window,
and that window is now the lane's.

One gap exists today and is not this design's: the `replace` runs under `Retry::standard()`, and
after an ambiguous attempt its `writeLoop` may sleep and reissue the same bytes gated only by the
cached flag. It is a BACKLOG item; the principled closure is a TTL on the GC lease, which would
make GC's writes time-fenced like the mount plane's. This design neither widens nor narrows it.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait of its own. Every catalog write already runs under
`Retry::standard()`, 90 s from the call, or a lease bound if shorter; the lane binds the policy at
entry, so time in the queue spends the same window rather than adding to it, which is what today's
backoff sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own fence and deadline, re-checked every slice | its window plus one slice |
| a combined member | the leader's write, then its own loop | the leader's deadline (not earlier than its own, by the combine rule), then its own |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it, and its `decide`'s reads run under a policy frozen before its window | its window plus one attempt timeout, unless the transport does not answer |

Nothing copies a deadline verdict from one operation to another: every `GaveUp` is the reporting
operation's own, with its own `Source`.

**A holder whose transport does not answer.** The engine calls the transport synchronously and
cannot cancel it. A stalled attempt ends at the backend's attempt timeout, which the engine
reserves per attempt. An attempt that trickles, delivering bytes slowly enough that no socket
timeout fires, or holder code that never returns for any other reason, has no bound in the engine;
the S3 transport's timeouts are per socket operation, not per request. Today that traps only its
own caller. In the lane it traps the hot key on this pool: every later catalog writer waits to its
own deadline and reports retry-later, until the holder returns, the connection is closed, or the
process restarts. This is accepted as a release-level availability tradeoff for that case:
nothing lands that should not, no second write starts under a running first one, the case is
visible while it happens (the keyed log line names the holder and its hold time), and the fix
belongs in the transport: a whole-request deadline in the S3 client, which bounds today's single
trapped writer too. Evicting the holder was designed three times and rejected each time; its
state machine cost more than the case is worth, and it is not to be reopened without the transport
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

| `driver_mutex` holder | what runs under it |
|---|---|
| `renewalWorkerMayRenew`, `renewalLive`, `renewalCancelled`, the `*ForTest` accessors | reads of driver state |
| `DriverLease::finish`, `admitRenewerCall`, `installRenewer`, `startRenewer`, `renewerReset` | driver state transitions, no I/O |
| `startBackgroundWorkers`, `stopBackgroundWorkers` | worker handles; joins happen with the lock released |
| `renewalLoop`, `remountLoop` | condition-variable waits and state reads; `remount_attempt()` runs after the lock is released |
| `lockTerminalPublication` and its three callers | atomic stores of terminal state; no backend request |
| `scheduleRemount` | a generation bump and a notify |

None reaches `CasRefLedger`, the catalog key, or any request on any plane, so no `driver_mutex`
holder ever waits for a ticket. The rule for future runtime code follows: nothing under
`driver_mutex` issues a write.

`CasHotKeys::mutex` is a leaf that calls nothing: it is held to push, inspect or erase an item, to
take a batch, to settle a batch, to read or set the spacing timestamp, and to move or clear
`remembered`. The fence, the `Liveness` closure, the clock, the verb's body and every `decide` run
with it released. The order is therefore: caller (no locks) → `CasHotKeys::mutex` → nothing; and,
with the ticket logically held, caller → `driver_mutex` → nothing, caller → backend mutex → nothing.
The one rule for future callers, stated where the catalog API is documented: a catalog mutation
is not entered while holding a ledger mutex, and no callback that runs while a ticket is held
(`decide`, `Liveness`, a backend hook) issues a write on any hot key of the pool.

### Invariants {#invariants}

- INV-HK1. Per pool and hot key, at most one conditional write is in flight at any moment, from
  any plane.
- INV-HK2. Writes of one pool to a hot key are applied in arrival order, a batch counting as its
  members in order.
- INV-HK3. A `readModifyWrite` that follows a `Committed` of this pool on a hot key, with no other
  ending in between and a candidate within the memory cap, obtains its base from memory and
  issues no read for it. A lost race's resolve read is not a base read.
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair this pool encoded
  and committed, durable at the moment of that commit. It never changes the semantics of a
  conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A `nullopt` from `decide` reaches its caller as `Declined` only when rendered on an
  observation of that call or in a batch that committed; an exception from `decide` reaches its
  caller unchanged, whatever the base, and no write is sent after it.
- INV-HK6. Every member of a batch sees, as its input, exactly the object it would have seen had
  the members before it committed one at a time with no external write between; a member's result
  is delivered only from a batch that committed, or as `Conflict`.
- INV-HK7. An item is removed only by its own guard; no item references a stack that may have
  unwound; a member's `decide` runs only while its caller is parked in the wait.
- INV-HK8. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`, and the guard
  neither allocates nor throws.
- INV-HK9. A member's own fence is checked before its `decide` runs and again after the commit it
  took part in; a `single_attempt` item and an item with an earlier deadline than the leader's are
  never combined.
- INV-HK10. On the `Generation` dialect, write attempts on a hot key from one pool are at least
  one second apart, and the spacing sleep starts only when it and what follows it fit the
  operation's bound.

### Kinship with `appendRefOps` {#kinship-with-appendrefops}

| | ref-log append lane | hot-key lane |
|---|---|---|
| item | `RefMutationItem` with `done`, `error`, `committed_id` under `ref_queue_mutex` | `Item` with `done`, `result` under `CasHotKeys::mutex` |
| leader | first to find `leader_active` false; serves until its own item is done | the front item; one batch per hold |
| combine | compatible items into one ref-log transaction, `build_ops` run by the leader | compatible `readModifyWrite`s into one `PUT`, `decide` run by the leader on the chained candidate |
| failure | a rejection is final per item | every member gets `Conflict` and re-decides in its own loop |
| exit | `completeOwnedItemsAndReleaseLeadership` | the ticket guard; settlement in one pass under the mutex |
| wait | `cv.wait`, woken at every handover | `cv.wait_for` in slices, own fence and deadline re-checked |
| key shape | write-once ref-log objects: a wedge and a `carved` mirror | replace-style control object: ambiguity settled by the engine's resolve read |

The shared shape is deliberate. Extracting a common flat-combining component is possible and is not
part of this design.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff(Dialect)` returns, on the `ETag` and `Emulated` dialects, `backoff(1)`,
uniform(0, 200 ms) (`backoff` doubles from attempt 2 on); on the `Generation` dialect, 1000 ms plus
`backoff(1)`. Both are flat: they do not grow with the writer's loss count. The `Generation` value is
the store's documented rate for one writer, applied to every key the loop serves, hot or not:
`_ckpt` and `gc/state` writers on GCS that lose a clean race retry no faster than once a second
each, without relying on a 429 the store only might send.

- `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`: on a `Conflict` whose inner
  write had no ambiguous attempt (a clean refused precondition, settled by the resolve read), a
  `pauseForConflict` that performs the same `gate` and `fits` checks as `pauseAndReissue` with
  `conflictBackoff()` and leaves `state.reissues` untouched. It records a new profile event,
  `CASRequestConflictPause`, and not `CASRequestReissue`; in these two loops the reissue counter
  then counts transport reissues only (`removeCurrent` and the read loops keep recording it as
  they do today).
- A `Conflict` whose inner write had an ambiguous attempt (`state.any_ambiguous`: a transport
  fault, a 429 among them, that the resolve read then settled as a lost race because the key had
  moved) keeps `pauseAndReissue` and its growing schedule: the fault is the signal that must pace
  the loop, and `writeLoop` already carries the distinction.
- `CasRefCatalog::deleteCompletedRemovingAtSnapshot` keeps `op.pause(Retry::backoff(attempt + 1))`:
  its `replace` returns a `Conflict` that carries no provenance, the loop is rare, and the growing
  schedule is the safe choice there.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. After Half 1, conflicts
arise only between pools and servers, and there a loss is dearer, because the losing pool's whole
lane waits behind its holder.

**The `Generation` dialect's write budget.** GCS documents about one mutation per second per
object name, says excess writes may be throttled, and asks applications not to exceed the rate. A
lane that drains holders at `PUT` latency is not paced by 429s it does not receive, so on that
dialect the lane spaces write attempts on a hot key itself, across holders: no attempt starts sooner
than one second after the previous attempt on that key from this pool ended, successful or not. The
wait is one sleep, the larger of the time to that boundary and, after a clean `Conflict`, the
dialect's conflict pause, so the jitter is never absorbed by the boundary and two servers whose
boundaries coincide do not retry at the same instant forever. The S3 and emulated dialects get no
spacing. Go/no-go for the `gcs` lane: the count of 429s on `ref_catalog` in `system.text_log` over
the acceptance run must not exceed today's, and the count of 412s must fall; both recorded in
`docs/superpowers/cas/BACKLOG/gcs.md`. With combining, one second carries every mutation that
arrived meanwhile, which is what makes that budget sufficient.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a clean lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff(dialect)`, flat: uniform(0, 200 ms) on the `ETag` and `Emulated`
   dialects, one second plus that on `Generation`, the store's documented rate for one writer;
   neither grows with the writer's loss count. The growing schedule belongs to transport faults, and
   to a conflict that settled one. Inside one pool a hot key never conflicts with itself: see the
   hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a
   settled fault, `backoff(attempt)` otherwise and after a fault; `deleteCompletedRemoving`'s
   `replace` cannot tell and keeps `backoff(attempt)`".
3. In the `readModifyWrite` section, one paragraph: on a hot key the initial observation may be
   the lane's remembered object, gated like an observation; a `nullopt` rendered on it is
   re-rendered on an observation before it is reported; queued calls may be combined into one
   `PUT` with as-if-serial semantics; pointer to this document.

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to departure, per item, on every exit.
- `CASHotKeyBatchMembers`: members per committed batch, contributors and held verdicts (sum;
  divide by `CASHotKeyBatches` for the mean).
- `CASHotKeyMemoryStarts` and `CASHotKeyReadStarts`: how a hold obtained its base.
- `CASHotKeyVerdictRestarts`: `nullopt`s rendered on memory and re-rendered on an observation.
- `CASHotKeyBatchConflicts`: members returned to their loops by a batch that did not commit.

One log line, naming the key: a waiter that left on its deadline while a front item was held, with
that item's ticket and how long it has held. It is what makes a stuck holder visible while it is
stuck.

The acceptance measurement is the existing one: ten minutes of the parallel stateless suite on the
CA-s3 lane, `system.query_log` for `DROP TABLE` and `CREATE TABLE` percentiles, and the count of
`PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after.

## Tests {#tests}

Engine level, in `gtest_cas_requests.cpp`'s harness: counting backend, write hooks, and a clock.
The harness's `FakeClock` is single-threaded; the multi-threaded cases use a synchronized clock (a
mutex-guarded `now` advanced by the test thread, sleeps recorded under the same mutex) added for
them. The `CasRequests` under test share one `CasHotKeys` with a predicate naming the test key. Each
`decide` decodes a list of tickets from `bytes` and appends its own, so order and membership are
visible in the object.

1. Serialization, combining, memory. N threads call `readModifyWrite`; a write hook parks the first
   holder, and each further thread is released only after the test observes its item queued
   (`queueDepthForTest(key)`), so arrival order is the release order. Assert: reads 1, writes 2
   (the parked one and one combined batch), no `Conflict`, the final object lists all tickets in
   arrival order, every caller received `Committed` with the batch's etag, and a following call
   starts from memory with zero reads (INV-HK1, HK2, HK3, HK6). A sibling mixes the six verbs from
   N threads and asserts through a hook recording overlapping calls that no two writes on the key
   were ever in flight together.
2. Batch failure cascades. As above, but a hook makes the combined `PUT` `Refused` once. Assert:
   the leader receives `Refused`, every member receives `Conflict{NotObserved}` and, through its
   own loop, observes, re-queues and lands in the next batch; the final object carries every
   ticket once; `CASHotKeyBatchConflicts` equals the member count. A sibling makes the `PUT` lose
   to an external `CasRequests`: members receive `Conflict{seen}` with the external object and
   re-decide on it without a further read.
3. Verdicts in a batch. A member's `decide` returns `nullopt` because the leader's chained change
   made its row differ from what it observed. Batch commits: the member receives `Declined` after
   the commit and its marker (through `casUpdateImpl`) is the one a serial `EntryChanged` gives.
   Batch fails: the member receives `Conflict`, re-decides on a fresh read, sees its row unchanged,
   and lands. A member's `decide` that throws a non-verdict exception receives that exception after
   the commit and contributes nothing (INV-HK5, HK6).
4. Member fences. A combined member whose operation was resumed under a stale generation receives
   `GaveUp{FenceLost}` with `sent_any = false` and its `decide` never ran; a member whose fence is
   tripped between the `PUT` and the post-commit check receives `GaveUp{FenceLost}` with
   `sent_any = true` while the object carries its ticket (INV-HK9). A `Retry::once` item and an
   item with an earlier deadline are not combined and run as their own holders.
5. Verdict on stale memory, at the catalog level in `gtest_cas_ref_catalog.cpp`. The pool commits
   once. An external `CasRequests` changes a row. A caller reads fresh outside the lane and runs
   the real `beginRemoving` through a hot `CasRequests`: `Transitioned`, not `EntryChanged`, one
   lane read, `CASHotKeyVerdictRestarts` 1. The matrix: for each of `createNamespaceStep1` (already
   present), `completeCreation`, `beginRemoving`, `reconcileStaleCreator`, `cancelStalledCreating`
   and the admission refusal, one case where memory is stale and the refusal would be false
   (positive outcome, as a cold key), and one where it is true on the observation (the same outcome
   and error class as a cold key, one lane read, no write, plus whatever reads the function itself
   makes after a mismatch, as `beginRemoving` does). `casAdmitEntry` of a namespace whose
   remembered row an external erase removed admits it, and of a namespace that is present raises
   its documented `LOGICAL_ERROR`. A `decide` that throws decode corruption on memory propagates it
   with no second read and no write.
6. Fence during step 1. A creator queued behind a parked holder has its fence tripped before its
   turn: `createNamespace` returns `FencedOut`, nothing was written, `resolveNamespaceLife` reports
   retry-later. Tripped between its landed step-1 `PUT` and the post-commit check: `FencedOut`
   again, with the `Creating` row durable for a later reconciler. `casAdmitEntry` under a tripped
   fence throws `throwCasTransientUnavailable`'s class, never a bare marker.
7. External writer on the write path. A second `CasRequests` writes between two writes of the
   first. Assert exactly one resolve read and one retry write, `Committed`, the committed body is
   the external writer's bytes plus this call's ticket, and the write after that starts from
   memory. A sibling has the external writer store malformed bytes, then repair them, between two
   calls: the second reads and commits, no corruption verdict.
8. Forgetting. `Refused`, a thrown `writeLoop`, a `remove`, a committed `replace`, a candidate one
   byte above the cap, and a `GaveUp{FenceLost}` after a landed `PUT` each clear the memory; a
   candidate of exactly the cap is remembered. A `memory_prepare_hook_for_test` that throws leaves
   the caller's `Committed` intact and the memory empty. A memory start refused at the gate reports
   `GaveUp` with `last_seen == NotObserved`.
9. Waiters leave on their own. While the holder is parked inside its `decide`'s nested read: a
   waiter whose deadline passes leaves with `GaveUp{Deadline}` carrying its own source and the
   keyed log line naming the holder; a waiter whose fence is tripped leaves with
   `GaveUp{FenceLost}`; a waiter whose lease budget runs out leaves with `GaveUp{Deadline, Lease}`;
   both failing reports `FenceLost`; a `remove` in the same position throws its loop's exception.
   The item is gone in every case (INV-HK7). A waiter whose deadline passes in the same slice the
   holder leaves never enters hold, never runs `decide`, reads nothing.
10. GC in the lane. An open-plane `replace` in the shape of the GC erase, with a cached-flag
    `Liveness`, waits for a parked mount-plane holder, lands, and the mount plane's next write pays
    one resolve read and one retry write, commits, and the one after starts from memory. The flag
    flipped to false while queued: the engine's gate before the attempt refuses with
    `GaveUp{FenceLost}` and nothing is sent.
11. Stalled holder. The holder is parked inside a `PUT` that does not return; the clock is advanced
    past every waiter's deadline. Assert each waiter leaves with its own `GaveUp` and the log line,
    no second write started, only the holder's item remains, and when the `PUT` is released the
    holder commits and the next writer proceeds. `cancelStalledCreating` through `dropNamespaceImpl`
    issues its terminality read under the frozen policy: with that read answering retryable
    failures, the holder gives up at the outer deadline, not 90 s later.
12. Half 2, parameterized over `readModifyWrite` and `readModifyWriteOnPresence`. K clean lost
    races: `CASRequestConflictPause` K, `CASRequestReissue` 0, every sleep at most 200 ms on
    `Emulated` and within [1000, 1200] ms on `Generation`. K races each preceded by a 429-class
    fault: `CASRequestReissue` K, `CASRequestConflictPause` 0, the growing schedule.
13. `Generation` spacing. N writes: consecutive attempts at least one second apart on the clock,
    through the plane's sleep function; a fence tripped during the spacing sleep ends the holder
    with `GaveUp{FenceLost}` and no attempt; a holder whose bound cannot fit the sleep plus its
    envelopes gives up before sleeping, a `remove` with room for one envelope is admitted; after a
    committed `replace`, a `Refused`, a `Declined` and an oversized commit the next write still
    waits to the boundary; two pools losing a race at the same boundary each record one second plus
    a jitter within [0, 200 ms]; a credential-refresh reissue and an ambiguous-attempt reissue each
    record exactly one sleep of `max(transport backoff, boundary)` (INV-HK10). On `ETag`, no
    spacing sleep.

Pool level, through the ledger:

14. N concurrent `createNamespace` calls on one `Pool`, distinct namespaces: zero catalog writes
    returned `Conflict`, catalog `PUT`s fewer than 2N (steps 1 and 3 combine across callers), and
    exactly one lane base read (`CASHotKeyReadStarts == 1`; the callers' own pre-check reads are
    outside the lane and not counted).
15. A second `Pool` over the same backend as the external writer: the first `Pool`'s next catalog
    mutation costs exactly one extra read and one retry write.

Regression: every existing test in `gtest_cas_ref_catalog.cpp` and its siblings runs unchanged,
without a hot predicate, and must stay green. That is the check that the catalog API was not
touched and that the marker transport inside `casUpdateImpl` preserves every outcome; the
existing tests already cover each marker and the admission refusal against a cold key.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- The GC erase's authority gap across its own internal reissue, and its principled closure, a TTL
  on the GC lease: a BACKLOG item, existing today.
- A whole-request deadline in the S3 transport, which makes a non-answering attempt end on its own
  and closes the accepted availability tradeoff: a BACKLOG item, and the only planned answer to
  that case.
- Declaring the `_ckpt` keys hot, with the per-key memory budget and lane erasure many hot keys
  need, and an audit of the liveness closures the append-lane operations carry: a BACKLOG item.
- Remembering a resolve read's object after validating it, so one external conflict costs one
  read rather than two: a BACKLOG note.
- The two remaining `DROP TABLE` costs stay in `{#stateless-lane-wall-time-is-drop-table}`.

## Revision history {#revision-history}

Revision 1 was brainstormed in chat. Revisions 2 to 15 were fourteen rounds of review by `codex`
(`gpt-5.6-sol`, high), each fixing that round's majors, and the fifteenth round ended on the
account's spend cap. Three of the review's removals were reversed in this revision, with the
argument the reviews did not weigh: combining (as-if-serial in queue order, verdicts delivered only
from a committed batch, `Conflict` to every member otherwise, which is what losers receive today);
one lane per pool with the GC erase in it unchanged (its authority window is the same class as a
`PUT` in flight, bounded by its own policy, and the erase is protected by the cut's etag and the
seal's proof, not by the flag); and memory for every `readModifyWrite` on the key instead of an
opt-in list (`casAdmitEntry` pre-checks presence, as `createNamespaceStep1` does). What the reviews
established and this revision keeps: a `nullopt` on memory is re-rendered on an observation and an
exception from `decide` propagates unchanged; the catalog's markers travel as `nullopt`; a resolve
read's bytes are never remembered; a `single_attempt` policy never starts from memory; the memory
start is gated like the read it replaces; the guard is the sole remover and never allocates; a
member's fence is checked before its `decide` and after the commit; `driver_mutex` was audited
rather than assumed; the conflict pause is flat per dialect and a conflict that settled a fault
keeps the growing schedule; the `Generation` dialect is spaced to its documented rate; a holder
whose transport never answers is an accepted tradeoff with its fix placed in the transport; and
the step-1 fence marker is caught where it was not.
