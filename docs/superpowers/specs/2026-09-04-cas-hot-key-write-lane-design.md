---
description: 'Design for a per-pool write lane on a hot CAS control object (the ref catalog first) inside the request engine: one FIFO per key shared by the pool''s planes, the queued read-modify-writes combined into one conditional PUT with as-if-serial semantics, the last committed object remembered so the next write needs no read, a decide''s refusal reported only from a proven base, and lost races between servers paced by a flat jitter. Motivated by measured CREATE/DROP starvation on the ref catalog.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'guide'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 20, 2026-09-04. Brainstormed against the measurements in
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
  `decide` renders, to the 90 s windows, to any read path, or to a single line of GC. In the
  engine: the lane; one ticket per write attempt, which moves `readModifyWrite`'s and
  `readModifyWriteOnPresence`'s initial observation inside their loops, behind the hold; one
  pre-attempt pause helper at every attempt's start and in the two existing reissue pauses; a
  clamp that bounds every read an operation issues inside a hold by the hold's own bound; and the
  flat conflict pause. Four internal changes in `CasRefCatalog.cpp`, all named below, and nothing
  in the ledger or GC. Every existing catalog test stays green unchanged.

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
`decide`'s refusal is reported only from a proven base. The GC erase's attempt becomes a
`readModifyWrite` whose `decide` refreshes the leader's authority and checks the row's exactness,
so it queues, combines and starts from memory like every other writer, with its authority refresh
now inside the hold, immediately before the `PUT`; its mandatory resolution read after a commit
stays.

**Half 2, conflict pacing.** In `readModifyWrite` and `readModifyWriteOnPresence`, a clean refused
precondition is repaid after `Retry::conflictBackoff()`, a flat uniform over [0, 200] ms, and no
longer advances the transport-fault counter; on the `Generation` dialect the hot keys are spaced to
the store's rate by the lane. The GC erase loop keeps its pacing. Two sentences of the backend
request contract change.

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
and `readModifyWriteOnPresence`, does the following around one write attempt (`removeCurrent`
holds one ticket around its whole head-then-remove loop, so its head reads run while holding).
Nothing runs under `mutex` except the steps marked. A refusal while waiting is reported in the verb's own result
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
    bool taken = false;                      /// guarded by `mutex`; set by a leader in the same critical section
                                             /// that takes the item; a taken item's caller may not leave until `done`
    bool done = false;                       /// guarded by `mutex`
    std::shared_ptr<const BatchOutcome> outcome;   /// guarded by `mutex`; set with `done` for a combined member
    std::exception_ptr error;                /// guarded by `mutex`; a held `decide` exception, delivered by its owner
};

/// One per batch, allocated by the leader before its write, shared by every member it settles:
/// the ending's class and the one observation it carries. A member copies what it needs out of it
/// on its own thread, so settlement under `mutex` assigns pointers and flags and allocates nothing.
struct BatchOutcome
{
    enum class Kind : uint8_t { Committed, Conflict, Unresolved };
    Kind kind;
    std::optional<Etag> etag;                /// `Committed`: the combined object's
    uint32_t attempts_sent = 0;
    bool resolved_by_read = false;
    Observation last_seen;                   /// `Conflict`: what the resolve read saw, or `NotObserved`
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

1. Outside `mutex`: the engine's own `gate(0)` on the caller's operation (the fence, atomics on
   the mount plane and always open on the other two, then the `Liveness` closure, a flag read for
   every closure in the tree today), then the caller's own bound against the engine's clock, in
   that order so a lost fence, a stopped task or an exhausted lease is reported as itself and not
   as a policy deadline. On any of the three the caller intends to leave, and leaving is decided
   under `mutex` in step 2: a caller whose item a leader has meanwhile `taken` may not leave,
   because its `decide` may be running on the leader's thread against this caller's stack; it stays
   parked until `done` and then returns what the leader settled, which for a fence lost or a lease
   exhausted is the same `GaveUp` the leader's own per-member gate renders. A caller whose item is
   not taken leaves with `GaveUp{FenceLost}`, `GaveUp{Deadline, Lease}`, or `GaveUp{Deadline}` with
   the bound's own source. Which operations the `Liveness` part of this reaches today: the GC
   erase's, whose closure is `Gc`'s cached flag; the ledger's two catalog writers admit without a
   closure (`mount_requests.resume(admitted_generation)`), so while waiting their escapes are the
   fence generation and the deadline, as they are today between attempts.
   What this step touches of the caller's `CasOperation` is disjoint from anything a leader running
   this item's `decide` may touch: `admitted_generation` and `liveness` are immutable after
   construction, and the fence and clock closures are the ones `CasRequests` already requires to be
   thread-safe; a leader's reads from inside `decide` write only `last_read_stop`, and the
   `written` local and the captured marker of `casUpdateImpl` live on this caller's stack and are
   touched only by the leader while this caller is parked here.
2. Under `mutex`: if `done` (a leader combined this item, below): leave the loop with the
   settled outcome. If step 1 decided to leave and the item is not `taken`: leave with that
   `GaveUp`. If step 1 decided to leave and the item is `taken`: fall through to the wait. If the
   item is the first in the queue that is not `done` (settled members stay queued until their own
   guards run): set `holder_since_ms` from the clock value read in step 1, set a local
   `entered_hold`, and leave the loop as the holder. Otherwise `cv.wait_for(lock, slice)` with a
   short slice (the pattern of `recovery_cv.wait_for(lock, 200ms)` in the ledger), release the
   mutex, and go to step 1. The clock is read in step 1, never under the mutex. The two decisions
   that matter for memory safety, "a leader takes this item" and "this item's caller leaves", are
   both made under `mutex` against the same `taken` flag, so they are exclusive (INV-HK7).

**Hold.** Run the verb's body on the caller's own thread, with the caller's own operation, policy,
bound and result type, exactly as today, plus the batch and memory steps below. While holding, the
`CasOperation` carries a pointer to the lane it holds (a per-operation field set at hold and
cleared by the guard; `CasRequests` itself still writes no member), and two things follow from
that pointer.

The first is the clamp: while an operation is inside a hold, its own or as a taken member whose
`decide` is running, every read it issues is bounded by the smaller of its own policy's bound and
the hold's bound (`Retry::bind`'s result, clamped by a per-operation deadline the lane sets and
clears with the pointer). A read the clamp refuses gives up the way a read gives up at its own
deadline, with `last_read_stop` set, and surfaces as that `decide`'s exception. So a `decide` that
reads inside a hold, as `reconcileStaleCreator` and `cancelStalledCreating` do through
`isCreatorFenceTerminal`, and as the GC erase now does through its authority refresh, ends inside
the hold's window whatever policy the caller froze or defaulted, and a hold never outlasts its
holder's own deadline plus one attempt for any reason but a transport that does not answer.

