---
description: 'Full account of the 2026-07-26 investigation that proved a CAS ref-prefix enumeration omits durable objects — problem statement, every method tried, what worked and what did not, the instrumentation built, and what the fix must address'
sidebar_label: 'LIST incompleteness: full investigation'
sidebar_position: 100
slug: /superpowers/reports/list-incompleteness-investigation-2026-07-26
title: 'LIST incompleteness in the CAS ref prefix — full investigation record'
doc_type: 'reference'
---

# LIST incompleteness in the CAS ref prefix — full investigation record {#investigation}

Written 2026-07-26 as the basis for designing the fix. It is deliberately a BRAINDUMP: the dead ends are
here in as much detail as the result, because three of them cost most of a day and the next person should
not pay for them again.

**Bottom line up front.** A ref-prefix enumeration failed to return two objects that had been durable for
nineteen seconds, while returning a third object written 2.2 ms *after* them. Every alternative explanation
the detector itself names is now excluded by measurement rather than by argument. The release blocker
`{#list-as-journal-dataloss-2026-07-25}` is no longer a model — it is observed. The store-side mechanism
remains unknown.

---

## 1. The problem as it stood {#problem}

### 1.1 The defect class {#defect-class}

GC folds ref-log transactions into blob in-degree. It discovers those transactions by LISTing the ref
prefix, folds what the listing returned, and then advances a durable cursor past them. **The cursor is
sealed above every record the round OBSERVED — not above every record that EXISTS.** Nothing in the
protocol proves the listing was complete.

If a listing omits a durable ref log, the cursor advances over it and that record can never be folded
again. Both directions of damage are permanent:

- **Retention.** A skipped `-1` leaves a residual `+1`, so the blob's in-degree never reaches zero and it
  is never reclaimed — a leak no incremental round can repair.
- **Deletion.** A skipped `+1` hides a live owner, so GC computes in-degree zero for a blob a committed
  manifest still references, and deletes it. **This is acknowledged-data loss.**

The second is why this is a release blocker, and why the whole investigation mattered.

### 1.2 What existed before this investigation {#prior-state}

- The mechanism was **mechanised in TLA+** (`CaRelinkConfirmCore.tla`, `_sab_holeylist`), which proves the
  mechanism is SUFFICIENT to cause the damage — not that it is what happens.
- **Probe A** had been built (commit `e01b5cd82be`) as a cheap detector and had FIRED in live soaks: 14
  disagreements in one run, later 7 firings retained in per-phase rows including one of 244,939 holes.
- It was not known whether the firings were real listing holes or **false positives from concurrent
  deletion between the round's two enumerations** — the probe's own message names both possibilities.
- A note in the backlog said the giant firing was "very likely a lost-lease artifact". That guess turned
  out to be wrong (§4.1).

### 1.3 How probe A works {#probe-a-design}

Two independent enumerations of `cas/refs/` happen per round:

- **walk 1** — `preFoldRefScan`, the pre-fold defer scan (`CasGc.cpp`, ~line 2405);
- **walk 2** — `Gc::fold`'s own enumeration (~line 1025).

Per namespace it compares the two id sets and reports a hole for any id present in one and absent from the
other, **provided the id is below the OTHER enumeration's own maximum for that namespace**. That witness
clause is what makes the signal meaningful: without it, a record appended between the two walks would
count, and appends are normal.

On any hole the round **aborts ref folding**: no cursor advance, no destructive action. This fail-closed
behaviour is why none of the observed firings caused damage.

**Two stated blind spots**, documented in the source and still open:

1. a hole that reproduces IDENTICALLY in both enumerations — nothing cheap catches that;
2. a namespace dropped WHOLESALE from one enumeration — it has no `ref_tables` entry, so the comparison
   loop never visits it. Widening the rule to use the pre-scan's own maximum as the witness was considered
   and REJECTED: it would fire on a namespace whose logs were all legitimately cleaned between the walks,
   and a false positive here blocks the cursor permanently rather than for one round.

---

## 2. Result {#result}

### 2.1 The captured firing {#captured}

