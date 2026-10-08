---
description: 'Fix for CAS-30: a delayed GC `.meta` delete can remove the condemn marker of a later incarnation, and a publisher can clear a marker GC already counted as confirmed. Graduation always reads the marker, through the fold read-ahead, and accepts only `Condemned` at the entry''s round or newer; the delete job removes only `Condemned` at its entry''s round or older; every GC marker write stamps the writing round. Rollout restarts every old-build node with GC disabled before any upgraded GC starts; the residual is accepted. No format change.'
sidebar_label: 'CAS GC: meta delete names its condemnation'
sidebar_position: 102
slug: /superpowers/specs/cas-gc-meta-delete-names-its-condemnation-design
title: 'CAS GC: a `.meta` delete removes only a marker no newer than its condemnation'
doc_type: 'reference'
---

# CAS GC: a `.meta` delete removes only a marker no newer than its condemnation — design {#cas-gc-meta-delete-names-its-condemnation}

- Date: 2026-10-01. Status: rev.9, final.
- Base: `altinity/antalya-26.6` @ `e2dd0f5d343`, worktree `/home/mfilimonov/workspace/ClickHouse/cas-30`. Code is
  cited by function. Paths are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless
  they start with `src/`, `docs/` or `tmp/`.
- Backlog: CAS-30 (this fix), CAS-334 (writer rule, next PR), CAS-329 (request cuts), CAS-330 (not needed,
  section 3), CAS-326 (monotonic round at rebuild), CAS-331 (variant study). Section 10 names the residual tasks.
- Inputs: `docs/superpowers/reports/2026-09-30-cas-30-stale-meta-delete-adjudication.md`,
  `docs/superpowers/reports/2026-09-30-cas-publisher-clears-own-marker.md` (model in `tmp/cas-pub-marker/`),
  `docs/superpowers/reports/2026-09-30-cas-331-condemn-marker-variants.md`.

## 1. Decision {#decision}

**Four rules, all in GC.** Together they close the four schedules of section 2:

| # | Rule | Site |
|---|---|---|
| G1 | Graduation reads the marker every time. The in-process confirmation registry goes. The read goes through the fold read-ahead | `confirm_condemned_marker`; `settleEntry` |
| C3 | Graduation of `(hash, token, condemn_round = c)` accepts only `Condemned` at a round `≥ c` | `confirm_condemned_marker` |
| C1 | The delete job reads the marker and deletes it only if it is `Condemned` at a round `≤` the entry's `c`, `If-Match` the etag it read | `deleteConfirmedMeta` |
| C2 | Every GC marker write stamps the round of the attempt that issues it, the gate's retry included, and replaces an older-round `Condemned` as it replaces `Clean` | `writeCondemnedMeta`; the retry in `confirm_condemned_marker` |

- **Budget before the read** (`settleEntry`); the read is hinted ahead for admitted entries, in its own window
  (5.6): ideally 3.1–9.4 s per full-budget round instead of 50–150 s. **8 requests per reclaimed blob** (3).
- **No format change, no marker memory, no new setting, no fallback read, no writer change.**
- **Upgrades are procedural** (11): restart every old-build node with `cas_gc_enabled = false`, then upgrade.
  The residual (10.4) is accepted by the owner.
- **Failure asymmetry.** Each rule errs toward keeping or re-reading a marker: over-refusal costs a round and a
  `PUT`, over-retention an orphan `.meta`. Under-refusal is silent loss.
- **Premises the code does not establish:** Q7 (10.2) and Q8 (10.4), both accepted by the owner.

## 2. The defect {#defect}

### 2.1 Code facts {#code-facts}

| # | Fact | Where |
|---|---|---|
| F1 | On `Removed` or `Gone`, the round schedules the marker delete with the ref only | `Gc::applyRedeleteOutcome` → `GcMetaWriter::scheduleConfirmedMetaDelete(ref)` |
| F2 | The job reads the marker, then deletes by the etag it just read: whatever is current goes | `deleteConfirmedMeta` (`loadMeta`, `deleteMetaExact`) |
| F3 | The job admits its own `CasOperation` on the fence-0 GC plane. No lease or `gc/state` check exists on its path | `GcMetaWriter::scheduleConfirmedMetaDelete` |
| F4 | The round drains the meta pool before its `gc/state` CAS, so a job of a round that loses the CAS still runs | `Gc::runRegularRound`, `meta_pool_wait` before `round_commit` |
| F5 | Graduation returns true on the in-process `(ref, token)` record without reading the marker | `confirm_condemned_marker` → `condemnMarkerConfirmedInProcess` |
| F6 | The record outlives failed attempts and leadership changes. A marker job of a losing attempt still records it | `GcMetaWriter` lives as long as the `Gc`; `scheduleCondemnMarkerWrite` → `noteCondemnMarkerDurable` |
| F7 | The redelete never reads the marker: `HEAD`, then `remove` by the observed body etag | `Gc::performRedeleteIo` |
| F8 | A writer that finds the body present and the marker not `Condemned` adopts the token (`Observed`), backfilling `Clean` if absent | `PartWriteTxn::ensureBlobPresent` |
| F9 | A publisher's reconcile writes `Clean` over any non-`Clean` marker after it loses its first write; a resurrect's first write replaces the `Condemned` it read | `reconcileMetaClean` in `PartWriteTxn::ensureBlobPresent` (`readModifyWrite`; `op.replace`) |
| F10 | `writeCondemnedMeta` leaves an existing `Condemned` of any round alone and reports success | `writeCondemnedMeta` |
| F11 | The marker has no token. Its bytes are a function of `(state, condemn_round, size)`. On S3 the etag is content-derived | `encodeBlobMeta`; `Backend/CasBackend.h` |
| F12 | Writers leave `Clean` on every publication and every adoption, so a condemn usually finds `Clean` | `reconcileMetaClean`, `putMetaIfAbsent` backfill |
| F13 | The gate runs before the graduation budget check, so a budget-carried entry is read every round | `settleEntry`, `confirm_condemned_marker(e)` before `graduationAvailable` |
| F14 | The gate's retry writes at the entry's round | `confirm_condemned_marker`: `scheduleCondemnMarkerWrite(..., entry.condemn_round, ...)` |
| F15 | An ambiguous conditional write is reissued while its precondition still holds. The earlier attempt is never cancelled and can land later | `CasOperation::writeLoop` ("Unresolved but repeatable") |
| F16 | `Clean` is `(Clean, 0, size)`: its bytes, and so its S3 etag, are the same for every incarnation of a hash | `reconcileMetaClean` |
| F17 | A `delete_pending` row is redeleted before any gate: the pending branch precedes the gate branch | `settleEntry` |
| F18 | `cas_gc_enabled` (setting `gc_enabled`, default `true`) is copied into a `const` member at disk construction; a config change reaches a running process only through a restart. When `false`, no scheduler starts, and `SYSTEM CAS GC RUN`, `START` and `REBUILD` throw | `ContentAddressedMetadataStorage` constructor, `runGarbageCollectionRoundNow`, `runGcRebuildNow`, `gcStart` |
| F19 | The redelete checks no lease: an attempt parked between its `HEAD` and its `remove` deletes the token whenever it resumes | `Gc::performRedeleteIo` |

### 2.2 Schedule S1: a stale delete job removes the next incarnation's marker {#s1}

Leader L1, successor L2, writers W1, W2; hash X; `T0` pending delete, condemned at `c0`.

1. **Round `c0+2`.** L1's redelete removes `T0` and queues job J (F1); L1 freezes before J runs. L2 steals, finds
   `T0` `Gone`, its own job removes `T0`'s marker, L2 commits.
2. W1 publishes `T1`, then drops it. **Round `c`:** L2 condemns `T1`; `Condemned(c)` lands; the registry records it.
3. **L1 resumes. J runs `GET .meta` → `Condemned(c)`, then `DELETE .meta If-Match` that etag (F2).**
4. **Round `c+1`:** graduation hits the registry (F5). Variant S1b: J lands after this graduation; no read
   follows (F7), so a fresh graduation read alone would not help.
5. **Round `c+2`:** W2 precommits after the cut, finds no marker, adopts `T1` (F8). L2's redelete deletes `T1`.

- **Stall:** the steal plus one L2 round, minutes at the default 60 s interval (derived). **Closed by C1:** J
  carries `c0`; the marker is `Condemned(c)`, `c ≥ c0+2` (section 4).

### 2.3 Schedule S2: the publisher clears a marker GC already counted {#s2}

Reachable in the code and in the model (`rv_bug`, trace read). No leader change, no GC stall.