The second is spacing. Every write attempt is preceded by one computation,
`owedSleepMs(delay_owed)`: the larger of the pause the loop owes (the growing transport backoff
after a fault or an ambiguous attempt, the flat conflict pause after a clean lost race, nothing
before a first attempt) and, on the `Generation` dialect, the time to the one-second boundary after
the lane's `last_write_attempt_end_ms`. It computes and gates nothing. Around it, each result
family has its own wrapper that gates, sleeps and reports in that family's terms: on the write
side, `pauseAndReissue` and `pauseForConflict` as today, plus a zero-owed call at the top of
`writeLoop`'s attempt loop, all reporting through `gaveUp`; on the read side, inside `readLoop`,
conditional on the call being a mutation (`removeUnder`'s lambda, so `remove` and `removeCurrent`'s
remove) and a lane being held, before every attempt including the first, reporting through the
three `giveUpRead*` throws that also set `last_read_stop`. `writeLoop`'s existing loop-top
`gate(reservedFor(0, 2))` is not duplicated: the wrapper performs it. Each wrapper runs the
engine's pre-sleep check, `gate(reservedFor(delay, envelopes))` and `fits`, with the envelope
count the verb reserves today (two for `writeLoop`, one for a `remove` attempt); sleeps once
through the plane's own sleep function; and runs the gate again after. The zero-owed call before a
hold's first attempt is the common case on a busy lane, a holder woken by the previous holder's
`notify_all` and issuing at once. After every write attempt on the key ends, successful or not, its
end is recorded in `last_write_attempt_end_ms` under `mutex`.

**Leave.** The ticket guard, in this order: under `mutex`, if `entered_hold`, apply the memory rule
(below; this is the only place the memory is written) and reset `holder_since_ms`; erase this
item; if the caller left on its deadline behind a held item, snapshot that item and
`holder_since_ms`; `cv.notify_all()`; after releasing the mutex, record the queue time and emit the
log line if a snapshot was taken, inside a catch-all in the `tryLogCurrentException` shape, so the
reporting tail can neither throw out of a guard nor terminate an unwinding thread.

A holder never waits for another ticket. An item is settled only by its own caller or, for a
combined member, by the leader that took it; it is removed only by its own guard, and its `decide`
lives on its caller's stack, which is alive as long as the item is in the queue and, once the item
is `taken`, until the leader has settled it, because a taken item's caller does not leave.

