# A publisher clears GC's marker on its own fresh incarnation: findings

- Code authority: `altinity/antalya-26.6` @ `e2dd0f5d343`, worktree `/home/mfilimonov/workspace/ClickHouse/cas-30`.
- Paths are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`.
- Model, cfgs, runner (`run.sh`, 8 workers, `nice -n 10`, 20-minute cap), logs and traces are in this directory.
- Nothing in `src/` was changed, built or run.

**The four GC changes assumed given.** "The four changes" means all of them; "the CAS-30 three" means C1 to C3.

| # | Change | Where |
|---|---|---|
| C1 | the meta-delete job deletes only `Condemned` at a round ≤ its captured round | `Gc/CasGcMetaWriter.cpp:86-92` |
| C2 | `writeCondemnedMeta` re-stamps an older-round `Condemned` | `:70-80` |
| C3 | the graduation re-read accepts only `Condemned` at a round ≥ the entry's | `Gc/CasGc.cpp:2091` |
| C4 | the in-process confirmation records its round; graduation requires it to be ≥ `entry.condemn_round`, for both producers (`Gc/CasGcMetaWriter.cpp:155-156`, `Gc/CasGc.cpp:2093`) | from the codex review of the CAS-30 spec |

## 1. Verdict and the reachable schedule {#verdict}

**Reachable, and it loses data.** A committed part names a deleted blob. The four GC changes do not close it.

**Schedule A: absent body, the reviewer's exact schedule.** Model `rv_bug`, trace read.

1. **Transaction U precommits X and abandons without publishing X.**
   - `stageManifest`, then `precommitAdd`: the manifest names every blob before any upload (`ContentAddressedTransaction.cpp:429-440`, `Pool/CasPartWriteTxn.cpp:747-801`).
   - U fails before uploading X. `abandon` appends the exact precommit removal (`Pool/CasPartWriteTxn.cpp:1109-1149`).
2. **GC round `c` takes its cut and folds U's `+1` and `-1`.**
   - X has deltas (`cur_touched`) and zero surviving edges (`Gc/CasBlobInDegree.cpp:638-664`).
   - With no carried row, `closeBlob` calls `head_blob` (`:565-577`).
3. **Writer W precommits after GC has read W's namespace in round `c`.**
   - Intake reads each ref log up to its frontier (`Gc/CasGc.cpp:2170-2199`).
   - HEADs happen only later, in the merge (`:3449-3452`, `:3469-3538`).
   - W's HEAD finds the body absent, so it publishes T1 (`Pool/CasPartWriteTxn.cpp:402-403`, `:465-495`).
4. **GC's candidate HEAD observes T1 and condemns it.**
   - The read-ahead take is at `Gc/CasGc.cpp:2009-2010`. No check relates the token to the cut: `Meta` is `{size, etag}` (`Backend/CasWriteResult.h:29`).
   - The marker job is queued at once (`Gc/CasGc.cpp:2046`). `writeCondemnedMeta` finds no meta and `putMetaIfAbsent(Condemned(c))` commits (`Gc/CasGcMetaWriter.cpp:73-76`).
   - The registry records `(X, T1)`, which is `(X, T1, c)` under C4 (`:155-156`, `:204-208`).
5. **W's reconcile overwrites the marker.**
   - `create(Clean)` returns `Conflict` (`Pool/CasPartWriteTxn.cpp:351-355`, `:363`).
   - `readModifyWrite` writes `Clean` over any non-`Clean` meta (`:369-389`).
6. **W's edge vanishes before cut `c+1`.** W aborts after the upload: its own `abandon` follows the reconcile, because the fan-out waits for every task (`ContentAddressedTransaction.cpp:1780-1782`). A committed ref dropped within the interval also works. W's `+1` and `-1` fold together in `c+1`, so the row is touched at zero, `peek_head` sees T1, and it is settled, not superseded.
7. **Round `c+1` graduates T1.**
   - `confirm_condemned_marker` returns true on the in-process record without reading the meta (`Gc/CasGc.cpp:2087`, then `Gc/CasBlobInDegree.cpp:493-508`).
   - Under C4 the record's round `c` is ≥ the entry's `c`, so C4 does not stop it.
8. **W2 adopts, and the delete removes the blob.**
   - W2 precommits after cut `c+2`, HEADs T1, reads `Clean` and adopts (`Observed`, `Pool/CasPartWriteTxn.cpp:424-461`).
   - Round `c+2`'s redelete HEADs, matches T1 and deletes it by etag (`Gc/CasGc.cpp:419-421`).
   - W2's committed part names a deleted blob.

**Schedule B: resurrect over a spare-left `Condemned`, fresh condemn.** Model `nosupersede/pm_bug`, trace read.

- Spares never clear the meta (`Gc/CasGc.cpp:965-977`). When T1's last owner drops, `head_blob` fires.
- W HEADs T1, reads `Condemned(r1)`, publishes T2, then `replace`s by the etag it read (`Pool/CasPartWriteTxn.cpp:356-360`).
- **Today (F8):** GC finds `Condemned(r1)`, leaves it and confirms (`Gc/CasGcMetaWriter.cpp:79`). W's `replace` succeeds. GC only needs its GET before W's `replace`.
- **With C2:** GC re-stamps. W's `replace` conflicts, and `readModifyWrite` writes `Clean`.
- **Equal rounds:** a marker from an aborted attempt of the same round is left alone, and W's `replace` wins on identical bytes (S3 etag = content).

**Schedule C: supersede.** Model `pm_bug`, trace read.

- A carried row, touched at zero, whose current token differs: `peek_head` (`Gc/CasGc.cpp:2066-2073`, `Gc/CasBlobInDegree.cpp:541-559`).
- Its marker is queued only after the whole fold (`Gc/CasGc.cpp:997-1026`), so W must be late by the rest of the fold. Much less likely.

**Race width.** GC's HEAD must follow W's PUT, and GC's marker write (or, today, its confirming GET) must land before W's reconcile.

- On the `head_blob` path GC needs HEAD, GET and PUT; W needs one request. W must be about 2 RTT late: one slow or retried S3 request inside its frozen 90-second policy (`Pool/CasPartWriteTxn.cpp:282`, `Backend/CasRetry.h:41`).
- Other conditions:
  - a recurring content hash touched at zero in round `c`;
  - W's whole lifetime inside one GC interval;
  - a third writer of the same content inside round `c+2`'s cut-to-delete window.
- Rare, not measured. The outcome is silent data loss.

**Safe variants:**

- **Restart or leader change** to an empty registry between `c` and `c+1`: the re-read finds `Clean`, refuses, and rewrites `Condemned(c)` (`Gc/CasGc.cpp:2091-2112`). Adopters in between are spared by the next cut.
- **A reconcile that throws** leaves the marker in place.
- **Ambiguous-publication retries** re-HEAD. They either adopt T1 with a backfill that only creates an absent meta, or republish T3. The exact-token delete of T1 then mismatches.

**Codex finding 1 (returning leader) is confirmed in the code.** Model `f1_bug`, trace read.

- The registry is created once per `Gc` (`Gc/CasGc.cpp:345`) and nothing clears it on lease loss.
- A losing attempt's marker job still records its confirmation (`Gc/CasGcMetaWriter.cpp:155-156`).
- A supersede forgets only the old token (`Gc/CasGc.cpp:1026`).
- Model trace:
  1. A stale attempt at round 1 HEADs T2, writes or finds `Condemned(1)`, and records `(T2, 1)`.
  2. The live round condemns T2 at 4, and its marker write is lost.
  3. The older incarnation's round-1 job deletes `Condemned(1)`, which C1 allows.
  4. Graduation accepts the cached `(T2, 1)`.
  5. W2 adopts through the absent meta, and the delete removes T2.
- C4 closes it when publication does not race: `f1_fix` and `f1_fix_big` are green.

## 2. Model changes {#model}

File: `CaGcCondemnMarkerGate.tla`, built on the CAS-30 variant from `tmp/cas30-meta-model`.

**How to read it:**

- Every added constant is inert when `FALSE`, and every added variable then stays constant.
- The 22 CAS-30 cfgs keep their verdicts, and every green one keeps its exact distinct count (`final2_regress.txt`).
- The first version is `CaGcCondemnMarkerGate.v1.tla`, with results in `v1/`. It is the same model before `Registry`, `StaleAttempts` and `ExactAbsentPath` were added.

**Candidate condition (the code's, replacing the over-approximation):**

- `dirty`: set by `Seed`, `WPrecommit` and `WDrop`. `GCut` moves it into `touched`.
- `RealCandidate`: `GSettleCondemn` needs `touched`; `GSettleIdle` also covers the untouched case.
- `SupersedeDue`: a carried row (`condemned` or `pending`), `touched`, not folded, `body.tok /= ret.tok`. It reads `body` because `peek_head` is not read ahead. It pre-empts `GSettleGraduate`, `GSettleGraduateDeferred`, `GSettleCarryRetry`, `GSettleRedelete` and `GSettleSuppressed`.

**Writers:**

- `Respawn`, `WRespawn`: a dropped writer slot starts a new build, which supplies the third writer.
- `premeta`, `FirstWriteWins(w)`, `ReconcileResult(w)`: the reconcile's first conditional write.
  - On the absent path it is a `create`, which wins iff the meta is absent.
  - On the resurrect path it is a `replace`, which wins iff the meta equals the bytes read before publication (S3 etag = content, so the ABA is modelled).
  - A loss falls into `readModifyWrite`: `Clean` today, or leave `Condemned` under `WriterLeavesMarker` (W1).
- `ExternalDrop`: `WDrop` while the reconcile is owed, as a namespace removal does.
- `ExactAbsentPath`: `Seed` creates an empty key, and `WPublish` requires body and meta absent. This is schedule A exactly.

**GC:**

- `AsyncMarker`, `mjob`, `GMarkerJob`: the marker write lands after the HEAD. `GCut` waits for it (`meta_pool_wait` before commit, `Gc/CasGc.cpp:1098-1106`).
- `Condemn(t, r)`: a shared helper for fresh condemn and supersede, with success and lost branches.
- `SuppressRounds`, `GSettleSuppressed`: a round that carries the row without graduating or deleting (`suppress_destructive`, `Gc/CasGc.cpp:3361-3362`).

**The returning-leader confirmation (C4 and codex finding 1):**

- `Registry`, `reg`: the in-process confirmation as a set of `<<token, round>>` that outlives rounds, failed attempts and leadership changes.
  - `RegAdd` on marker success (`Condemn`, `GMarkerJob`, `GSettleCarryRetry`) and on the graduation re-read memo (`Gc/CasGc.cpp:2093`).
  - `RegForget` where the code forgets: spare, spare-loud, redelete, supersede of the old token.
- `RegRound` (C4): `InProc == \E p \in reg : p[1] = ret.tok /\ p[2] >= ret.cr`.
- `StaleAttempts`, `GStaleAttempt(r)`, `sa_done`: one uncommitted attempt of this process at round `r` HEADs the current token, writes `CondemnedAfterWrite(r)` and keeps `<<tok, r>>` in `reg`. This represents a confirmation that survives a failed attempt and a later reacquisition. Bounded to one per behaviour.
- `GRestart`: with `Registry`, it empties `reg` (a restart, or a new leader with nothing cached).

**Fix switches:**

| Switch | Candidate |
|---|---|
| `GateAlwaysReads` | G1: `GateOK` uses `MetaGuards` only |
| `WriterLeavesMarker` | W1 |
| `DeleteReads` | G2: a refusal drops the entry |
| `ForgetOnTouch` | G7: `GCut` clears the record of a touched condemned row |
| `GateTouched` | naive G7 |

**Invariants:**

- `NoDangle == \A w : committed[w] => body.present`.
- `MarkerPreservedPending == (ret.st = "pending" /\ body.present /\ body.tok = ret.tok) => Cond`: a delete-pending entry whose token is still current keeps a `Condemned` marker.
- `MarkerPreservedConfirmed == (ret.st = "condemned" /\ GateConfirmed /\ TokCurrent) => Cond`, where `GateConfirmed == ~GateAlwaysReads /\ EffConfirmed`: the gate would accept the entry on its in-process record, and the marker is `Condemned`.
- `WitNoDeleteAfterRefusal`: a reachability witness, expected to fail. It shows that a gate refusal over a writer's `Clean` is still followed by a delete.

## 3. Results {#results}

"Base" means `SplitPublish`, `RealCandidate`, `Respawn` and C1 to C3 on (plus C4 in battery 2). "All" means `AsyncMarker`, `Prefetch`, `PersistConf`, `ReconcileFault` and `StaleMarkers` (plus `StaleAttempts` in battery 2). `MaxGen 3`, `MaxRounds 7` unless noted. The seconds column is not comparable across batteries: the machine was shared, and battery 2 ran about 3× slower.

### Battery 1: before `Registry` (in-process confirmation = `ret.conf`) {#battery1}

| cfg | models | expect | result | distinct | s |
|---|---|---|---|---:|---:|
| `pm_bug` | base | violation | `NoDangle` violated (supersede path, trace read) | 81,904 | 0 |
| `pm_bug_async` | + AsyncMarker | violation | violated | 77,551 | 1 |
| `pm_bug_all` | + all | violation | violated | 323,244 | 1 |
| `pm_bug_supp` | + SuppressRounds | violation | violated | 82,024 | 1 |
| `pm_nosplit` | all, no split publish | green | green | 2,254,201 | 5 |
| `pm_overapprox` | old candidate rule | violation | violated | 55,382 | 1 |
| `nosupersede/pm_bug` | supersede disabled | violation | violated (schedule B, trace read) | 122,429 | 1 |
| `nosupersede/pm_bug_async` | same, Async | violation | violated | 119,193 | 0 |
| `absentonly` | supersede and resurrect disabled, `MaxRounds 8` | violation | violated (absent path over a leftover `Condemned`) | 59,953 | <5 |
| `pm_g1` | G1 | green | green | 200,073 | 1 |
| `pm_g1_async` | G1, Async | green | green | 239,061 | 1 |
| `pm_g1_all` | G1, all | green | green | 3,051,835 | 7 |
| `pm_g1_supp` | G1, all, Suppress | green | green | 3,051,835 | 7 |
| `pm_g1_overapprox` | G1, all, old candidate rule | green | green | 1,587,493 | 5 |
| `pm_g1_norestamp` | G1, all, no C2 | green | green | 3,096,056 | 6 |
| `pm_g1_nogateround` | G1, all, no C3 | violation | violated | 1,840,056 | 4 |
| `pm_g1_extdrop` | G1, ExternalDrop | violation | violated (the residual, trace read) | 53,604 | 1 |
| `pm_g1_wit` | G1, Async, witness | reachable | witness hit | 9,661 | 1 |
| `pm_g1_big` | G1, all, `MaxGen 4`, `MaxRounds 8` | green | green | 12,913,130 | 29 |
| `pm_g1_supp_big` | same + Suppress | green | green | 12,913,130 | 29 |
| `nosupersede/pm_g1_all` | G1, all, supersede disabled | green | green | 3,184,724 | 8 |
| `pm_g7` | G7 | green | green | 169,161 | 1 |
| `pm_g7_all` | G7, all, Suppress | green | green | 2,091,827 | 5 |
| `pm_g7_naive` | naive G7 | violation | violated | 113,956 | 1 |
| `pm_g7_extdrop` | G7, ExternalDrop | violation | violated | 47,670 | 1 |
| `pm_g7_wit` | G7 witness | reachable | hit | 9,579 | 1 |
| `pm_g7_big` | G7, all, `MaxGen 4`, `MaxRounds 8` | green | green | 8,550,660 | 20 |
| `nosupersede/pm_g7_all` | G7, supersede disabled | green | green | 2,161,403 | 5 |
| `pm_w1` | W1 | green | green | 564,972 | 1 |
| `pm_w1_async` | W1, Async, Prefetch, PersistConf, Fault | green | green | 1,416,918 | 3 |
| `pm_w1_stale` | W1, all | violation | violated (equal-round ABA, trace read) | 733,583 | 2 |
| `pm_w1_norestamp` | W1, no C2 | violation | violated | 127,290 | 0 |
| `pm_g2` | G2 | green | green | 197,235 | 1 |
| `pm_g2_all` | G2, all | violation | violated (a stale `Condemned` re-validates the delete, trace read) | 433,375 | 2 |
| `pm_g1g2_extdrop` | G1+G2, all, ExternalDrop | violation | violated | 483,759 | 1 |
| `pm_g1w1_extdrop` | G1+W1, ExternalDrop, no stale markers | green | green | 1,837,210 | 4 |
| `pm_g1w1_extdrop_stale` | same + StaleMarkers | violation | violated | 539,859 | 1 |
| `pm_g1w1_all` | G1+W1, all | green | green | 35,904,415 | 75 |

### Battery 2: C4 given; `Registry`, `RegRound` and `StaleAttempts` added {#battery2}

`final2_neg.txt`, `final2_pos.txt`, `final2_big.txt`.

| cfg | models | expect | result | distinct | s |
|---|---|---|---|---:|---:|
| `rv_bug` | schedule A (`ExactAbsentPath`), four changes | violation | `NoDangle` violated, trace = schedule A | 17,866 | 5 |
| `rv_bug_mp` | same | violation | `MarkerPreservedPending` violated | 3,177 | 4 |
| `rv_bug_async` | + Async | violation | `NoDangle` violated | 14,165 | 5 |
| `f1_bug` | finding 1: no C4, StaleAttempts, no split publish | violation | `NoDangle` violated, trace read | 1,333,161 | 31 |
| `f1_bug_mp` | same | violation | `MarkerPreservedPending` violated | 321,419 | 8 |
| `f1_fix` | four changes, all, Suppress, no split publish | green | green (`NoDangle` + both marker invariants) | 37,162,840 | 114 |
| `f1_fix_big` | same, `MaxGen 4`, `MaxRounds 8` | green | green | 241,031,093 | 726 |
| `pm_nosplit` | same cfg as `f1_fix` | green | green | 37,162,840 | 101 |
| `pm_bug` | base, four changes | violation | violated | 90,777 | 7 |
| `pm_bug_all` | + all | violation | violated | 1,641,510 | 45 |
| `pm_overapprox` | old candidate rule | violation | violated | 61,647 | 7 |
| `rv_g1` | G1, schedule-A path, all | green | green (all invariants) | 1,951,931 | 65 |
| `rv_g1_big` | same, Suppress, `MaxRounds 8` | green | green | 3,906,064 | 12 |
| `pm_g1` | G1 | green | green | 241,547 | 2 |
| `pm_g1_all` | G1, all, Suppress | green | green (all invariants) | 108,547,117 | 326 |
| `pm_g1_noreg` | same, C4 off | green | green | 108,547,117 | 306 |
| `pm_g1_overapprox` | G1, all, old candidate rule | green | green | 57,644,865 | 157 |
| `pm_g1_norestamp` | G1, all, no C2 | green | green | 79,572,691 | 207 |
| `pm_g1_nogateround` | G1, all, no C3 | violation | violated | 35,332,696 | 860 |
| `pm_g1_extdrop` | G1, ExternalDrop | violation | violated (residual) | 73,907 | 8 |
| `pm_g1_wit` | G1 witness | reachable | hit | 11,400 | 6 |
| `pm_g1_big` | G1, all, Suppress, `MaxGen 4`, `MaxRounds 8` | green | **not finished**: timeout at 1200 s, ≈228M distinct, no error | — | 1200 |
| `pm_g1_g4` | G1, all, Suppress, `MaxGen 4`, `MaxRounds 7` | green | **not finished**: stopped by the interruption at ≈270M distinct, 35.5M on queue, no error | — | ~900 |
| `pm_g1_r8` | G1, all, Suppress, `MaxRounds 8` | green | not run | — | — |
| `pm_g7_all` | G7, all, Suppress | green | **FAIL**: `MarkerPreservedPending` violated | 180,871 | 0 |
| `pm_g7_big` | same, bigger bounds | green | **FAIL**: same counterexample | 261,366 | 1 |
| `pm_g7_mpc` | G7 | violation | `MarkerPreservedConfirmed` violated | 5,511 | 5 |
| `pm_g7_naive` | naive G7 | violation | violated | 164,032 | 9 |
| `pm_w1` | W1, Async, Prefetch, PersistConf, Fault | green | green | 1,532,050 | 4 |
| `pm_w1_stale` | W1, all | violation | violated | 9,526,734 | 226 |
| `pm_g2_all` | G2, all | violation | violated | 1,890,991 | 60 |
| `pm_g1w1_extdrop` | G1+W1, ExternalDrop, StaleAttempts, no stale markers | green | **FAIL**: `MarkerPreservedPending` violated | 57,061 | 0 |
| `pm_g1w1_extdrop_stale` | same + StaleMarkers | violation | violated | 6,626,988 | 168 |

**CAS-30 regression, both batteries.** All 22 cfgs keep their verdicts, and the green ones keep their exact distinct counts: `atomic_regress` 20,802, `fix` 52,645, `fix_both` 104,271, `fix_stale` 719,379, `fix_stale_big` 2,591,915, `job_only` 40,197 and the rest (`final2_regress.txt`).

## 4. FAIL rows: the counterexamples {#fails}

### `pm_g7_all` and `pm_g7_big` (same trace): real in the code, and it falsifies G7 {#fail-g7}

1. A `Condemned(1)` exists on T1, left by a deposed leader or an aborted attempt of round 1 (`GStaleMarkerWrite`).
2. W precommits, HEADs T1, reads `Condemned(1)` and publishes T2 (resurrect path). Its reconcile is still owed.
3. This process's uncommitted attempt of round 1 HEADs T2. `writeCondemnedMeta(1)` finds `Condemned(1)`, leaves it (equal round, `Gc/CasGcMetaWriter.cpp:79` plus C2) and returns true. The registry now holds `(X, T2, 1)` (`:155-156`). The attempt loses its `gc/state` CAS; the registry survives (`Gc/CasGc.cpp:345`).
4. W's `replace` by the etag of `Condemned(1)` wins on identical bytes, so the meta is `Clean`. W drops.
5. The live round 1, the same process's retry, has X touched: a fresh `head_blob` condemns T2 at 1. Its marker job fails (throws or loses the CAS; `Gc/CasGcMetaWriter.cpp:110-126` swallows it).
6. **Why G7 misses it.** G7 forgets on a touch of a carried row. Here the touch was in the condemning round, before the row existed, so there was nothing to forget. The next round is untouched.
7. The gate accepts `(X, T2, 1)`: round 1 ≥ `entry.condemn_round` 1, so C4 passes. The entry graduates over `Clean`, and an adopter plus the delete give `NoDangle` (`MarkerPreservedPending` fires first).

**In code terms this needs:**

- an aborted attempt of round `r` that confirms T2 by finding an equal-round marker;
- W's `replace` of that same marker landing after it;
- the live retry's own marker write failing.

It is a real code schedule, narrower than schedule A. It shows that equal-round confirmations cannot be told apart. G1 is immune to it.

### `pm_g1w1_extdrop`: real in the code; the G1 residual in its W1-hardened form {#fail-g1w1}

1. An aborted attempt of round 1 leaves `Condemned(1)` on T1.
2. W HEADs T1, reads `Condemned(1)`, publishes T2, and its reconcile is owed.
3. A namespace removal lands W's `-1` while the reconcile is in flight (`Pool/CasMountRuntime.cpp:231-245`; `requireAlive` runs only at the loop head, `Pool/CasPartWriteTxn.cpp:395`).
4. The live round 1 condemns T2. Its marker job finds the equal-round `Condemned(1)` and leaves it.
5. Round 2 graduates by re-read: `Condemned(1)` ≥ 1.
6. W's delayed `replace` by the etag of `Condemned(1)` wins on identical bytes. W1 does not help, because it acts only on a lost first write. The meta is now `Clean` under a pending entry.

In battery 1 this cfg was green because the equal-round marker came only from `StaleMarkers`, which was off there. In battery 2 `StaleAttempts` supplies it. This is the same residual as `pm_g1w1_extdrop_stale`.

### `pm_g1_big`: a timeout, not a counterexample {#fail-g1big}

- `pm_g1_big` hit the 20-minute cap at about 228M distinct states with no violation.
- The reduced replacement `pm_g1_g4` (`MaxRounds 7`) was stopped by the interruption at about 270M distinct states with no violation.
- The G1 proof in battery 2 therefore rests on `pm_g1_all` (108.5M), `rv_g1_big` and the unfinished large runs. Battery 1's `pm_g1_big` (12.9M, before `Registry`) finished green.

## 5. Candidates {#candidates}

| Candidate | Code shape | Lines | Requests added | Model result | Verdict |
|---|---|---|---|---|---|
| **G1**: the gate always re-reads | delete `Gc/CasGc.cpp:2087-2088` (the `condemnMarkerConfirmedInProcess` shortcut); C3's round check stays. The registry then has no reader and can be removed (~30 lines, optional). | −2 | +1 meta GET per graduation. Today it is a serial `loadMeta` in the merge (N graduations ≈ N × RTT per round). A read-ahead needs a graduation candidate list that does not exist; `hintRead` at `Gc/CasGcReadAhead.h:48` is the only building block. | green on all invariants in every cfg except `ExternalDrop`: `pm_g1_all` 108.5M, `rv_g1_big`, `pm_g1_noreg`, `pm_g1_norestamp`, `pm_g1_overapprox`. Needs C3 (`pm_g1_nogateround` fails). Largest bounds unfinished. | **chosen** |
| **G7**: forget the in-process record on a touch | callback from `closeBlob` (touched, zero, not superseded) → `forgetCondemnMarker`; must also fire in suppressed rounds | ~10 + a parameter through `foldDeltasIntoGeneration` (3 call sites) | +1 GET per touched-at-zero carried row | battery 1 green; battery 2 **fails** (§4); the naive form fails too | rejected |
| **W1**: the writer leaves a `Condemned` it lost to | `readModifyWrite` lambda: `if (observed.state == MetaState::Condemned) return std::nullopt;` (`Pool/CasPartWriteTxn.cpp:378`) | ~2 | none; saves a PUT on a lost race; the next writer republishes the body once | green without stale markers; fails on the equal-round ABA (`pm_w1_stale`) and without C2 (`pm_w1_norestamp`) | unsound alone; narrows G1's residual but does not close it |
| **W2**: overwrite only a provably stale marker | the only free round is the pre-publication meta's `cr` (resurrect path); none on the absent path | ~4 | none | not modelled separately: its first `replace` has W1's ABA hole, and it overwrites more | dominated by W1 |
| **G2**: the redelete re-reads first | GET in `performRedeleteIo` before the HEAD; a refusal must drop the entry, because the run is already written (`Gc/CasBlobInDegree.cpp:482`), so it leaks | ~20 + a new outcome kind | +1 GET per delete, on workers | `pm_g2_all` fails: a stale `Condemned` written after the adopter read `Clean` re-validates the delete, and no later cut catches that adopter | unsound |
| **G3**: condemn only tokens published before the cut | HEAD gives only `{size, etag}`; publication time needs a body GET of the envelope provenance plus a clock comparison | — | +1 GET per condemn | — | not available for free |
| G1+W1 | both | ~0 | as G1 | green without an equal-round marker; fails with `StaleAttempts`/`StaleMarkers` plus `ExternalDrop` | optional hardening only |
| G1+G2 | both | ~18 | as G1 + G2 | fails (`pm_g1g2_extdrop`) | no gain |

## 6. Recommendation after battery 2 {#recommendation}

**G1.** Graduation always re-reads the meta and requires `Condemned` at a round ≥ the entry's (C3). The in-process shortcut goes away.

**Failure asymmetry:**

- Over-refusing costs one round of delay plus one marker PUT per refusal (`pm_g1_wit`: the entry is still deleted later).
- Under-refusing is silent data loss.

**Why it holds:**

- While the body is T, only T's publisher writes `Clean`. Any other publisher changes the token.
- The publisher's own `-1` follows its reconcile (fan-out wait, `ContentAddressedTransaction.cpp:1780-1782`). So the reconcile lands before the cut that makes the row zero, and the re-read sees it.
- Adopters that read `Clean` before the re-read have their edge in the delete round's cut and are spared.
- Stale markers written later cannot re-validate an entry that already graduated: that is G2's hole, not G1's.

**What G1 does not close: the `extdrop` residual** (`pm_g1_extdrop`, trace read):

1. W publishes T into X.
2. GC condemns T, and its marker lands.
3. `dropNamespace` for W's namespace makes its removal transaction durable. This is W's `-1`. Cancellation is signalled only afterwards (`Pool/CasMountRuntime.cpp:231-245`), and W's reconcile is still in flight: `requireAlive` runs only at the publication loop head (`Pool/CasPartWriteTxn.cpp:395`).
4. W's reconcile is delayed past the next graduation re-read. It retries inside the frozen 90-second policy (`Pool/CasPartWriteTxn.cpp:282`), which means more than one GC interval (60 s default).
5. The re-read sees `Condemned` and graduates.
6. W's reconcile lands `Clean`.
7. A writer in another namespace precommits after the delete round's cut and adopts T.
8. The delete removes T.

**What would close it:**

- W1 closes it except for the equal-round ABA (`pm_g1w1_extdrop` fails).
- Fully: the namespace removal would have to wait for in-flight builds, or the reconcile would have to be refused once the removal is durable. Neither is simple, and neither was modelled.

**Likelihood.** A `DROP TABLE` must coincide with a blob reconcile stalled for more than one GC interval, plus a cross-namespace adopter in the window. Very rare.

## 7. Interaction with the four CAS-30 changes {#interaction}

- **C1, the job's round check.** Still needed. The job is independent of the gate, and without C1 a stale job deletes a live marker (CAS-30 S1/S2). All G1 cfgs keep C1 on.
- **C2, the re-stamp.** G1 does not need it in the model: `pm_g1_norestamp` is green in both batteries. It stays harmless and still makes a lost race visible to W1. There is no model evidence that it is needed under G1.
- **C3, the re-read round check.** **Required** by G1: `pm_g1_nogateround` fails in both batteries. G1 makes C3 the only graduation evidence.
- **C4, the round-qualified in-process confirmation.** **Unnecessary under G1**, because G1 deletes the only reader of the registry. Evidence: `pm_g1_noreg` (C4 off) is green with the same 108,547,117 states as `pm_g1_all`.
  - Without G1, C4 closes codex finding 1 (`f1_bug` fails, `f1_fix` green) but not schedule A (`rv_bug` fails with C4 on).
  - C4 cannot tell equal-round confirmations apart (§4, `pm_g7_all`).
  - Implementing G1 therefore replaces C4. Implementing C4 alone leaves schedule A open.

## 8. Limits and unverified premises {#limits}

**Not run:**

- Nothing in `src/` was built, run or provoked. Every code statement is by reading at `e2dd0f5d343`.
- Race widths, likelihood and G1's round-latency impact are derived, not measured.

**Model bounds:**

- One hash, two writer slots with respawn, at most one `GStaleAttempt` per behaviour.
- `MaxGen` ≤ 4, `MaxRounds` ≤ 8.
- The largest G1 bounds in battery 2 did not finish: `pm_g1_big` timed out at about 228M states, `pm_g1_g4` was stopped at about 270M. `pm_g1_r8` was not run.

**Model simplifications:**

- Clamps are modelled only as `SuppressRounds`.
- The supersede marker is queued after the fold in code. In the model the job may land any time before the next cut, which is a superset.
- G2's read and delete are one step.
- A stale job is any member of `mdq`, unfenced.

**Premises assumed, not verified:**

1. The per-request lease fence stops a superseded writer epoch's reconcile from landing after the successor's stale-precommit sweep (`Backend/CasRequests.h:323`, `Pool/CasRefLedger.cpp:4790-4806`). If this is false, the `extdrop` residual has a second trigger.
2. A namespace clamp holds that namespace's deltas and suppresses destructive work round-wide (`Gc/CasGc.cpp:3361-3362`). Not traced through the intake.
3. An aborted round attempt's marker job still lands (`drainOnExitNoThrow`). Not traced end to end. The equal-round cases (`pm_w1_stale`, `pm_g7_all`, `pm_g1w1_extdrop`) depend on it.
4. On GCS and in memory the etag is version-unique, so the equal-bytes `replace` win is S3-only. On those backends the ABA needs GC to leave the marker untouched, which it does for an equal round.