1. **Before round `c`:** transaction U precommits a manifest naming X and abandons before uploading X.
2. **Round `c`:** U's `+1` and `−1` leave X touched at zero in-degree; `closeBlob` calls `head_blob`. After
   intake, W precommits, `HEAD`s X absent, publishes `T1`. The read-ahead `HEAD` observes `T1`; the marker job
   creates `Condemned(c)` and records `(X, T1)`.
3. **W's reconcile:** its `create(Clean)` conflicts; `readModifyWrite` writes `Clean` over `Condemned(c)` (F9).
   W aborts; its `+1` and `−1` fold in cut `c+1`.
4. **Round `c+1`: graduation hits the registry (F5), zero requests where a `GET .meta` was needed.**
5. **Round `c+2`:** W2 precommits after the cut, reads `Clean`, adopts `T1`. The redelete deletes `T1`.

- **Race width:** GC's `HEAD`, `GET`, `PUT` between W's body `PUT` and its reconcile; rare. **Closed by G1:** the
  round-`c+1` read sees `Clean`, refuses and rewrites the marker.

### 2.4 Schedule S3: one marker object stands for two incarnations {#s3}

F10 lets the object written for `T0` stand for `T1`; a job of `T0`'s entry then removes `T1`'s guard.

1. **Round `c0+2`:** L1 removes `T0` and freezes before scheduling its job. L2 steals and commits; its own job
   for `T0` fails (swallowed), so `Condemned(c0)` stays.
2. W1 publishes `T1`; its reconcile throws after the publication, so `Condemned(c0)` stays.
3. **Round `c1`:** L2 condemns `T1`; `writeCondemnedMeta` leaves `Condemned(c0)` and confirms (F10).
4. **Round `c1+1`:** graduation accepts on the registry, or on a re-read that sees any `Condemned`.
5. L1's job for `T0` reads `Condemned(c0)` and deletes it. W2 adopts `T1` in `c1+2`; the redelete deletes it.

- **Closed by C3 + C2 + C1:** C2 re-stamps to `Condemned(c1)` at step 3. If that write fails, C3 refuses
  `Condemned(c0) < c1` at step 4 and the retry re-stamps. C1 makes L1's job leave `Condemned(c1)`.

### 2.5 Schedule S4: a publisher's late `Clean` matches a recreated marker {#s4}

S3 (content etag). No leader change, no namespace removal.

1. **Round `c`:** as S2 step 2, GC writes `Condemned(c)`, etag `M`.
2. **W's reconcile** loses its first write; `readModifyWrite` sends A: `PUT Clean If-Match M`. A times out and
   stays in flight; the resolve read still sees `M`, so `writeLoop` reissues (F15): B lands `Clean`. W abandons;
   its `−1` is durable.
3. **Round `c+1`:** the gate reads `Clean`, refuses, retries at the entry's round (F14): `Condemned(c)` again,
   the same bytes, the same etag `M` (F11). **Round `c+2`:** the gate graduates `T1`.
4. **A lands:** `If-Match M` matches; the marker is `Clean`. **Round `c+3`:** W2 adopts `T1`; the redelete
   deletes it.

- **Why it exists:** `writeLoop` assumes an earlier attempt lands only while its precondition holds; on S3 a
  replaced precondition returns when someone writes the same bytes. **Closed by C2's retry stamp** (5.2).

## 3. End state: the request chain per reclaimed blob {#end-state}

| # | Request | Thread | Today | PR1 |
|---|---|---|---|---|
| 1 | `HEAD` blob at condemn (`head_blob`, read ahead) | read-ahead → round | yes | yes |
| 2 | `GET .meta` in `writeCondemnedMeta` | meta pool | yes | yes |
| 3 | marker write `Condemned(c)` | meta pool | `casMeta` over `Clean` (the common case, F12), `putMetaIfAbsent` over absent | same, and also over an older-round `Condemned` (C2) |
| 4 | `GET .meta` at graduation | round (serial) | only when the registry misses | **always**, after the budget check, read ahead (5.6) |
| 5 | `HEAD` blob before the delete (`performRedeleteIo`) | redelete I/O | yes | yes |
| 6 | `DELETE` blob `If-Match` the observed token | redelete I/O | yes | yes |
| 7 | `GET .meta` in the delete job | meta pool | yes | yes |
| 8 | `DELETE .meta` | meta pool | `If-Match` the etag of 7, always | `If-Match` the etag of 7, **only if 7 read `Condemned` at a round `≤ c`** |
| | **Total** | | 7 (8 on a registry miss) | **8** |

**Graduation reads per round:**

- **Count:** accepted reads, at most `gc_round_graduation_budget` (`B`, default 5,000), plus every refusal (no
  cap; a refusal consumes no slot). A refusal costs 3 requests: the gate's `GET`, then the retry's `GET` and
  `PUT`. During a marker-write outage every eligible entry refuses each round, as today after every restart.
- **Latency** (5.6): 16 workers at the default `gc_io_concurrency`; each hinting site keeps a window of 64 queued.
  5,000 reads ideally take `ceil(5000 / 16) = 313` request latencies: 3.1–9.4 s at 10–30 ms. Without read-ahead:
  50–150 s. The ideal needs eligible rows within the buffered range and workers not busy with head hints; sparse
  candidates overlap less, down to serial. Derived, not measured.

**PR2, CAS-329 (later, A/B on `utils/ca-soak/gc-ab`):** drop request 5 (request 6 names the persisted token instead);
request 2 only if a blind `create` pays (F12). Also V1 and V2r. **PR3, CAS-330, is not needed:** every leader's
job reads the marker.

## 4. The invariant {#invariant}

**Claim.** While an entry `E1 = (X, T1, c1)` that a PR1 gate graduated is `delete_pending`, and X's body is
`T1`, X's marker is `Condemned` at a round `≥ c1`. Exceptions: the residuals of section 10.

"Graduation read" means the PR1 gate read that accepted `E1`, in any leader's tenure. An entry a pre-PR1 build
graduated has no such read and is outside the claim: the procedure and residual of section 11 and 10.4 cover it.

**Consequence.** A writer that reads the marker after `E1`'s graduation read sees `Condemned` and republishes.
An adopter that read `Clean` or absent before that read precommitted before it, so its edge is in `E1`'s delete
round's cut at the latest, and `settleEntry` spares the entry. The delete round is always later than the
graduation read's: only an attempt that adopted the pending state redeletes (F17).

**Premises.**

| Premise | Discriminator | Verdict |
|---|---|---|
| Q1. A token never reappears once its body is removed or absent | the random incarnation tag in every envelope (`PartWriteTxn::ensureBlobPresent`); the exact-token blob delete relies on the same fact | assumed, as today |
| Q2. `gc/state.round` never decreases while `gc/state` is readable | writers: the round CAS (`+1`), `acquireOrRenewLease` (copies), `rebuildBaseline` (`max(max_fence_round, state.round, max_gen) + 1`) | holds; 7.2 for a lost `gc/state` |
| Q3. A condemn or supersede stamps the round of the attempt that makes it, and commits by CAS on the state it adopted | `condemn_round = state.round + 1` feeds `head_blob` and the supersede in `closeBlob`; `round_commit` | holds |
| Q4. Every writer of a blob `.meta` is known | GC `writeCondemnedMeta` and `deleteConfirmedMeta`; writer backfill and `reconcileMetaClean` in `PartWriteTxn::ensureBlobPresent`. `CasInspect` and fsck only read | holds |
| Q5. A publisher's reconcile **task** ends before its own `−1` | the fan-out wait in `ContentAddressedTransaction` joins every blob task before `abandon` | holds, except when a namespace removal lands the `−1` (10.1) |
| Q6. A job is scheduled only after its entry's token was `Removed` or `Gone` | `Gc::applyRedeleteOutcome`; `Mismatch` schedules nothing | holds |
| Q7. Every **request** a publisher's reconcile sent lands, if at all, before that publisher's `−1` is durable | none in the code: a task that ends does not cancel an attempt still in flight (F15), and `GaveUp` does not establish that the store dropped it | **premise**, accepted; 10.2 |
| Q8. No destructive request outstanding at a PR1 gate read lands after it: an old-build job's, or an upgraded leader's stalled redelete | the procedure (11) for old processes and their queued jobs; none for a request the store accepted, or an attempt parked before its `remove` | **premise**, accepted; 10.4 |

**Argument.** Take the graduation read of `E1` in round `g1`. It accepted `Condemned(r)` with `r ≥ c1` (C3).
Every later change to the marker comes from one of the Q4 writers:

1. **GC marker writes never lower it.** `writeCondemnedMeta(r')` leaves a `Condemned` at a round `≥ r'` and
   replaces an older one with `Condemned(r')` (C2). The marker stays `Condemned` at a round `≥ r`.