```
namespace     ca_soak_ch1/store/3ba/3ba2c30d-d1b2-4b17-a536-d311f1e239d4@cas@
hole          epoch 1, seq 0x1430c    head_verdict = present
hole          epoch 1, seq 0x1430d    head_verdict = present
direction     missing from the pre-fold scan          (walk 1 missed them)
pre_max       epoch 1, seq 0x1430e                    (walk 1 DID return this)
fold_max      epoch 1, seq 0x14a10
fired at      16:47:38
counters      CasGcRefScanDisagreements=2  ProbeAHolePresent=2  ProbeAHoleAbsent=0
```

### 2.2 The timing, from `system.blob_storage_log` {#timing}

```
Upload  16:47:19.211480   .../_log/0000000000000001-000000000001430c.zst   200 B
Upload  16:47:19.212340   .../_log/0000000000000001-000000000001430d.zst   200 B
Upload  16:47:19.213680   .../_log/0000000000000001-000000000001430e.zst   195 B
```

Strict id order, 2.2 ms apart, **19 seconds before the disagreement was reported**. Walk 1 returned
`0x1430e`, so walk 1 ran after `.213680`, by which time the other two had been durable for 1.3–2.2 ms.

### 2.3 Every branch, and how it died {#branches}

| branch | how it was excluded | by |
|---|---|---|
| Phantom key (walk 2 invented it) | `head_verdict = present` at firing time | measurement |
| Concurrent deletion | 0 ref-object deletes by EITHER node in 16:47:00–16:48:00; ch2 has never deleted a ref object at all | measurement |
| Stale-epoch writer minting a low id | all holes epoch `0x4`/`0x1`, near-consecutive sequences, not an older epoch | measurement |
| Out-of-order appends | 65,263 ref-log uploads, **zero out of order** partitioned by writer epoch | measurement |
| Object not yet landed at walk-1 time | written 19 s before the report; witness written 2.2 ms AFTER the holes | measurement |
| Late-landing `Unresolved` PUT freeing an id | `resolveByExactGet` never returns a plain absent verdict; absent ⇒ `Unresolved`, lane stays wedged, id never freed | source |
| Page-limit mismatch between the walks | both use 1000 | source |
| Different key filters between the walks | both call `parseRefObjectKey`; only asymmetry is strictness | source |

**What remains: the store returned an incomplete answer about a prefix it had already written.**

### 2.4 The fail-closed path worked end to end {#failclosed}

The same three keys were `Delete`d at `16:53:59`, six minutes later. Folding aborted, the cursor held, a
later complete enumeration folded them, and GC reclaimed them normally. **No leak resulted.** The detector
produced exactly the outcome it exists for.

### 2.5 What is still unknown {#unknown}

- **The store-side mechanism.** `blob_storage_log` records object writes and deletes, **not LIST calls**.
  Whether a page was dropped, a boundary mishandled, or something else entirely — invisible from here.
- **Whether AWS S3 does this.** Everything here is RustFS, the store the soak and CI run on. The design
  conclusion does not depend on the answer: GC must not trust LIST completeness either way.
- **The shape's cause.** Holes come in SHORT ADJACENT RUNS, not pages (§4.4). Nothing yet predicts that.

---

## 3. What the holes look like {#shape}

From the historical firings, decoded out of the text log:

```
ch1 06:06:09 — 13 holes across 2 namespaces
   11 holes  span 0x1f171..0x1f17f  (15 id slots)
    2 holes  span 0x1119c..0x1119d  ( 2 id slots)
```

and from the live capture: 2 holes at `0x1430c..0x1430d`, immediately below the witness `0x1430e`.

**Two tight contiguous clusters, not pages.** A dropped LIST page would be ~1000 keys. Ref-log keys are
`<epoch hex>-<seq hex>.zst` zero-padded, so lexicographic order IS id order and these are physically
adjacent keys: the walk returned what came after the run and skipped the run itself.

Gaps inside a span (e.g. `1f172`, `1f177` absent from the hole list) are NOT evidence of interleaving —
`appendRefOps` legitimately leaves id gaps ("the id is a safe gap") on its conclusive-rejection path.

Direction counts across the historical data:

| direction | ch1 | ch2 | what it excludes |
|---|---:|---:|---|
| missing from the FOLD's scan (walk 2) | 0 | 338,559 | nothing — deletion explains it |
| missing from the PRE-FOLD scan (walk 1) | **30** | **28** | deletion cannot: walk 2 saw the object |