**One ticket per write attempt.** For `readModifyWrite` the ticket wraps one iteration of its
loop, not the call: a hold begins with the base, runs `decide` inside the hold, sends the one
write, and ends; a `Conflict` ends the hold, the loop pauses `conflictBackoff`, and the next
iteration takes a new ticket. On entering a hold the loop's carried `current` is not the base: the
base is `remembered` if present (the pool's last commit, which a same-pool holder may have produced
since this caller's last resolve read), else the object this call's own last resolve read saw, if
it has one, else an observation. Either hint is validated by the `PUT`; preferring memory is
preferring the common case in a lane, a same-pool commit since this caller last looked.

### The batch {#the-batch}

A `readModifyWrite` holder is a leader. Its one write attempt is:

1. **Base.** `remembered`, if present and the policy may reissue; else this call's own last
   resolve-read object, if any; otherwise `observe`, as today. A memory start is gated exactly like
   the observation it replaces: `gate(reservedFor(0, 1))` and `fits`, the same checks the read loop
   makes before its first attempt, so a caller past its deadline, lease or fence never runs
   `decide` on memory. `WriteState::last_seen` stays `NotObserved` until this call performs a real
   read.
2. **The leader's decide** on the base. Bytes: the candidate. A `nullopt` on an observed base:
   `Declined`, as today. A `nullopt` on memory: not a result; clear the memory, `observe`, `decide`
   again on the observation, and continue with what that renders (at most one restart per attempt).
   An exception from `decide` propagates unchanged, whatever the base.
3. **Combine.** Under `mutex`, walk the queue behind the leader and take every item that is
   compatible: not `done`, a `readModifyWrite` (`decide != nullptr`), `policy.single_attempt ==
   false`, and `bound.deadline_ms` not earlier than the leader's; stop at the first item that is
   not. The leader copies the taken `shared_ptr`s into a vector and sets each item's `taken` in
   the same critical section, under the mutex, which is a leaf that calls nothing, exactly as the
   ledger's carve does; it also allocates the batch's `BatchOutcome` there, so nothing after this
   point allocates on the settlement path; an allocation failure propagates to the leader before
   anything was taken. The batch is what is queued at that walk; nothing arriving later joins it,
   and there is no other cap. Its cost has two parts: one `decide` per member on the leader's
   thread, a decode and an encode of the catalog each, about a millisecond per ten thousand rows;
   and, for the decisions that read inside `decide` (`reconcileStaleCreator` and
   `cancelStalledCreating`, both rare, a stalled creation; and the GC erase's authority refresh),
   one serialized read each under the hold, clamped to the leader's bound (Hold, above), so the
   chain ends inside the leader's window whatever policy each member froze. Release the mutex. For
   each taken item in order: first `fits(reservedFor(0, 2), leader's bound)`; if the leader's
   remaining window no longer covers a write, stop taking and go to step 4 with what the chain has
   (the untouched items stay in the queue, `taken` never set). Then the item's own fence,
   `Liveness` and lease budget, through its own operation's `gate(reservedFor(0, 2))`, the
   reservation its own `writeLoop` would make: lost, stopped or out of budget: settle it now with
   the corresponding `GaveUp` and skip its `decide`. Otherwise
   `decide(Object{bytes = candidate, etag = base etag})`, with the member's operation clamped to
   the leader's bound for the duration. Bytes: the item contributes and the candidate is those
   bytes. `nullopt`: held as a verdict, the candidate unchanged. An exception: held as that item's
   error, the candidate unchanged. The chain continues.
4. **Write.** One `writeLoop` of the candidate against the base etag (a create when the base was
   absence), under the leader's operation, policy and bound, with the pause helper above.
5. **Settle.** A held error is delivered to its owner on every ending: the member contributed
   nothing, so throwing is exactly what its own call would have done, and a `decide` that threw is
   never run again (INV-HK5). Every other taken member is told the class the leader's ending
   belongs to, never a stronger one (INV-HK11):
   - `Committed`: each contributor is gated once more on its own operation, `gate(0)`; lost:
     `GaveUp{FenceLost}` with `sent_any = true`, the engine's own rule for a single write whose
     fence was lost in flight; otherwise `Committed{etag, attempts_sent, resolved_by_read}` with the
     batch's etag and `resolved_by_read` and `attempts_sent = 0`: the batch's attempts are counted
     once, by the leader, since operator counters sum over every ending of a write and a member's
     own operation sent nothing; the same zero goes into every member's `Conflict` and `GaveUp`.
     Each held `nullopt` is delivered as `Declined{seen = the batch's base object}`: `seen` is a
     real observation, the base the batch was decided from, deliberately not the key's state after
     the commit and never the chained `Object` its `decide` received, which pairs the running
     candidate's bytes with the base's etag, was never at the key, and exists only as `decide`'s
     input. So `seen` is a proven-durable predecessor of the state the `decide` was shown, not that
     state; a caller re-deriving its refusal from `seen` would see the base without the prefix
     members' mutations. No catalog caller does; `casUpdateImpl` rethrows its captured marker.
   - `Conflict`: every contributor and every held `nullopt` receives the same `Conflict{seen}`. This
     is the class the engine gives a solo writer in the same position, and it has the engine's own
     meaning, which is two-sided: `writeLoop` returns `Conflict` both when no attempt of this inner
     write was ambiguous (nothing of ours applied) and, after an ambiguous attempt, when the
     precondition has moved and the object is not our bytes, which means ours may have landed and
     been superseded. A solo `readModifyWrite` re-decides on `seen` in both cases today, so every
     `decide` on a hot key already tolerates re-deciding against a state its own mutation may be
     in; the catalog's are token-exact and resolve exactly that shape (`beginRemoving` answers
     `AlreadyRemoving`, creation answers `Superseded` and resumes on its own `Creating` row). A
     member told `Conflict` is therefore told no more than the leader was.
   - `Refused`: every contributor and every held `nullopt` receives `Conflict{NotObserved}`.
     `writeLoop` returns `Refused` only with no ambiguous attempt of this inner write outstanding,
     so nothing applied; but it returns before any resolve read of that attempt, and `WriteState`
     is per call while a call spans several holds, so `state.last_seen` there is the leader's
     previous hold's observation, which can be older than a member's own read before it enqueued.
     Handing that to a member would let it render a `nullopt` on a state the store had already
     left, the exact hazard the memory rule exists to prevent; `NotObserved` costs each member one
     read on a path that is already terminal for the leader. The `Conflict` arm above has no such
     problem: its `last_seen` is that same `writeLoop`'s own resolve read, made after every member
     had enqueued.
   - `GaveUp`, or an exception out of `writeLoop`, with `state.sent_any == false`: nothing was
     sent; every contributor and every held `nullopt` receives `Conflict{NotObserved}` and their
     own loops observe and re-queue.
   - `GaveUp`, or an exception out of `writeLoop`, with `state.sent_any == true`: the batch's fate
     is unknown; every contributor and every held `nullopt` receives
     `GaveUp{Unresolved, sent_any = true, last_seen}` with its own bound's `Source`. This
     over-reports in one corner, deliberately: after an inner `Conflict` that proved nothing applied,
     a later `GaveUp` in the outer loop still carries `sent_any`, and members are told `Unresolved`
     where `Conflict` would have been true; the safe direction, and not to be "fixed" by weakening
     the discriminator.
   The leader writes the batch's `BatchOutcome`, allocated in step 3, on its own thread, then under
   `mutex` assigns the pointer to every taken item, sets `done`, and does `cv.notify_all()`: no
   allocation and no copy under the mutex. Each member copies what it needs (the observation, for
   its own loop) on its own thread after waking; an allocation failure there is that member's own
   exception, as an allocation in its own loop would be. At the memory cap that is one catalog body
   per member of a failed batch, transiently; accepted, and if it ever matters the place it goes is
   a `shared_ptr<const Observation>` inside `WriteState`, an engine type change. A member whose
   batch sent anything sets its own `WriteState::sent_any` on waking, so a later ending of its own
   loop reports what a solo writer in its position would report. The memory is touched only by the
   leader's guard.

   The leader's guard settles too. If the leader unwinds with members taken and not yet settled (a
   throw from `valueFor` or from `observe`'s rethrow of a local fault before any attempt, or any
   throw after one), the guard, in the same critical section in which it erases the leader's own
   item and before it notifies, delivers every held error to its owner and gives every other taken
   member `Conflict{NotObserved}` if nothing was sent and `GaveUp{Unresolved}` if something was,
   using the `BatchOutcome` the leader allocated in step 3. One critical section, so no taken and
   unsettled member is ever the first item not `done`; unconditional on how the leader left. No
   taken member is ever left `!done` behind a departed leader.

A member that received `Conflict` is back in its own `readModifyWrite` loop, exactly where a loser
of a race lands today: the loop keeps `seen` as its own last observation, pauses `conflictBackoff`,
takes a new ticket, and on entering its next hold re-decides on the base that hold provides
(memory, or `seen`, or a read). The queue re-forms by itself; the members of a failed batch wake
together and stand in roughly their old order behind whoever arrived meanwhile. A member that
received `GaveUp{Unresolved}` returns it, as its own call would: for the catalog that is
`throwCasWriteRetryLater`, and the client's retry reads a catalog that either carries the mutation
(`AlreadyRemoving`, `Superseded`) or does not.

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
call's own `mutate` produced, which is contained in the committed object but is not it; neither
function has a production caller (both are called only from `src/Disks/tests/`), so the production
writers through `casUpdateImpl` are exactly `createNamespaceStep1`, `completeCreation`,
`beginRemoving`, `reconcileStaleCreator` and `cancelStalledCreating`, none of which reads it.