2. **Jobs of other incarnations carry a round `< c1`.** Let job J belong to `E0 = (X, T0, c0)`, and let `T1` be
   X's body when J acts.
   - `T0 ≠ T1`: J's redelete saw `T0` removed or absent (Q6), and `T0` never reappears (Q1).
   - So `T1` was published after J's redelete. That redelete ran in an attempt whose adopted state had `E0`
     pending, so round `c0+1` had committed before it.
   - `E1` was committed at `c1` by an attempt that `HEAD`ed `T1` after its publication. Its CAS found
     `gc/state.round = c1 − 1`, after round `c0+1` committed. By Q2 and Q3, `c1 − 1 ≥ c0 + 1`.
   - So `c0 ≤ c1 − 2 < r`, and C1 makes J leave the marker. An older-build job ignores the round: Q8.
3. **Jobs of `T1`'s own entries** (`E1`, or an earlier entry for `T1` that was spared) fire only after `T1`'s
   body is removed or absent (Q6). Then the claim no longer applies.
4. **Writers.** The backfill creates only over an absent marker. A reconcile writing `Clean` belongs to `T1`'s
   publisher. Its task ends before its `−1` (Q5) and every request it sent lands before that `−1` (Q7). `E1`
   graduated at zero in-degree, so that `−1`, and every reconcile request before it, precede cut `g1`. The
   graduation read follows the cut (5.6) and saw the result.
5. **Late requests.** Only a publisher's `Clean` over `Condemned` lowers protection (point 4, Q7). A late GC
   write only raises the marker; a late job's `DELETE If-Match` names a round `≤ c0 < c1`, other bytes than
   any guard of `E1` on S3 and another version elsewhere.