---

## 4. Methods that did NOT work {#failures}

This section is the point of the document.

### 4.1 Lease correlation — refuted a hypothesis I had recorded as likely {#failed-lease}

Correlated all seven firings against `gc_fence` / `gc_fence_out` / `mount_remount` / `mount_conflict`.
**No lease-class event inside any firing window**, ±60 s halo included, on either node. `gc_fence` fired
400 times on ch1 and 4,245 on ch2 during the run, so the logging was live.

This KILLED my earlier note that the 244,939 firing was "very likely a lost-lease artifact".

**Method trap hit here:** the first correlation used a correlated subquery and returned `NULL`, not `0`.
Reading `NULL` as "no events" would have reached the same conclusion **by accident**. Redone as an offline
comparison against the full event list.

### 4.2 Grepping the soak's server logs from the host — produced a CONFIDENT FALSE ZERO {#failed-permissions}

I grepped for probe A lines, got `0` everywhere, and wrote in the worklog that the logs held none. The
files are `-rw-r----- syslog:syslog` and my user is not in `syslog`. **Every grep was PERMISSION DENIED**,
and my own `2>/dev/null || echo 0` converted each denial into a zero.

Re-run inside the containers, the same logs held **177,276** probe A lines.

**Rule:** never let `|| echo 0` stand in for a command that can fail for reasons other than "no match".
Read container logs via `docker exec`, not from the host.

### 4.3 Three direct hammer runs against the store — all negative, ~19M keys {#failed-hammer}

Built `utils/ca-soak/scripts/list_consistency_hammer.py`: write a known key set, LIST the prefix repeatedly
under concurrent mutation, and diff each answer against the set known durable when the listing began —
**using probe A's own witness rule**, so a hit would be the same shape of evidence.

| run | pagination | rounds | pages | keys listed | deletes under each walk | HOLES |
|---|---|---:|---:|---:|---:|---:|
| add-only | continuation | 24 | ≤888 | 6.85M | — | **0** |
| held population + deletion behind the cursor | continuation | 40 | 151–166 | 6.2M | ~31,000 | **0** |
| held population + deletion | **start-after** (what CAS does) | 40 | 151–166 | 6.08M | ~15,500 | **0** |

**Why it found nothing is still not fully known.** The third run reproduces CAS's actual pagination —
`forEachListedKey` resumes by LAST KEY RETURNED and `ObjectStorageBackend::list` builds a FRESH
`object_storage->iterate(..., start_after)` per page, so every CAS page boundary stitches two independent
enumerations — and it still found nothing at 6M keys.

**Remaining differences between the hammer and the real walk**, in suspicion order:

1. **Retries inside a paginated walk.** CAS lists through the ClickHouse S3 client with its own retry and
   timeout budget, under chaos (the soak freezes and kills RustFS). The hammer used boto3 against a healthy
   store. Untested, and the only plausible producer of short adjacent runs.
2. **Concurrency profile.** The real walk runs alongside inserts, merges, mutations and a second node's GC.
3. **Key shape.** CAS keys are nested paths containing `@cas@`; the hammer's are flat `k-NNNNNNNN`.

#### Three design errors I made in this experiment, in order {#hammer-errors}

1. **Unbounded writers.** Writers ran flat out through the listing phase, so the prefix grew all run: by
   round 24 one listing covered 887,614 keys / 888 pages, disk dropped 12 GB and load hit 8.3. Killed it.
2. **A hole rule that would have manufactured its own finding.** With deleters added, `snapshot − listed`
   counts every key legitimately deleted DURING the walk. Caught by RE-READING the rule against the new
   deleter before running — the only one of the three caught before it cost anything. Fixed with a monotone
   deletion floor (the deleter always takes the smallest live keys, so one watermark is exact).
3. **Unthrottled deleters.** Capping only the writers let 3 deleters at 200 keys/batch outrun 4 writers by
   ~10:1; the population drained from 150,000 to ONE key by round 31 and most rounds listed 1–3 keys.
   **Its "40 rounds, zero holes" verdict measured nothing, and I nearly recorded it as a third clean
   result.**

Common cause of all three: launching without doing the steady-state arithmetic first. Writer throughput
~1,100 keys/s against deleter throughput ~12,000 keys/s is half a line of arithmetic that predicts the
collapse.

