---
description: 'Design for closing the CAS fetch-by-relink commit-before-release gap (codex-6, #42/#43) with publish-then-confirm: the receiver publishes its manifest + precommit as today, then asks the sender "is your committed ref for this part still live?" and promotes only on a positive answer. EDGE-BEFORE-OBSERVE lifted to part level; zero GC/codec/sweep changes. Supersedes both the 2026-07-15 retention-pin design and the short-lived 2026-07-23 reserved-precommit design (found unsound in adversarial review). Also covers B66b relink-into-detached and the RPL-5 REPLACE PARTITION queue-clone test slice.'
sidebar_label: 'CAS Fetch-Handoff Publish-Confirm'
sidebar_position: 20260724
slug: /superpowers/specs/cas-fetch-handoff-publish-confirm-design
title: 'CAS fetch-handoff — publish-then-confirm relink'
doc_type: 'reference'
---

# CAS fetch-handoff — publish-then-confirm relink {#cas-fetch-handoff-publish-confirm}

**Date:** 2026-07-23
**Branch:** `cas-gc-rebuild`
**Status:** design (user-approved brainstorm 2026-07-23, incl. the failure-matrix correction)
**Covers:** codex-6 / #42/#43 (relink commit-before-release gap), B66b (relink-into-detached, RPL-4
perf cliff), RPL-5 test slice (`REPLACE PARTITION` queue-clone relink proof).
**Out of scope:** B66a (local-backend torn read — `BACKLOG.md` §14), the pre-existing ordinary
`Precommit` unmatched-`-1` hazard discovered during review (own BACKLOG §3 entry), bulk warm-up.

## Problem {#problem}

Same-pool replication uses fetch-by-relink: the sender transfers only the part's manifest; the
receiver publishes its own ref over the already-shared blobs (zero byte cost). The gap
(self-documented at `ContentAddressedMetadataStorage.cpp:1990`): the sender is fire-and-forget —
it releases the part when the HTTP response completes, before the receiver's publish. If the
receiver's `precommitAdd` edge-PUT is not yet durable while the sender's now-`Outdated` part is
collected (removal `-1` folded, condemned, graduated, deleted — ≥ 3 GC rounds + `old_parts_lifetime`),
the receiver later commits a manifest whose blobs are gone — a dangling committed manifest
(fsck-detected, not silent).

## Design history — why the previous two designs are gone {#history}

- **Retention pin** (`2026-07-15-cas-fetch-handoff-retention-pin-design.md`, superseded): a
  pool-global GC overlay — a second reachability source, union folding, removal-deferral,
  owner-liveness machinery. Rejected as structural growth in the most safety-critical component.
- **Reserved precommit** (2026-07-23, removed same day; git history is its archive): the receiver
  reserves the manifest identity before the fetch and the *sender* materializes the body at the
  reserved key. Found **unsound** by an adversarial review (codex `gpt-5.6-sol`, xhigh): a sender
  PUT still in flight can land *after* the receiver was fenced and GC dead-build-skipped the
  reservation's `+1`; the successor's lazy stale-precommit sweep then removes the binding, the fold
  reads the late body and emits an **unmatched `-1`** — deleting blobs under a later successful
  retry. Root lesson: a *foreign writer* into another server's manifest domain invalidates the
  dead-build skip's "the body will never return" assumption and drags the whole state model
  (codec, sweep, recovery, fold) into scope.

Both designs tried to *carry trust across two single-writer domains*. This design stops doing
that: each party only ever writes its own domain, and the receiver *verifies* instead of trusting.

## Core idea {#core-idea}

**Publish, then confirm, then promote** — EDGE-BEFORE-OBSERVE lifted to part level:

1. The receiver runs today's flow unchanged: stage its own manifest body (own write, own tree),
   `precommitAdd` — its `+1` edge is now durable in its own journal, with a present body (**T1**).
2. **Confirm** (one lightweight read-only interserver query): "is your committed ref for
   `<part>` still live?" The sender — the single lease-holding writer of its namespace — answers
   from its in-memory ref-table state, authoritatively, with zero S3 I/O (**T2**).
3. Answer *yes* → `promote`. Anything else → abort the precommit (ordinary removal append) and
   fail per the matrix below.