- **Round separation holds for job-producing entries only** (`E0`'s pending round had committed). A supersede
  can put committed incarnations one round apart, and stale attempts write equal-round markers for different
  tokens; the argument compares only a job's round with a committed entry's.
- **Byte-identical markers.** C1 decides on the bytes it read; `If-Match` removes those bytes or nothing. An S3
  rewrite of the same bytes carries a round `≤ c0`, not a pending guard. A publisher's late `Clean` is the other
  request that can meet recreated bytes: S4, 10.2.

## 5. PR1 {#pr1}

### 5.1 Graduation: budget first, then the read {#pr1-accept}

`settleEntry`, the non-pending branch with `condemn_round < current_round`:

1. **Budget.** The entry is admitted if it holds a read reservation (5.6), or if
   `graduations_used + reserved < max_graduations` (or the budget is unbounded); then it reserves. Otherwise
   carry it unchanged. No read.
2. **Gate.** Top up the read-ahead hints (5.6) and call `confirm_condemned_marker(e)`. It takes the marker read,
   releases the reservation, and accepts iff the marker exists, is `Condemned`, and has
   `condemn_round ≥ e.condemn_round`.
3. Accept: consume a slot (`graduations_used`), publish `delete_pending`. Refuse: carry; the gate has scheduled
   the marker retry at this round (5.2).

| Marker at the read | Verdict | Why |
|---|---|---|
| absent or `Clean` | refuse, carry, retry the write | a writer may have adopted; S2 |
| `Condemned`, round `< c` | refuse, carry, retry the write (C2 re-stamps) | may be another incarnation's object, which that incarnation's job may remove; S3 |
| `Condemned`, round `≥ c` | accept | jobs that can act while `T` is current carry rounds `< c` (section 4) |
| read throws | refuse, carry, `CASGCMetaWriteAnomaly` | missing evidence, never a wedge |

- **Invariant:** `graduations_used + reserved ≤ max_graduations`. So an entry holding a reservation is always
  admitted, and reads per round never exceed accepted reads plus refusals.
- **Order:** the scan stops at a touched row, so reservations are always a contiguous run of eligible rows right
  after the cursor and the round graduates the same set at every concurrency. **A refusal consumes no slot**, as
  today.

### 5.2 Every GC marker write stamps its own round (C2) {#pr1-restamp}

`writeCondemnedMeta(round r)` leaves the marker only if it is `Condemned` at a round `≥ r`. It replaces an
older-round `Condemned`, like `Clean`, by `casMeta` `If-Match` the etag it read. Every caller passes the round of
the attempt that calls it, `condemn_round` in `Gc::fold`: `head_blob`, the supersede, and the gate's retry. Only
the retry changes (F14).

- **The retry stamps this round (S4).** It runs in a round `g > c`, so `Condemned(g)` never recreates the bytes
  `M = Condemned(c)` a publisher's late `Clean If-Match M` names. When the gate accepts, nothing is written, so
  the stamp cannot help L2 (10.2).
- **Safety:** `pm_g1_norestamp` is green in both batteries: a higher round makes every job more conservative.
  **Liveness:** needed after a spare (7.1). **Cost:** one `PUT`, only over absent, `Clean` or older.

### 5.3 The delete job (C1) {#pr1-delete}

```cpp
/// Drop the marker of a blob whose body is confirmed deleted or absent, but only while it is `Condemned`
/// at `condemn_round` or older: a newer marker, or `Clean`, belongs to a later incarnation.
void scheduleConfirmedMetaDelete(const BlobRef & ref, uint64_t condemn_round);
```

The job captures `condemn_round` by value. It reads, checks, and deletes `If-Match` the etag it read.
`Mismatch` and `Gone` are silent, as today.

**Orphans.** The job leaves a `Condemned` marker above its round over an absent body. Sources: a refused
graduation (the retry stamped `g > c`; counted by `CASGCCondemnMarkerUnconfirmedCarry`), a stale attempt that
raised the round, a surviving pre-rebuild marker (7.2), a swallowed job. The next writer of the hash publishes
fresh and reconciles the marker to `Clean`. A hash never written again keeps it: at most one per hash, no global
bound, as today; fsck counts it in the advisory `meta_without_body`.

### 5.4 What is removed (G1) {#pr1-removed}

- `GcMetaWriter::noteCondemnMarkerDurable`, `condemnMarkerConfirmedInProcess`, `forgetCondemnMarker`, their
  `State` twins, `condemn_marker_mutex`, `condemn_markers_confirmed`, `condemnMarkerKey`, and every
  `forgetCondemnMarker` call (`Gc::applyRedeleteOutcome`, the `merge.spared` and `merge.replaced` loops).
- The `token` parameter of `scheduleCondemnMarkerWrite` (it only keyed the registry); `writeCondemnedMeta`'s
  unused `bool` result; the registry shortcut and the memo in `confirm_condemned_marker`.

### 5.5 The `marker_confirmed` bit {#pr1-marker-confirmed}

`RetiredEntry::marker_confirmed` and `CondemnedRow::marker_confirmed` (bit 1 of `flags`) stay in the row format.

- **Writing:** set with `delete_pending`, as today: the bit patterns released builds write.
- **Reading:** the gate stops reading it (today it skips the gate on a non-pending row with the bit). No
  released build writes such a row; reading it would trust a claim the round did not check. B3 pins it.

### 5.6 Graduation read-ahead {#pr1-read-ahead}

The gate's `GET .meta` goes through the fold's `GcReadAhead`, the pool `head_blob` already uses. **Only the
moment of the fetch moves, never an accept or refuse decision.**

**What is prefetched.** `hintRead` of `layout.blobMetaKey(ref)` for a carried entry that will reach the gate. The
gate's `takeRead` decodes it as `loadMeta` does and rethrows a worker's exception into its `catch` (5.1).
`graduation_reserved` holds the keys of entries admitted to a read and not yet taken: every outstanding hint, and
the entry at the gate; with `graduations_used` it never exceeds `max_graduations`.

**Own window.** Graduation hints throttle on `graduation_reserved.size() < reads.window()`, head hints on
`reads.pendingHeads() < reads.window()`, not the shared `pending()`: an untaken head hint (a candidate that keeps
another edge) stays counted until the fold ends, and 63 of them would leave graduation one slot. At most
`2 × window()` requests queue on the 16 workers.

**The gate, in order.** The cursor has already consumed the current entry's row, so a scan offers only later rows.

1. `settleEntry` admits the current entry (5.1), reserving its key and hinting it if the window has room;
   otherwise its take reads inline. This precedes any look-ahead, so a hint never takes its slot.
2. `PriorEdgeCursor::scanAhead` decodes rows of the open segment into a bounded buffer and offers each condemned
   row once.
3. The callback reserves and hints a row only if all hold:
   - it is not `delete_pending`, `condemn_round < current_round`, and the round is not `suppress_destructive`;
   - its blob has **no delta and no source retirement this round**: `last_verdict_is_remove`'s key set, hoisted
     to fold scope as `touched_blobs`. A touched row STOPS the scan (it is decided inline when the cursor reaches
     it; skipping it would let reservations run past it and change the budget decision between concurrency 1 and
     8). An untouched carried row has no edge, is not superseded, reaches the gate;
   - `graduations_used + graduation_reserved.size() < max_graduations` (or the budget is unbounded);
   - `graduation_reserved.size() < reads.window()`.
   The scan stops at a touched row, when the budget or window is full, when the buffer holds
   `kGraduationLookaheadRows`, or when the segment ends.
4. The gate takes the read and releases the reservation.

**Boundaries.**

- **Segment edge:** the scan never opens a segment; offers resume once the merge opens the next one.
- **Checksum:** `PriorEdgeCursor::advance` verifies a segment's checksum when it drains, as today. Rows hinted
  before that cost at most a window of wasted reads when it throws; the failed fold commits nothing.
- **Row cap:** `kGraduationLookaheadRows = 16384`. It bounds memory, not requests; the implementer reports its
  effect on the `gc-ab` stand.
- **Concurrency 1:** window 0, no hints, no scan: today's code path.
- **Waste:** none when the merge completes; at most `reads.window()` reads when it throws. Latency: section 3.
- **Touched entries** read inline. Only they can be superseded, so no supersede wastes a hinted read.

**Why an early read is sound.** `GcReadAhead`'s header argues from write-once objects; a marker is not. The gate
needs only what section 4, point 4 uses: the read follows the round's cut. Hints are issued in the merge, after
intake, when `deltas` is final. The fold writes no marker of a hinted hash between hint and take: condemns and
supersedes touch only touched blobs, and the retry follows the take. The take does not re-read, exactly as an
inline read issued at the fetch. The scan decodes the rows the merge would, in order, with the same checks; a
corrupt row throws a few rows earlier and the round fails closed.

### 5.7 Code shape {#pr1-code}

`Gc/CasGcMetaWriter.cpp`:

```cpp
void writeCondemnedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                        uint64_t condemn_round, uint64_t size)
{
    const auto lm = loadMeta(op, layout, ref);
    const BlobMeta desired{.state = MetaState::Condemned, .condemn_round = condemn_round, .size = size};
    if (!lm)
        putMetaIfAbsent(op, layout, ref, desired);
    /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
    else if (lm->meta.state != MetaState::Condemned || lm->meta.condemn_round < condemn_round)
        casMeta(op, layout, ref, lm->etag, desired);
}

/// A later incarnation that can own a job is always condemned at a larger round, so a newer marker, or
/// `Clean`, is live.
void deleteConfirmedMeta(CasOperation & op, const Layout & layout, const BlobRef & ref, uint64_t condemn_round)
{
    const auto lm = loadMeta(op, layout, ref);
    if (!lm || lm->meta.state != MetaState::Condemned || lm->meta.condemn_round > condemn_round)
        return;
    deleteMetaExact(op, layout, ref, lm->etag);
}
```

The file's rationale comment drops its in-process paragraph and states the three round rules instead.
`Pool/CasBlobMeta.{h,cpp}` gains `decodeLoadedMeta(std::optional<Object>)`, the decode half of `loadMeta`.

`Gc/CasBlobInDegree.h` gains `struct GraduationReadHints` with two callbacks: `admit(ref)` reserves a budget
slot for the entry at the gate and hints its read if the window has room (false: no budget; idempotent);
`offer(ref, row)` reserves and hints a later row or skips it (false: window or budget full). Its header says the
gate still decides on the value it takes; only the moment of the fetch moves.

`foldDeltasIntoGeneration` takes `const GraduationReadHints * graduation_reads` (null: no read-ahead).

`Gc/CasBlobInDegree.cpp` adds `PriorEdgeCursor::scanAhead(size_t max_rows, offer)`, with the contract of 5.6.

`settleEntry`, the gate branch; graduation itself is unchanged, and **the pending branch is unchanged from the
base**:

```cpp
/// Budget before the gate: a carried entry must not cost a marker read every round.
const bool admitted = graduation_reads ? graduation_reads->admit(e.ref)
                                       : (!work_budget || work_budget->graduationAvailable());
if (!admitted)
    rmr.still_retired.push_back(e);
else
{
    /// After the admission: a look-ahead hint must never take the settling entry's slot.
    if (graduation_reads)
        cursor.scanAhead(kGraduationLookaheadRows, graduation_reads->offer);
    if (!confirm_condemned_marker || confirm_condemned_marker(e))
        /* graduate as today: ++graduations_used, publish delete_pending */;
    else
        rmr.still_retired.push_back(e);
}
```

`Gc/CasGc.cpp`, in `Gc::fold`:

- `touched_blobs`: the blob refs of `last_verdict_is_remove`, hoisted to fold scope; `head_candidates` unchanged.
- `topUpHeadHints` throttles on `reads.pendingHeads()`.
- `graduation_reserved` and the `admit`/`offer` closures implement 5.6. A set left non-empty after a shard's
  merge is discarded (`discardRead`, counted wasted). `graduation_reads` is passed only when `reads.window() > 0`.

`confirm_condemned_marker`:

```cpp
const String key = layout.blobMetaKey(entry.ref);
graduation_reserved.erase(key);
try
{
    /// An older round may be another incarnation's marker, which that incarnation's delete job may remove.
    if (const auto lm = decodeLoadedMeta(reads.takeRead(key));
        lm && lm->meta.state == MetaState::Condemned && lm->meta.condemn_round >= entry.condemn_round)
        return true;
}
/// the `catch` and `CASGCCondemnMarkerUnconfirmedCarry` stay
/// This round, not the entry's: a publisher's late `Clean If-Match` names the bytes it read, and on S3 an
/// entry-round rewrite recreates exactly those bytes.
meta_writer->scheduleCondemnMarkerWrite(entry.ref, condemn_round, entry.size);
return false;
```

`Gc::applyRedeleteOutcome` passes `entry.condemn_round` to `scheduleConfirmedMetaDelete`.

`Gc/CasGcReadAhead.h` gains `size_t pendingHeads() const { return heads.size(); }`. Its "why a result may be
fetched early" gains one sentence: a blob marker is not write-once; it is hinted only after the cut, which is all
the gate's argument needs.

| File | Change | Lines (estimate) |
|---|---|---|
| `Gc/CasGcMetaWriter.{h,cpp}` | registry out; job takes the round and checks it; re-stamp; comments | −50, +12 |
| `Gc/CasGc.cpp` | gate takes the read-ahead value; retry at this round; `admit`/`offer` closures and reservations; head hints on their own count; `touched_blobs` hoisted; calls lose the token; job gets the round; `forget` calls out | −14, +45 |
| `Gc/CasBlobInDegree.{h,cpp}` | budget before gate; gate ignores the bit; `GraduationReadHints`; `PriorEdgeCursor::scanAhead` with its buffer | +90 |
| `Gc/CasGcReadAhead.h` | `pendingHeads`; header sentence | +4 |
| `Pool/CasBlobMeta.{h,cpp}` | `decodeLoadedMeta` | +8 |
| `docs/en/antalya/cas/architecture/garbage-collection.md` | phase 9 and cost table: one `.meta` `GET` per graduation read, read ahead; `#condemn-delete` edge | ~10 |
| `docs/en/sql-reference/statements/system.md`, `docs/en/antalya/cas/operations/debugging.md` | operator rule B (7.2); upgrade procedure (11) | ~15 |
| `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`, `gtest_cas_blob_indegree.cpp`, `gtest_cas_gc_read_ahead.cpp` | tests of section 9 | ~550 |

User docs: the `#condemn-delete` edge becomes "then drop the `.meta` if it is still `Condemned` at round `≤ n`";
the graduation edge says "reads `.meta`: `Condemned` at round `≥ n`".

## 6. Schedules under PR1, request by request {#leader-schedules}

Entry `E = (X, T, c)`. Rounds are `gc/state` rounds.

| Schedule | Round `c+1` (graduation) | Round `c+2` (delete) | Marker | Rule |
|---|---|---|---|---|
| One leader L | `GET` → `Condemned(c)`, accept | `HEAD`, `DELETE` blob; job `GET` → `Condemned(c)`, `DELETE If-Match` | deleted | — |
| Leader change or restart between `c+1` and `c+2` | L1 as above, commits | L2: `HEAD`, `DELETE` blob; job carries `c` from the row: `GET` → `Condemned(c)`, `DELETE If-Match` | deleted, no orphan | C1 |
| Pending entry left by the old build (S2 on the base: pending over `Clean`) | — | the upgraded leader redeletes without a read | outside the guarantee (11, 10.4) | — |
| L's graduation attempt loses its CAS | L1's read leaves no state; L2 commits `c+1` on its own read | whoever runs `c+2` | deleted | G1 |
| **S1**, deposed L1 with a queued job (2.2) | — | J carries `c0`; `GET` → `Condemned(c)`, `c ≥ c0+2` → leave; S1b the same | kept | C1 |
| **S2**, publisher reconcile (2.3) | `GET` → `Clean` → refuse, carry, write `Condemned(c+1)` | graduates at `c+2`; an adopter between the reconcile and that cut is spared by the cut; the own job leaves `Condemned(c+1)` | kept; orphan after the delete | G1, C2 |
| **S3**, object stands for two incarnations (2.4) | condemn re-stamps `Condemned(c0)` → `Condemned(c1)`; if that fails, `GET` → `Condemned(c0) < c1` → refuse, retry stamps `c1+1` | L1's job `GET` → `> c0` → leave | kept | C2, C3, C1 |
| **S4**, late publisher `Clean` after a reissue (2.5) | `GET` → `Clean` → refuse, write `Condemned(c+1)` | `c+2` graduates on `Condemned(c+1)`; A `If-Match Condemned(c)` → 412 | kept | C2 |
| **Returning leader** | A's losing round-1 attempt `HEAD`s `T1` and writes `Condemned(1)`. B's frozen job for `T0` (round 1) may delete it: equal rounds, allowed. A returns; round 3 supersedes to `(X, T1, 3)`; its marker write fails. Round 4 `GET` → absent or `Condemned(1) < 3` → refuse, write `Condemned(4)` | round 5 graduates; B's job, if it lands later, `GET` → `Condemned(4) > 1` → leave | kept | G1, C3, C1 |
| **Rebuild** (surviving `Condemned(100)`, rounds below 100, every node restarted, no request in flight) | A condemns `T0` at 2 (marker stays 100), graduates at 3 on 100 | A deletes `T0` at 4, freezes; its job `GET` → `100 > 2` → leave. B steals, finds `T0` `Gone`; W publishes `T1`, reconcile fails; B condemns `T1` at 5, graduates at 6 on 100; A's job, `T1`'s job: `100 > 2`, `100 > 5` → leave | orphan `Condemned(100)`, no loss | C1 |

**The CAS-331 schedules.** Safe: P1, P1b, 4.13 (under Q7, else L1b), 4.10 (the job reads). Delayed: 4.8. One
orphan, no loss under rule B: 4.5 (7.2). F12 removes most of PR2's request-2 cut.

## 7. Liveness and the rebuild corner {#liveness}

### 7.1 Liveness {#liveness-argument}

Take `E = (X, T, c)` whose body stays `T` and whose in-degree stays zero. Count only **eligible rounds**: the
graduation budget admits `E`, destructive work is not suppressed, the gate's read succeeds, and the retry write
commits.

1. An eligible round `k` refuses only on absent, `Clean` or `Condemned(< c)`; its retry leaves a round `≥ k ≥ c`.
2. The next eligible round accepts, unless another writer changed the marker: `T`'s publisher writes `Clean`
   once per publication (S2), a GC write only raises the round, a job removes only rounds below `c` (section 4).
3. So `E` graduates within two eligible rounds of the last foreign write, and is redeleted in the next committed
   round.

- Budget exhaustion, suppression, failed reads or writes, lost rounds and leader changes delay it while they last.
- **Spare, then re-condemn, needs the re-stamp.** A spare leaves `Condemned(r1)`. Condemning the same token again
  at `c > r1` finds it; without the re-stamp the write leaves it, C3 refuses it, and every retry repeats that.

### 7.2 The rebuild corner, under owner decisions A and B {#rebuild}

**When rounds regress.** An absent `gc/state` is rebuilt (`rebuildBaseline`, round
`max(max_fence_round, state.round, max_gen) + 1`) or bootstrapped at 0. Rounds regress only when the surviving
`gc/gen` numbers are below the lost round. No condemned row survives: the rebuild condemns nothing.

**What carries a pre-rebuild round:**

- **`.meta` objects**, such as `Condemned(100)`: harmless. The gate accepts one for any `c ≤ 100` and every job
  leaves it: an orphan (section 6, last row).
- **A pre-rebuild queued job:** its round can exceed a post-rebuild guard's, so C1 lets it delete that guard.
- **A request a pre-rebuild process already sent:** `DELETE If-Match M` on `Condemned(100)` lands after every
  restart and removes the guard a post-rebuild entry graduated on, on every backend.

**A: the corner is accepted.** **B: operator rule:** stop or restart every server of the pool before a rebuild
that follows a lost `gc/state`, and do not resume a frozen one. This removes queued jobs and attempts, **not a
request the store already accepted**; B closes the known schedules only under backend quiescence (10.3).
Operator text for `SYSTEM CAS GC REBUILD` and the `cas-gc-rebuild` row: "Before rebuilding a lost `gc/state`,
stop or restart every server of the pool. A request a server sent before it stopped can still reach the object
store afterwards; this rule does not cover it."

**D: monotonic round at rebuild and bootstrap** is CAS-326. It also closes the in-flight case: a condemn at
`c > 100` re-stamps the marker (C2, C3), so graduation accepts only bytes no pre-rebuild `If-Match` names. The
base's unconditional job deletes any marker in the same corner, so PR1 is never worse.

## 8. Model {#model}

**Module.** `CaGcCondemnMarkerGate.tla` in `tmp/cas-pub-marker/`. It extends the CAS-30 variant in
`tmp/cas30-meta-model/`, whose 22 cfgs keep their verdicts and green distinct counts in it
(`tmp/cas-pub-marker/final2_regress.txt`). PR1 promotes it to `docs/superpowers/models/` with its cfgs, rows in
`run_condemnmarker.sh`, and a regenerated `CaGcCondemnMarkerGate_RESULTS.md`.

**PR1 switches.**

| Switch | Value | Code |
|---|---|---|
| `GateOnMarker`, `GateAlwaysReads`; `GateRoundCheck`; `JobRoundCheck`; `Restamp` | `TRUE` | G1; C3; C1; C2's re-stamp |
| `RetryStampsCurrent` | `TRUE` | **new**; C2's retry stamp |
| `SplitMetaDelete`, `SplitPublish`, `RealCandidate`, `Respawn` | `TRUE` | the code's shape |
| `Registry`, `RegRound` | `FALSE` | no registry |
| `PersistConf` | either | the carried bit; `GateAlwaysReads` ignores it (5.5) |
| `WriterLeavesMarker`, `DeleteReads`, `GateTouched`, `ForgetOnTouch`; `ExternalDrop` | `FALSE` | rejected candidates; residual 10.1 |
| `OutstandingReconcile` | `"none"` in the PR1 cfgs | **new**; 10.2 |
| `EtagIsContent` | `TRUE` | **new**; S3; `FALSE` for GCS and in-memory |

Fault switches ("all"): `AsyncMarker`, `Prefetch`, `ReconcileFault`, `StaleMarkers`, `StaleAttempts`,
`SuppressRounds`. `ExactAbsentPath` makes S2 exact.

**New constants, each inert at its default.**

- `RetryStampsCurrent`: `GSettleCarryRetry`'s write stamps `round`, not `ret.cr`. `mjob` becomes `0..MaxRounds`
  (0 = none) and carries the round the queued job stamps.
- `OutstandingReconcile`: `WReconcile` may leave one in-flight request `late = [expect, moved]`. `"resolved"`:
  the success arm leaves it (S4, L1b); `"gaveup"`: the fault arm leaves it, marker unchanged (L2). The task still
  ends, so `WDrop` is enabled. `WLateLand` matches iff `meta = late.expect` under `EtagIsContent`, else iff
  `~late.moved` (set by every step that changes `meta`), and writes `Clean`. `WLateDie`: it never lands.

**Processes.** No process identity or build version. Stale processes act through `GStaleAttempt`,
`GStaleMarkerWrite` and `GMetaDeleteJob` (unfenced).

**Invariants.**

| Invariant | Meaning | Status |
|---|---|---|
| `NoDangle` | a committed writer's body is present | exists |
| `MarkerPreservedPending` | a pending entry whose token is current keeps a `Condemned` marker | exists; section 4's claim without the round |
| `JobSparesPendingGuard` | `\A c \in mdq : ~(ret.st = "pending" /\ TokCurrent /\ JobWouldDelete(c))`, `JobWouldDelete(c) == ~JobRoundCheck \/ (Cond /\ meta.cr <= c)` (`mdq` holds rounds, as `GMetaDeleteJob` reads it): no queued job can remove the live guard | **new**; section 4's point 2 |
| `NoWedgedCarry` | `~(ret.st = "condemned" /\ ret.retried /\ Cond /\ meta.cr < ret.cr)`: after a retry write that completed without error, the marker is not an older round; `ret.retried` is set by a completed retry write, only under a new constant `TrackRetry` | **new**; the re-stamp regression check, not an eventual-graduation property |
| `MarkerPreservedConfirmed` | vacuous under `GateAlwaysReads` | kept for the regression cfgs |

**Required before PR1 merges.** `MaxGen 3`, `MaxRounds 7`, `TwoWriters`, through `run.sh` or
`run_condemnmarker.sh` (8 workers, `nice -n 10`). Regression cfgs keep their own values. All evidence so far ran
with `Registry TRUE`; the `*_noregistry` cfgs are the first `Registry FALSE` runs.

| cfg | Adds to the PR1 switches | Expect | Evidence so far |
|---|---|---|---|
| `pm_g1_noregistry` | — | green, all invariants | none (`pm_g1`, `Registry TRUE`: green, 241,547) |
| `pm_g1_all_noregistry` | all faults | green, all invariants | none (`pm_g1_all`, `Registry TRUE`: green, 108,547,117) |
| `rv_g1_noregistry` | `ExactAbsentPath`, all | green | none (`rv_g1`, `Registry TRUE`: green, 1,951,931) |
| `pm_g1_f1` | `f1_fix`'s cfg with `GateAlwaysReads TRUE`: `StaleAttempts`, `SplitPublish FALSE` | green | not run |
| `pm_g1_live` | `TrackRetry`, `AsyncMarker`, `ReconcileFault`, `SuppressRounds`; `StaleMarkers`, `StaleAttempts FALSE` | `NoWedgedCarry` green | not run |
| `pm_g1_late_resolved` | `OutstandingReconcile "resolved"`, `AsyncMarker`; `StaleMarkers`, `StaleAttempts FALSE` | green: S4 closed | not run |
| `pm_g1_late_resolved_entry` | the same, `RetryStampsCurrent FALSE` | `NoDangle` violated (S4) | not run |
| `pm_g1_late_resolved_version` | the same, `RetryStampsCurrent FALSE`, `EtagIsContent FALSE`, `StaleMarkers`, `StaleAttempts` | green: a replaced version never returns | not run |
| `pm_g1_late_resolved_stale` | `pm_g1_late_resolved` with `StaleMarkers`, `StaleAttempts` | `NoDangle` violated (L1b) | not run |
| `pm_g1_late_gaveup` | `OutstandingReconcile "gaveup"`, `ReconcileFault` | `NoDangle` violated (L2) | not run |
| `pm_g1_nogateround` | `GateRoundCheck FALSE`, all | violation (C3 load-bearing) | violated, 35,332,696, 860 s (`Registry TRUE`) |
| `pm_g1_nojob` | `JobRoundCheck FALSE`, `StaleMarkers` | `JobSparesPendingGuard` or `NoDangle` violated (S1) | not run |
| `pm_g1_norestamp_live` | `pm_g1_live`, `Restamp FALSE` | `NoWedgedCarry` violated (the spare-then-re-condemn wedge) | not run |
| `pm_g1_extdrop` | `ExternalDrop` | `NoDangle` violated (10.1) | violated, 73,907 (`Registry TRUE`) |
| `rv_bug`, `f1_bug` | today's code | `NoDangle` violated (S2, returning leader) | violated |
| regression | every existing cfg of both directories, unchanged, new constants at their defaults | same verdicts; green rows keep their distinct counts | — |

- **Every violation trace is read** and matched to its schedule. A green-expected row that violates is a wrong
  cfg or a new schedule, and reopens the section it closes.
- **Not required, started before merge:** `pm_g1_big` (all faults, `MaxGen 4`, `MaxRounds 8`, 2 h cap). It timed
  out at 20 minutes and about 228 M states in battery 2. An unfinished run is reported with its distinct count.

**What the model does not show.**

- `GSettleGraduate` makes the gate read, the graduation and the round commit one step. Under PR1 this loses no
  interleaving that matters: a losing attempt's read leaves no state, and a `Clean` write between the read and
  the commit needs a reconcile request after its publisher's `−1` (`ExternalDrop`, `OutstandingReconcile`).
- The read-ahead (5.6; RA1, RA1b, RA5), a destructive request landing after a PR1 read (10.4), an old build's
  pending entries, and GC's own ambiguous writes (every GC write is atomic; L1b's GC-reissue source is argued).
