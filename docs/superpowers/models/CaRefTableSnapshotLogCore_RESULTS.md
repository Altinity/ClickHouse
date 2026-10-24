# CaRefTableSnapshotLogCore — TLA+ gate results (v9)

Model: `CaRefTableSnapshotLogCore.tla`. Gates the writer and recovery core of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-1** (per-namespace contiguous
ids, the every-attempt rule), **INV-2** (every epoch transition closed in-band by a `slot-occupy`
seal), **INV-4** (`_ckpt` gating of deletion and of the recovery base) and §4 Recovery. Task 1 of
the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0 gate — it blocks the C++ work.

Runner: `./run_refsnaplog.sh` (runs every config and checks its expected verdict, including *which*
invariant a sabotage is required to break; sabotages run FIRST, because a green is only evidence
once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers auto`, 32 workers. Every number below is real TLC output from the
run of 2026-07-28, not an estimate. Whole harness: **129 s, 15/15 expectations met.**

## Task 5b exact committed-frontier extension {#task-5b-exact-committed-frontier-extension}

The 2026-08-02 extension adds the distinct runtime states `LogDurable`, `FrontierDurable`,
`Installed`, and `Acknowledged`. The exact `_ckpt.committed_through` frontier is the only durable
authority for install, acknowledgement, successor allocation, snapshot publication, and seal
publication. Immutable object producer fences are deliberately separate from checkpoint-CAS
admission fences: after a remount, the current writer may checkpoint exactly one valid old-fence
successor at `frontier + 1`. Every checkpoint CAS binds an exact sampled token, so an intervening
checkpoint update invalidates it; a request already issued under the then-current fence may still
linearize after a fence move.

Recovery after an ambiguous response performs an explicit exact `_ckpt` read and validates both
the sampled token and exact chain before installing. Seal publication is two actions: a
conditional-create of the immutable seal, followed by a same-contribution checkpoint CAS that
atomically advances `lastEpochSeal` and `committedThrough`. Both delivered and lost seal-CAS
responses are covered. The honest model also publishes snapshot bodies before checkpointing them,
and neither snapshots nor seals may name a value above the frontier.

The focused official gate was:

```bash
TLC_JAR=../../../tmp/tla2tools-official.jar \
  REFSNAPLOG_FOCUSED=1 TLC_WORKERS=1 ./run_refsnaplog.sh
```

The pinned checker SHA-256 is
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`. The runner's mandatory
positive temporal smoke was green (2 generated / 1 distinct, depth 1), and all 19 focused
expectations were met:

| cfg | expected result | states (gen / distinct) | depth | s |
|---|---|---:|---:|---:|
| `_frontier_sab_ackbefore` | `INV_ACK_NOT_BEFORE_FRONTIER` violated | 14 / 11 | 4 | 2 |
| `_frontier_sab_nextbefore` | `INV_NEXT_ID_NOT_BEFORE_FRONTIER` violated | 14 / 11 | 4 | 2 |
| `_frontier_sab_installabove` | `INV_INSTALL_NOT_ABOVE_FRONTIER` violated | 14 / 11 | 4 | 1 |
| `_frontier_sab_staleadvance` | `INV_EXACT_COMMITTED_FRONTIER` violated | 18 / 13 | 4 | 2 |
| `_frontier_sab_snapshotabove` | `INV_SNAPSHOT_NOT_ABOVE_FRONTIER` violated | 4 / 4 | 2 | 2 |
| `_frontier_sab_sealabove` | `INV_SEAL_NOT_ABOVE_FRONTIER` violated | 4 / 4 | 2 | 1 |
| `_frontier_witness_crash_prepared` | `W_CRASH_PREPARED` violated as evidence | 7 / 6 | 3 | 2 |
| `_frontier_witness_crash_logdurable` | `W_CRASH_LOG_DURABLE` violated as evidence | 13 / 10 | 4 | 1 |
| `_frontier_witness_crash_frontierdurable` | `W_CRASH_FRONTIER_DURABLE` violated as evidence | 53 / 32 | 6 | 2 |
| `_frontier_witness_crash_installed` | `W_CRASH_INSTALLED` violated as evidence | 93 / 55 | 7 | 2 |
| `_frontier_witness_lostresponse` | exact-read lost-response recovery reached | 888 / 412 | 11 | 1 |
| `_frontier_witness_exactsuccessor` | new-fence adoption of exact old-fence successor reached | 290 / 154 | 9 | 2 |
| `_frontier_witness_oldwriterseal` | old writer's log loses to successor seal | 124 / 71 | 7 | 2 |
| `_frontier_witness_issuedlinearizes` | already-issued CAS linearizes after fence move | 48 / 28 | 6 | 1 |
| `_frontier_witness_snapshot` | honest snapshot publication reached | 103 / 59 | 7 | 2 |
| `_frontier_witness_seal` | delivered seal publication reached | 123 / 70 | 7 | 2 |
| `_frontier_witness_lostseal` | lost seal-CAS response resolved by exact read | 250 / 134 | 8 | 1 |
| `_frontier_safe` | **GREEN** | 103,335 / 35,166 | 36 | 3 |
| `_v9_safe` (unchanged legacy control) | **GREEN** | 7,949,790 / 1,701,470 | 48 | 70 |

The earlier full-suite attempt also completed the unchanged `_v9_safe_deep` control green at
177,992,259 generated / 32,925,718 distinct states, depth 61, in 325 s. The following
`_v9_flip_latepred_deep` run was stopped for resource pressure after 118 s at depth 28, with
36,641,133 generated / 7,959,696 distinct / 2,023,004 queued states. It is **incomplete, not a
green result**; the completed 2026-07-28 baseline for that same legacy config remains recorded in
the historical table below. Per the focused review gate, the expensive deep run was not repeated.

Constants unless the row says otherwise: `MaxSeq = 4`, `MaxRestarts = 2`.

## Headline

1. **`LatePredecessorPut` is flipped from counterexample to proof.** Under rev.4 it was the
   documented expected-fail — a fenced predecessor's PUT landing below an already-published
   snapshot, losing acked data. Under v9 the identical adversary is **GREEN**, and the flip is
   bought by two independent v9 invariants, each of which is shown to be individually load-bearing:
   INV-1 contiguity means a straggler is always at the frontier and can never land below a
   published snapshot at all, and INV-2's slot occupancy decides the frontier slot — adopt it if
   the PUT landed, fence it forever if it did not.
2. **The flip is a controlled experiment, not an assumption**, and all three arms are committed
   configs so the claim stays executable:

   | config | straggler | seal occupancy | verdict |
   |---|---|---|---|
   | `_v9_flip_latepred` | ON | ON | GREEN |
   | `_sab_noseal` | ON | OFF | RED (`INV_RECOVERY`) |
   | `_sab_noseal_nolate` | OFF | OFF | GREEN |

   Exactly one variable moves between adjacent arms, so the red is caused by the straggler and
   cured by the seal's occupancy — neither verdict can be explained by some third difference.
   (`_sab_noseal_nolate` is the most expensive config in the harness at 26.1 M distinct states and
   32 s: with no seal, no id is ever consumed by one, so the writer and successive mounts keep
   finding room to interleave.)
3. **Every invariant has been seen red.** Five safety properties plus `TypeOK`, and nine sabotage
   configs between them — seven breaking exactly one property apiece, two more pinning the second
   consequence of a sabotage whose first consequence would mask it — plus a reachability witness and
   a green control. No property in this model is green for free.
4. **A skipped `_ckpt` merge loses data silently, not loudly.** `_sab_sealclobbersbase` was
   expected to end in the fail-closed corruption stop. It does — but that is not its *shortest*
   consequence. TLC first finds a behaviour where the sealer's stale write-back regresses the base
   to 0 ("no checkpoint") after the covered prefix has been legitimately cleaned, so the next mount
   reconstructs an EMPTY namespace over live data and reports success (`INV_RECOVERY`). Both
   branches are pinned, by `_sab_sealclobbersbase` and `_sab_sealclobbersbase_nofail`.
5. **The late PUT reaches no state the honest protocol does not.** `_v9_safe` and
   `_v9_flip_latepred` have *identical* distinct-state counts — 1,701,470 at `MaxSeq = 4` and
   32,925,718 at `MaxSeq = 5` — while `LatePredecessorPut` contributes 105,366 extra transitions.
   The straggler is no longer a distinct hazard; it is the ordinary adoption path arriving early.

## Summary table

| cfg | expected | TLC verdict | states (gen / distinct) | depth | s |
|---|---|---|---|---|---|
| `_sab_reuseafterambiguous` | violation | **RED as required** — `INV_NO_PHANTOM` | 780 / 430 | 11 | 0 |
| `_sab_gaponfail` | violation | **RED as required** — `INV_DENSE` | 470 / 282 | 10 | 1 |
| `_sab_noseal` | violation | **RED as required** — `INV_RECOVERY` | 1,210 / 577 | 11 | 1 |
| `_sab_blindput` | violation | **RED as required** — `INV_NO_GHOST` | 1,635 / 721 | 13 | 0 |
| `_sab_scanistruth` | violation | **RED as required** — `INV_RECOVERY` | 6,440 / 2,688 | 13 | 1 |
| `_sab_cleanupaboveckpt` | violation | **RED as required** — `INV_RECOVERY` | 1,862 / 891 | 11 | 1 |
| `_sab_staleckptcorruption` | violation | **RED as required** — `INV_NOFAIL` | 4,452 / 1,965 | 13 | 0 |
| `_sab_sealclobbersbase` | violation | **RED as required** — `INV_RECOVERY` | 111,113 / 38,038 | 17 | 1 |
| `_sab_sealclobbersbase_nofail` | violation | **RED as required** — `INV_NOFAIL` | 912,773 / 262,523 | 22 | 2 |
| `_witness_hintlie` | violation = evidence | **VIOLATED as required** — `W_NO_HINT_HOLE` | 9,457 / 3,821 | 14 | 0 |
| `_sab_noseal_nolate` (the flip's control) | green | **PASS** — `No error has been found` | 129,363,412 / 26,118,591 | 72 | 32 |
| `_v9_safe` | green | **PASS** — `No error has been found` | 7,949,790 / 1,701,470 | 48 | 3 |
| `_v9_flip_latepred` | green | **PASS** — `No error has been found` | 8,055,156 / 1,701,470 | 48 | 3 |
| `_v9_safe_deep` (`MaxSeq = 5`) | green | **PASS** — `No error has been found` | 177,992,259 / 32,925,718 | 61 | 42 |
| `_v9_flip_latepred_deep` (`MaxSeq = 5`) | green | **PASS** — `No error has been found` | 180,199,737 / 32,925,718 | 61 | 42 |

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

**`_sab_sealclobbersbase` → `INV_RECOVERY`** (INV-4, the `_ckpt` semantic-max merge).
`WAppendStart` → `WAttemptAmbiguous` → `ReaderStart` → `RReadCkpt` → `RFetchBase` →
`RAdoptStraggler` → `WriterPublishSnapshot` → `WriterCkptAdvance` → `RWalkStep` → `GcCleanupLog` →
`RSealSlot` → `ReaderReset` → `ReaderStart` → `RReadCkpt` → `RFetchBase` → `RSealSlot`,
final state `ckpt = [base |-> 0, seal |-> 1]`, `writtenEver = {1}`, `logs = {}`, `rTail = {}`,
`rPhase = "done"`.
The sealer read `_ckpt` at the start of its recovery (`rBase = 0`), a concurrent publisher advanced
the base to 1, cleanup legitimately deleted the now-covered log 1, and then the sealer wrote its
sampled body back verbatim — regressing the base to 0, which means *no checkpoint at all*. The next
mount finds no base to read, walks from 0, finds slot 1 absent because it was legitimately cleaned
under a base that no longer exists, seals there and **reports success with an empty namespace over
live data.** This is the finding worth carrying into the implementation: skipping the merge does not
fail closed, it loses data quietly.

**`_sab_sealclobbersbase_nofail` → `INV_NOFAIL`** (the same sabotage, second consequence).
… → `WriterCkptAdvance` → `GcCleanupSnap` → `RWalkStep` → `RSealSlot` → `ReaderReset` →
`ReaderStart` → `RReadCkpt` → `RFetchBase`, final state `ckpt = [base |-> 1, seal |-> 3]`,
`snaps = {2}`, `publishedEver = {1,2}`, `rBase = 1`, `rPhase = "corrupt"`.
Here the regressed base still names a real snapshot id — but one that snapshot-cleanup deleted while
the base was 2, which was legal precisely because deletion is gated strictly below the *then-current*
base. The next mount samples 1, its exact-key GET 404s, the `_ckpt` reread shows an unchanged token,
and the three-way revalidation is obliged to declare corruption. This is the branch INV-4's merge
obligation was written for; it is checked in its own narrowed config because `INV_RECOVERY` breaks
first and shallower.

**`_witness_hintlie` → `W_NO_HINT_HOLE`** (reachability witness; the violation is the evidence).
`WAppendStart` → `WResolveDurable` → `WAppendStart` → `WResolveDurable` → `ReaderStart` →
`RReadCkpt` → `RFetchBase` → `RScanStep` → `RScanInstall`, whose final state is
`logs = {1,2}`, `writtenEver = {1,2}`, `rScanPos = 2`, `rSeenLogs = {2}`, `rBase = 0`,
`rTail = {2}`, `rPhase = "done"`.
The enumeration ran to the *end* of the key space — nothing remained after its cursor, so it looked
complete — and still omitted log 1, which was present the whole time and which nothing ever
deleted. The reader installed the fold of `{2}` alone over a two-record history. That is the observed `0x1430c`/`0x1430d` shape
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

Re-measured after the `_ckpt` merge was introduced: every *transition* count above is byte-identical
to the run before it (25,638 / 300,480 / 284,871 / 337,228 / 159,922 / 204,776 / 305,342), and
`_v9_safe` still generates 7,949,790 / 1,701,470. Replacing the field-surgical `EXCEPT` with the
read-modify-write merge changed the honest state space by exactly nothing — which is the point: it
adds no honest behaviour, it only makes the dishonest one expressible.

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
- **Concurrent recoverers are unrepresentable here, so one INV-2 branch is never exercised.** The
  model has a single reader phase (`rPhase`), so there is at most one recovery in flight at a time.
  The consequence is specific: `slot-occupy` returning `Occupied` **with an `EpochSeal` in it** — a
  second recoverer arriving at a frontier a first recoverer already closed, which INV-2 says
  terminates the walk — cannot occur. What IS exercised is `Occupied` with a *straggler's record*
  (`RAdoptStraggler`, 25,638 transitions) and a later mount consuming an *older* epoch's seal
  mid-walk (`RWalkStep`, via `ReaderReset`). The missing branch is two recoverers racing for the
  same slot, and it needs a model with per-actor recovery state. **Hand-off: `CaCasMountCore`
  (task 5)**, which already models recovery generations and is where "an old-generation wedge or
  recovery result returning after the successor sealed the slot is refused by a generation recheck"
  (spec §9) belongs. This sits alongside the `INV_NO_PHANTOM` hand-off above; both are task 5's.
- **Spec gap 2 — the seal grammar is not modelled and is a C++-test obligation.** INV-2 requires
  that a seal transaction contain exactly one seal operation, and that `prev_epoch_seal` be present
  on exactly sequence 1 of every non-genesis epoch and forbidden everywhere else. This module treats
  a seal as an opaque slot occupant that folds as a no-op, so neither rule is expressible here and
  neither is checked. They are structural encoding rules, best discharged by codec round-trip and
  rejection tests in the implementation plan, not by TLC.
- **`ckpt.seal` carries no proof obligation in this module, deliberately.** Its consumers — a later
  mount locating the previous epoch's terminating record, and the GC fold crossing epochs (§5) — do
  not exist in a single-recoverer, single-namespace model, so any invariant over it here would be
  green by construction and unfalsifiable, which the house rule forbids. It is still merged by
  semantic maximum and typed, so `CaCasMountCore` (task 5) and `CaRefDeltaIntakeCore` (task 2)
  inherit a field that behaves correctly. The reason is recorded at its declaration in the module.
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
bash docs/superpowers/models/run_refsnaplog.sh          # 15 configs, ~129 s, exits nonzero on surprise
```

Logs land in `tmp/tlc_CaRefTableSnapshotLogCore_<cfg>.log`.
