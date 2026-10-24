# Leaked `_manifests` with long GC rounds: RCA

Date 2026-09-30. Branch `cas/gc-request-cuts` @ dfba328a747 (FINAL binary `clickhouse-after-ac48289f453`).
No code change, no commit: nothing in the branch is defective.

## Verdict

**Harness timing artefact, not a leak.** The unreachable `_manifests` in both failing runs belong to parts that
the writers' outdated-part cleanup removed AFTER the last GC round of the end checkpoint listed the ref stream.
The end checkpoint's drain (`drain_condemned_pipeline`, 150 s budget) ended before any round could fold those
removal logs. On a live stack with the same binary and configuration, further forced rounds reclaim every such
manifest (unreachable `_manifests` 15677 -> 0 within 6 rounds). BEFORE passed because in both of its runs the
last cleanup batch landed before the last round; all four runs finished within seconds of the deadline.

## Evidence chain

### 1. Which manifests leaked

| run | leaked | namespace | build sequences |
|---|---|---|---|
| `long-final-a` | 9 | all `ca_soak_ch1` (table `9c68ad26-...`) | 0x2e9e, 0x2e9f, 0x2faa, 0x7a6a..0x7a6f |
| `long-final-a2` | 63 (40 sampled) | all `ca_soak_ch2` (table `0364a906-...`) | 0x3c50..0x4727 and 0x8e59..0x8e82 |

Source: harness `forensics/object_lifetimes.json` under
`utils/ca-soak/scenarios/runs/20260930T112929_S11_seed20260930` and `...T114459...`. The `ca_events` summary
extract is capped at 100000 rows and stops in the first minutes of the workload, so the ref-log history of these
manifests is not in the artefacts.

### 2. Part removals continue through the end checkpoint in every run

`RemovePart` rows of user tables in `predown/ch*/part_log.tsv` per minute, `long-final-a2`:
11:52 3972, 11:54 4293, 11:55 2524, 11:56 2409, 11:57 3819, 11:58 1230, 11:59 21. `long-before-a` has the
same shape (10:36 3260 ... 10:40 3292, 10:41 290). Every forced round of the checkpoint folds 70-500 new ref
logs (`fold_ref_intake.logs_applied`) and deletes 2000-12000 manifests (`manifest_deletes.accepted`), in
BEFORE and FINAL alike. The pool is not quiescent during the "fixpoint".

### 3. Removals after the last round's listing match the residual