- One hash, one retired entry, two writer slots with respawn, one `GStaleAttempt` and one late request per
  behaviour. No liveness property. The meta value is its bytes, so under `EtagIsContent` it stands for S3's
  content etag.

## 9. Tests, failing first {#tests}

- **Framework:** gtest, deterministic, no sleeps, no death tests. Suites `CASGcMetaWriter`
  (`src/Disks/tests/gtest_cas_gc_meta_writer.cpp`), `CASThreeCursorMerge` (`gtest_cas_blob_indegree.cpp`), RA tests
  (`gtest_cas_gc_read_ahead.cpp`, `gc_io_concurrency` 1 and 8), all inside the `CAS*` gate filter.
- **Seams:** `PoolConfig::gc_redelete_apply_hook_for_test` on a one-shot latch freezes a leader in S1's window.
  Test-local backends: a one-shot hook before the first `head` of one key (W2 after the cut, before the redelete
  `HEAD`); `ContentEtagBackend` (equal bytes, equal etags, as on S3); a hook after a `get` returns (RA1b); a hook
  that parks the first `.meta` `GET` until N more arrive, with a bounded wait that fails the test (RA5).
- **Fixture:** the part-written-and-dropped fixture of `ThrowingRoundDrainsBeforeReturning`, as a helper that
  drives rounds until a predicate on `previewDeletes` holds. B2, RA2 and RA5 reconstruct the `Gc` (same `gc_id`)
  before the measured round, so the base's registry is empty and its gate reads. Tests meant to expose the
  registry shortcut keep one instance.
