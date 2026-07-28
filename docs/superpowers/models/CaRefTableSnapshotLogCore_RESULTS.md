# CaRefTableSnapshotLogCore — TLA+ gate results (v9)

Model: `CaRefTableSnapshotLogCore.tla`. Gates the writer and recovery core of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-1** (per-namespace contiguous
ids, the every-attempt rule), **INV-2** (every epoch transition closed in-band by a `slot-occupy`
seal), **INV-4** (`_ckpt` gating of deletion and of the recovery base) and §4 Recovery. Task 1 of
the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0 gate — it blocks the C++ work.

Runner: `./run_refsnaplog.sh` (runs every config and checks its expected verdict, including *which*
invariant a sabotage is required to break). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers auto`, 32 workers. Every number below is real TLC output from the
run of 2026-07-28, not an estimate. Whole harness: **96 s, 12/12 expectations met.**

Constants unless the row says otherwise: `MaxSeq = 4`, `MaxRestarts = 2`.

## Headline

1. **`LatePredecessorPut` is flipped from counterexample to proof.** Under rev.4 it was the
   documented expected-fail — a fenced predecessor's PUT landing below an already-published
   snapshot, losing acked data. Under v9 the identical adversary is **GREEN**, and the flip is
   bought by two independent v9 invariants, each of which is shown to be individually load-bearing:
   INV-1 contiguity means a straggler is always at the frontier and can never land below a
   published snapshot at all, and INV-2's slot occupancy decides the frontier slot — adopt it if
   the PUT landed, fence it forever if it did not.
2. **The flip is a controlled experiment, not an assumption.** `_sab_noseal` is the same
   behaviour with the seal's *occupancy* removed while keeping its *conclusion*, and it is RED on
   `INV_RECOVERY`. Removing the straggler from that same config (`LatePred = FALSE`, run as a
   diagnostic) turns it GREEN again — so the red is caused by the straggler and cured by the seal,
   with nothing else varying.
3. **Every invariant has been seen red.** Six properties, six sabotages that break exactly one
   apiece, plus a reachability witness. No property in this model is green for free.
4. **The late PUT reaches no state the honest protocol does not.** `_v9_safe` and
   `_v9_flip_latepred` have *identical* distinct-state counts — 1,701,470 at `MaxSeq = 4` and
   32,925,718 at `MaxSeq = 5` — while `LatePredecessorPut` contributes 105,366 extra transitions.
   The straggler is no longer a distinct hazard; it is the ordinary adoption path arriving early.

## Summary table

| cfg | expected | TLC verdict | states (gen / distinct) | depth | s |
|---|---|---|---|---|---|
| `_v9_safe` | green | **PASS** — `No error has been found` | 7,949,790 / 1,701,470 | 48 | 4 |
| `_v9_flip_latepred` | green | **PASS** — `No error has been found` | 8,055,156 / 1,701,470 | 48 | 3 |
| `_v9_safe_deep` (`MaxSeq = 5`) | green | **PASS** — `No error has been found` | 177,992,259 / 32,925,718 | 61 | 43 |
| `_v9_flip_latepred_deep` (`MaxSeq = 5`) | green | **PASS** — `No error has been found` | 180,199,737 / 32,925,718 | 61 | 41 |
| `_sab_reuseafterambiguous` | violation | **RED as required** — `INV_NO_PHANTOM` | 575 / 322 | 10 | 0 |
| `_sab_gaponfail` | violation | **RED as required** — `INV_DENSE` | 657 / 391 | 10 | 1 |
| `_sab_noseal` | violation | **RED as required** — `INV_RECOVERY` | 1,226 / 583 | 12 | 1 |
| `_sab_blindput` | violation | **RED as required** — `INV_NO_GHOST` | 2,738 / 1,104 | 12 | 0 |
| `_sab_scanistruth` | violation | **RED as required** — `INV_RECOVERY` | 4,385 / 1,889 | 12 | 1 |
| `_sab_cleanupaboveckpt` | violation | **RED as required** — `INV_RECOVERY` | 2,448 / 1,072 | 12 | 1 |
| `_sab_staleckptcorruption` | violation | **RED as required** — `INV_NOFAIL` | 3,845 / 1,731 | 13 | 0 |
| `_witness_hintlie` | violation = evidence | **VIOLATED as required** — `W_NO_HINT_HOLE` | 9,261 / 3,739 | 14 | 1 |

Sabotage state counts are "explored before the violation was found" and vary a little between runs
with parallel workers; the verdict does not.

## The properties

```
INV_RECOVERY   == (rPhase = "done") => (Reconstruct = WState)
INV_NOFAIL     == rPhase # "corrupt"
INV_DENSE      == \A i \in (writtenEver \cup sealedIds) :
                      \A j \in 1..i : j \in (writtenEver \cup sealedIds) \/ j = pendingSlot
