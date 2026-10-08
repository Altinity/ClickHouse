# CAS-29.9 janitor batches: A/B on the local stand

M1 and M4a pass their bars on the headline metric (drain wall). M3 and M2 were not measured on the stand, and the authority-GET rows are unmeasured. Two wire-DELETE rows miss the strict 1% bar; the extra base deletes are from an unidentified family (below).

- Base: `e2dd0f5d343`. After: `a3b8d44c73f`. Binaries in `cas-83/tmp/ab/`.
- Stand: `ch1` + RustFS + Keeper, rounds forced every 10 s, driver `tmp/gc-cuts-stand/value/m1.py`. Runs alternated base/after.
- Raw data: `tmp/gc-cuts-stand/value/m1-ab-*.json`, `m1-m4-*.json`, `m4ref-m4ref-*.json`; tables `ab-m1.md`, `ab-m4.md`. Not committed.
- Bars are from `## Value proof {#value-proof}` in the design spec.

## M1: long-stream drain {#m1}

Medians of 3 runs per cell.

| № | Cell / metric | Bar | Base | After | Result |
|---|---|---|---|---|---|
| 1 | 7k, 0 ms: janitor phase | ≤ 1 s | 0.885 s (first janitor round) | 0.541 s | PASS |
| 2 | 7k, 0 ms: janitor-active rounds | 1 | 3 | 1 | PASS |
| 3 | 7k, 0 ms: `batches` | ≤ 10 | n/a | 3 | PASS |
| 4 | 7k, 0 ms: `DiskS3DeleteObjects`, drain | after ≤ base; saving within 1% of `janitor_deleted` − `batches` | 2556 | 20 | saving PASS; 1% MISS (see below) |
| 5 | 7k, 0 ms: `DiskS3ListObjects`, drain | after ≤ base | 75 | 44 | PASS |
| 6 | 20k, 0 ms: janitor-active rounds | 1 | 16 | 1 | PASS |
| 7 | 20k, 0 ms: drain wall | ≤ 90 s and below every base run | 1306.8..1307.2 s | 30.3..31.6 s | PASS |
| 8 | 20k, 0 ms: `batches` | ≤ 25 | n/a | 16 | PASS |
| 9 | 20k, 0 ms: janitor phase | reported | 0.944 s per page, 1 page per round | 3.30 s (17 pages) | reported |
| 10 | 20k, 0 ms: `DiskS3DeleteObjects`, drain | as row 4 | 16175 | 52 | saving PASS; 1% MISS |
| 11 | 20k, 0 ms: `DiskS3ListObjects`, drain | after ≤ base | 3134 | 160 | PASS |
| 12 | 7k, 20 ms: janitor phase | ≤ 5.3 s | 22.3 s (one page) | 1.197 s | PASS |
| 13 | 100k, 0 ms: janitor-active rounds | ≤ 3 | not run (spec) | 2 | PASS |
| 14 | 100k, 0 ms: `budget_exhausted` | 1 on every janitor round but the last | not run | 1 on round 2, 0 on round 3 (all 3 runs) | PASS |

Rows 4 and 10, the 1% bar. Paired run 1: base 2556 deletes with 2338 janitor keys, after 20 deletes with 3 batches. Saving is 2536; the formula gives 2335 (8.6% off). At 20k: 16142 against 15928 (1.3%). In base, round 3 sends 200 non-janitor deletes and later janitor rounds send 1 to 2 extra each; after finishes before those rounds exist. Family of the extra deletes: unresolved. In base round 3 at 7k, `DiskS3DeleteObjects` is 1200 while `CASRootDelete` is 1000 and `CASManifestDelete` is 0, so the extra 200 come from neither counter; the same round also shows `DiskS3HeadObject` 100 and `DiskS3GetObject` 120. The per-round data holds only process-wide counts, so I cannot name the family. The bar is not adjusted: strict reading is a MISS.

Wire DELETE count falls by about 99.7% at 20k (16175 to 52). Drain wall at 7k: 112 s to 11.7 s (0 ms), 259 s to 124 s (20 ms). At 20 ms the after side is bound by the first folding round (~100 s, finding 1).

### Per-page time split (spec open question) {#per-page}

`list_ms` dominates every after-run; `delete_ms` is at most 63 ms per round on the lat0 cells.

| Cell | phase | `list_ms` | `catalog_ms` | `delete_ms` | `drain_wait_ms` |
|---|---|---|---|---|---|
| 7k, 0 ms | 535..548 ms | 366..377 | 0 | 3..5 | 147..161 |
| 20k, 0 ms | 3169..3541 ms | 3069..3464 | 0..1 | 9..10 | 29..50 |
| 7k, 20 ms | 1178..1216 ms | 821..838 | 80 | 24 | 120..152 |
| 100k, round 2 | 20.56..20.77 s | 20.12..20.20 s | 27..29 | 0 | 316..452 |

Per page after: 134 to 208 ms at 0 ms (7k, 20k), 433 to 468 ms at 100k (round 2), 295 to 304 ms at 7k and 20 ms. Report 08's 0.4 to 0.6 s per page shows up at 100k only. The hot spot is the page LIST, not the delete.

## M4a: store without batch delete, 20 ms {#m4a}