- **Read counts:** "gate `GET`" is the graduation read; backend counts add the retry's `GET` and `PUT` per refusal.
- **Commit 1:** every test except CB1–CB3, against today's API. U1 and U2 call `scheduleConfirmedMetaDelete(ref)`.
  Run on the base: the red set must equal the "Red on base" column. **Commit 2:** the fix, and CB1–CB3, which
  test `scanAhead`, an API the base lacks; U1 and U2 pass the round; all green.
- **Mutation checks**, run once and reported: each named rule reverted alone turns the named test red.

| # | Test | Asserts after the fix | Red on base because |
|---|---|---|---|
| U1 | `ConfirmedMetaDeleteSparesANewerRound` | marker `Condemned(5)`, job for round 3 → `Condemned(5)`, same etag | the job deletes it |
| U2 | `ConfirmedMetaDeleteSparesACleanMarker` | marker `Clean`, job for round 3 → `Clean` | the job deletes it |
| U3 | `ConfirmedMetaDeleteRemovesItsOwnOrAnOlderRound` | `Condemned(3)` and `Condemned(2)`, job 3 → both absent | green: guard against leaking |
| U4 | `CondemnMarkerWriteRestampsAnOlderRound` | `Condemned(3)`, write at 5 → `Condemned(5)` | stays `Condemned(3)` |
| U5 | `CondemnMarkerWriteLeavesAnEqualOrNewerRound` | `Condemned(5)`, `Condemned(7)`, write at 5 → unchanged, same etag | green: guard |
| U6 | `ConfirmedMetaDeleteRefusesAMarkerChangedAfterItsRead` | `setHoldDeletes`: job 3 reads `Condemned(3)`; test re-stamps `Condemned(5)`; `landPendingDelete` → `Mismatch`, `Condemned(5)` stays | green: guard for `If-Match` |
| R1 | `GraduationRereadsTheMarker/OwnMarker` | condemned at `c`, test writes `Clean`: `c+1` refuses, `UnconfirmedCarry` +1, marker `Condemned(c+1)`; `c+2` graduates | graduates on the registry |
| R1 | `GraduationRereadsTheMarker/LostAttemptMarker` | A's losing attempt writes the marker while parked on Y; B commits `c`; test deletes the marker; A steals back: `c+1` refuses, writes `Condemned(c+1)` | A graduates on its registry |
| R2 | `GraduationRefusesAnOlderRoundMarker` | test re-stamps `Condemned(c−1)`: `c+1` refuses and stamps `c+1`; `c+2` graduates; after delete: body gone, `Condemned(c+1)` stays | accepts `Condemned(c−1)` |
| R3 | `SparedThenRecondemnedBlobGraduates` | spared at `r1`, re-condemned at `c > r1`: graduates within two rounds | green; red with C3 and no re-stamp |
| R4 | `GraduationAndDeleteRequestCounts` | `CountingBackend`, concurrency 1 and 8: graduation round 1 `.meta` `GET`; delete round 1 `GET`, 1 conditional delete, no gate `GET`; marker gone | graduation makes 0 `GET` |
| R5 | `NewLeaderDeletesAGraduatedEntry` | A graduates X and is destroyed; B's first round removes the body and, through its job at round `c`, the marker; no orphan | green: guard that C1 takes the row's round across a leader change |
| R7 | `StaleMetaDeleteSparesSuccessorMarker/{BeforeGraduation,AfterGraduation}` | S1 replayed: A parks after removing `T0` (apply hook); B steals, deletes `T0`'s marker; W1 writes and drops `T1`; B condemns at `c1`; A released before or after `c1+1`, its job (round `c0`) runs; marker stays `Condemned(c1)`; head hook commits W2 after B's `c1+2` cut: W2 not `Observed`, outcome `Replaced`, body present, W2's part reads back | A's job deletes the marker; W2 adopts; B deletes `T1` |
| R8 | `LatePublisherCleanMissesTheRetriedMarker` | S4 on `ContentEtagBackend`: test lands `Clean If-Match M`; `c+1` refuses, marker `Condemned(c+1)` (not `M`); `c+2` graduates; late `Clean If-Match M` refused; W2 does not adopt, body present. Mutation: retry at `entry.condemn_round` loses the blob | `c+1` graduates on the registry |
| R9 | `GraduationAcceptsANewerRoundMarker` | test re-stamps `Condemned(c+3)`: `c+1` graduates, no carry, no write; after delete: body gone, `Condemned(c+3)` stays | job deletes `Condemned(c+3)`; red if C3's `≥` becomes `==` |
| B1 | `CASThreeCursorMerge.GraduationBudgetIsCheckedBeforeTheGate` | cohort 10, `max_graduations = 3`: 3 gate calls, 3 graduated; gate refusing the first 2: 5 calls, 3 graduated | 10 gate calls in both |
| B2 | `GraduationReadsStayWithinTheBudget` | 3 untouched condemned blobs, budget 1, fresh `Gc`: 1 `.meta` `GET`, 0 wasted, concurrency 1 and 8 | 3 `GET`s |
| B3 | `CASThreeCursorMerge.MarkerConfirmedBitDoesNotSkipTheGate` | non-pending row with the bit, refusing gate: 1 gate call, carried | 0 gate calls; graduates |
| RA1 | `GraduationReadAheadReadsEachRound` | concurrency 8, test writes `Clean`: `c+1` 1 gate `GET`, refuse, 2 `GET`s and 1 `PUT` in total, marker `Condemned(c+1)`; `c+2` 1 new `GET`, accept; each gate `GET` follows its round's last ref-log read | graduates at `c+1` on the registry |
| RA1b | `GraduationReadAheadDecidesOnTheFetchedValue` | the read-completion hook writes `Clean` after the hinted `GET` returned `Condemned(c)`: gate accepts, 1 `GET`; same at concurrency 1 | 0 `GET`s: the registry hits |
| RA2 | `GraduationReadAheadRequestCounts` | concurrency 8, reconstructed `Gc`, 10 eligible, budget 3: 3 `GET`s, 0 wasted; 2 `Clean`: 5 gate `GET`s + 2 retry `GET`s + 2 `PUT`s; re-referenced blob: no `GET`; `suppress_destructive`: none; all equal concurrency 1 | 10 `GET`s at the gate before the budget (F13) |
| RA3 | `GraduationReadAheadSurfacesAReadError` | hinted `GET` throws once: carried, `CASGCMetaWriteAnomaly` +1, round commits, next graduates | graduates on the registry |
| RA4 | `GraduationReadAheadSkipsSupersededEntries` | 20 eligible, 10 superseded in each of 3 rounds: no hint for them, 0 wasted | green: guard against hinting a touched entry |
| RA5 | `GraduationReadAheadIgnoresUnusedHeadHints` | window 32, reconstructed `Gc`: 32 untaken head hints plus 20 graduation candidates; the parking hook is released by 8 further graduation `GET`s, not the timeout; all 20 graduate | graduation `GET`s are serial; the wait times out |
| CB1 | `CASThreeCursorMerge.ScanAheadStopsAtTheRowCap` | `kGraduationLookaheadRows + 100` rows in one segment: nothing past the cap offered; merge result identical | new API, commit 2 |
| CB2 | `CASThreeCursorMerge.ScanAheadStopsAtTheSegmentEnd` | no row of segment 2 offered before it opens; offers resume; result identical; last-row entry of segment 1 still admitted and read | new API, commit 2 |
| CB3 | `CASThreeCursorMerge.ScanAheadKeepsRowAndChecksumValidation` | corrupt row, and bad seal checksum on segment 2: `CORRUPTED_DATA` with and without scan; no successor state, no body deleted | new API, commit 2 |