INV_NO_GHOST   == \A i \in writtenEver : i \notin sealedIds
INV_NO_PHANTOM == ~ phantomEver
```

`WState` is `Replay(writtenEver)` — the oracle over everything ever durably appended, which never
shrinks when cleanup deletes objects. `Reconstruct` is what recovery installs: the frozen body of
the `_ckpt` base snapshot folded with the ids the arithmetic walk proved present above it.

## The counterexamples, one per sabotage

Each row is the exact action sequence TLC reported.

**`_sab_reuseafterambiguous` → `INV_NO_PHANTOM`** (INV-1, the every-attempt rule).
`WAppendStart` → `WAttemptAmbiguous` → `WResolveConclusiveReject` → `WOrphanLands`.
The lane frees an id whose attempt had an unknown outcome instead of staying wedged, and reports
the operation as failed. The attempt's bytes are still in flight; when they reach the store, an
operation whose caller was told it FAILED is durable in the history. Note what the store still
prevents: the conditional create keeps the orphan out of any slot that has been taken, so this is
not a density or a recovery defect — it is a broken promise to the caller, which is exactly the
damage the every-attempt rule exists to prevent.

**`_sab_gaponfail` → `INV_DENSE`** (INV-1, contiguity).
`WAppendStart` → `WResolveConclusiveReject` → `WAppendStart` → `WResolveDurable`.
The rev.4 allocator: a definitely-failed conditional create burns its id and the writer moves on,
leaving what rev.4 called "a safe id gap". It is not safe once absence is the walk's terminator —
recovery reads a 404 at the gap, concludes it has reached the frontier, seals there and installs a
state missing every record above the gap. This is the spec's stated root cause, *absence is
undecidable in a sparse id space*, as a four-step counterexample. `INV_RECOVERY` breaks here too;
`INV_DENSE` is reported because TLC reaches the density violation first.

**`_sab_noseal` → `INV_RECOVERY`** (INV-2, slot occupancy — the flip's control).
`WAppendStart` → `ReaderStart` → `RReadCkpt` → `RFetchBase` → `RSealSlot` → `LatePredecessorPut`.
Recovery still concludes the frontier from the absent expected-next id but does not occupy the
slot. The drained lane's PUT then lands in a slot a completed recovery already declared final, and
the installed state is missing it — the rev.4 counterexample, reproduced. **Diagnostic control (not
a committed config):** the same config with `LatePred = FALSE` is `No error has been found`.

**`_sab_blindput` → `INV_NO_GHOST`** (INV-2, "the store's conditional create is the fence").
`WAppendStart` → `ReaderStart` → `RReadCkpt` → `RFetchBase` → `RSealSlot` → `LatePredecessorPut`.
Same trace shape, different mechanism: here the seal *is* placed, and the lane overwrites it with
an unconditional PUT. This pins an implementation requirement that is easy to lose in review — every
ref-log write is a create-if-absent, never a put. This config deliberately checks only
`TypeOK INV_NO_GHOST`: `INV_RECOVERY` also breaks and is checked first, which would otherwise hide
the property the config exists to falsify.

**`_sab_scanistruth` → `INV_RECOVERY`** (§4/§5, "LIST is a zero-trust hint").
`WAppendStart` → `WAttemptAmbiguous` → `WResolveDurable` → `ReaderStart` → `WriterPublishSnapshot`
→ `RReadCkpt` → `WriterCkptAdvance` → `GcCleanupLog` → `RFetchBase` → `RScanInstall`.
The reader folds what the enumeration returned instead of walking the tail by arithmetic. TLC's
shortest counterexample is the *other* half of that mistake: the hint reader has no vanish
detection, so when cleanup legitimately advances `_ckpt` and deletes a covered log between the
reader's `_ckpt` sample and its fetch, the arithmetic walk restarts (`RWalkVanish`) while the hint
reader silently folds a short history. The hidden-hole half is pinned separately, below, so neither
half rests on the other.

**`_sab_cleanupaboveckpt` → `INV_RECOVERY`** (INV-4, the log-deletion gate).
`WAppendStart` → `WAttemptAmbiguous` → `ReaderStart` → `RReadCkpt` → `RFetchBase` →
`RAdoptStraggler` → `GcCleanupLog` → `RSealSlot`.
Cleanup deletes a log the checkpoint does not cover. Recovery's walk reads a 404 at a DURABLE id
under an unchanged `_ckpt` token, cannot distinguish it from the frontier, and seals there —
including, in this trace, over an id it had adopted moments earlier.

**`_sab_staleckptcorruption` → `INV_NOFAIL`** (INV-4, the snapshot-deletion gate).
`WAppendStart` → `WAttemptAmbiguous` → `WResolveDurable` → `ReaderStart` →
`WriterPublishSnapshot` → `WriterCkptAdvance` → `GcCleanupSnap` → `RReadCkpt` → `RFetchBase`.
Snapshot cleanup races the checkpoint away — it deletes the snapshot `ckpt.base` still points at
instead of only snapshots strictly below it. The reader's exact-key GET 404s, the `_ckpt` reread
shows an unchanged token, and the three-way revalidation is then obliged to declare corruption.
This is the config that proves the *strictly below* in INV-4 is doing work: with the gate honest,
`corrupt` is unreachable, because a stale pointer can only under-clean.

**`_witness_hintlie` → `W_NO_HINT_HOLE`** (reachability witness; the violation is the evidence).
`WAppendStart` → `WResolveDurable` → `WAppendStart` → `WResolveDurable` → `ReaderStart` →
`RReadCkpt` → `RFetchBase` → `RScanStep` → `RScanInstall`, reaching
`logs = {1,2}`, `rScanPos = 2`, `rSeenLogs = {2}`, `rTail = {}`.
The enumeration ran to the *end* of the key space — nothing remained after its cursor, so it looked
complete — and still omitted log 1, which was present the whole time and which nothing ever
deleted. That is the observed `0x1430c`/`0x1430d` shape
(`reports/2026-07-26-list-incompleteness-investigation.md`) reproduced formally, and it is why
recovery may not consume a listing as truth.

## Non-vacuity of the green runs

Action coverage (`-coverage 1`) on `_v9_safe`, as `distinct-states-found : transitions`:

| action | `_v9_safe` | `_v9_flip_latepred` |
|---|---|---|
| `RSealSlot` | 267,079 : 284,871 | 267,532 : 284,871 |
| `RWalkStep` | 287,362 : 337,228 | 291,713 : 337,228 |
| `RWalkVanish` (restart on a covered deletion) | 127,935 : 159,922 | 127,698 : 159,922 |
| `RAdoptStraggler` (slot-occupy returned Occupied) | 3,429 : 25,638 | 0 : 25,638 |
| `LatePredecessorPut` | **0 : 0** (disabled) | **3,567 : 105,366** |
| `WResolveSealRejected` (wedged lane meets the seal) | 8,238 : 204,776 | 8,191 : 204,776 |
| `WAttemptAmbiguous` | 1,437 : 158,270 | 1,630 : 158,270 |
| `ReaderReset` (a second mount walks across the seal) | 55,049 : 300,480 | 55,091 : 300,480 |
| `WOrphanLands`, `RScanStep`, `RScanInstall` | **0 : 0** (sabotage-only, correctly unreachable) | 0 : 0 |

So the greens exercise the whole protocol: recovery completes 267k times, the Occupied branch of
`slot-occupy` fires, the vanish-restart path fires, the wedged lane meets a successor's seal, and
second mounts walk across seals. In `_v9_flip_latepred`, `RAdoptStraggler` finds no *new* states
only because `LatePredecessorPut` is explored first and lands in the same post-states — its 25,638
transitions still fire.

## Scoping — what this model deliberately does not cover

- **One namespace, one incarnation.** The catalog, ref-layer incarnations, namespace rebirth and
  rev.4's durable `Completed` marker are gone from this module; ops reduce to
  `birth`/`mut`/`remove`. Namespace lifecycle is `CaRefCatalogCore`'s and
  `CaRefNsCleanupStaleLeaderCore`'s subject (plan tasks 3 and 4).
- **The LIST hint is not modelled on the honest path**, because v9 consumes it nowhere: recovery is
  a point read plus arithmetic. It exists only as the omission-capable enumeration reachable under
  `SabotageScanIsTruth`/`_witness_hintlie`.
- **Ordinary appends are frozen while a reader recovers** (`ReaderInactive`), because recovery is a
  fresh mount with the previous writer's lane drained. What the drained lane can still do — land
  its in-flight PUT — is modelled, as `LatePredecessorPut` and `RAdoptStraggler`.
- **Seals are never cleaned** (they are tiny, and a walk crossing a dead epoch must find them).
- **`INV_NO_PHANTOM` is a ghost-flag property** in the house style of rev.4's `INV_RECREATE`: the
  flag is set by the orphan's *landing*, not by the sabotage action itself, so the counterexample is
  a real four-step behaviour rather than a tautology. It is still the weakest of the six, because
  the model gives the writer perfect knowledge of `writtenEver`; a writer-local view would be needed
  to model the byte-comparison branch of the wedge, and that belongs with `CaCasMountCore`'s
  wedge-retry work (plan task 5).
- **Bounds.** `MaxSeq = 4` / `MaxRestarts = 2` as the plan requires, plus the two `_deep` configs at
  `MaxSeq = 5`. `ReaderStart` additionally requires `NextSlot <= MaxSeq + 1` so the frontier seal
  always fits inside the modelled id space; that is a bound, not a protocol rule.

## History — the rev.4 and rev.6 configs this replaces

Nine configs were deleted in this rewrite. Their verdicts are preserved here because several of
them are the reason v9 exists.

| deleted cfg | what it proved | why it is gone |
|---|---|---|
| `_safe` | rev.4 honest protocol green: ordered scan, pick-greatest-snapshot, cleanup gated on an OBSERVED durable snapshot | superseded by `_v9_safe`; the scan-based recovery it validated is the thing v9 removes |
| `_latepred` | **RED on `INV_RECOVERY`** — the documented Phase-1 late-predecessor limitation, retained as expected-fail with an explicit "do not strengthen the model to make this green" | superseded by `_v9_flip_latepred`, which **is** green. The limitation is closed by spec §2 INV-1 + INV-2, not by assuming a mount fence can cancel an in-flight S3 request. `_sab_noseal` keeps the old counterexample runnable |
| `_sab_deletebeforesnapshot` | RED on `INV_RECOVERY` — cleanup deleting a log with no covering durable snapshot | the covering rule is now `_ckpt`-based, so the successor is `_sab_cleanupaboveckpt` |
| `_sab_vanishiscorruption` | RED on `INV_NOFAIL` — a vanished selected object treated as corruption instead of a restart | v9 replaces the two-way vanish rule with INV-4's three-way revalidation, whose gate is proven by `_sab_staleckptcorruption`; `INV_NOFAIL` survives under that config |
| `_sab_recreatebeforecompleted` | RED on `INV_RECREATE` — recreation over Removed without the durable `Completed` marker | namespace rebirth left this module with the single-incarnation scope; the property moves to `CaRefCatalogCore` (incarnations make debris inert, so there is no `Completed` marker to wait on) |
| `_sab_remountkeepsoldepoch` | RED on `INV_RECOVERY` — a self-remount stamping fresh appends at the old, below-durable epoch | unrepresentable under INV-1: ids are derived from state, so a remount's append is arithmetically at the frontier. What used to need a sabotage is now a type-level impossibility |
| `_rev6_safe`, `_rev6_latedelivery`, `_rev6_freshreader` | the rev.6 coverage-at-birth seal: the late PUT still lands but is folded out of a ghost oracle (`droppedEver`), with `INV_FRESH_READER` and `INV_SNAP_DETERMINISTIC` as the containment | v9 does not need a ghost oracle. The straggler is either adopted or fenced by a real object in the store, so the reader's oracle is plain `Replay(writtenEver)` again and `droppedEver` is deleted |

The through-line: rev.4 could only *contain* the late predecessor (accept it, then fold it out of a
contract-clean oracle). v9 makes it unrepresentable — contiguity denies it a slot below the
frontier, and the `slot-occupy` conditional create denies it the frontier slot itself.

## Reproduce

```bash
bash docs/superpowers/models/run_refsnaplog.sh          # 12 configs, ~96 s, exits nonzero on surprise
```

Logs land in `tmp/tlc_CaRefTableSnapshotLogCore_<cfg>.log`.
