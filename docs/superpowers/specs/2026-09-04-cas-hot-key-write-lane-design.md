---
description: 'Design record for the per-pool write lane above the request engine for hot compare-and-swap objects (the ref catalog first). Phase A shipped: one FIFO per key shared by the pool''s planes, one conditional write in flight per key, an LRU of last known objects so the next write needs no read, a decide''s verdict delivered only from a fresh read, and lost races between servers paced by a flat jitter. Combining, GCS spacing and the hold clamp are phase B, designed and deferred to BACKLOG.'
sidebar_label: 'Hot-key write lane'
sidebar_position: 45
slug: /superpowers/specs/cas-hot-key-write-lane
title: 'CAS hot-key write lane'
doc_type: 'reference'
---

# CAS hot-key write lane {#cas-hot-key-write-lane}

Revision 34, 2026-09-04, phase A. **Status (groomed 2026-09-25): phase A shipped as designed.**
Implemented on branch `cas-hot-key-lane`, merged into `cas-gc-rebuild` at `9bf134686af` and landed
on `altinity/antalya-26.6` at `4ec755474fb`. Verified against this document's checklist (below) by
the whole-branch review recorded in
`docs/superpowers/cas/2026-09-04-hot-key-lane-phase-a-rulings.md`: every rule kept, with one
recorded exception (a fifth `friend` grant, `CasHotKeys` on `CasRequests`, needed because
friendship does not reach through `CasOperation::owner`). Two follow-ups are still open and
tracked in `docs/superpowers/cas/BACKLOG.md` under `{#hot-key-lane-phase-a-followups}`: the
acceptance measurement below (Observability) has not been run, and the new death-test twin has
never executed under a debug/sanitizer build. Phase B (combining, GCS spacing, the hold clamp, the
GC erase inside the lane, `_ckpt` through the lane) is tracked separately under
`{#hot-key-lane-phase-b}` and is out of scope for this document's status; the interface contract
phase B must not break is kept below under [Designed for phase B](#designed-for-phase-b).

This is a trimmed version of the original 1020-line revision-34 document, kept as a design record
now that phase A is implemented. The full text, including the measured RCA numbers, the complete
class pseudocode, the lock-order audit tables, and the 34-revision review history, is recoverable
from git history at `docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md` before
this trim, and the RCA itself lives on in `docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md`.

## The problem, in one paragraph {#the-problem}

Every `CREATE TABLE`/`DROP TABLE` on a content-addressed disk mutates one pool-wide object,
`cas/ref_catalog`, through a conditional write. `CasRefCatalog::casUpdateImpl` used to call
`CasOperation::readModifyWrite` under `Retry::standard()`: a `GET` then a `PUT`, and on a lost race
a sleep on the growing full-jitter schedule shared with transport faults, so the oldest loser
became the likeliest to lose again. Worse, every writer in one process raced every other writer in
the *same* process, though compare-and-swap is needed only against other servers. Measured on ten
parallel stateless jobs: `DROP TABLE` p90 11.9 s, max 34.7 s; 113 `PreconditionFailed` in 80 s from
53 threads; one loser making 35 attempts with gaps growing to the 5 s cap. Full numbers: the RCA
doc above and `docs/superpowers/cas/BACKLOG/performance.md` `{#ref-catalog-cas-starvation}`.

## Overview {#overview}

**Half 1, the lane.** A per-pool component above the request engine, `CasHotKeys`, reachable from
any of the pool's three `CasRequests` planes through the operation that admits a write. A caller
submits a mutation of one key: a `decide` over the key's bytes. The lane takes a FIFO ticket for
the key, and when the ticket is at the front the caller obtains a base (the cache's last known
object, else a `GET`), runs its `decide`, and lands the candidate in one conditional write through
the engine, on its own operation and thread. The engine's `WriteResult` is returned as it is,
nothing reclassified, so a caller handles it exactly as it handles today's, plus one rule: resubmit
on `Conflict`. A verdict a `decide` renders on a cached base — a refusal by exception, or "nothing
to write" — is never reported; the lane reads and decides again. `CasRefCatalog` writes through the
lane; the GC erase does not (see [Placement of what this does not do](#placement-of-what-this-does-not-do)).

**Half 2, conflict pacing.** In `CasOperation::readModifyWrite` and `readModifyWriteOnPresence`, a
clean refused precondition is repaid after `Retry::conflictBackoff()`, a flat uniform over
[0, 200] ms, and no longer advances the transport-fault counter. The lane's callers pause by the
same rule between a `Conflict` and their next submission.

## The contract of a `decide` {#the-contract-of-a-decide}

A caller may write a key through the lane when every `decide` it will ever submit for that key
meets four conditions (the catalog's meet them today; they are the entry condition for any other
key):

1. It may be run more than once, and a later run on a fresh read is the decision that counts;
   running it has no externally visible effect that a second run would double.
2. It refuses by throwing and declines by returning nothing, and either is the verdict
   `readModifyWrite` would render on the same base. The lane reports a verdict only when it was
   rendered on a fresh read; one rendered on a cached base is discarded and the `decide` runs again
   on a read.
3. It issues no write through the lane to any key, and its reads go through the operation it was
   given, under a policy its caller chose knowing they run inside the hold.
4. It reads `base->bytes` and never `base->etag`. Phase A does not need this condition; phase B
   does (combining hands a member a chained `Object` whose etag is a placeholder), and it is stated
   now so no phase A caller is re-audited for it.

## Invariants {#invariants}

- **INV-1.** Per constructed `Pool` and key written through the lane, at most one conditional write
  is in flight at any moment from the operations that write through it. A writer that does not use
  the lane (the GC erase) races the lane's writes as it races everything today.
- **INV-2.** Holds on a key are entered in the order their submissions were queued; a submission
  resubmitted after a `Conflict` queues behind whoever arrived meanwhile.
- **INV-3.** The cache never changes the semantics of a conditional write: a write decided on a
  stale entry cannot land and costs one 412 and one resolve read. A verdict a `decide` renders on a
  cached base is never delivered; it is re-rendered on a read, or replaced by that read's own
  `GaveUp`. The store's answer to a write is always delivered, because it is never false. A
  `single_attempt` submission does not start from the cache.
- **INV-4.** `submit` returns for its caller a result the engine produced for that submission,
  through the paths `readModifyWrite` uses; the lane invents no result, reclassifies none, and
  lets no exception of its own replace one. What one submission is not is one `readModifyWrite`
  call: each submission has its own `WriteState`, and the caller's loop, not the engine, decides
  whether a `Conflict` is followed by another.
- **INV-5.** An item is removed only by its own guard; no item outlives its caller's stack; a lane
  is erased only while its queue is empty; `CasHotKeys::mutex` is held across no callback, no I/O
  and no `decide`; the guard neither allocates nor throws.
- **INV-6 (Half 2).** A clean lost race is repaid after `conflictBackoff()` and does not advance
  the reissue counter; a conflict that settled an ambiguous attempt is repaid on the growing
  schedule, the engine's internal reissues restarting per submission.

## The callers {#the-callers}

**The catalog.** `CasRefCatalog::casUpdateImpl` submits through `op.hotKeys().submit(...)` instead
of calling `readModifyWrite`, in a hand-written loop under a policy frozen once at entry: on
`Conflict`, pause (`conflictBackoff()` if clean, `Retry::backoff` on the caller's own fault count
if the conflict settled a transport fault) and resubmit; anything else is handled as
`readModifyWrite`'s result was, including the lost-fence marker translation `createNamespace`'s
step 1 now also catches. No attempt cap: the loop ends at the frozen deadline through `GaveUp{Deadline}`.
`casUpdate`, `casAdmitEntry` and the five lifecycle functions keep their signatures, markers and
outcomes.

**The GC erase.** `deleteCompletedRemovingAtSnapshot` does **not** write through the lane; it stays
the hand-written loop it is today (snapshot read, `replace` against the snapshot's etag, resolve
read, growing pause). It races the lane's writes as it races every writer today: the loser's
precondition is refused and settled by its own resolve read, and a GC erase that wins leaves the
lane's cache stale, costing the pool's next catalog submission one 412 and one resolve read.
Bringing it into the lane is phase B, `{#hot-key-lane-phase-b}`.

## Designed for phase B {#designed-for-phase-b}

Phase B (BACKLOG `{#hot-key-lane-phase-b}`) adds combining, `Generation` spacing and the hold
clamp. What phase A fixed so that none of them reopens the callers — the interface contract phase B
must preserve:

- `submit`'s signature, `Decide = DecideOnObject`, and "returns the engine's `WriteResult`
  unchanged" do not change. Combining is internal to the lane: whether the holder also lands the
  items queued behind it. A caller's loop, its `Conflict` handling and its pause rule are the same
  in both phases.
- The queue holds `Item`s, one per submission, and the guard is the single remover. Phase B adds to
  `Item` the leader's view of a member (`op`, `bound`, `decide`, `taken`, `result`) and adds
  settlement of taken members to the guard's critical section; the FIFO, the slice wait and the
  leave rule stay.
- The hold is base, decide, write, settle, in that order. Combining inserts "take and chain" between
  decide and write, and "tell each member" into settle. The cache stores the candidate the write
  landed.
- `Conflict::any_ambiguous` is what a member is told with its `Conflict`.
- The lane is erased when its queue empties; spacing changes only that rule (an emptied lane
  outlives its last write by the interval) and adds one timestamp to `Lane`.
- The stalled-holder tradeoff and the `driver_mutex` audit are the same for a batch as for a single
  hold.

## Half 2: conflict pacing {#half-2-conflict-pacing}

`Retry::conflictBackoff()` returns `backoff(1)`, uniform over [0, 200] ms inclusive. It is flat: it
does not grow with the writer's loss count. On a `Conflict` whose inner write had no ambiguous
attempt (a clean refused precondition, settled by the resolve read), `readModifyWrite` and
`readModifyWriteOnPresence` pause by `conflictBackoff()` and leave `state.reissues` untouched,
recording `CASRequestConflictPause` rather than `CASRequestReissue`. A `Conflict` whose inner write
had an ambiguous attempt keeps the growing `pauseAndReissue` schedule: the fault is the signal that
must pace the loop. Why flat is right and growing was wrong: a clean conflict is a lost race the
resolve read has already settled; the writer holds the fresh object and has nothing to wait for
except desynchronisation from its competitors. Growing the pause with the writer's own loss count
makes the oldest loser the slowest and therefore the likeliest to lose again.

Half 2 is engine-wide; the lane covers only the keys that write through it. The seven other
`readModifyWrite` sites (`publishCkpt` on `_ckpt` above all) keep their in-process contention but
now retry at a flat mean of 100 ms instead of a schedule saturating at 5 s — the same fairness fix
at smaller scale. Whether `_ckpt` also needs the lane (phase B) is gated on measuring peak request
rate on a contended `_ckpt` key against S3/GCS per-prefix budgets; see `{#hot-key-lane-phase-b}`.

## Observability {#observability}

Profile events, mirroring the ref-log lane's `CASRefQueueWaitMicroseconds`:

- `CASHotKeyQueueWaitMicroseconds`: time from enter to hold or departure, per item, on every exit.
- `CASHotKeyCacheStarts` and `CASHotKeyReadStarts`: how a hold obtained its base.
- `CASHotKeyCacheVerdictsReread`: verdicts rendered on the cache and re-rendered on a read.

One log line, naming the key: a waiter that left on its own fence, lease or deadline while another
item was at the front, with that item's ticket and how long it has held.

**The acceptance measurement (still open, see status banner above):** ten minutes of the parallel
stateless suite on the CA-s3 lane, `system.query_log` for `DROP TABLE`/`CREATE TABLE` percentiles,
and the count of `PreconditionFailed` on `ref_catalog` in `system.text_log`, before and after; the
same run's `CASHotKeyQueueWaitMicroseconds` per submission against the `PUT` rate on the key gates
phase B's combining (it pays when the queue wait, not the write, dominates a submission). Recorded
in `docs/superpowers/cas/BACKLOG.md` under `{#ref-catalog-cas-starvation}` and
`{#hot-key-lane-phase-b}` once run.

## Placement of what this does not do {#placement-of-what-this-does-not-do}

- Combining, `Generation` spacing, the hold clamp, the GC erase inside the lane, and `_ckpt`
  through the lane: BACKLOG `{#hot-key-lane-phase-b}`, with the rules twenty-two review rounds
  established for them and the measurement that gates each.
- Freezing `dropNamespaceImpl`'s policy once and passing it to `cancelStalledCreating`'s
  creator-fence read, as `resolveNamespaceLife` already does: a line under the same BACKLOG item.
- A whole-request deadline in the S3 transport (the only planned answer to a holder whose transport
  never answers): a BACKLOG item.
- The two remaining `DROP TABLE` costs (part-commit round trips, `StackTrace` capture for expected
  412s) stay in `{#stateless-lane-wall-time-is-drop-table}`.

## Revision history (condensed) {#revision-history}

Revisions 1-26 (2026-09-04) designed a larger change — combining, an in-lane GC erase, memory,
`Generation` spacing, at times eviction of a stalled holder — through sixteen `codex` and six
`opus` review rounds, and never converged (findings per round stayed between eight and
twenty-two). Revision 26 is that line's last, commit `26bde9f9604`; its content is
`{#hot-key-lane-phase-b}`. Revision 27 cut the landing to what the measurement needed: the FIFO
ticket, the cache under one absolute rule, the engine's result unchanged, the flat conflict pause
with the growing schedule for a settled fault, the step-1 marker catch, the `driver_mutex` audit,
and the stalled holder as an accepted tradeoff. Revisions 28-34 folded six further `codex` review
rounds (criticals on: `single_attempt` honoured in the catalog loop; the creator-fence re-decide
flip between two `decide` runs, resolved by stating the second run is a serial execution at the
fresh read; a cached start must never end in a hint-dependent result other than `Committed`/`Conflict`;
withdrawing a patch that would have issued two writes per submission; an unbounded byte-weighted
cache of empty objects; the resolve-read-after-ambiguity `Committed` rule inherited from the
backend contract, not invented here). Revision 34 (this one) records the sixth `codex` review of
revision 33: no critical, no major, and the verdict that the design was sound enough to implement
from — which the implementation plan and its review then did. Checklist a review verifies against,
each item traced to code and test in
`docs/superpowers/cas/2026-09-04-hot-key-lane-phase-a-rulings.md`:

a verdict on a cached base is never delivered, whatever refused its validation; the base read is
the engine's `observe` with `readModifyWrite`'s conversion, never the throwing `read`; the caller's
policy is frozen once, not a fresh `standard`; `Conflict::any_ambiguous`, because `attempts_sent`
cannot stand in; a cache fill that throws after a landed `PUT` is never the result; the guard is
the single remover and neither allocates nor throws; a waiter re-checks its own fence, `Liveness`,
lease and deadline every slice, in that order; the lane is erased only when empty; no attempt cap
on the catalog loop; the 200 ms pause overshoot accepted; eviction of a stalled holder rejected; the
`driver_mutex` audit; the step-1 marker catch; engine changes are exactly the four this document
names (plus the recorded fifth `friend` grant); one instance per pool, declared before its three
planes; no durable-format, key-shape or protocol-step change.