**Changed existing tests:**

- Removed: `CondemnMarkerConfirmationIsVisibleAfterDrain`. No token:
  `RealCondemnMarkerJobCompletesAcrossOwnerDestruction`. Round 1 over `Condemned(1)`:
  `RealConfirmedMetaDeleteCompletesAcrossOwnerDestruction`. Reworded registry comments:
  `ThrowingRoundDrainsBeforeReturning`, `gtest_cas_gc_ack_floor.cpp`.
- The implementer greps: `CountingBackend` goldens gain one `.meta` `GET` per graduation the registry served.

**Gate lanes:** the `CAS*` gtests under ASan and TSan, and the fast-test (no S3) compile.

## 10. Residuals {#residuals}

Not closed by PR1. Every one also loses data on the base.

### 10.1 Namespace removal during a reconcile (`extdrop`) {#extdrop}

1. W publishes `T` into X; GC condemns `T`. `dropNamespace` makes W's `−1` durable while W's reconcile is in
   flight (`requireAlive` runs only at the publication loop head in `PartWriteTxn::ensureBlobPresent`).
2. The reconcile retries past the next graduation read, which accepts `Condemned`; then it lands `Clean`.
3. A writer in another namespace adopts `T` after the delete round's cut. The delete removes `T`.

- **Model:** `pm_g1_extdrop` violates `NoDangle`. W1 does not close it under PR1 (`pm_g1w1_extdrop` fails on an
  equal-round byte-identical marker); under CAS-331's V2r it does (`v2r_w1_extdrop_r6`, bounded).
- **Closure:** CAS-334: the removal waits for in-flight builds, or the reconcile is refused once it is durable.

### 10.2 A publisher's `Clean` request that lands late {#late-requests}

Q7 fails: one `PUT Clean If-Match M` that W sent (attempt A) is still in flight after W's `−1`. **Accepted for
PR1 by the owner (2026-09-30); closed by CAS-334, the next PR.**

| Case | W's outcome | Backend | Under PR1 |
|---|---|---|---|
| L1: GC's retry recreates `M` (S4) | success via reissue B | content etag | **closed** by the retry stamp (5.2) |
| L1 or L1b on a version etag | success | GCS, in-memory | **closed**: B replaced version `M`, and no write brings it back |
| L1b: another GC write of the same round recreates `M` after B | success via reissue B | content etag | **residual** |
| L2: W gives up, marker unchanged | `GaveUp`, the part aborts | any | **residual** |

- **L1b:** sources are GC's own reissued `Condemned(c) If-Match Clean` (F16), a second attempt of round `c`, a
  deposed leader's queued write at `c`. A stays in flight until after the read that accepts the recreated `M`.
- **L2:** A stays in flight past the first graduation read after W's `−1` folds; that read accepts the unchanged
  `Condemned(c)` and writes nothing, so no stamp helps.
- **CAS-334:** publishers never turn `Condemned` into `Clean`, on both paths of `reconcileMetaClean`. No new
  request, no format change. Cost: later writers of a live hash republish instead of adopting until GC settles it.
- **Not closing either:** captured etags, a leadership check, a synchronous marker delete. Model:
  `pm_g1_late_resolved_stale` (L1b), `pm_g1_late_gaveup` (L2) must violate. Needs a request in flight for over
  one GC interval (60 s); no backend documents a bound.

### 10.3 Requests in flight across a rebuild {#pre-recovery-requests}

The in-flight `DELETE If-Match M` of 7.2, landing after a rebuild that regressed rounds. CAS-326 closes it.

### 10.4 Old-build work and destructive requests outstanding past a PR1 read {#old-build-requests}

Q8 fails, or an entry reaches the redelete without a PR1 read. **Accepted by the owner (2026-10-01): upgrades are
procedural for now (11).** A restart revokes none of these once its request has reached the store:

- **Old-build job.** It deletes any marker it reads (F2). A delayed job reads `M` and sends
  `DELETE .meta If-Match M`; the upgraded GC graduates `T1` on `M`; the `DELETE` lands; W2 adopts `T1` after the
  next cut; the redelete removes it.