**Effects to name.** A member's `decide` runs on the leader's thread while its caller is parked in
the wait loop. The two threads touch disjoint parts of the member's `CasOperation`, as Wait step 1
states: the parked caller reads immutable fields and thread-safe closures, the leader's nested
reads write `last_read_stop`, and the `casUpdateImpl` locals on the caller's stack are touched by
the leader alone until the caller wakes. Profile events and an exception's stack belong to the
leader's thread. A `decide` that issues reads holds the lane for those reads
(`reconcileStaleCreator` and `cancelStalledCreating` read the mount key inside `decide`, the GC
erase reads `gc/state`); the clamp bounds every one of them by the hold's bound, whatever policy
the caller passed, so they end inside the holder's window, and the `dropNamespaceImpl` freeze that
an earlier revision added for the cancellation's read is unnecessary and not made.

### The memory {#the-memory}

**Memory rule.** Applied in the ticket guard and nowhere else. After a hold ends: a `Committed` of
a `readModifyWrite` remembers the candidate that write landed, combined or not, and the etag the
store returned for it, if the candidate is within the size cap; every other ending of every verb
forgets. That is the only source. A remembered candidate decodes and passes
`CatalogLifeIndex::throwIfAmbiguous`, which `casUpdateImpl` runs on every base and which is a
throw, not a verdict: the base the candidate was built from had already passed it, every catalog
mutation preserves each existing `(ns, incarnation)` pair or inserts one freshly minted 128-bit
incarnation, and `encodeRefCatalog` and `checkCatalogAdmission` ran on the candidate before the
`PUT`. So the memory never presents a base that a fresh read of the same commit would not. Bytes that
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
sent to catch it. Hence the rule in step 2, INV-HK5 verbatim: **a `nullopt` is reported as
`Declined` only when it was rendered on an observation no older than this call's arrival in the
queue** (its own `GET`, its own resolve read after a lost race, or the resolve read of a batch it
was in) **or in a batch that committed** (which proves the base). A contribution needs no such
rule: the `PUT` validates its base.

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
`LOGICAL_ERROR` it is today, with `throwCatalogWriteFailure`'s message reworded, since "the write
was declined, which this call cannot produce" stops being true of a captured marker. Decode
corruption and `identityPreserving`'s `LOGICAL_ERROR` propagate from wherever they are thrown.

The second internal change: `casAdmitEntry` checks presence itself before inserting, like
`createNamespaceStep1`, and carries the duplicate as the same marker, translating it back to its
documented `LOGICAL_ERROR` after the loop. Today it lets `encodeRefCatalog`'s grammar check report
the duplicate, which on a stale memory base would raise a false `LOGICAL_ERROR` for a row the store
no longer has. `casAdmitEntry` has no production caller; its callers are the tests, and they must
not fail falsely on a hot key. The third: `createNamespace` catches `CatalogFenceMovedMarker` around step 1 and
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

The GC erase, `deleteCompletedRemovingAtSnapshot`, is today a hand-written loop: refresh the
leader's authority (one `gc/state` read into a cached flag the operation's `Liveness` closure
returns), check the exact row on the snapshot, `replace` against the snapshot's etag, then a
mandatory resolution read that is the authority for what happened and the next attempt's base,
then a growing pause. Its precondition is the etag it last read, and the row's exactness is what it
actually protects: on a refused precondition it already moves to the resolution read's newer etag
and tries again against the same exact row.

Put into a FIFO as it is, that loop loses with certainty: the etag it holds when it enqueues is
moved by every catalog write queued ahead of it, its resolution read and its growing pause then run
outside any ticket while more writes arrive, and under the sustained arrival this design targets
it would be refused at every hold until `kMaxCatalogCasAttempts` or its frozen window, where today
it at least wins a race sometimes. The erase is what reclaims catalog rows, and
`checkCatalogAdmission` refuses `CREATE TABLE` against the catalog's own bound, so a starved erase
is not a GC-only cost.

So the erase's attempt becomes what every other catalog writer is, and this is the fourth internal
change in `CasRefCatalog.cpp`: the loop's body from "refresh authority" through the `replace` is a
`readModifyWrite` whose `decide`, on whatever base the hold gives it (memory, or an observation),
first runs `refresh_authority` and then `op.admitted()`, throwing `CatalogFenceMovedMarker` if the
leader's authority is gone (carried as `nullopt` and rethrown, as every marker is, and mapped to
`FencedOut` as today); then finds the exact observed row and throws `CatalogEntryMismatchMarker`
if it is absent or differs (mapped as today: the resolution read that follows tells `Deleted` from
`EntryChanged`); otherwise returns the candidate with the row erased. After a `Committed` the
mandatory resolution read runs as today, remains the authority the reconciler feeds into its next
selection, and a `Committed` the read contradicts is retry-later as today. The reconciler,
`CatalogLifecycleReconciler`, and everything in `Gc/` are untouched: the outcomes, the result
struct and the `refresh_authority` closure are the same.

What this buys and costs. The erase queues, combines and starts from memory like any other
writer, and its precondition is the pool's latest commit rather than a cut that FIFO guarantees is
stale, so it lands at its first hold unless another server or pool wrote meanwhile. Its authority
refresh now runs inside the hold, immediately before the `PUT`, which is a narrower window than
today's, where local work and the `PUT`'s own flight sit between them; the queue wait is no longer
inside that window at all. Its `gc/state` read inside `decide` is clamped to the hold's bound. The
hand-written loop's own pacing (`Retry::backoff(attempt + 1)`) goes with the loop, so Half 2 has
no exception for it. What the flag protected and did not: the `PUT` is conditional on the base's
etag, so any catalog write by a successor moves it and a late erase is refused; and the row is one
a parent seal proved clean, so if nothing moved, the late erase is the erase the successor would
perform (a `Removing` life admits no new publication, so no hold can reappear). A GC erase that
is combined into a mount-plane leader's batch, or leads a batch with mount-plane members, is gated
per member like any other: each member's own fence, `Liveness` and lease budget before its
`decide`, and its own fence after the commit.

