# Task 6b-2 report: before/after stand runs for the GC request cuts

Status: DONE. All listed runs executed; results in the tables below (same content as `tmp/gc-cuts-stand/COMPARISON.md`).

## Run table

Times are local (harness directories are named in UTC = local - 2 h); wall = harness start to end. Run directories: `utils/ca-soak/scenarios/runs/<dir>`; collected copies: `tmp/gc-cuts-stand/runs/<label>/`.

| label | scenario | binary | verdict | local time | run dir (UTC) |
|---|---|---|---|---|---|
| before-s30 | S30 dev | before | PASS 8/8 | 10:13-10:14 | 20260930T081348_S30 |
| after-s30 | S30 dev | after | PASS 8/8 | 10:15-10:15 | 20260930T081510_S30 |
| before-s46 | S46 dev, budget 5 | before | PASS 9/9 | 10:16-10:19 | 20260930T081604_S46 |
| after-s46 | S46 dev, budget 5 | after | FAIL 0/1: "a GC leader was identified: none" 16 s after stack start | 10:19-10:19 | 20260930T081913_S46 |
| before-s11ci-1 | S11 ci | before | FAIL 11/12: `dryrun subset of deletable`, 2965 candidates class `reachable` | 10:20-10:27 (7.2 min) | 20260930T082000_S11 |
| after-s11ci-1 | S11 ci | after | PASS 12/12 | 10:27-10:34 (7.1) | 20260930T082731_S11 |
| before-s11ci-2 | S11 ci | before | PASS 12/12 | 10:34-10:41 (7.2) | 20260930T083446_S11 |
| after-s11ci-2 | S11 ci | after | PASS 12/12 | 10:42-10:49 (7.2) | 20260930T084205_S11 |
| before-s11ci-3 | S11 ci | before | PASS 12/12 | 10:49-10:57 (8.5) | 20260930T084927_S11 |
| after-s11ci-3 | S11 ci | after | PASS 12/12 | 10:58-11:07 (9.8) | 20260930T085811_S11 |
| after-s46-rerun | S46 dev, budget 5 | after | PASS 9/9 | 11:08-11:11 | 20260930T090814_S46 |
| before/after-s30-r1, -r2 | S30 dev | before, after | PASS 8/8 (all four) | 11:12-11:16 | 20260930T0912xx..0915xx_S30 |

Binaries: BEFORE `gc-request-cuts/build/clickhouse-before-61d24118251`, sha256 `eecb2cf3d45259e4940d108db47f7059c9b803dee28bfe74ee970c97bae64710`; AFTER `.../clickhouse-after-27436f2ae8c`, sha256 `287f235f4d73f23e5e1dd3a1967fdb1f88ebd396cc1d2d3693b83b1d62ef8729`; both `--version` = `26.6.4.20001.altinityantalya`. The base binary starts on this stand (first BEFORE run, S30, PASS).

## Deviations and disclosures

- Extra runs not on the list: `after-s46-rerun` (the one allowed rerun of a failed run) and two additional S30 pairs (`*-s30-r1`, `*-s30-r2`), added because the first after-S30 janitor phase was a 2-3x outlier.
- The first `after-s46` failure is a harness precondition (leader not yet identified 16 s after `up`), not a scenario finding; the run has no `raw/` (nothing collected beyond the wrapper snapshot). Rerun passed.
- A driver script `tmp/gc-cuts-stand/driver.sh` ran the list sequentially (STOP-file check, `docker ps` check and teardown before/after each run). Its "SCENARIO STATUS CHANGED" lines are false alarms of my own path comparison (baseline taken from a different cwd); the contents were identical. Verified at the end: `git status --short utils/ca-soak/scenarios` shows only `RUN_HISTORY.md` (`+116`, the pre-existing modification, unchanged) and the two untracked `s08_sidecar.sh`, `s13_sidecar.sh` that were already there. The five `configs/*` modifications were not touched. No STOP file appeared. `docker ps`: only the buildx container.
- No commits, no edits to the code worktree, no subagents.
- New untracked output only under `tmp/gc-cuts-stand/` and the harness's own `scenarios/runs/`, `logs/`.

## Concerns

1. A0 was not exercised by any run (see T3). Proving it needs a restart or leader change while rows are carried, which no existing scenario places; a dedicated scenario or a hand-driven variant is needed.
2. A2 and B4-in-S11 show no measurable effect; A4 is the only change measured on S11 outside the spread.
3. One base-binary S11 failure (`before-s11ci-1`, fsck dry-run candidates reachable) was not root-caused. It is on the BEFORE side, so it says nothing against the changes, but it means the stand's dry-run assertion is not deterministic on the base.
4. n = 3 per side, one outlier run on the AFTER side (`after-s11ci-3`); the wrong-direction rows list it.
5. HEAD and LIST counts are process-wide (not attributed to GC rounds); round-row GETs hold ~0.4% of the window's GETs. There is no exact "total S3 requests of the GC".


Binaries (release, same build dir): BEFORE `clickhouse-before-61d24118251`, sha256 `eecb2cf3d45259e4940d108db47f7059c9b803dee28bfe74ee970c97bae64710`;
AFTER `clickhouse-after-27436f2ae8c`, sha256 `287f235f4d73f23e5e1dd3a1967fdb1f88ebd396cc1d2d3693b83b1d62ef8729`.
Both report `--version` = `26.6.4.20001.altinityantalya` (the version string does not separate them; the sha256 does).
Every figure below is MEASURED from `runs/<label>/` (source: `raw/gc_log_*.tsv` Finish/Phase rows, `snap/metric_log_*.tsv`, `predown/*/part_log.tsv`) by `compare.py`, `t2.py`, `t3.py`, unless marked DERIVED.

## Method