### 4.4 The "dropped page" model — killed by the data {#failed-page-model}

The natural reading of an incomplete listing is a lost page. The shape refutes it: 13 holes, not ~1000.
Offset-pagination skew is independently excluded — the deletion run removed ~31,000 keys from behind the
cursor on each of 40 walks and a cursor-shifting store could not have survived that.

### 4.5 Reconstruction from logs, generally {#failed-forensics}

Every method above reconstructs the scene AFTERWARDS from ids in a log. That approach is structurally
incapable of answering the central question, because **by the time anyone reads the log the object has
legitimately been deleted either way** — so "the listing omitted a durable object" and "the listing
returned a key that does not exist" are indistinguishable in hindsight. They are opposite defects that
indict different components.

---

## 5. What DID work {#successes}

### 5.1 Reading the direction the probe already logged {#worked-direction}

The single most productive step before instrumentation. Probe A logs each hole's direction, and the two
directions are not equally informative: "missing from the fold's own scan" is fully explained by
concurrent deletion, while "missing from the pre-fold scan" is not, because walk 2 demonstrably saw the
object. That split turned 338,617 undifferentiated holes into 58 that mattered.

### 5.2 Making the detector answer at firing time {#worked-instrumentation}

Described in §6. Three hammer runs and ~19M listed keys found nothing; one instrumentation change found it
in **four minutes**.

### 5.3 The audit log and the second node — both prompted by the user {#worked-user}

Three interventions, each closing a branch I had left open:

1. **"Why can't you trace this through the CA log? We write the full context of every critical decision
   into `details`."** → Found that `gc_fold_end` records `anomalies: '1'`, a bare COUNT, while
   `recordAnomaly` receives `(namespace, shard, ManifestId, reason)` and drops all four; `gc_fold_begin`
   carries an empty `detail` on every row. This WAS the reason everything had become archaeology.
2. **"Did you look at blob_storage_log?"** → It carries every object write with a microsecond timestamp.
   I had written that the write time was "not currently reachable"; that was wrong. This converted the
   append-ordering argument from a source reading into a measurement (65,263 uploads, zero inversions) and
   established the 19-second gap.
3. **"Look at the second node."** → ch2 has never deleted a ref object, and during the firing minute
   neither node deleted any. The concurrent-deleter branch — the probe's own alternative — died on a log
   I had not opened.

**The generalisable lesson: when a defect resists reproduction, ask what the system already recorded
before building a rig to record it again.**

### 5.4 Verifying tests fail {#worked-failing-tests}

Every test written in this round was confirmed RED before being trusted. Two caught real inversions of
intent that a green-only check would have shipped (§6.3).

---

## 6. Instrumentation added {#instrumentation}

### 6.1 `gc_anomaly` audit event {#instr-event}

`CasEventType::GcAnomaly` → `gc_anomaly` in `system.content_addressed_log`, emitted per hole with:

| detail key | meaning |
|---|---|
| `probe` | which probe fired (`A`) |
| `log` | the hole's ref txn id |
| `direction` | which enumeration missed it — the discriminator of §5.1 |
| `head` | `present` / `absent` / `probe_failed` / `not_probed` |
| `fold_max`, `pre_max` | each enumeration's maximum for the namespace |
| `hole_ordinal`, `row_cap` | position and the cap, so truncation is never invisible |

plus `namespace`, `outcome = ref_folding_aborted`, and the reason string.

### 6.2 The HEAD verdict — the decisive addition {#instr-head}

At firing time, HEAD the hole key:

- **present** → an enumeration omitted a durable object; the listing was incomplete.
- **absent** → the other enumeration returned a key that is not there; indicts the client/iterator and
  would invalidate the LIST-completeness reading entirely.

**Unrecoverable after the fact**, which is why it must be taken in the moment. Wrapped so a failing probe
degrades to `probe_failed` rather than turning a diagnostic into a round failure.

### 6.3 Two ProfileEvents, and why BOTH forms exist {#instr-counters}

`CasGcProbeAHolePresent` / `CasGcProbeAHoleAbsent`, registered in `utils/ca-soak/soak/signals.py` so
preflight FAILS if the binary lacks them rather than silently reading zero.

Three carriers on purpose, each covering another's blind spot:

- `EventEmitter` no-ops when no audit sink is installed;
- the text log is rotated and root-owned (§4.2);
- only the counters are readable by the soak harness, CI and `system.events`.

**Vindicated immediately:** after the soak, `system.events` read `present=0 absent=0` on both nodes —
chaos had restarted the servers and process-local counters reset — while the two `gc_anomaly` rows
survived intact. **Shipping only counters would have erased the finding.**

The test pins the DIRECTION of the verdict, not just its presence: `HoleyListBackend` hides a key without
deleting it, so `present` is the only correct answer. Verified failing both ways — verdict not taken, and
verdict inverted. An inverted verdict is worse than none: it would aim every future investigation at the
listing client while the store was at fault.

### 6.4 Volume cap {#instr-cap}

32 rows per round (one firing had 244,939 holes). The cap and the true total travel in every row, and a
truncated round logs an explicit line naming how many were counted but not detailed. 32 rows can never read
as 32 holes.

### 6.5 Also landed this round {#instr-other}

- **`ca-fsck` summary line** moved into `Cas::formatFsckSummary` so it is reachable from a unit test, with
  `corrupted_runs` added — it had been counted, part of `clean()`, rendered under `--detail`, and absent
  from the summary AND from the exit code, so a failing seal checksum was invisible twice over. The test is
  written against `clean()`'s own terms so the next unrendered finding fails there.
- **`stale_edge_verdict`** now returns `unchecked` on any partial scan, with positive findings still
  checked FIRST: partial weakens proofs of absence, never evidence of presence.
- **Four pre-existing RED harness tests** cleared (all stale, each the tail of a deliberate 2026-07-22
  change nobody updated the test for). 277 pass.

---

## 7. Adjacent findings {#adjacent}

### 7.1 The retention leak is a CONSEQUENCE of this defect {#adjacent-leak}

In-degree is a SET of source edges. A residual `+1` survives only if its `-1` never folded, and there are
exactly two ways:

1. **The `-1`'s log was omitted from an enumeration and the cursor sealed above it** — this defect.
2. **The `-1` reached the reducer BEFORE its `+1`**, so `present` was false and the remove was dropped as
   a no-op; the `+1` then landed with nothing to cancel it. `CasGcUnmatchedRemoveDeltas` counts this — 22
   occurrences in a 20-minute soak.

**The reducer is correct in both cases** and must not be changed: a set cannot cancel an element it never
received, and materialising a negative edge instead of dropping an unmatched remove is how a false deletion
would be born. The earlier note that "the obvious remedy is wrong" understates it — there is nothing to fix
at that layer at all.

Path 2 is UNINVESTIGATED and is not rare. `unmatched_remove_example` hands back one example per round.

### 7.2 GC performance, measured for the first time {#adjacent-perf}

- `fold_ref_intake` dominates and is LINEAR in the ref-log backlog: **256 logs/s** sustained.
- The flat cost is **per REQUEST (~0.92 ms)**, not per log. `S3GetObject` decomposes EXACTLY into
  `CasRefLogBodyGets + CasRefManifestBodyFoldGets` — one GET per log body plus one per emitted manifest
  edge — so "4.15 GETs per log" is `1 + edges_per_log`, and `edges_per_log` climbs 1.54 → 3.73 with
  backlog. Every edge also costs a HEAD: round trips = `logs + 2 × edges`.
- **39.6% of intake manifest fetches are re-reads.** 7,565 edges over 4,573 distinct manifests, measured by
  decoding all 959 ref-log transactions on the stand. **Intra-transaction redundancy is exactly ZERO** —
  all of it is cross-transaction: 2,991 manifests carry both an add and a remove edge in the window, so the
  same body is read once when the ref is published and again when it is dropped. A ROUND-scoped body cache
  is therefore a real lever; an op- or transaction-scoped one would save nothing.
- **`pending_deletes` hit 77.2 seconds** in a single occurrence against 243 ms for the next worst phase.
  Nobody has looked at it.

### 7.3 fsck cannot run on a real pool {#adjacent-fsck}

The entry gate timed out on a **5.5 GB** pool (`exceeded 180s`), and an earlier reading showed
`reachable=0` after 160 s — the scan had not finished its first phase. The timeout now degrades honestly to
a logged skip, but **the gate does not run**, exactly when the pool is large enough to matter.