Under `INV-NO-DANGLE`, every blob of the part stays alive exactly as long as the sender's
committed ref lives, and the blobs lose that protection only through the sender's removal `-1`.
So one part-level check at the right moment replaces any per-blob probing — and the "right
moment" is *after* the receiver's own evidence is durable, which is what kills the TOCTOU.

## Why there is no TOCTOU {#correctness}

Let T1 = receiver's `+1` durable (body present), T2 = sender's "yes", T3 = sender's removal `-1`
appended. Physical deletion of a blob requires three GC phases: condemn (round N) →
`delete_pending` (N+1) → pre-CAS delete (N+2), each derived from folded journals.

- **`-1` appended after T2** (the "sender deletes immediately after answering" case): any fold
  that reads the `-1` runs after T3 > T2 > T1, when the receiver's `+1` is durable and foldable.
  The round's NET in-degree includes both → never zero → condemn never happens.
- **A round straddles T1** (reads the receiver's journal before T1, the sender's after T3): that
  round can condemn. But the next round's fold *cannot miss* a durable `+1` — the cursor never
  advanced past it — so the blob's in-degree recovers and the entry is **spared** during
  graduation. The spare-even-`delete_pending` behavior is existing, verified machinery (confirmed
  against code in the adversarial review). Deletion needs the `+1` to be invisible for three
  consecutive folds — impossible for a durable journal entry.
- **False-404 of the receiver's staged body** at a later fold: live precommit + absent body =
  today's fold barrier — anomaly + clamp + `suppress_destructive`. This design changes **nothing**
  in the fold, so the existing hammer keeps covering the store-lie case.
- **`-1` appended long before T1** (the original codex-6 window — receiver stalled with nothing
  durable while blobs die): confirm returns *no* at T2 (the binding is gone), promote never runs,
  no committed ref exists, nothing dangles.

A frozen receiver is safe at every point: before T1 nothing exists; after T1 its durable
precommit folds and protects the blobs until its abort/stale-sweep removal — which pairs the
folded `+1` with a matching `-1`. No unmatched decrements exist anywhere in this protocol.

## The confirm query {#confirm-query}

- Semantics: "does your ref table hold a **committed binding** for ref `<part_name>` in the
  namespace serving this table, right now?" Answered by the sender's
  `ContentAddressedMetadataStorage` from the single-writer in-memory state. It is deliberately
  about the *journal binding*, not the `DataPart` object.
- Pessimism is fine: a part mid-removal (rebound to `delete_tmp_*`) answers *no* even though its
  blobs are still transiently protected — the receiver just takes the failure path. Never answer
  *yes* on anything but a live committed binding for the exact ref name.
- Transport: a new lightweight query on the existing interserver endpoint (same authentication
  and addressing as the fetch itself). One extra RTT per relink; no S3 requests.

## Failure matrix {#failure-matrix}

| Confirm outcome | Meaning | Action |
|---|---|---|
| yes | binding committed at T2 | `promote`; then the normal `tmp-fetch_<part>` → final rename |
| no (binding gone / `delete_tmp`) | the source no longer serves this part | abort precommit → **fail the fetch attempt with a retryable error** — do NOT byte-re-request the same sender (it has nothing to serve); the replication queue's existing machinery does the "several attempts": backoff, source re-selection, covering-part discovery (`findReplicaHavingCoveringPart`) |
| confirm unreachable | liveness unprovable | same as *no* (if it was a blip, the next queue attempt confirms fine; if the sender is dead, the queue picks another source) |
| relink-mechanism failure while the sender still has the part (protocol-version mismatch, manifest decode failure, publish/`promote` aborted on a condemned blob or ref conflict) | relink cannot work here, bytes can | today's same-sender byte re-request (`DataPartsExchange.cpp:731-738`), unchanged |

Error classification for the *no*/unreachable rows: throw a retry-later code **outside** the
`ABORTED` exemption so `ReplicatedMergeTreeQueue`'s existing backoff engages (this is exactly
defect 2 of finding #37 — do not reproduce it). Manual `SYSTEM FETCH PART`/`FETCH PARTITION`
callers surface the error to the user, which is appropriate.

The abort path is today's abandon: append the precommit removal; if the `+1` already folded, the
removal pairs it; the staged manifest body becomes an orphan and the existing orphan-manifest
sweep reclaims it.

