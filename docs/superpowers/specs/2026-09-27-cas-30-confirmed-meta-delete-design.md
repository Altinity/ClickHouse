---
description: 'Design for CAS-30: the delayed confirmed-meta delete re-validates at execution time that the blob body is absent, so it can never remove a condemn marker that fences a live incarnation.'
sidebar_label: 'CAS-30 confirmed-meta delete'
sidebar_position: 40
slug: /superpowers/specs/2026-09-27-cas-30-confirmed-meta-delete-design
title: 'CAS-30: confirmed-meta delete re-validates body absence — design'
doc_type: 'reference'
---

# CAS-30: confirmed-meta delete re-validates body absence — design (rev.2) {#cas-30-design}

Task: `CAS-30` (milestone M1, type bug, data-loss class). Branches: `cas-gc-rebuild` and
`altinity/antalya-26.6`, same code in both. Format: no on-S3 change.

Revision history: rev.1 proposed round-discriminated deletes plus marker claiming (A+B) and a TLA+
extension; codex review round 1 (`docs/superpowers/reports/2026-09-27-cas-30-codex-reviews/review_r1.md`)
found three MAJOR defects (the graduation gate accepts any `Condemned` marker regardless of round, so B is
not a graduation precondition; `rebuildBaseline` picks the new round from `gc/gen` debris and never from
marker rounds, so round equality has no epoch boundary across a rebuild; the model's writer has no drop
lifecycle, so the proposed trace was unreachable) and recommended the body-absence re-validation below.
rev.2 adopts it and drops A, B and the TLA+ extension.

## 0. Problem {#problem}

Every blob body `blob/<hash>` has a sibling freshness marker `.meta` (`BlobMeta{state, condemn_round, size}`).
The marker is the writer's adopt gate: a writer that finds the body present and the marker absent or `Clean`
adopts the current incarnation without re-upload (`PartWriteTxn::ensureBlobPresent`); a writer that finds
`Condemned` re-materializes under a fresh incarnation. GC graduates a condemned entry to `delete_pending` only
after the `Condemned` marker is confirmed durable (`confirm_condemned_marker` in `CasGc.cpp`, model
`CaGcCondemnMarkerGate`), and the next round deletes the body by exact incarnation token.

After the body delete (`Removed` or `Gone`) the round schedules `deleteConfirmedMeta` on the bounded meta pool
(`GcMetaWriter::scheduleConfirmedMetaDelete`). The job captures only the blob ref; when it runs it calls
`loadMeta` and deletes **whatever marker it sees then** by the etag of that read.

The invariant the marker exists for: **from the moment a condemn marker for incarnation X is confirmed
durable until X's body is confirmed deleted or absent, the marker stays `Condemned`.** Equivalently: a marker
may be removed only while no body exists at the key.

### Reachability {#reachability}

Within one leader the job cannot break the invariant. A new incarnation t2 can exist only after t1's body is
deleted, which happens in the `pending_deletes` phase of round R_d; the job is queued in that phase and
drained in `meta_pool_wait` before `round_commit` (`CasGc.cpp:941-962`); a condemn of t2 is a fold of a
round ≥ R_d+1. By then the job has run.

The only path is a **live deposed leader**. The lease is checked once at round start (`lease` phase,
`CasGc.cpp:422`); the fold can outlast the lease; `pending_deletes` is documented as "pre-CAS, safe at any
leader staleness" and runs regardless. Leader A, deposed mid-round, deletes body t1 and queues job J behind
other meta jobs on a slow store. While J waits: writer W publishes t2 at the same key and later drops its
part; new leader B condemns t2 and confirms `Condemned(R_B)`. J runs, reads B's marker, deletes it by that
etag. An adopter W2 then finds body t2 present and marker absent, backfills `Clean` and adopts t2 without
re-upload; if its edge lands inside the `[cut, pending_deletes]` window of B's deleting round, B deletes body
t2 by exact token and W2's committed ref names a deleted blob.

A killed process is harmless: its queued job never runs. The window needs a still-running deposed leader
parked in `meta_pool_wait` for longer than W's insert plus B's fold, then a same-hash republish and a
same-hash adopt in one specific round window. Narrow, but it is exactly the adversary the surrounding phase
promises to be safe against, and the fix costs one request per deleted blob.

## 1. Change {#change}

One change, inside `deleteConfirmedMeta` (`Gc/CasGcMetaWriter.cpp`), no signature change:

1. `loadMeta`; absent → done. Keep the etag of this read.
2. `op.head(layout.blobKey(ref))`; body **present** → done. Some incarnation lives at the key; whether it is
   `Clean` (a writer's) or `Condemned` (another condemn's fence), the marker is not this job's to remove.
3. Body absent → `deleteMetaExact` with the etag from step 1. `Mismatch` → done (the marker changed hands
   between the read and the delete; whoever changed it owns it). `Gone` → done.

Why this is sound against a concurrent publication (the protocol is HEAD-before-PUT, untouchable):

- A writer that publishes after step 2 observed the body absent, PUT a fresh incarnation, then reconciles the
  marker to `Clean` (`reconcileMetaClean`: `create` on the `Absent` reason, `Conflict` → read-modify-write).
  Either its write lands before step 3, and step 3 gets `Mismatch`; or step 3 deletes the old marker first,
  and the writer's `create` succeeds. Both end with a `Clean` marker over a live body.