One gap exists today and stays: after an ambiguous attempt, `writeLoop` may sleep and reissue the
same bytes gated only by the cached flag, without running `decide` again, so the refresh is per
decision and not per physical reissue. It is a BACKLOG item; the principled closure is a TTL on
the GC lease, which would make GC's writes time-fenced like the mount plane's. This design neither
widens nor narrows it.

### Deadlines and fences {#deadlines-and-fences}

The lane adds no unbounded wait of its own. Every catalog write already runs under
`Retry::standard()`, 90 s from the call, or a lease bound if shorter; the lane binds the policy at
entry, so time in the queue spends the same window rather than adding to it, which is what today's
backoff sleeps already spend.

| who | bounded by | worst case |
|---|---|---|
| a waiter | its own fence and deadline, re-checked every slice | its window plus one slice |
| a combined member | the leader's write, then its own loop | the leader's deadline, which the combine rule requires to be no later than the member's own, so a taken member is settled inside its own window; then its own |
| the holder | its own deadline; the engine starts no attempt that cannot finish inside it, and every read its `decide` or a taken member's `decide` issues is clamped to it | its window plus one attempt timeout, unless the transport does not answer |

Nothing copies a deadline verdict from one operation to another: every `GaveUp` is the reporting
operation's own, with its own `Source`.

**A holder whose transport throttles it.** A transport fault inside a hold is repaid by
`pauseAndReissue`'s growing schedule, up to 5 s per reissue, on a counter that lives in the call's
`WriteState` and so persists across the leader's holds; those sleeps happen inside the hold, and
every other catalog writer of the pool waits through them. Today the same N writers retry
independently under a throttling store and some land. This is accepted, with the same three
clauses as the case below: nothing lands that should not; it is visible in the keyed log line; and
a store that throttles the pool's front writer is throttling the pool, so the fix is the transport's
and the store's, not the lane's. Releasing the ticket across a transport backoff and re-queueing was
considered and not taken: it would make the batch's reissue race the writers behind it, which the
precondition makes safe but which turns one throttled writer's delay into a lost race for every
member.

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
take a batch, to settle a batch, to read or set the spacing timestamp, and to move or clear
`remembered`. The fence, the `Liveness` closure, the clock, the verb's body and every `decide` run
with it released. The order is therefore: caller (no locks) → `CasHotKeys::mutex` → nothing; and,
with the ticket logically held, caller → `driver_mutex` → nothing, caller → backend mutex → nothing.
The one rule for future callers, stated where the catalog API is documented: a catalog mutation
is not entered while holding a ledger mutex, and no callback that runs while a ticket is held
(`decide`, `Liveness`, a backend hook) issues a write on any hot key of the pool.

### Invariants {#invariants}

- INV-HK1. Per constructed `Pool` and hot key, at most one conditional write is in flight at any
  moment, from any of its planes. The pool factory's bootstrap `create` of the catalog runs before
  the `Pool` exists and is outside this; it already resolves its one race by reading the winner's
  object.
- INV-HK2. Writes of one pool to a hot key are applied in the order their holds were entered, a
  batch counting as its members in queue order; a member re-queued by a failed batch enters a new
  hold behind whoever arrived meanwhile.
- INV-HK3. A `readModifyWrite` under a policy that may reissue, following a `Committed` of this
  pool on a hot key with no other ending in between and a candidate within the memory cap, obtains
  its base from memory and issues no read for it. A `single_attempt` call observes as today. A lost
  race's resolve read is not a base read.
- INV-HK4. The memory of a hot key is either absent or a `(bytes, etag)` pair this pool encoded
  and committed, durable at the moment of that commit. It never changes the semantics of a
  conditional write: a stale memory costs one 412 and one resolve read.
- INV-HK5. A `nullopt` from `decide` reaches its caller as `Declined` only when rendered on an
  observation no older than that call's arrival in the queue (its own read, or the resolve read of
  a batch it was in) or in a batch that committed; an exception from `decide` reaches its caller
  unchanged, whatever the base, and no write is sent after it.
- INV-HK6. Every member of a batch sees, as its input, exactly the object it would have seen had
  the members before it committed one at a time with no external write between; a member's
  contribution or `nullopt` is delivered only from a batch that committed, and otherwise becomes
  the class the leader's ending belongs to; a member's exception is delivered on every ending,
  including the leader's unwind; no taken member is left unsettled.
- INV-HK7. An item is removed only by its own guard; no item references a stack that may have
  unwound; "a leader takes this item" and "this item's caller leaves" are decided under one mutex
  against one flag and are exclusive, so a member's `decide` runs only while its caller is parked,
  and what the parked caller touches of its own operation is disjoint from what the leader touches.
- INV-HK8. `CasHotKeys::mutex` is held across no callback, no I/O and no `decide`, and the guard
  neither allocates nor throws.
- INV-HK9. A member's own fence, `Liveness` and lease budget for one write are checked before its
  `decide` runs, and its fence again after the commit it took part in; a `single_attempt` item and
  an item with an earlier deadline than the leader's are never combined; every read a `decide`
  issues inside a hold is clamped to the hold's bound.
- INV-HK10. On the `Generation` dialect, write attempts on a hot key from one pool are at least
  one second apart, and the spacing sleep starts only when it and what follows it fit the
  operation's bound.
- INV-HK11. A member is told the class the leader's ending belongs to and never a stronger one:
  `Committed` only from a landed batch, `Conflict` with the engine's own two-sided meaning only
  when the leader was told `Conflict` or nothing was sent, `Unresolved` whenever something was sent
  and the fate is unknown.

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