## B66b: relink-into-detached {#b66b}

Independent of the confirm mechanics and now nearly free, because relink **already** publishes
under the temporary ref and re-keys on commit (`relinkPartToDisk`: "Stage under the tmp-fetch
dir. The caller commits via `renameTempPartAndReplace`, whose `moveDirectory(tmp-fetch_<part> ->
<part>)` re-keys this server's committed ref"):

- Lift the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`).
- For `to_detached`, publish under the detached temporary name (`detached/` + tmp prefix +
  part name) and let the existing caller-side rename install the final detached name — collision
  semantics are inherited from the byte path *by construction* (same caller code).
- The confirm step is identical. Cross-pool `FETCH PARTITION` keeps streaming bytes (the
  same-pool advertise check is untouched).

This closes the RPL-4 perf cliff (same-pool `FETCH PART/PARTITION` and `to_detached` replication
fetches stream full bytes today).

## Protocol versioning {#versioning}

New relink cookie value (the receiver's advertisement also carries it). A new sender relinks only
for receivers advertising the confirm-capable version — otherwise it streams bytes. A new
receiver seeing an old/unknown relink cookie falls back exactly as today (the unknown-cookie
byte re-request already exists, `DataPartsExchange.cpp:741-747`). Mixed-version windows therefore
degrade to bytes, never to the unconfirmed relink with the known gap.

## Scope and non-goals {#scope}

- **Zero changes** to the GC fold, journal codec, snapshot format, sweeps, orphan protection, or
  the anomaly/suppression semantics. No new journal states. No foreign writers. Every write stays
  in its owner's domain.
- The ordinary write path (INSERT/merge) is untouched.
- B66a — `BACKLOG.md` §14 (local-backend atomicity, orthogonal).
- The **pre-existing** ordinary-`Precommit` unmatched-`-1` hazard (false-404 at activation-fold ×
  dead-build skip × body readable at removal-fold), discovered while reviewing the abandoned
  reserved design, is tracked as its own BACKLOG §3 item — same family the reserved counterexample
  exploited, exists today independently of this spec, requires the false-404 store class.
- Bulk write-replica warm-up (future): confirm generalizes trivially — one batched confirm for N
  parts after N publishes; nothing here precludes it.

## Testing {#testing}

- **Race integration tests** (failpoint between `precommitAdd` and confirm):
  (a) sender drops the part in the window → confirm answers *no* → abort → retryable failure →
  queue re-selects (assert covering-part / other-replica path fires, and no byte re-request goes
  to the original sender); (b) sender alive → confirm *yes* → promote → relink proof
  (`CasBlobPut == 0`).
- **codex-6 regression:** stall the receiver's publish ≥ 3 GC rounds under an aggressive config
  (`old_parts_lifetime` small) while the sender's part is merged away and GC runs to fixpoint →
  the stalled attempt must NOT produce a committed ref (confirm rejects on resume); fsck clean,
  dangling=0, zero unmatched decrements.
- **Straddle-spare regression:** force the condemn-then-spare interleaving (receiver's `+1` folds
  one round after the sender's `-1`) and assert the blob is spared and never deleted.
- **B66b:** same-pool `FETCH PART` into `detached/` → relink proof + subsequent `ATTACH` reads
  correctly; detached name collision → behavior identical to the byte path; cross-pool → bytes.
- **RPL-5 slice (pulled in):** `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on a 2-replica CA
  table (extend `test_cas_replicated_relink`): assert the queue-cloned `REPLACE_RANGE` fetch
  relinks (blob-count proof), not a silent byte re-fetch.
- **Version-mix test:** confirm-capable receiver × legacy sender cookie → clean byte fallback.
- **TLA mini-model:** two journals + round fold with cursors + three-phase graduation + spare;
  property: *if confirm observed a committed binding at T2 and the receiver's activation precedes
  T2, no blob of the receiver's manifest is deleted before the receiver's own removal event*.
  Model the straddle interleaving and the false-404 branch (as the existing clamp).

## Naming {#naming}

"Publish-then-confirm relink". The confirm query: `partRefStillCommitted` (read-only interserver
query). No new journal vocabulary — deliberately.