### 7.4 Probe A is `reported-not-gated` {#adjacent-gating}

`CasGcProbeAHolePresent peak=2, nonzero_in=28/88 reads`. A soak can go GREEN with confirmed enumeration
holes. Defensible while the product fails closed — folding aborted, nothing deleted, the run genuinely was
safe — but now that the holes are confirmed rather than suspected, this should be a deliberate decision.

---

## 8. What the fix must address {#fix-requirements}

Not a design, a requirements list.

### 8.1 The core requirement {#fix-core}

**GC must not advance a cursor over records whose enumeration it cannot prove complete.** The committed
direction is an authoritative per-namespace CHAIN plus a complete-cut gate: a record whose predecessor link
is unaccounted for is not a record the round may seal past. Whatever the shape, the invariant is that
completeness becomes PROVABLE from the data rather than assumed from a listing.

### 8.2 Requirements the evidence adds {#fix-evidence}

- **Do not assume the holes are page-shaped.** They are short adjacent runs (§3). A fix keyed on page
  boundaries would miss them.
- **Do not rely on the store.** The mechanism inside RustFS is unknown and AWS S3 is untested. The fix must
  hold against a store that answers inconsistently, because we have one.
- **Keep probe A.** It is the reason no damage occurred, and it is now the only thing that catches this
  class in the field. Its two blind spots (§1.3) should be closed by the chain, not by widening its witness
  rule — that was already considered and rejected as a permanent-block risk.
- **Keep the HEAD verdict.** It distinguishes two opposite defects and cannot be recovered afterwards.

### 8.3 Separate work the fix does NOT cover {#fix-separate}

- The **56 already-leaked blobs** need a one-off reconciliation; no incremental round can reclaim them.
- **Path 2 of the leak** (§7.1) is an independent investigation.
- **The fsck entry gate** (§7.3) and the **`pending_deletes` cost** (§7.2) are performance work.

---

## 9. Method notes worth carrying forward {#method-notes}

1. **Ask what the system already recorded before building a rig.** Three hammer runs, ~19M keys, an
   afternoon — versus one instrumentation change and four minutes.
2. **A count is not a record.** `anomalies: '1'` could not even distinguish which anomaly fired. If a
   decision is critical enough to abort a round, its context belongs in `detail`.
3. **Counters die with the process; audit rows do not.** Chaos restarts wiped the ProfileEvents that
   carried this finding. Ship both.
4. **`|| echo 0` on a grep is a lie generator.** Permission denied, missing file and no-match all become
   the same confident zero.
5. **`NULL` from a correlated subquery is not `0`.** It nearly produced the right answer for the wrong
   reason.
6. **An experiment must reproduce the MECHANISM, not just the shape.** Two hammer runs tested a pagination
   mode CAS never uses.
7. **Do the steady-state arithmetic before launching.** All three hammer design errors were predictable on
   half a line.
8. **Record a known design gap BEFORE the verdict lands**, so the verdict cannot be quietly reinterpreted
   to fit.
9. **Verify tests fail.** Two of this round's tests caught inverted-intent bugs that a green-only check
   would have shipped.
10. **A partial answer is not a weak answer — it is a different one.** Wiring `--partial` into a gate whose
    job is to WAIT FOR PROOF guaranteed failure, and the failure message then claimed data loss that had
    not happened.

---

## 10. Artifacts {#artifacts}

| what | where |
|---|---|
| Captured raw evidence | `docs/superpowers/reports/2026-07-26-list-incompleteness-proof/` |
| Store hammer | `utils/ca-soak/scripts/list_consistency_hammer.py` |
| Probe A + `gc_anomaly` | `src/Disks/.../Gc/CasGc.cpp`, `Primitives/CasEvent.{h,cpp}` |
| Verdict test | `src/Disks/tests/gtest_cas_holey_list_detector.cpp` |
| Signal registration | `utils/ca-soak/soak/signals.py` |
| Backlog entries | `{#probe-a-proven-by-measurement}`, `{#probe-a-caught-live}`, `{#probe-a-hammer-negative}`, `{#anomaly-detail-is-a-bare-count}`, `{#leak-is-a-consequence-of-the-hole}` |