`Retry::conflictBackoff()` returns `backoff(1)`, uniform over [0, 200] ms inclusive (`backoff`
draws `thread_local_rng() % (ceiling + 1)` and doubles the ceiling from attempt 2 on). It is flat:
it does not grow with the writer's loss count, and it knows no dialect, which keeps `Retry` free of
the backend as it is today. The `Generation` dialect's per-object budget is held where it matters,
on the hot keys, by the lane's spacing below; on keys written by one or two writers, blob metas and
plain objects among them, a one-second floor per lost race would pay a rate budget nobody is
spending, on the insert path.

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
- `CasRefCatalog::deleteCompletedRemovingAtSnapshot` no longer has a pacing of its own: its attempt
  is a `readModifyWrite` (see [GC in the lane](#gc-in-the-lane)), so the two bullets above cover
  it.

Why flat is right and growing was wrong: a clean conflict is a lost race the resolve read has
already settled; the writer holds the fresh object and has nothing to wait for except
desynchronisation from its competitors. Growing the pause with the writer's own loss count makes
the oldest loser the slowest and therefore the likeliest to lose again. On the hot key, after
Half 1, conflicts arise only between pools and servers, and there a loss is dearer, because the
losing pool's whole lane waits behind its holder.

Half 2 is engine-wide, and Half 1 covers one key. Seven other `readModifyWrite` sites keep their
in-process contention (`publishCkpt` on `_ckpt` above all: both writers publish on every snapshot
and seal, and it is step 2 of every `CREATE TABLE`), and on the `ETag` dialect their writers'
retry pace goes from a schedule saturating at 5 s to a flat 200 ms. This is deliberate, and it is
the same fairness fix for the same starvation shape at smaller scale, but it raises the aggregate
attempt rate on a contended non-hot key by up to M × 5 per second for M writers. Why that is
acceptable on S3: a refused precondition is a 412 with no body, S3 has no per-object write limit,
and its per-prefix request budget is thousands per second, far above what a handful of writers per
namespace produce; sustained pressure past it answers `SlowDown`, a transport fault that takes the
growing schedule. On `Generation` the one-second floor holds the store's documented rate per
writer. Go/no-go for `_ckpt`, alongside the `ref_catalog` one: the aggregate attempt rate on
`_ckpt` keys over the acceptance run must not exceed today's by more than the number of writers
per key; if it does, declaring `_ckpt` hot is the answer, not a slower jitter.

**The `Generation` dialect's write budget.** GCS documents about one mutation per second per
object name, says excess writes may be throttled, and asks applications not to exceed the rate. A
lane that drains holders at `PUT` latency is not paced by 429s it does not receive, so on that
dialect the lane spaces write attempts on a hot key itself, across holders: no attempt starts sooner
than one second after the previous attempt on that key from this pool ended, successful or not. The
wait is one sleep: the time to that boundary plus, after a clean `Conflict`, the flat conflict
jitter, so the jitter is never absorbed by the boundary and two servers whose boundaries coincide
do not retry at the same instant forever. The S3 and emulated dialects get no spacing. Go/no-go for
the `gcs` lane: the count of 429s on `ref_catalog` and on `_ckpt` keys in `system.text_log` over
the acceptance run must not exceed today's, and the count of 412s on `ref_catalog` must fall; all
recorded in `docs/superpowers/cas/BACKLOG/gcs.md`. If `_ckpt` fails that gate on GCS, declaring it
hot is the answer. With combining, one second carries every mutation that arrived meanwhile, which
is what makes that budget sufficient. The spacing sleep goes through the plane's own sleep
function, which is interruptible on the mount plane and a plain sleep on the other two.

### Edits to the backend request contract {#edits-to-the-backend-request-contract}

In `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`:

1. In `readModifyWrite`, replace "Conflicts spend the same budget as errors: a hot key is also a
   failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
   `_ckpt` and catalog loops reissue without a sleep." with: "Conflicts spend the same deadline as
   errors but not the same pace: a clean lost race is settled by the resolve read and reissued after
   `Retry::conflictBackoff()`, a flat uniform over [0, 200] ms that does not grow with the writer's
   loss count. The growing schedule belongs to transport faults, and to a conflict that settled
   one. Inside one pool a hot key never conflicts with itself, and on the `Generation` dialect a
   hot key's attempts are spaced to the store's rate: see the hot-key write lane."
2. In the hand-written loop rule, after "sleeps with the engine's jitter between iterations", add:
   "the flat `conflictBackoff` after a refused precondition when the loop can tell it from a
   settled fault, `backoff(attempt)` otherwise and after a fault". In the site table, the
   `deleteCompletedRemoving` row becomes "`readModifyWrite`, `standard`, decide = authority refresh
   plus exact-row erase; the post-write resolution read stays and stays authoritative".
3. In the `readModifyWrite` section, one paragraph: on a hot key the initial observation may be
   the lane's remembered object, gated like an observation; a `nullopt` rendered on it is
   re-rendered on an observation before it is reported; queued calls may be combined into one
   `PUT` with as-if-serial semantics; pointer to this document.

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to departure, per item, on every exit.
- `CASHotKeyBatches`: committed batches with at least one member besides the leader.
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
   N threads, staged the same way, and asserts through a hook recording overlapping calls that no
   two writes on the key were ever in flight together and that they started in release order. A
   second sibling installs a hook that asserts `CasHotKeys::mutex` is not held whenever `decide`,
   a `Liveness` closure or a backend hook runs (INV-HK8).
2. Batch failure cascades. As above, but a hook makes the combined `PUT` `Refused` once. Assert:
   the leader receives `Refused`, every member receives `Conflict{NotObserved}` and, through its
   own loop, observes, re-queues and lands in the next batch; the final object carries every
   ticket once; `CASHotKeyBatchConflicts` equals the member count. A sibling makes the `PUT` lose
   to an external `CasRequests`: members receive `Conflict{seen}` with the external object and
   re-decide on it without a further read. A third sibling makes the `PUT` ambiguous (the transport
   throws after the store applied it) and refuses the resolve read by the bound: the leader
   receives `GaveUp{Unresolved}`, every member receives `GaveUp{Unresolved, sent_any = true}` with
   its own `Source`, no member re-decides, and the object carries every ticket (INV-HK11). A
   fourth sibling makes the `PUT` ambiguous and then has an external `CasRequests` overwrite the
   landed object before the resolve read: the leader receives the engine's precondition-moved
   `Conflict`, members receive the same, and each member's re-decide through the real
   `beginRemoving` resolves `AlreadyRemoving`, never `EntryChanged`. A fifth makes the leader's
   `writeLoop` throw before any attempt (a token of another key) with members taken: every member
   receives `Conflict{NotObserved}` from the leader's guard, a member whose `decide` had thrown
   receives its exception, and none is left waiting (INV-HK6); a hook between the guard's settle
   and its erase asserts `queueDepthForTest` is unchanged and no second `decide` ran, so the
   one-critical-section ordering has a failing-first test. A sixth: the leader's first hold
   loses a race and resolves an object O1; a member then reads the key itself, sees a newer O2, and
   enqueues; the leader's second hold takes it and is `Refused` by a hook. Assert the member
   receives `Conflict{NotObserved}`, reads before re-deciding, and never renders a `nullopt` on O1
   (INV-HK5). A seventh: a hook throttles the leader's `PUT` with a transport fault K times; assert
   the waiters stay queued through the growing backoff and the batch then lands, and that the
   keyed log line is emitted for any waiter whose deadline passes meanwhile.
3. Verdicts and errors in a batch. A member's `decide` returns `nullopt` because the leader's
   chained change made its row differ from what it observed. Batch commits: the member receives
   `Declined` after the commit and its marker (through `casUpdateImpl`) is the one a serial
   `EntryChanged` gives. Batch fails provably: the member receives `Conflict`, re-decides on a fresh
   read, sees its row unchanged, and lands. A member's `decide` that throws a non-verdict exception
   receives that exception whether the batch commits or fails, its `decide` runs exactly once (a
   counter), and it contributes nothing (INV-HK5, HK6).
4. Member fences, and the take/leave race. A combined member whose operation was resumed under a
   stale generation receives `GaveUp{FenceLost}` with `sent_any = false` and its `decide` never
   ran; a member whose fence is tripped between the `PUT` and the post-commit check receives
   `GaveUp{FenceLost}` with `sent_any = true` while the object carries its ticket (INV-HK9). A
   `Retry::once` item and an item with an earlier deadline are not combined and run as their own
   holders. The race: a hook parks the leader between taking the members and running their
   `decide`s, and the test trips one member's fence and advances another member's deadline past
   its bound while parked; both members stay parked (the queue depth does not change), the leader
   runs the first's `gate(0)` and skips its `decide` with `GaveUp{FenceLost}`, runs the second's
   `decide` normally, and each returns only after `done` (INV-HK7). Under ASan the same test with
   the `taken` check removed is the use-after-free the check exists to prevent.
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
   `GaveUp` with `last_seen == NotObserved`. A catalog committed at the admission boundary is
   re-decided from memory at once and decodes without a verdict.
9. Waiters leave on their own. While the holder is parked inside its `decide`'s nested read: a
   waiter whose deadline passes leaves with `GaveUp{Deadline}` carrying its own source and the
   keyed log line naming the holder; a waiter whose fence is tripped leaves with
   `GaveUp{FenceLost}`; a waiter whose lease budget runs out leaves with `GaveUp{Deadline, Lease}`;
   both failing reports `FenceLost`; a `remove` in the same position throws its loop's exception.
   The item is gone in every case (INV-HK7). A waiter whose deadline passes in the same slice the
   holder leaves never enters hold, never runs `decide`, reads nothing.
10. GC in the lane. The real `deleteCompletedRemovingAtSnapshot`, on an open-plane `CasRequests`
    sharing the lane, with a cached-flag `Liveness` and a counting `refresh_authority`, is called
    while a mount-plane holder is parked with one more mount writer queued: it waits, is combined
    into or led after that batch, its `decide` ran `refresh_authority` exactly once per hold and
    inside it, the erase lands in one `PUT`, the mandatory resolution read follows, and the call
    returns `Deleted`; the mount plane's next write starts from memory (the erase's commit is a
    `readModifyWrite`'s and is remembered). A sibling keeps N mount writers arriving for the
    erase's whole window and asserts the erase returns `Deleted` at its first or second hold, never
    exhausting `kMaxCatalogCasAttempts`. A sibling flips the flag to false inside
    `refresh_authority`: the `decide` throws the fence marker, nothing is sent, and the reconciler
    sees `FencedOut`. A sibling has an external `CasRequests` change the row between the erase's
    enqueue and its hold: the `decide` sees the mismatch on the fresh base, nothing is sent, and the
    resolution read tells `Deleted` from `EntryChanged` as today. The
    flag flipped to false by the test while queued: the wait loop's gate refuses with
    `GaveUp{FenceLost}` and nothing is sent. This proves the gate is wired into the wait, not
    anything about GC's real authority window, whose refresh runs before the ticket is taken.
11. Stalled holder. The holder is parked inside a `PUT` that does not return; the clock is advanced
    past every waiter's deadline. Assert each waiter leaves with its own `GaveUp` and the log line,
    no second write started, only the holder's item remains, and when the `PUT` is released the
    holder commits, the memory holds its candidate, and the next writer starts from it. A waiter
    with a `Liveness` closure flipped to false while parked leaves at the next slice with
    `GaveUp{FenceLost}`. `cancelStalledCreating` through `dropNamespaceImpl`
    issues its terminality read under the frozen policy: with that read answering retryable
    failures, the holder gives up at the outer deadline, not 90 s later.
12. Half 2, parameterized over `readModifyWrite` and `readModifyWriteOnPresence`. K clean lost
    races on a non-hot key: `CASRequestConflictPause` K, `CASRequestReissue` 0, every sleep within
    [0, 200] ms on every dialect. K races each preceded by a 429-class fault: `CASRequestReissue`
    K, `CASRequestConflictPause` 0, the growing schedule. A `remove` on a hot `Generation` key
    whose first attempt takes a transport fault records two attempts a second apart, the reissue
    spaced from inside `readLoop`.
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
  Its entry condition, which the catalog meets and which is a property of a key's `decide`s, not
  of the engine: every `decide` on the key resolves "my own mutation may already be in the state I
  am re-deciding against" without a wrong verdict, as `beginRemoving` does with `AlreadyRemoving`
  and creation does with `Superseded`; `publishCkpt`'s declines must be checked against exactly
  that before `_ckpt` is declared.
- A member's nested read that the clamp refuses surfaces as that member's own retry-later; if the
  acceptance run shows reconciliations or cancellations failing that way under load, a decide-side
  budget is the follow-up: a BACKLOG note.
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
rather than assumed; the conflict pause is flat and a conflict that settled a fault keeps the
growing schedule; the `Generation` dialect's hot keys are spaced to its documented rate; a holder
whose transport never answers is an accepted tradeoff with its fix placed in the transport; and
the step-1 fence marker is caught where it was not.

Revision 17 folds in a review of revision 16 by a fresh `opus` reviewer, which found the three
reversals sound and six majors, all fixed here: a member of a batch whose leader could not prove
the `PUT` dead now receives `GaveUp{Unresolved}` rather than a manufactured `Conflict`
(INV-HK11); a member's `decide` exception is delivered on every ending and is never re-run; the
wait loop consults the `Liveness` closure, so a stopped task parked in the queue leaves at the next
slice; the GC erase's commit forgets the memory, so the next mount write reads once and the text
and test no longer claim a 412; the parked caller and the leader touch disjoint parts of the
member's operation, stated as such instead of "one thread at a time"; and Half 2's engine-wide
scope is argued for the seven non-hot sites with a go/no-go on `_ckpt`. Minor corrections: the
holder is the first item not yet `done`; the batch is allocated under the leaf mutex and has no
cap; the memory is written in the guard only; the guard's reporting tail is inside a catch-all;
the lane pointer the sleep helper reads is named; `Declined{seen}` is the base on purpose; the
`driver_mutex` audit lists every acquirer and its count; the `CREATE` percentiles cite both
windows; `casAdmitEntry` is named as test-only; `removeCurrent` holds one ticket around its loop.

Revision 18 folds in a second `opus` review, of revision 17, which checked the previous round's
nineteen findings one by one and found one critical: nothing made "a leader takes this item" and
"this item's caller leaves" exclusive, so a caller leaving on its fence or deadline in the instant
after being taken would have had its `decide` run on a dead stack (revision 2 had "a carved item
cannot leave" and the rewrites lost it). Fixed by a `taken` flag set in the take's own critical
section and checked under the same mutex before any departure (INV-HK7). Its majors, all fixed:
`writeLoop` returns `Conflict` in a second, ambiguous-then-superseded arm, so the settlement now
tells members the class the leader was told, with that arm's two-sided meaning argued from what a
solo writer already tolerates, instead of a discriminator that was false for it (INV-HK11); a
leader that unwinds with members taken settles them from its guard; the ticket boundary inside
`readModifyWrite` is stated (one ticket per attempt, the base chosen at the hold); settlement
shares one pre-allocated `BatchOutcome` and copies nothing under the mutex; the batch's cost is
stated in two clauses with the frozen-policy line as a precondition; and the `Generation`
one-second conflict floor is gone, since the lane's spacing already holds the budget on the hot
keys and the floor would have landed on blob metas and plain objects for nothing. Minors: which
operations the wait's `Liveness` check reaches; `Unresolved` over-reported after an inner
`Conflict`, deliberately; INV-HK2 speaks of holds; the memory's round-trip argument is the real
one; `conflictBackoff` knows no dialect and its interval is inclusive; `throwCatalogWriteFailure`'s
message; two `driver_mutex` rows relabelled; the spacing sleep is interruptible on the mount
plane only.

Revision 19 folds in a third `opus` review, of revision 18, which confirmed the `taken` handshake
and, checking the previous reviewer's counter-example against the code, found it does not exist:
`beginRemoving` re-reads on its mismatch marker and answers `AlreadyRemoving`, so forwarding the
engine's two-sided `Conflict` to members is sound. Its three majors, all fixed: the `Generation`
spacing had no call site before a hold's first attempt, the common case, so the pre-attempt helper
is now called at the top of every attempt loop with a zero owed delay; the `Refused` settlement
handed members a `last_seen` from the leader's previous hold, older than a member's own read, so
`Refused` now settles as `Conflict{NotObserved}` and INV-HK5 speaks of an observation no older
than the item's arrival; and the GC erase's cut etag is fixed before it enqueues, so FIFO turns
today's race into a certain refusal per batch queued ahead, stated as its cost with a converging
shape, a test that asserts that shape, and the ticket-around-read-and-replace change placed as a
note. Minors: `Declined{seen}` is the batch's real base, never the chained input; the leader's
guard settles and erases in one critical section; a throttled holder's in-hold backoff is a named
tradeoff; INV-HK1 is scoped to a constructed pool; INV-HK3 excludes `single_attempt`; the
`Conflict`-forwarding property is the entry condition for declaring a key hot; a member sets its
own `sent_any`; `casUpdate` is test-only too; per-member copies of a failed batch's observation are
accepted and their alternative named.

Revision 20 folds in a fourth `opus` review, of revision 19, which found the safety core stable
and the remaining problems in the time dimension. Its four majors, all fixed by two
simplifications and two contracts. A taken member's nested read ran under the member's own
window, later than the leader's, so a hold could outlast the leader's deadline and produce a batch
the leader could no longer write: every read an operation issues inside a hold is now clamped to
the hold's bound, for the holder and for taken members alike, which also retires the
`dropNamespaceImpl` freeze an earlier revision added. The GC erase, queued as the hand-written
loop it is, would have been refused at every hold under sustained load, because its etag is fixed
before it enqueues and its resolution read and growing pause run outside any ticket: its attempt is
now a `readModifyWrite` whose `decide` refreshes authority and checks the exact row, so it queues,
combines and starts from memory like every writer, with its authority refresh inside the hold
immediately before the `PUT`, and Half 2 has no exception left. `removeUnder` has no attempt loop
of its own, so the spacing call site moves inside `readLoop`, conditional on a mutation under a
held lane. The pause helper's contract is split into a computation that gates nothing and
per-family wrappers that gate, sleep and report in their family's terms. Minors: members are gated
with the reservation their own write would make and the leader stops taking when its own window
no longer fits a write; the memory section quotes INV-HK5 verbatim; the Goals bullet names the
`readModifyWrite` restructuring; a member's endings carry `attempts_sent = 0` with the reason;
`CASHotKeyBatches` is defined; `Declined{seen}` is named as a proven predecessor of the state the
`decide` saw; the guard's settle-then-erase ordering has a failing-first test.