- **Upgraded leader's stalled redelete** (F19). A graduates `T1`; an old job removes the marker; A's next
  round `HEAD`s `T1` and stalls before its `remove`. W reads the absent marker and adopts `T1`. B takes the lease,
  folds W's edge, spares `T1`, commits. A resumes and deletes `T1`.
- **Pending entries the old build graduated.** The pending branch redeletes them without a read (F17). One whose
  marker is `Clean` (S2 on the base) or was removed by an old job loses a blob to a writer that adopts after the
  cut.

**Boundary.** The quiescence premise at a moment Q covers every outstanding destructive request and stalled
attempt, of the old build and the new alike. Q also requires that no earlier tenure starts new destructive work
after Q: a stalled tenure that resumes after Q sends a new request past the boundary.

- **Without quiescence the residual has no finite endpoint.** No backend documents when a request stops landing,
  and no round or owner the new build keeps changes when one lands.
- **With it,** an entry graduated on a PR1 read after Q is covered by section 4. Entries pending at Q are not;
  the window closes once they are redeleted.
- **No rule is added.** Any read before a destructive step loses to a request landing after it.
- **Model:** not expressible. `GSettleRedelete` makes the body delete, the entry removal and the round end one
  step; splitting it is out of scope for PR1.

## 11. Rollout and scope of the guarantee {#rollout}

Upgrades are procedural for now: few deployments, each upgraded by hand. **Procedure.** `cas_gc_enabled` is read
once, at disk construction (F18); only a restart applies a change.

1. On every old-build node, set `<cas_gc_enabled>false</cas_gc_enabled>` in each CAS disk's config and restart
   the process. It then runs no GC and refuses `SYSTEM CAS GC RUN`, `START` and `REBUILD`.
2. Only after step 1 has finished on every node, upgrade nodes, removing the override; upgraded GC starts.
3. `SYSTEM CAS GC RUN`, `START` or `REBUILD` on an old process not restarted per step 1, or an old-build
   restart with GC enabled, invalidates the procedure.

- **The stop does not drain the old build's pending cohort.** No pending count is exposed (`system.cas_gc_log`
  reports per-round counts only), and `gc_round_graduation_budget = 0` means unbounded, so an old round that
  redeletes may graduate a new cohort. Entries pending at step 1 are redeleted by the upgraded GC without a PR1
  read; they are part of residual 10.4.
- **What the procedure removes:** old processes, their queued jobs and attempts. **Not** a request the store
  accepted before the restart, the old build's pending cohort, nor a stalled upgraded attempt (10.4).
- **Downgrade to 26.6.4:** the same procedure. Nothing new on disk; the old build is outside the guarantee (5.5).
- **Operator note** in `docs/en/antalya/cas/operations/debugging.md`: "Before upgrading, set `cas_gc_enabled` to
  `false` on every server of the pool and restart each one; the setting takes effect only at restart. Then
  upgrade, removing the setting on each upgraded server. Never run `SYSTEM CAS GC` commands on, or re-enable GC
  on, a server still on the old version. A delete request sent before a restart can still reach the object store
  later and remove a marker GC relies on, so a blob can be lost; nothing bounds when such requests stop."
- **Not covered:** section 10's residuals; the rebuild corner without rule B (7.2); out-of-band `.meta` writes.

**Rejected: re-gate before every redelete.** One round and one `GET` per reclaimed blob forever, and it still
loses to a request landing after the re-read. **Rejected: re-gate once per tenure** (r5–r8's rule RG): it
protects only entries an old build graduated, and upgrades are procedural (10.4).

## 12. Review findings {#review-answers}

Reviews: `tmp/cas30/spec_review_r1.log` … `spec_review_r8.log`, all REVISE.

| Review | # | Finding | Answered by |
|---|---|---|---|
| r1 | 4 MAJOR, 2 MINOR | registry needs a round; publisher sibling; returning leader and marker invariant; test order; recovery premises; captured etags | G1 (5.4); S2; section 6; `MarkerPreservedPending`, `JobSparesPendingGuard`, `pm_g1_nojob`; R1, R7; 7.2; 5.3 |
| r2 | 2 MAJOR, 4 MINOR | gate before budget; rebuild residual; S3 alias; liveness; orphans; scope | budget first (5.1, 5.6), B1, B2, RA2; rule B, 10.3; R8; eligible rounds (7.1), `NoWedgedCarry`; 5.3; 10, 11 |
| r3 | 2 MAJOR, 3 MINOR | task vs request completion (S4, `GaveUp`); rule B vs accepted requests; evidence labels; read ceiling; tests | Q5/Q7; S4; retry stamp (5.2); L1b, L2, R8; 10.3, CAS-326; `*_noregistry` cfgs; section 3; R9, B3 |
| r4 | 1 MAJOR, 5 MINOR | inherited pending entries skip the gate; budget omits the current entry; read-ahead bounds | r4–r8: rule RG; r9: owner accepts the old build's pending cohort as residual (10.4, 11); reservations (5.6); RA1, RA1b, RA4, CB1–CB3; 10.2; section 4 |
| r5 | 1 MAJOR, 4 MINOR | G2's owner comparison unsound; shared window; checksum at drain; quiesced old-job model | G2 withdrawn; own window, RA5; CB3; the model cfg went with RG |
| r6 | 3 MAJOR, 2 MINOR | residual window ends too early; "stop again" does not keep old GC off; inherited cohort unstated; fixtures; `c.r` | 10.4 restated; config override (11); 11 states the cohort; reconstructed `Gc` in RA2, RA5; `meta.cr <= c` |
| r7 | MAJOR 1 | the recovery endpoint ignores an upgraded leader's stalled redelete | Q8 and 10.4 cover every outstanding destructive request of either build; F19 |
| r7 | MAJOR 2 | the config override does not reach a running old process | F18 (`const` at construction); old nodes restart before any upgraded GC; commands on a surviving old process invalidate it (11) |
| r7 | MAJOR 3, MINOR 4 | no test of an unread inherited pending entry; RG's delay understated | moot: RG dropped in r9 |
| r8 | MAJOR 1 | the endpoint needs earlier tenures kept from starting new destructive work after Q | accepted by owner decision, not by a rule: one sentence in 10.4's boundary; residual accepted (13) |
| r8 | MINOR 2 | R11 asserts marker removal against C1; wrong base failure for `Suppressed` | moot: R11 removed with RG |

## 13. Assumptions and owner decisions {#questions}

**Decided by the owner:**

- **L1b and L2 (10.2) are accepted for PR1** (2026-09-30). The writer rule "publishers never turn `Condemned`
  into `Clean`" and the `extdrop` closure are CAS-334, the next PR.
- **The rollout residual (10.4) is accepted** (2026-10-01): "upgrades are procedural for now". Section 11 is a
  procedure plus that residual; rule RG is dropped.

**No open questions remain.** Nothing in the design is undecided.

Assumptions, stated so the work can continue:

1. **The graduation read-ahead ships in PR1** (owner decision). `kGraduationLookaheadRows = 16384`; the
   implementer reports the graduation phase time and memory on the `gc-ab` stand at concurrency 1 and 16.
2. **A refusal consumes no graduation slot**, as today; counting reads would let refusals crowd out graduations.
3. **The job's bound stays the entry's round** (5.3). A bound at the redelete round `d` (delete `Condemned`
   below `d`) would also remove the retry's orphan; it goes to CAS-329 with a model cfg as its entry condition.
4. **CAS-30 wording.** Title: "A delayed `.meta` delete never removes the marker of a pending incarnation whose
   body is current". Criteria: the four rules of section 1; the budget before the read, prefetched only for
   admitted entries in its own window; the operator docs state the procedure and its residual; the `CAS*` gtests
   green under ASan and TSan; section 8's required runs match; section 10's residuals are backlog tasks.

## Limits {#limits}

- **Nothing in `src/` was built or run.** The code shape and every test are designs.
- **Model:** every required cfg in section 8 is specified, not run. Existing G1 evidence ran with `Registry TRUE`.
  A destructive request landing late, of either build, and the old build's pending cohort are not modelled.
- **Section 4's argument** is by reading the code. Q1 is assumed; Q7 and Q8 are premises the code does not
  establish.
- **Reading claims the implementer re-checks against the final diff:** the fold writes no marker of a hinted hash
  between hint and take; an untouched carried condemned row has no edge and reaches the gate (RA2, RA4 assert
  zero waste); no read hint is outstanding when the merge phase starts; `cas_gc_enabled` is read only at disk
  construction and, `false`, refuses `RUN`, `START` and `REBUILD` (F18).
- **Stall lengths, race widths, request lifetimes and latencies** are derived, not measured. **Backends:** S3,
  GCS and in-memory only.