- A writer whose publication fails after the body PUT leaves a fresh body with the old or no marker. No
  old exact-token delete can match that fresh incarnation, and the next condemn of it writes its own marker.
- A later condemn of a fresh incarnation (the CAS-30 trace) is covered by step 2: the body is present.

Cost: one `HEAD` per confirmed meta delete on the async pool, i.e. per deleted blob. It joins the `.meta`
delete that already exists there; the round thread is not touched. When `CAS-284.5` batches `.meta` deletes
after bulk body deletes, the same re-validation applies per key (the bulk path must not skip it).

### Not changed {#not-changed}

- `BlobMeta`, `RetiredEntry`, `gc/state`, outcome logs: no field added, no on-S3 change.
- `writeCondemnedMeta`, the graduation gate, the writer: untouched. The gate keeps accepting any durable
  `Condemned` marker (per-hash by design).
- `scheduleConfirmedMetaDelete(const BlobRef &)` keeps its signature; the call site in `pending_deletes`
  is unchanged.

### Alternatives considered {#alternatives}

- **Round-discriminated delete plus marker claiming (rev.1 A+B).** Needs round uniqueness across rebuilds,
  which `rebuildBaseline` does not guarantee (it numbers from `gc/gen` debris, never from marker rounds), and
  needs the graduation gate to become round-aware. More state-machine surface for the same invariant.
- **Thread the etag captured at decision time** (the task's wording). A leader that inherits a
  `delete_pending` row after a handoff never read the marker and has no etag; and the etag alone does not
  stop a later condemn from borrowing the old marker. Rejected as the sole mechanism.
- **Never delete the marker.** Leaks one `.meta` key per deleted blob forever. Rejected.

## 2. TLA+ {#tla}

`CaGcCondemnMarkerGate` drops the marker atomically with the body delete (`GSettleRedelete`), is
single-leader, and its writer has no drop lifecycle, so the CAS-30 trace is outside its perimeter and a
faithful extension needs a second writer lifecycle (publish, abandon, condemn, second adopt) plus a
cross-leader delayed job. That is a new focused model, not a cfg row. Given the size of the fix (one `HEAD`
guarding one delete, argued directly from HEAD-before-PUT) and the narrow reachability, no model work is
proposed. The models README gets one sentence in the `CaGcCondemnMarkerGate` row: "the delayed
confirmed-meta delete and a deposed leader's late job are outside this model; see CAS-30".

## 3. Tests, failing first {#tests}

- **T1** `CASGcMetaWriter.ConfirmedMetaDeleteKeepsTheMarkerWhileABodyExists` (gtest, plain
  `InMemoryBackend` through `Pool::open`): publish a blob body at the key (through a `PartWriteTxn`, so the
  key and envelope are real), then overwrite its marker with `Condemned(7)` through `casMeta`;
  `scheduleConfirmedMetaDelete(ref)`; `drain`; the marker is still `Condemned(7)` with the same etag. Red
  today (deleted).
- **T2** `CASGcMetaWriter.ConfirmedMetaDeleteRemovesTheMarkerOfAnAbsentBody`: seed `Condemned(1)` with no
  body; schedule; drain; marker absent. Green today; pins that the guard does not over-refuse.
- **T3** `CASGcMetaWriter.ConfirmedMetaDeleteMismatchesAMarkerReplacedAfterItsRead`: seed `Condemned(1)`
  with no body; hold the job at its `remove` with a **remove-only, one-shot** latch (a small subclass of
  `InMemoryBackend` in the test; `MetaWriteLatchBackend` blocks every armed meta write and would block the
  replacement below); schedule; await the latch; replace the marker with `Clean` through a separate
  `CasOperation`; release; drain; the marker is `Clean`. Pins step 3's etag guard.
- The existing `CASGcMetaWriter.RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction` keeps its shape
  (no body seeded, so the delete still happens) and stays green.
- Suites `CASGcMetaWriter*`, `CASBlobMeta*`, ack-floor and retired-in-run gtests green; gate `CAS*` under
  ASan and TSan (`utils/cas-gate`). No `LOGICAL_ERROR` is added, so no death test.

Acceptance criteria of `CAS-30`, to be rewritten in the task (not "restated"): (1) T1; (2) "no call path
deletes a marker without observing, in the same job, that no body exists at the key, and the delete is exact
on the etag of that job's own read"; (3) suites green under ASan and TSan. The task's priority drops from
critical to medium with the reachability argument of §0 recorded in its notes.

## 4. Risks {#risks}

- **One extra HEAD per deleted blob** on the async pool. Bounded by the pool; no round-thread cost.
- **A skipped delete leaves a marker behind** (step 2). It is a live incarnation's marker and its owner
  removes or rewrites it; no new leak class.
- **The delete pool stays advisory.** A lost or thrown delete leaves a `Condemned` marker over an absent
  body; a writer re-materializes once. Unchanged, and the safe direction.

## 5. Review questions {#review-questions}

1. Is the argument in §1 complete against every interleaving of one delayed job, one publishing writer and
   one condemning leader? Name a counterexample or the happens-before that closes it.
2. Is skipping the TLA+ work defensible given §2, or is there a cheap focused model that pays for itself?
3. Anything simpler than one guarding `HEAD`?