`support_batch_delete` false. 20k keys, 2 runs per side with netem 20 ms; 1 run per side at 0 ms (reported, not judged). Base was capped at 5 janitor-active rounds (30 rounds).

| № | Metric | Bar | Base | After | Result |
|---|---|---|---|---|---|
| 1 | janitor-active rounds | after ≤ 2 | 5 at the cap, 1 page each | 2 (11000 keys, then 4959 and 4966) | PASS |
| 2 | janitor phase time | reported | 22.1..22.5 s per page | 20.8 s (round 2), 9.1 s (round 3) | reported |
| 3 | `delete_jobs` = janitor keys; `batches_held` 0 | | n/a | 11000/11000 then 4958/4959 and 4965/4966; `batches_held` 0; `jobs_in_flight_max` 16 | PASS, jobs one short of keys in the last round (exact delete) |
| 4 | authority GETs per page | after = base | unmeasured | unmeasured | not judged |
| 5 | drain wall | below every base run | ≥ 1458.0 and ≥ 1462.3 s at the cap | 1114.9, 1116.4 s | PASS |
| 6 | wire DELETE, drain | after ≤ 1.01 × base | 40204 at the cap (5 of 16 pages) | 51159, 51166 | not judged: base truncated |
| 7 | authority GETs, drain | after ≤ base | unmeasured | unmeasured | not judged |

- `blob_storage_log` on the stand holds only `Upload` and `Delete` rows, no `Read` rows, so rows 4 and 7 are unmeasured.
- Row 5: base drain at the cap is a lower bound. Extrapolation: 11 more janitor rounds at ~26 s plus 88 other rounds at ~3.9 s gives ~2090 s total.
- Row 6: base extrapolated is 40204 + about 11 × 1000 to 1002, close to after's ~51.2k. This is an estimate, not a measurement.
- Predicted drain of ~2.5 min is missed by a wide margin on both sides: round 1 alone takes ~1074 s (finding 1).
- 0 ms runs: after 45.6 s (2 rounds); base 325.4 s at the cap.
- Implied one-key DELETE latency on the after side: `delete_ms` × 16 / keys = 22 ms in round 2. The bar's 20 ms condition is applied as written.

### M4 ref-cleanup {#m4-ref}

The precondition (at least 2000 ref keys in one round) was not met with 500-insert batches: 1030 to 1290 keys per round. With 2500 inserts before a single round it was met (5000). `ref_object_cleanup` after 2500 single-row inserts, `support_batch_delete` false, 20 ms. One run per side.

| Metric | Bar | Base | After | Result |
|---|---|---|---|---|
| ref delete requests | after = base | 5000 | 5000 | PASS |
| phase duration | after ≤ base + spread | 109.40 s | 109.76 s | PASS on one run; spread not estimated |
| authority GETs | after = base | unmeasured | unmeasured | not judged |

## M3: steady state {#m3}

Not measured on the stand. The quiet-pool property (one logical page, no batches, on a folding round) is covered by the unit tests `SteadyStateTakesOnePage` and `MidStreamPassOverLivePagesTakesOnePage`. The S11 before/after comparison of process-wide `DiskS3ListObjects` per round does not exist.

## Findings {#findings}

1. **(CAS-337) First folding round after the DROP is ~18 min at 20 ms, on both sides.** Round 1 wall: 1073.4 to 1077.4 s at 20 ms against 36 to 37 s at 0 ms. Its process-wide counters: `DiskS3DeleteObjects` 35000, of which `CASManifestDelete` 30000 and `CASRootDelete` 5000. That is ~36 ms per key, consistent with one key per request issued serially; the run JSONs do not record the phase name, so which phase it is comes from the stand's `cas_gc_log`, not from these files. The janitor then drains in 2 rounds on the after side (11000 keys with `budget_exhausted` 1, then 4959 to 4966 keys), against 1000 keys per round on base.
2. **M1 at 20k:** after drains in 1 janitor round (17 pages, 16 batches, 3.17 to 3.54 s). Base needs 16 janitor-active rounds over 129 rounds, 1307 s.

3. **(CAS-338) `fold_ref_intake` costs about 4.3 GETs per part.** Taken from the owner's ruling; not re-measured in these runs.

## Not done, and why {#deviations}

- **M3 (S11 steady state): not run.** Stopped by the owner's ruling before the first run. No `COMPARISON.md` exists.
- **M2 (S30): not run.** Reported-only measurement; not started.
- **M4a first base attempt died** on the client's 300 s receive timeout in the 1074 s fold round; `--receive_timeout=7200` was added to `m1.py` and `m4ref.py` and the chain was rerun.
- **M4 ref-cleanup: one run per side, not two.** The stand was stopped after the first pair.
- **`m4ref.py` changed.** Per-batch windows of 500 inserts deleted 1030 to 1290 keys, never the 2000 the script waits for. It now inserts 2500 rows, then runs up to 4 GC rounds.
- **First M4 ref-cleanup attempts discarded.** Two orchestrator scripts ran concurrently and tore down each other's stand; I killed both and restarted from a down stand. No numbers from those attempts are used.
- Authority GET counts are unmeasured everywhere (no `Read` rows in `blob_storage_log`); `ns_auth_gets` 0 in the JSONs means unmeasured.
- Base was not run at 100k (spec) and was capped at 5 janitor rounds in M4a (brief).