Last round = last `defer_decision` phase row on ch1 (the round's ref listing).

| run | last round listing | last `RemovePart` | removals after listing | residual unreachable `_manifests` |
|---|---|---|---|---|
| `long-before-a` | 10:41:32.37 | 10:41:17.76 | 0 | 0 |
| `long-before-a3` | 12:15:13.73 | 12:15:07.70 | 0 | 0 |
| `long-final-a` | 11:43:05.03 | 11:43:20.37 | 3 on ch1, 6 on ch2 | 9, all ch1 |
| `long-final-a2` | 11:59:06.27 | 11:59:12.61 | 21 on ch2 (one batch at 11:59:12.606) | 63, all ch2 |

In `long-final-a2` ch1 removed its own 21 parts between 11:58:53 and 11:59:06; round 16 folded them
(`logs_applied` 12, `manifests_deleted` 63). ch2's matching batch of 21 came 6 s after that round and 5 s before
the final fsck, and left 63 = 3 x 21 manifests. In `long-final-a` ch1's 3 late parts give 9 = 3 x 3. The 3:1
ratio holds in both runs; its cause is not established (see Limits). ch2's 6 late removals in `long-final-a`
left no unreachable manifest; not explained (see Limits).

### 4. Reproduction on a live stack: the manifests are reclaimed

`runs/leak-final-live`: FINAL binary, S11 `--scale ci`, seed 20260930, `longround/gc-long-interval.xml`
mounted (`config.d/zz_gc_extra.xml`, `cas_gc_interval_sec` 300). The scheduled round folded 54796 edges in
40.7 s with 19528 memo hits, so the long-round configuration was active. The harness verdict was PASS: its
probe read 0 unreachable at 14:32:58, before the cleanup wave started. Stack left up, then
`leakrca/probe.sh` (fsck `--detail`, outdated part counts, one `SYSTEM CAS GC RUN ca` on ch1, 11 s wait):

| time | unreachable `_manifests` | pending-gc + awaiting-gc | outdated parts ch1 / ch2 |
|---|---|---|---|
| 14:34:18 (right after PASS) | 15677 | 3341 | 4130 / 4776 |
| 14:35:34 | 1378 | 6613 | 3395 / 3268 |
| 14:36:43 | 5898 | 8159 | 1540 / 2286 |
| 14:37:49 | 5970 | 3594 | 441 / 472 |
| 14:38:41 | 2730 | 953 | 1 / 2 |
| 14:39:17 | 9 | 903 | 0 / 0 |
| 14:39:45 | 0 | 502 | 0 / 0 |
| 14:40:30 .. 14:48:19 (iterations 8-40) | 0 | 0 | 0 / 0 |

So with the same binary, the harness verdict depends on where the cleanup wave falls relative to the probe.
Once the writers stop removing parts, unreachable `_manifests` reach 0 and stay there for 33 more rounds.
Files: `tmp/gc-cuts-stand/leakrca/live1/probe.log`, `fsck_*.txt`, `phase_counters.tsv`. Stack torn down.

### 5. The code: each suspect checked

- (a) A removal edge served from the memo queues its cleanup exactly like a read.
  `Gc::foldManifestEdges` runs `mf_cleanup.emplace(id, fold->etag)` for `sign < 0` on both paths. Dedup by
  `ManifestId` is the same `std::map` as in the base. The `Etag` is informational: phase 15 `manifest_deletes`
  deletes write-once keys without a precondition (`removeChunkWriteOnceOrOneByOne`); the etag only rides the
  event. Eviction and an oversized body fall back to `readManifestFold`, which queues the same entry.
- (b) Publish and removal of one manifest in one fold: the removal edge queues the cleanup regardless of
  in-degree. Fenced by `CASGCFold.ManifestFoldedForPublishAndDropIsReadOnce`: it asserts the body is deleted
  after the round, and `expectSameOutcome` compares the whole store with the no-memo fold.
- (c) Budgets: `manifest_deletes` is unbudgeted and drained `attempted == accepted` in every round of both
  failing runs. The fold intake was never clamped or held (`tables_clamped` 0, `tables_held` 0). The residual
  is removal logs written after the last round, not deferred deletes.
- (d) Orphan sweep: not changed by the branch. Its `deleted` was 0 in every round of both runs, and it is not
  the path that reclaims owner-removed manifests.

Nothing in the branch can make a folded removal edge skip its body delete. The only effect of the memo on
`foldManifestEdges` results is that an edge may return `true` from a memoized body where a fresh read would
have found it absent. That leads to more deleting, never less, and `FoldedManifestBodyIsNeverDeletedDuringTheFold`
fences that no folded body is deleted mid-fold.

## Part 2: wasted read-aheads

**The rise is HEADs in `fold_reduce`, not manifest GETs.** The GC-log `Phase` rows carry per-phase
`ProfileEvents`. Every run has `CASGCReadAheadWasted` = 0 in `fold_ref_intake`, so no manifest body is fetched and
discarded. Every read-ahead waste is in `fold_reduce`, the HEAD window. The double-hint shape from the brief
cannot occur: `GcReadAhead::hint` returns early on `slots.contains(key)`, so a second edge of the same
manifest in one log is one request. The first take consumes the slot and inserts into the memo, and the
second edge hits the memo. The hint loop in `Gc::fold` also skips a `ManifestId` the memo already holds.
No fix needed.

**Real manifest GET saving = memo hits - 0.**

| run | edges | manifest GETs | memo hits | GETs / 1000 edges |
|---|---|---|---|---|
| `long-final-a` | 133868 | 81792 | 52076 | 611 |
| `long-final-a2` | 136849 | 86982 | 49867 | 636 |
| `leak-final-live` (harness end) | 54796 | 35268 | 19528 | 644 |
| `leak-final-live` (after the 40 probe rounds) | 133140 | 82278 | 50862 | 618 |
| BEFORE (all runs) | = edges | = edges | 0 | 1000 |

### HEAD requests per run (`fold_reduce` rows, both nodes)

hint hits = takes served by a read-ahead; inline = take with no hint (`CASGCReadAheadMiss`); wasted = hinted,
never taken; total = hits + inline + wasted = HEAD requests the round issued; takes = hits + inline.

| run | rounds | hint hits | inline | wasted | total HEADs | takes | total / takes |
|---|---|---|---|---|---|---|---|
| `before-s11ci-1` | 10 s | 50966 | 48 | 0 | 51014 | 51014 | 1.000 |
| `before-s11ci-2` | 10 s | 46514 | 46 | 0 | 46560 | 46560 | 1.000 |
| `before-s11ci-3` | 10 s | 49296 | 54 | 0 | 49350 | 49350 | 1.000 |
| `before-s11ci-4` | 10 s | 46352 | 45 | 0 | 46397 | 46397 | 1.000 |
| `before-s11ci-5` | 10 s | 51159 | 56 | 0 | 51215 | 51215 | 1.000 |
| `final-s11ci-1` | 10 s | 52175 | 47 | 93 | 52315 | 52222 | 1.002 |
| `final-s11ci-2` | 10 s | 46421 | 46 | 42 | 46509 | 46467 | 1.001 |
| `long-before-a` | 300 s | 17365 | 3635 | 63 | 21063 | 21000 | 1.003 |
| `long-before-a3` | 300 s | 15331 | 4138 | 63 | 19532 | 19469 | 1.003 |
| `long-final-a` | 300 s | 18900 | 26 | 6517 | 25443 | 18926 | 1.344 |
| `long-final-a2` | 300 s | 19547 | 30 | 4742 | 24319 | 19577 | 1.242 |
| `leak-final-live` (harness end) | 300 s | 7222 | 4 | 63 | 7289 | 7226 | 1.009 |
| `leak-final-live` (after 40 probe rounds) | 300 s | 19299 | 27 | 2821 | 22147 | 19326 | 1.146 |

The destructor's waste lands after `reduce_timer.reset()` and so outside every phase row. For BEFORE that is
about 540 per run (system.events 600 vs 63 in the rows). For FINAL it is about 0 (system.events 5630 =
the median of the row values 6517 and 4742).

**The head-window change raises the HEAD requests a round issues.** With 10-s rounds the rise is at most
0.2 % of takes. With long rounds it is 15-34 % of takes: about +4.4k HEADs per run (BEFORE 21063 and 19532,
FINAL 25443 and 24319). In return it moves about 3.6-4.1k inline HEADs per run to read-ahead (inline 3635 and
4138 -> 26 and 30). That changes latency, not the request count. The mechanism: BEFORE throttled on
`reads.pending()`, which counts slots left untaken, so the window shut and extra candidates were never hinted.
FINAL throttles on its own outstanding HEADs (`outstanding_heads` in `Gc::fold`). The window stays open and
hints up to `reads.window()` superset candidates ahead of each take. Candidates the merge passes are discarded
by `discardHeadHintsBelow` and `resetHeadHints` and counted as wasted. Net per long-round run, the branch saves
about 50k manifest GETs and adds about 4.4k HEADs.

## Part 3: fix

None. No change in the branch is defective. The residual comes from the harness: its drain ends on a fixed
deadline while writers are still removing outdated parts. `drain_condemned_pipeline` stops when the fsck count
reaches 0 or at 150 s, and the probe can read 0 before the cleanup wave, as in `leak-final-live`. A harness
that waited for zero outdated parts on every replica before the final forced rounds would remove the race.
That is a tracked-harness change and was not made here.

## Part 4: verification

No build or gate: HEAD is unchanged, so the earlier gates on dfba328a747 stand. The fixed-binary stand runs
were replaced by the live reproduction above, with the same FINAL binary. The harness verdict was PASS, and on
the live pool unreachable `_manifests` went to 0 and stayed there for 33 rounds.

## Limits

- n = 1 live reproduction. It did not hit the race; it shows reclamation, not the failing timing itself.
- Why each late part removal leaves 3 manifests is not established. So is why ch2's 6 late removals in
  `long-final-a` left none. A likely cause is that part_log's `RemovePart` time is not the time the ref drop
  was logged, but it was not checked. The ref-log history of the leaked keys is missing from the capped
  event extract.
- The HEAD comparison between rows covers different workloads (takes 18.9k-21.0k per long run). Per-take
  ratios are the comparable figure.