- **Round class** (S11 ci): a round is `before` / `during` / `after` the delete phase by its end/start time against the window of `MutatePart*` events in `part_log` (162..249 s per run). Both nodes are pooled. `ALL` = every round.
- **Working round** = a round whose `Finish` row has `duration_ms >= 20` (a follower's no-op rounds are 0-1 ms and would dilute per-round means). Per-round durations are DERIVED: phase sum / working rounds.
- **Round row** = summed `ProfileEvents` of `Finish` rows in `cas_gc_log`: attributed to the GC round thread. It does NOT include `DiskS3HeadObject` / `DiskS3ListObjects`, and it holds only ~0.4% of the GETs that the process-wide window sees (manifest body GETs are not attributed to the round thread: median 62160 `CASRefManifestBodyFoldGets` vs median 1036 round-row `DiskS3GetObject` in `ALL`).
- **Metric_log window** = `system.metric_log` 1 s buckets over the round intervals (+-1 s), process-wide: includes insert/merge/mutation traffic in `before` and `during`; `after` is GC plus the tail of merges/mutations.
- **Total S3 requests of the GC** cannot be measured exactly. Closest figures, labelled: `round row GetObject+DeleteObjects` (under-counts), and the `after`-class metric_log window (near-GC-only).
- **Normalisation.** Rounds are timer-driven (`gc_interval_sec` 10) and the after-class work varies from 366 to 29576 emitted edges between runs, so absolute totals are not comparable. Per-edge (`x per 1000 emitted edges`) is used for body-read counters and round time; per-object (`per 1000 objects deleted`, `CASRefCleanupObjectsDeleted`) for delete counters.
- **Spread** = min..max of the 3 runs per side. An effect is called outside the spread only when the two ranges are disjoint. n = 3 per side.
- **Machine noise.** Another agent built and ran unit tests in the code worktree during the first part of the session; run times are in the run table. `uptime` at 11:16 showed a 15-minute load average of 8.05 (includes these runs' own load). Runs 3 (`after-s11ci-3`, 10:58-11:07) is the slowest S11 run and the only outlier in several rows; whether it is noise or a real effect is not resolved by these data.

## T1: S11 ci, three runs per side, median (min..max)

Source: `compare.py` over `runs/{before,after}-s11ci-{1,2,3}`. Rows marked [round row] or [metric_log window] say where the figure comes from. after/before = ratio of medians.


### round class: before

| figure | before median (min..max) | after median (min..max) | after/before |
|---|---|---|---|
| working rounds (>=20 ms) | 11 (10..11) | 10 (10..13) | 0.909 |
| all rounds (incl. no-op) | 27 (25..29) | 25 (24..28) | 0.926 |
| round ms, total | 5.62e+04 (5.016e+04..6.985e+04) | 5.02e+04 (4.007e+04..5.092e+04) | 0.893 |
| round ms / working round | 5109 (5016..6350) | 4007 (3917..5020) | 0.784 |
| fold_ref_intake ms, total | 6637 (5629..9458) | 4910 (4794..5197) | 0.740 |
| fold_ref_intake ms / working round | 603.3 (562.9..859.9) | 479.4 (399.8..491) | 0.795 |
| fold_reduce ms, total | 6750 (5244..1.056e+04) | 6405 (4770..7009) | 0.949 |
| fold_reduce ms / working round | 613.6 (524.4..960) | 539.2 (477..640.5) | 0.879 |
| defer_decision ms, total | 6673 (5777..8265) | 6018 (5644..6748) | 0.902 |
| defer_decision ms / working round | 606.6 (577.7..751.4) | 564.4 (519.1..601.8) | 0.930 |
| manifest_deletes ms, total | 4857 (4274..5505) | 3883 (3781..4187) | 0.799 |
| manifest_deletes ms / working round | 441.6 (427.4..500.5) | 378.1 (322.1..388.3) | 0.856 |
| namespace_cleanup ms, total | 2161 (2046..2626) | 1983 (1738..2337) | 0.918 |
| namespace_cleanup ms / working round | 216.1 (186..238.7) | 179.8 (173.8..198.3) | 0.832 |
| ref_object_cleanup ms, total | 2.887e+04 (2.681e+04..3.303e+04) | 2.389e+04 (1.859e+04..2.725e+04) | 0.827 |
| ref_object_cleanup ms / working round | 2681 (2624..3003) | 1859 (1838..2725) | 0.694 |
| manifest body GETs (FoldGets) | 1.684e+04 (1.64e+04..1.714e+04) | 1.256e+04 (1.231e+04..1.33e+04) | 0.746 |
| memo hits | 0 (0..0) | 3744 (3670..3957) | n/a |
| emitted edges | 1.684e+04 (1.64e+04..1.714e+04) | 1.631e+04 (1.598e+04..1.726e+04) | 0.969 |
| read-ahead hit | 6.748e+04 (6.613e+04..7.1e+04) | 6.293e+04 (6.069e+04..6.505e+04) | 0.932 |
| read-ahead miss | 26 (24..26) | 24 (24..28) | 0.923 |
| read-ahead wasted | 0 (0..0) | 0 (0..0) | n/a |
| bulk delete requests | 50 (50..51) | 49 (48..54) | 0.980 |
| objects deleted (RefCleanup) | 2.799e+04 (2.77e+04..2.862e+04) | 2.782e+04 (2.685e+04..2.942e+04) | 0.994 |
| CASGCGet | 253 (235..254) | 234 (233..277) | 0.925 |
| CASGCList | 19 (17..19) | 17 (17..21) | 0.895 |
| CASGCDelete | 16 (14..16) | 14 (14..18) | 0.875 |
| DiskS3GetObject [round row] | 460 (428..462) | 426 (424..503) | 0.926 |
| DiskS3DeleteObjects [round row] | 66 (64..67) | 63 (62..72) | 0.955 |
| DiskS3HeadObject [metric_log window] | 1.022e+04 (9741..1.106e+04) | 8967 (8832..9690) | 0.878 |
| DiskS3ListObjects [metric_log window] | 155 (152..161) | 153 (149..167) | 0.987 |
| DiskS3GetObject [metric_log window] | 9.425e+04 (8.877e+04..9.606e+04) | 8.33e+04 (8.32e+04..8.514e+04) | 0.884 |
| DiskS3DeleteObjects [metric_log window] | 64 (61..66) | 62 (59..71) | 0.969 |
| GC-attributed S3 requests [round row: GetObject+DeleteObjects] | 526 (492..529) | 489 (486..575) | 0.930 |
| body reads (FoldGets+hits) = edges | 1.684e+04 (1.64e+04..1.714e+04) | 1.631e+04 (1.598e+04..1.726e+04) | 0.969 |
| FoldGets per 1000 edges | 1000 (1000..1000) | 770.4 (770.4..770.7) | 0.770 |
| round-row S3 requests per 1000 edges | 30.86 (29.99..31.24) | 30.41 (29.99..33.32) | 0.985 |
| round ms per 1000 edges | 3279 (3058..4149) | 2950 (2457..3141) | 0.900 |
| bulk delete requests per 1000 objects deleted | 1.786 (1.782..1.805) | 1.787 (1.761..1.836) | 1.001 |
| round-row DeleteObjects per 1000 objects deleted | 2.341 (2.311..2.358) | 2.309 (2.265..2.447) | 0.986 |
| ref_object_cleanup ms per 1000 objects deleted | 1009 (967.9..1180) | 812 (668.4..1015) | 0.805 |

### round class: during

| figure | before median (min..max) | after median (min..max) | after/before |
|---|---|---|---|
| working rounds (>=20 ms) | 10 (9..13) | 11 (10..19) | 1.100 |
| all rounds (incl. no-op) | 25 (25..27) | 27 (27..39) | 1.080 |
| round ms, total | 7.551e+04 (7.375e+04..7.678e+04) | 7.982e+04 (7.813e+04..1.117e+05) | 1.057 |
| round ms / working round | 7678 (5673..8390) | 7103 (5878..7982) | 0.925 |
| fold_ref_intake ms, total | 9160 (9059..9774) | 8208 (7290..1.071e+04) | 0.896 |
| fold_ref_intake ms / working round | 905.9 (704.6..1086) | 662.8 (563.9..820.8) | 0.732 |
| fold_reduce ms, total | 7770 (6473..8606) | 1.087e+04 (7002..3.503e+04) | 1.400 |
| fold_reduce ms / working round | 647.3 (597.7..956.2) | 1087 (636.6..1844) | 1.680 |
| defer_decision ms, total | 9986 (8300..1.043e+04) | 9810 (8814..1.277e+04) | 0.982 |
| defer_decision ms / working round | 998.6 (638.5..1159) | 891.8 (463.9..1277) | 0.893 |
| manifest_deletes ms, total | 1.337e+04 (1.211e+04..1.73e+04) | 1.256e+04 (1.004e+04..1.692e+04) | 0.939 |
| manifest_deletes ms / working round | 1337 (1330..1345) | 1256 (528.4..1538) | 0.939 |
| namespace_cleanup ms, total | 2057 (1990..3079) | 2352 (2244..5145) | 1.143 |
| namespace_cleanup ms / working round | 228.6 (153.1..307.9) | 224.4 (213.8..270.8) | 0.982 |
| ref_object_cleanup ms, total | 3.214e+04 (2.562e+04..3.368e+04) | 3.29e+04 (3.233e+04..3.457e+04) | 1.024 |
| ref_object_cleanup ms / working round | 3368 (1971..3571) | 2991 (1820..3233) | 0.888 |
| manifest body GETs (FoldGets) | 2.668e+04 (2.646e+04..3.459e+04) | 1.926e+04 (1.879e+04..2.128e+04) | 0.722 |
| memo hits | 0 (0..0) | 9728 (9154..1.06e+04) | n/a |
| emitted edges | 2.668e+04 (2.646e+04..3.459e+04) | 2.899e+04 (2.794e+04..3.188e+04) | 1.087 |
| read-ahead hit | 8.572e+04 (8.362e+04..9.198e+04) | 8.197e+04 (7.96e+04..8.366e+04) | 0.956 |
| read-ahead miss | 18 (18..20) | 20 (20..30) | 1.111 |
| read-ahead wasted | 27 (27..96) | 51 (27..63) | 1.889 |
| bulk delete requests | 61 (59..63) | 64 (63..71) | 1.049 |
| objects deleted (RefCleanup) | 3.553e+04 (3.435e+04..3.654e+04) | 3.773e+04 (3.741e+04..3.869e+04) | 1.062 |
| CASGCGet | 223 (218..242) | 243 (237..335) | 1.090 |
| CASGCList | 18 (18..20) | 20 (20..28) | 1.111 |
| CASGCDelete | 19 (19..22) | 22 (21..30) | 1.158 |
| DiskS3GetObject [round row] | 409 (400..438) | 444 (434..610) | 1.086 |
| DiskS3DeleteObjects [round row] | 80 (78..85) | 86 (85..101) | 1.075 |
| DiskS3HeadObject [metric_log window] | 2150 (1480..5292) | 2431 (1444..3298) | 1.131 |
| DiskS3ListObjects [metric_log window] | 6.044e+04 (5.347e+04..6.299e+04) | 6.034e+04 (5.235e+04..6.128e+04) | 0.998 |
| DiskS3GetObject [metric_log window] | 1.153e+05 (1.143e+05..1.209e+05) | 1.085e+05 (1.076e+05..1.157e+05) | 0.941 |
| DiskS3DeleteObjects [metric_log window] | 1078 (759..4395) | 1750 (1083..2073) | 1.623 |
| GC-attributed S3 requests [round row: GetObject+DeleteObjects] | 489 (478..523) | 530 (519..711) | 1.084 |
| body reads (FoldGets+hits) = edges | 2.668e+04 (2.646e+04..3.459e+04) | 2.899e+04 (2.794e+04..3.188e+04) | 1.087 |
| FoldGets per 1000 edges | 1000 (1000..1000) | 667.5 (664.4..672.4) | 0.667 |
| round-row S3 requests per 1000 edges | 18.07 (15.12..18.33) | 18.97 (16.28..24.53) | 1.050 |
| round ms per 1000 edges | 2854 (2132..2878) | 2796 (2503..3853) | 0.980 |
| bulk delete requests per 1000 objects deleted | 1.67 (1.66..1.834) | 1.696 (1.628..1.898) | 1.016 |
| round-row DeleteObjects per 1000 objects deleted | 2.195 (2.19..2.474) | 2.279 (2.197..2.7) | 1.038 |
| ref_object_cleanup ms per 1000 objects deleted | 904.5 (745.8..921.8) | 871.9 (835.6..924) | 0.964 |

### round class: after

| figure | before median (min..max) | after median (min..max) | after/before |
|---|---|---|---|
| working rounds (>=20 ms) | 10 (9..13) | 11 (9..14) | 1.100 |
| all rounds (incl. no-op) | 20 (19..26) | 20 (17..29) | 1.000 |
| round ms, total | 1.059e+04 (7365..1.17e+04) | 5229 (1685..2.991e+04) | 0.494 |
| round ms / working round | 900 (736.5..1177) | 475.4 (187.2..2136) | 0.528 |
| fold_ref_intake ms, total | 1065 (1010..1106) | 563.7 (127.9..2514) | 0.529 |
| fold_ref_intake ms / working round | 101 (85.11..118.3) | 51.24 (14.22..179.6) | 0.508 |
| fold_reduce ms, total | 2188 (1937..3900) | 1596 (636..8754) | 0.729 |
| fold_reduce ms / working round | 243.1 (193.7..300) | 145.1 (70.66..625.3) | 0.597 |
| defer_decision ms, total | 1163 (485..1230) | 776.7 (270.4..2757) | 0.668 |
| defer_decision ms / working round | 89.49 (53.89..123) | 70.61 (30.05..196.9) | 0.789 |
| manifest_deletes ms, total | 2826 (579.7..5167) | 447.6 (19.66..8930) | 0.158 |
| manifest_deletes ms / working round | 217.4 (57.97..574.1) | 40.7 (2.185..637.9) | 0.187 |
| namespace_cleanup ms, total | 516.9 (447.9..701.4) | 298.7 (256.6..1017) | 0.578 |
| namespace_cleanup ms / working round | 44.79 (39.76..77.93) | 28.51 (27.15..72.62) | 0.637 |
| ref_object_cleanup ms, total | 1568 (867.8..1762) | 1376 (273.5..3792) | 0.877 |
| ref_object_cleanup ms / working round | 120.6 (96.42..176.2) | 125.1 (30.39..270.9) | 1.037 |
| manifest body GETs (FoldGets) | 1.074e+04 (2985..2.015e+04) | 1500 (322..1.811e+04) | 0.140 |
| memo hits | 0 (0..0) | 623 (44..1.147e+04) | n/a |
| emitted edges | 1.074e+04 (2985..2.015e+04) | 2123 (366..2.958e+04) | 0.198 |
| read-ahead hit | 1.795e+04 (9131..2.643e+04) | 6042 (1505..2.804e+04) | 0.337 |
| read-ahead miss | 4 (4..8) | 4 (2..11) | 1.000 |
| read-ahead wasted | 48 (0..63) | 0 (0..269) | 0.000 |
| bulk delete requests | 13 (7..17) | 6 (3..25) | 0.462 |
| objects deleted (RefCleanup) | 3525 (2064..4090) | 2928 (516..3097) | 0.831 |
| CASGCGet | 132 (119..192) | 133 (109..216) | 1.008 |
| CASGCList | 12 (11..18) | 12 (10..21) | 1.000 |
| CASGCDelete | 4 (4..11) | 4 (2..14) | 1.000 |
| DiskS3GetObject [round row] | 192 (174..294) | 192 (150..342) | 1.000 |
| DiskS3DeleteObjects [round row] | 21 (11..24) | 10 (5..39) | 0.476 |
| DiskS3HeadObject [metric_log window] | 2689 (562..3028) | 480 (0..8076) | 0.179 |
| DiskS3ListObjects [metric_log window] | 60 (24..910) | 31 (28..77) | 0.517 |
| DiskS3GetObject [metric_log window] | 2.025e+04 (1.323e+04..2.655e+04) | 7853 (1643..3.363e+04) | 0.388 |
| DiskS3DeleteObjects [metric_log window] | 19 (10..2332) | 10 (5..6115) | 0.526 |
| GC-attributed S3 requests [round row: GetObject+DeleteObjects] | 203 (195..318) | 202 (155..381) | 0.995 |
| body reads (FoldGets+hits) = edges | 1.074e+04 (2985..2.015e+04) | 2123 (366..2.958e+04) | 0.198 |
| FoldGets per 1000 edges | 1000 (1000..1000) | 706.5 (612.3..879.8) | 0.707 |
| round-row S3 requests per 1000 edges | 29.62 (9.677..68.01) | 95.15 (12.88..423.5) | 3.212 |
| round ms per 1000 edges | 1090 (525.7..2467) | 2463 (1011..4604) | 2.260 |
| bulk delete requests per 1000 objects deleted | 3.688 (1.711..8.236) | 5.814 (2.049..8.072) | 1.576 |
| round-row DeleteObjects per 1000 objects deleted | 6.809 (2.689..10.17) | 9.69 (3.415..12.59) | 1.423 |
| ref_object_cleanup ms per 1000 objects deleted | 430.9 (420.4..444.8) | 530.1 (469.9..1225) | 1.230 |

### round class: ALL

| figure | before median (min..max) | after median (min..max) | after/before |
|---|---|---|---|
| working rounds (>=20 ms) | 30 (29..37) | 32 (29..46) | 1.067 |
| all rounds (incl. no-op) | 71 (70..82) | 71 (69..96) | 1.000 |
| round ms, total | 1.423e+05 (1.343e+05..1.553e+05) | 1.317e+05 (1.234e+05..1.925e+05) | 0.926 |
| round ms / working round | 4477 (4197..4907) | 4185 (3857..4542) | 0.935 |
| fold_ref_intake ms, total | 1.748e+04 (1.57e+04..1.972e+04) | 1.313e+04 (1.276e+04..1.843e+04) | 0.751 |
| fold_ref_intake ms / working round | 533.1 (523.3..602.6) | 400.5 (398.9..452.8) | 0.751 |
| fold_reduce ms, total | 1.754e+04 (1.365e+04..2.223e+04) | 1.792e+04 (1.337e+04..5.08e+04) | 1.021 |
| fold_reduce ms / working round | 600.8 (455.1..604.9) | 617.8 (417.8..1104) | 1.028 |
| defer_decision ms, total | 1.759e+04 (1.699e+04..1.773e+04) | 1.832e+04 (1.661e+04..1.868e+04) | 1.042 |
| defer_decision ms / working round | 566.5 (479.1..606.5) | 518.9 (398.2..644.1) | 0.916 |
| manifest_deletes ms, total | 2.213e+04 (1.823e+04..2.563e+04) | 2.115e+04 (1.646e+04..2.316e+04) | 0.955 |
| manifest_deletes ms / working round | 692.6 (607.6..763.2) | 567.6 (503.4..660.8) | 0.819 |
| namespace_cleanup ms, total | 5133 (4805..5687) | 4484 (4388..8498) | 0.874 |
| namespace_cleanup ms / working round | 165.7 (138.7..189.6) | 154.6 (137.1..184.7) | 0.933 |
| ref_object_cleanup ms, total | 6.188e+04 (6.022e+04..6.225e+04) | 5.985e+04 (5.287e+04..6.225e+04) | 0.967 |
| ref_object_cleanup ms / working round | 2075 (1628..2134) | 1652 (1353..2064) | 0.796 |
| manifest body GETs (FoldGets) | 6.216e+04 (4.606e+04..6.375e+04) | 3.392e+04 (3.285e+04..5.067e+04) | 0.546 |
| memo hits | 0 (0..0) | 1.432e+04 (1.352e+04..2.515e+04) | n/a |
| emitted edges | 6.216e+04 (4.606e+04..6.375e+04) | 4.823e+04 (4.637e+04..7.582e+04) | 0.776 |
| read-ahead hit | 1.774e+05 (1.61e+05..1.81e+05) | 1.486e+05 (1.459e+05..1.751e+05) | 0.837 |
| read-ahead miss | 48 (46..54) | 48 (46..69) | 1.000 |
| read-ahead wasted | 90 (27..144) | 51 (27..332) | 0.567 |
| bulk delete requests | 126 (118..127) | 119 (114..150) | 0.944 |
| objects deleted (RefCleanup) | 6.621e+04 (6.586e+04..6.832e+04) | 6.848e+04 (6.606e+04..6.993e+04) | 1.034 |
| CASGCGet | 591 (590..687) | 610 (579..828) | 1.032 |
| CASGCList | 48 (47..57) | 49 (47..70) | 1.021 |
| CASGCDelete | 39 (37..49) | 39 (38..62) | 1.000 |
| DiskS3GetObject [round row] | 1036 (1029..1192) | 1062 (1008..1455) | 1.025 |
| DiskS3DeleteObjects [round row] | 166 (155..175) | 159 (152..212) | 0.958 |
| DiskS3HeadObject [metric_log window] | 1.473e+04 (1.245e+04..1.904e+04) | 1.299e+04 (1.089e+04..1.934e+04) | 0.882 |
| DiskS3ListObjects [metric_log window] | 6.15e+04 (5.369e+04..6.317e+04) | 6.053e+04 (5.259e+04..6.146e+04) | 0.984 |
| DiskS3GetObject [metric_log window] | 2.354e+05 (2.173e+05..2.37e+05) | 2.007e+05 (1.986e+05..2.273e+05) | 0.853 |
| DiskS3DeleteObjects [metric_log window] | 1149 (842..6793) | 2140 (1152..7936) | 1.862 |
| GC-attributed S3 requests [round row: GetObject+DeleteObjects] | 1202 (1184..1367) | 1221 (1160..1667) | 1.016 |
| body reads (FoldGets+hits) = edges | 6.216e+04 (4.606e+04..6.375e+04) | 4.823e+04 (4.637e+04..7.582e+04) | 0.776 |
| FoldGets per 1000 edges | 1000 (1000..1000) | 703.2 (668.3..708.4) | 0.703 |
| round-row S3 requests per 1000 edges | 21.99 (18.85..25.7) | 24.05 (21.99..26.33) | 1.094 |
| round ms per 1000 edges | 2499 (2232..2916) | 2662 (2539..2731) | 1.065 |
| bulk delete requests per 1000 objects deleted | 1.913 (1.727..1.918) | 1.738 (1.726..2.145) | 0.908 |
| round-row DeleteObjects per 1000 objects deleted | 2.507 (2.269..2.657) | 2.322 (2.301..3.032) | 0.926 |
| ref_object_cleanup ms per 1000 objects deleted | 914.3 (911.1..934.5) | 890.2 (772..906) | 0.974 |

mutation window s: {'before': [162.4, 168.5, 173.7], 'after': [166.5, 177.4, 248.8]}

### T1 per run (not aggregated)

class before

| figure | before-s11ci-1 | after-s11ci-1 | before-s11ci-2 | after-s11ci-2 | before-s11ci-3 | after-s11ci-3 |
|---|---|---|---|---|---|---|
| working rounds (>=20 ms) | 11 | 10 | 10 | 10 | 11 | 13 |
| round ms, total | 56197 | 50199 | 50165 | 40072 | 69851 | 50915 |
| emitted edges | 17140 | 15984 | 16405 | 16307 | 16835 | 17258 |
| manifest body GETs (FoldGets) | 17140 | 12314 | 16405 | 12563 | 16835 | 13301 |
| memo hits | 0 | 3670 | 0 | 3744 | 0 | 3957 |
| objects deleted (RefCleanup) | 28618 | 26854 | 27696 | 27820 | 27988 | 29418 |
| bulk delete requests | 51 | 48 | 50 | 49 | 50 | 54 |
| DiskS3DeleteObjects [round row] | 67 | 62 | 64 | 63 | 66 | 72 |
| DiskS3GetObject [round row] | 462 | 424 | 428 | 426 | 460 | 503 |
| DiskS3HeadObject [metric_log window] | 10218 | 9690 | 9741 | 8967 | 11055 | 8832 |
| DiskS3GetObject [metric_log window] | 96062 | 83300 | 88770 | 83195 | 94246 | 85143 |
| fold_reduce ms, total | 6750 | 6405 | 5244 | 4770 | 10560 | 7009 |
| ref_object_cleanup ms, total | 28869 | 27247 | 26806 | 18595 | 33031 | 23888 |
| manifest_deletes ms, total | 4857 | 3883 | 4274 | 3781 | 5505 | 4187 |

class during

| figure | before-s11ci-1 | after-s11ci-1 | before-s11ci-2 | after-s11ci-2 | before-s11ci-3 | after-s11ci-3 |
|---|---|---|---|---|---|---|
| working rounds (>=20 ms) | 9 | 10 | 10 | 11 | 13 | 19 |
| round ms, total | 75507 | 79821 | 76779 | 78128 | 73754 | 111691 |
| emitted edges | 26459 | 31884 | 26675 | 27941 | 34588 | 28986 |
| manifest body GETs (FoldGets) | 26459 | 21282 | 26675 | 18787 | 34588 | 19258 |
| memo hits | 0 | 10602 | 0 | 9154 | 0 | 9728 |
| objects deleted (RefCleanup) | 35532 | 38687 | 36535 | 37732 | 34351 | 37413 |
| bulk delete requests | 59 | 63 | 61 | 64 | 63 | 71 |
| DiskS3DeleteObjects [round row] | 78 | 85 | 80 | 86 | 85 | 101 |
| DiskS3GetObject [round row] | 400 | 434 | 409 | 444 | 438 | 610 |
| DiskS3HeadObject [metric_log window] | 1480 | 3298 | 2150 | 1444 | 5292 | 2431 |
| DiskS3GetObject [metric_log window] | 114341 | 115739 | 115252 | 107574 | 120908 | 108498 |
| fold_reduce ms, total | 8606 | 10875 | 6473 | 7002 | 7770 | 35033 |
| ref_object_cleanup ms, total | 32140 | 32327 | 33679 | 32897 | 25620 | 34571 |
| manifest_deletes ms, total | 12108 | 12557 | 13375 | 16917 | 17296 | 10039 |

class after

| figure | before-s11ci-1 | after-s11ci-1 | before-s11ci-2 | after-s11ci-2 | before-s11ci-3 | after-s11ci-3 |
|---|---|---|---|---|---|---|
| working rounds (>=20 ms) | 9 | 9 | 10 | 11 | 13 | 14 |
| round ms, total | 10593 | 1685 | 7365 | 5229 | 11700 | 29906 |
| emitted edges | 20151 | 366 | 2985 | 2123 | 10736 | 29576 |
| manifest body GETs (FoldGets) | 20151 | 322 | 2985 | 1500 | 10736 | 18109 |
| memo hits | 0 | 44 | 0 | 623 | 0 | 11467 |
| objects deleted (RefCleanup) | 2064 | 516 | 4090 | 2928 | 3525 | 3097 |
| bulk delete requests | 17 | 3 | 7 | 6 | 13 | 25 |
| DiskS3DeleteObjects [round row] | 21 | 5 | 11 | 10 | 24 | 39 |
| DiskS3GetObject [round row] | 174 | 150 | 192 | 192 | 294 | 342 |
| DiskS3HeadObject [metric_log window] | 3028 | 0 | 562 | 480 | 2689 | 8076 |
| DiskS3GetObject [metric_log window] | 26547 | 1643 | 13232 | 7853 | 20246 | 33627 |
| fold_reduce ms, total | 2188 | 636 | 1937 | 1596 | 3900 | 8754 |
| ref_object_cleanup ms, total | 868 | 274 | 1762 | 1376 | 1568 | 3792 |
| manifest_deletes ms, total | 5167 | 20 | 580 | 448 | 2826 | 8930 |

class ALL

| figure | before-s11ci-1 | after-s11ci-1 | before-s11ci-2 | after-s11ci-2 | before-s11ci-3 | after-s11ci-3 |
|---|---|---|---|---|---|---|
| working rounds (>=20 ms) | 29 | 29 | 30 | 32 | 37 | 46 |
| round ms, total | 142297 | 131705 | 134309 | 123429 | 155305 | 192512 |
| emitted edges | 63750 | 48234 | 46065 | 46371 | 62159 | 75820 |
| manifest body GETs (FoldGets) | 63750 | 33918 | 46065 | 32850 | 62159 | 50668 |
| memo hits | 0 | 14316 | 0 | 13521 | 0 | 25152 |
| objects deleted (RefCleanup) | 66214 | 66057 | 68321 | 68480 | 65864 | 69928 |
| bulk delete requests | 127 | 114 | 118 | 119 | 126 | 150 |
| DiskS3DeleteObjects [round row] | 166 | 152 | 155 | 159 | 175 | 212 |
| DiskS3GetObject [round row] | 1036 | 1008 | 1029 | 1062 | 1192 | 1455 |
| DiskS3HeadObject [metric_log window] | 14726 | 12988 | 12453 | 10891 | 19036 | 19339 |
| DiskS3GetObject [metric_log window] | 236950 | 200682 | 217254 | 198622 | 235400 | 227268 |
| fold_reduce ms, total | 17543 | 17916 | 13654 | 13369 | 22230 | 50796 |
| ref_object_cleanup ms, total | 61876 | 59848 | 62248 | 52867 | 60220 | 62251 |
| manifest_deletes ms, total | 22133 | 16460 | 18228 | 21146 | 25628 | 23156 |

## T2: S30, janitor batch deletes (dev scale, three runs per side)

Source: `t2.py` over `namespace_cleanup` Phase rows of `cas_gc_log`; the phase's own `ProfileEvents` give the requests (before: `DiskS3DeleteObjects` equals `janitor_deleted` exactly, so attribution is exact). Runs `*-s30` are the listed pair, `*-s30-r1/r2` are two additional pairs (not in the run list, added to check the one outlier).

| run | rounds | janitor_keys | janitor_deleted | DELETE requests in janitor phase (PE DiskS3DeleteObjects) | CASBulkDeleteRequests in janitor phase | keys per DELETE request | namespace_cleanup ms (sum) | max single | S3WriteMicroseconds/DELETE req (ms) |
|---|---|---|---|---|---|---|---|---|---|
| before-s30 | 30 | 782 | 383 | 383 | 0 | 1.00 | 430 | 67 | 0.71 |
| before-s30-r1 | 30 | 798 | 395 | 395 | 0 | 1.00 | 494 | 75 | 0.80 |
| before-s30-r2 | 30 | 762 | 378 | 378 | 0 | 1.00 | 471 | 69 | 0.79 |
| after-s30 | 32 | 895 | 430 | 70 | 10 | 6.14 | 978 | 134 | 7.12 |
| after-s30-r1 | 28 | 778 | 388 | 68 | 8 | 5.71 | 338 | 49 | 2.73 |
| after-s30-r2 | 30 | 746 | 372 | 68 | 8 | 5.47 | 319 | 43 | 2.43 |

## T3: S46 with `gc_round_graduation_budget`=5

Source: `t3.py`, `Finish` rows with duration >= 20 ms. Runs: `before-s46` and `after-s46-rerun` (the first after run `after-s46` failed at start, see report). Only ch2 held the lease, so only its rounds work.


## before-s46
| node | time | round | trigger | condemned | graduated | redeleted | dur ms | CASGCGet | CASGCHead | CASGCMetaOps | CASGCOverwrite | CASGCDelete | DiskS3GetObject | CASOtherGet | CASRefEmittedEdges | CASGCCondemnMarkerUnconfirmedCarry |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch2 | 08:16:23 | 1 | Scheduled | 20 | 0 | 0 | 309 | 132 | 0 | 0 | 1 | 0 | 183 | 3 | 140 | 0 |
| ch2 | 08:18:07 | 2 | Scheduled | 0 | 5 | 0 | 338 | 97 | 0 | 0 | 4 | 0 | 182 | 24 | 0 | 0 |
| ch2 | 08:18:17 | 3 | Scheduled | 0 | 5 | 5 | 68 | 76 | 0 | 0 | 4 | 0 | 121 | 4 | 0 | 0 |
| ch2 | 08:18:27 | 4 | Scheduled | 0 | 5 | 5 | 79 | 76 | 0 | 0 | 4 | 2 | 121 | 4 | 0 | 0 |
| ch2 | 08:18:37 | 5 | Scheduled | 0 | 5 | 5 | 67 | 76 | 0 | 0 | 4 | 2 | 121 | 4 | 0 | 0 |
| ch2 | 08:18:48 | 6 | Scheduled | 0 | 0 | 5 | 79 | 76 | 0 | 0 | 4 | 3 | 121 | 4 | 0 | 0 |
| ch2 | 08:18:58 | 6 | Scheduled | 0 | 0 | 0 | 25 | 11 | 0 | 0 | 2 | 0 | 14 | 3 | 0 | 0 |
TOTAL {'CASGCGet': 544, 'CASGCHead': 0, 'CASGCMetaOps': 0, 'CASGCOverwrite': 23, 'CASGCDelete': 7, 'DiskS3GetObject': 863, 'CASOtherGet': 46, 'CASRefEmittedEdges': 140, 'CASGCCondemnMarkerUnconfirmedCarry': 0, 'cond': 20, 'grad': 20, 'red': 20}

## after-s46-rerun
| node | time | round | trigger | condemned | graduated | redeleted | dur ms | CASGCGet | CASGCHead | CASGCMetaOps | CASGCOverwrite | CASGCDelete | DiskS3GetObject | CASOtherGet | CASRefEmittedEdges | CASGCCondemnMarkerUnconfirmedCarry |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch2 | 09:08:33 | 1 | Scheduled | 20 | 0 | 0 | 309 | 132 | 0 | 0 | 1 | 0 | 183 | 3 | 140 | 0 |
| ch2 | 09:10:24 | 2 | Scheduled | 0 | 5 | 0 | 210 | 97 | 0 | 0 | 4 | 0 | 182 | 24 | 0 | 0 |
| ch2 | 09:10:34 | 3 | Scheduled | 0 | 5 | 5 | 58 | 76 | 0 | 0 | 4 | 0 | 121 | 4 | 0 | 0 |
| ch2 | 09:10:44 | 4 | Scheduled | 0 | 5 | 5 | 60 | 76 | 0 | 0 | 4 | 2 | 121 | 4 | 0 | 0 |
| ch2 | 09:10:54 | 5 | Scheduled | 0 | 5 | 5 | 69 | 76 | 0 | 0 | 4 | 2 | 121 | 4 | 0 | 0 |
| ch2 | 09:11:04 | 6 | Scheduled | 0 | 0 | 5 | 76 | 76 | 0 | 0 | 4 | 3 | 121 | 4 | 0 | 0 |
| ch2 | 09:11:14 | 6 | Scheduled | 0 | 0 | 0 | 27 | 11 | 0 | 0 | 2 | 0 | 14 | 3 | 0 | 0 |
TOTAL {'CASGCGet': 544, 'CASGCHead': 0, 'CASGCMetaOps': 0, 'CASGCOverwrite': 23, 'CASGCDelete': 7, 'DiskS3GetObject': 863, 'CASOtherGet': 46, 'CASRefEmittedEdges': 140, 'CASGCCondemnMarkerUnconfirmedCarry': 0, 'cond': 20, 'grad': 20, 'red': 20}

**T3 reading.**
- Carried candidates per round (both runs, identical): 20 condemned in round 1, then 15, 10, 5, 0 carried while 5 graduate per round (`entries_graduated` 5,5,5,5). The budget knob works on both binaries.
- No counter separates the condemn-marker `.meta` re-read: `CASGCCondemnMarkerUnconfirmedCarry` is 0 in both (it counts a failed GET, not a successful one), `CASGCMetaOps` and `CASGCHead` are 0 in every row. Closest figure, LABELLED as such: round-row `CASGCGet` per round, whole run 544 before, 544 after; `DiskS3GetObject` 863 and 863; per graduated blob (20) 27.2 and 27.2 GETs.
- The carry does not cost extra GETs in either binary in this run, so A0 has no measured effect here. Cause (DERIVED from `run.log` and the round table): all six leader restarts of the run (11:08:41..11:10:03 local) precede the first graduating round (11:10:24 local); the carry then lasted rounds 2..5 in ONE process, where the in-process confirmation already spares the re-read. A0's saving needs a restart or leader change while rows are carried; S46 does not place one there.

## What the numbers say

**A4, manifest body memo: works, outside the spread.**
- Manifest body GETs per 1000 emitted edges: 1000 (1000..1000) -> 703 (668..708) in `ALL`; 770 before, 667 during, 707 after (after-class range 612..880). Before the change every edge costs a body GET (`FoldGets` = edges in all three BEFORE runs), so the 30% cut is measured, not derived. Memo hits + FoldGets = edges in every AFTER run (DERIVED identity, holds).
- The saved GETs show in the process-wide window (`DiskS3GetObject`, `ALL`: 235.4k -> 200.7k, ratio 0.853), but the two ranges (217k..237k vs 199k..227k) overlap, because of the insert traffic in that window and the varying edge count.
- Time: `fold_ref_intake` per working round `ALL` 533 ms (523..603) -> 401 ms (399..453), ranges disjoint; `before` class 603 -> 479 ms. `round ms / working round` `ALL` 4477 (4197..4907) -> 4185 (3857..4542): -6.5%, INSIDE the spread. The memo cuts the intake phase but not, measurably, the whole round: `ref_object_cleanup` dominates the round (2075 of 4477 ms per working round before).
- Distinct `ManifestId`s per round are not exposed; memo hit rate is 29..33% of edges (`ALL`: 14.3k/48.2k, 13.5k/46.4k, 25.2k/75.8k).

**A2, head window: no effect visible in S11; nothing moved outside the spread.**
- Read-ahead miss 48 -> 48 (`ALL`), wasted 90 (27..144) -> 51 (27..332). HEAD requests in the window 14.7k (12.4k..19.0k) -> 13.0k (10.9k..19.3k). All inside spread.
- HEAD and LIST are not attributed to the round, so the window figures are contaminated by other traffic; S11 blob retirement is a small part of its work. A2 is not shown to cut requests on this stand and not shown to add any.

**B4, janitor batches: S11 does not reach it (`janitor_deleted` 0); S30 does. See T2.**
- S30: 383/395/378 janitor keys deleted took exactly 1 DELETE request per key before; 430/388/372 keys took 70/68/68 requests after (5.5..6.1 keys per request, ~6x fewer requests). Outside the spread (before 378..395 requests, after 68..70).
- Janitor phase time: before 430/494/471 ms, after 978/338/319 ms. The first after run is an outlier (its S3 read init time was 4x its siblings for the same GET count); the two repeats are ~0.7x of before. n = 3 per side, one outlier: direction favourable, magnitude uncertain.
- In S11 the ref cleanup path (`CASBulkDeleteRequests` per 1000 objects, `ALL`: 1.913 -> 1.738) is inside the spread and is not the janitor.

**A0, carried marker: not reached by any run, so no effect can be claimed.** See T3.

**Anything that moved the wrong way (reported, not explained away):**
1. `fold_reduce` per working round, `during` class: 647 ms (598..956) -> 1087 ms (637..1844), ratio 1.68. Driven by `after-s11ci-3` (35.0 s summed vs 6.5..10.9 s in the five other runs); ranges overlap, so not established as a regression, and it is not excluded either (A4 changes the fold). `ALL`: 601 (455..605) -> 618 (418..1104), same run.
2. `after` class, per-edge figures: round ms per 1000 edges 1090 -> 2463 (x2.26), round-row S3 requests per 1000 edges 29.6 -> 95.2 (x3.2), bulk deletes per 1000 objects 3.7 -> 5.8. The denominators differ 80-fold between runs (366..29576 edges), the AFTER runs finished the reclaim with far fewer edges in this class in 2 of 3 runs, so a fixed per-round cost is divided by fewer edges. Not a like-for-like comparison; flagged, not resolved.
3. Round-row `DiskS3GetObject` + `DiskS3DeleteObjects`, `during` class: +8% (489 -> 530), `ALL` +1.6% (1202 -> 1221), inside the spread; working-round count +10% (`during`).
4. Metric_log window `DiskS3DeleteObjects` `ALL`: 1149 (842..6793) -> 2140 (1152..7936), x1.86; both spreads span an order of magnitude (mutation-driven part removal in the window), inside the spread.
5. Base binary (BEFORE) failed one S11 run (`before-s11ci-1`): `dryrun subset of deletable`: 2965 candidates not deletable, class `reachable`. Passed in the other 5 S11 runs of either binary. It is a base-binary behaviour on this stand, not caused by the changes; no root cause was pursued here.
