# Confirmation runs: FINAL binary vs BEFORE, S11 ci and S30 dev

Run date 2026-09-30, 11:41-12:12 local, one run at a time, machine quiet (no builds or tests), no STOP file, `docker ps` clean before each run, teardown after each.
Definitions (round class, working round >= 20 ms, spread = min..max) as in `task-6b2-report.md` Method. All figures MEASURED from `runs/<label>/` by `confirm.py` (wraps `compare.py` metrics) and `t2.py`; "DERIVED" marks formulas.

## Binaries

| role | file | sha256 |
|---|---|---|
| BEFORE | `gc-request-cuts/build/clickhouse-before-61d24118251` | `eecb2cf3d45259e4940d108db47f7059c9b803dee28bfe74ee970c97bae64710` |
| FINAL | `gc-request-cuts/build/clickhouse-after-ac48289f453` | `8c75bd2902b2c46a2c36d9ca9aca02725df6ecad7d3f914f33fb6141aa830bce` |

Both `--version` = `26.6.4.20001.altinityantalya`. The sha256 in each `runs/<label>/binary.txt` matches.

## Run verdicts

| run | binary | scenario | harness verdict | local time |
|---|---|---|---|---|
| before-s11ci-4 | BEFORE | S11 ci | PASS | 11:41-11:48 |
| final-s11ci-1 | FINAL | S11 ci | PASS | 11:48-11:55 |
| before-s11ci-5 | BEFORE | S11 ci | PASS | 11:55-12:04 |
| final-s11ci-2 | FINAL | S11 ci | PASS | 12:04-12:11 |
| final-s30-1 | FINAL | S30 dev | PASS | 12:11-12:12 |

No FAIL, so the `dryrun subset of deletable` race did not occur; no rerun.

## C1: S11 ci, median (min..max)

Columns: BEFORE 1-3 and AFTER 1-3 (old binary `27436f2ae8c`) are recomputed from the earlier runs by the same script; BEFORE 4-5 and FINAL 1-2 are the quiet-machine runs. "memo hits / edges" is DERIVED as `CASRefManifestBodyMemoHits / CASRefEmittedEdges`; "GETs per 1000 edges" is DERIVED as `1000 * CASRefManifestBodyFoldGets / CASRefEmittedEdges`.

### class during

| figure | BEFORE 1-3 | BEFORE 4-5 (quiet) | AFTER 1-3 (old bin) | FINAL 1-2 (quiet) |
|---|---|---|---|---|
| fold_reduce ms / working round | 647.3 (597.7..956.2) | 925.1 (829.8..1020) | 1087 (636.6..1844) | 1016 (790.8..1242) |
| fold_ref_intake ms / working round | 905.9 (704.6..1086) | 1030 (927..1134) | 662.8 (563.9..820.8) | 863.9 (703.3..1025) |
| round ms / working round | 7678 (5673..8390) | 8000 (6617..9383) | 7103 (5878..7982) | 8484 (7688..9281) |
| working rounds | 10 (9..13) | 10 (9..11) | 11 (10..19) | 10.5 (10..11) |
| emitted edges | 2.668e+04 (2.646e+04..3.459e+04) | 2.854e+04 (2.734e+04..2.973e+04) | 2.899e+04 (2.794e+04..3.188e+04) | 3.831e+04 (2.834e+04..4.827e+04) |
| manifest body GETs per 1000 emitted edges | 1000 (1000..1000) | 1000 (1000..1000) | 667.5 (664.4..672.4) | 657.8 (645.4..670.2) |
| memo hits / edges | 0 (0..0) | 0 (0..0) | 0.3325 (0.3276..0.3356) | 0.3422 (0.3298..0.3546) |
| read-ahead hit | 8.572e+04 (8.362e+04..9.198e+04) | 8.991e+04 (8.928e+04..9.055e+04) | 8.197e+04 (7.96e+04..8.366e+04) | 8.779e+04 (7.801e+04..9.758e+04) |
| read-ahead miss | 18 (18..20) | 19 (18..20) | 20 (20..30) | 19 (18..20) |
| read-ahead wasted | 27 (27..96) | 33 (27..39) | 51 (27..63) | 67.5 (42..93) |
| round ms per 1000 edges | 2854 (2132..2878) | 2751 (2662..2841) | 2796 (2503..3853) | 2453 (1923..2983) |
| bulk delete req per 1000 objs | 1.67 (1.66..1.834) | 1.699 (1.698..1.7) | 1.696 (1.628..1.898) | 1.835 (1.689..1.982) |
| DiskS3HeadObject [window] | 2150 (1480..5292) | 2672 (2416..2928) | 2431 (1444..3298) | 4514 (3694..5335) |
| DiskS3GetObject [window] | 1.153e+05 (1.143e+05..1.209e+05) | 1.195e+05 (1.19e+05..1.201e+05) | 1.085e+05 (1.076e+05..1.157e+05) | 1.202e+05 (1.083e+05..1.321e+05) |

### class ALL

| figure | BEFORE 1-3 | BEFORE 4-5 (quiet) | AFTER 1-3 (old bin) | FINAL 1-2 (quiet) |
|---|---|---|---|---|
| fold_reduce ms / working round | 600.8 (455.1..604.9) | 562.2 (530.2..594.2) | 617.8 (417.8..1104) | 598.1 (505.2..691.1) |
| fold_ref_intake ms / working round | 533.1 (523.3..602.6) | 524 (452.5..595.5) | 400.5 (398.9..452.8) | 495.6 (423.3..567.9) |
| round ms / working round | 4477 (4197..4907) | 4156 (3451..4861) | 4185 (3857..4542) | 4669 (4166..5172) |
| working rounds | 30 (29..37) | 34 (29..39) | 32 (29..46) | 29.5 (27..32) |
| emitted edges | 6.216e+04 (4.606e+04..6.375e+04) | 5.608e+04 (4.761e+04..6.456e+04) | 4.823e+04 (4.637e+04..7.582e+04) | 5.578e+04 (4.708e+04..6.448e+04) |
| manifest body GETs per 1000 emitted edges | 1000 (1000..1000) | 1000 (1000..1000) | 703.2 (668.3..708.4) | 691.3 (677.4..705.2) |
| memo hits / edges | 0 (0..0) | 0 (0..0) | 0.2968 (0.2916..0.3317) | 0.3087 (0.2948..0.3226) |
| read-ahead hit | 1.774e+05 (1.61e+05..1.81e+05) | 1.729e+05 (1.602e+05..1.856e+05) | 1.486e+05 (1.459e+05..1.751e+05) | 1.547e+05 (1.461e+05..1.632e+05) |
| read-ahead miss | 48 (46..54) | 50.5 (45..56) | 48 (46..69) | 46.5 (46..47) |
| read-ahead wasted | 90 (27..144) | 52.5 (39..66) | 51 (27..332) | 67.5 (42..93) |
| round ms per 1000 edges | 2499 (2232..2916) | 2523 (2085..2961) | 2662 (2539..2731) | 2499 (2166..2832) |
| bulk delete req per 1000 objs | 1.913 (1.727..1.918) | 1.812 (1.723..1.901) | 1.738 (1.726..2.145) | 1.815 (1.73..1.899) |
| DiskS3HeadObject [window] | 1.473e+04 (1.245e+04..1.904e+04) | 1.479e+04 (1.293e+04..1.665e+04) | 1.299e+04 (1.089e+04..1.934e+04) | 1.559e+04 (1.344e+04..1.774e+04) |
| DiskS3GetObject [window] | 2.354e+05 (2.173e+05..2.37e+05) | 2.304e+05 (2.141e+05..2.468e+05) | 2.007e+05 (1.986e+05..2.273e+05) | 2.13e+05 (2.024e+05..2.235e+05) |

### per-run fold_reduce ms / working round

| run | during | ALL |
|---|---|---|
| before-s11ci-1 | 956.2 | 604.9 |
| before-s11ci-2 | 647.3 | 455.1 |
| before-s11ci-3 | 597.7 | 600.8 |
| before-s11ci-4 | 1020.5 | 594.2 |
| before-s11ci-5 | 829.8 | 530.2 |
| after-s11ci-1 | 1087.5 | 617.8 |
| after-s11ci-2 | 636.6 | 417.8 |
| after-s11ci-3 | 1843.8 | 1104.3 |
| final-s11ci-1 | 1241.7 | 691.1 |
| final-s11ci-2 | 790.8 | 505.2 |

## C2: S30 janitor row, FINAL next to the six earlier runs (`t2.py`)

| run | rounds | janitor_keys | janitor_deleted | DELETE requests in janitor phase (PE DiskS3DeleteObjects) | CASBulkDeleteRequests in janitor phase | keys per DELETE request | namespace_cleanup ms (sum) | max single | S3WriteMicroseconds/DELETE req (ms) |
|---|---|---|---|---|---|---|---|---|---|
| before-s30 | 30 | 782 | 383 | 383 | 0 | 1.00 | 430 | 67 | 0.71 |
| before-s30-r1 | 30 | 798 | 395 | 395 | 0 | 1.00 | 494 | 75 | 0.80 |
| before-s30-r2 | 30 | 762 | 378 | 378 | 0 | 1.00 | 471 | 69 | 0.79 |
| after-s30 | 32 | 895 | 430 | 70 | 10 | 6.14 | 978 | 134 | 7.12 |
| after-s30-r1 | 28 | 778 | 388 | 68 | 8 | 5.71 | 338 | 49 | 2.73 |
| after-s30-r2 | 30 | 746 | 372 | 68 | 8 | 5.47 | 319 | 43 | 2.43 |
| final-s30-1 | 28 | 770 | 372 | 69 | 9 | 5.39 | 317 | 37 | 2.42 |

## Verdicts

1. **Memo effect reproduced: yes.** FoldGets per 1000 edges, `during`: FINAL 657.8 (645.4..670.2) vs old AFTER 667.5 (664.4..672.4) vs BEFORE 1000 (1000..1000). `ALL`: FINAL 691.3 (677.4..705.2) vs old AFTER 703.2 (668.3..708.4). Memo hits / edges `during`: 0.342 vs 0.333. The FINAL ranges lie inside or next to the old AFTER ranges.
2. **`fold_reduce`, quiet machine: overlapping, FINAL not lower.** `during` per working round: BEFORE 4-5 = 1020.5 and 829.8 (range 829.8..1020.5, median 925); FINAL 1-2 = 1241.7 and 790.8 (range 790.8..1241.7, median 1016). The ranges overlap; FINAL median is 1.10x BEFORE (DERIVED 1016/925) and its maximum is the highest of the four quiet runs. `ALL`: BEFORE 4-5 530.2..594.2 vs FINAL 505.2..691.1, overlapping, medians 562 vs 598 (1.06x). What the numbers show: the 1.68x of `after-s11ci-3` is not a stable regression signal (that run at 1843.8 is the maximum of all ten runs, and the earlier BEFORE 1-3 median of 647 was itself lower than the quiet BEFORE 4-5 values 830 and 1020, so run-to-run spread in this class is at least 600..1850 regardless of binary). What they do not show: that FINAL is equal to BEFORE; with n = 2 per side, a difference of up to about +25% in the median (upper end 1242 vs BEFORE 1020) cannot be excluded. FINAL runs also emitted more edges in the `during` class (38310 median vs 28540 for BEFORE 4-5, 1.34x), which alone raises per-round work (DERIVED: edges per working round 3650 vs 2854, 1.28x, against a `fold_reduce` ratio of 1.10x).
3. **Other movements vs the earlier after-runs (`during` class):**
   - `DiskS3HeadObject` (process-wide window): FINAL 4514 (3694..5335) vs old AFTER 2431 (1444..3298) and BEFORE 4-5 2672 (2416..2928): disjoint from both. Per emitted edge (DERIVED) 0.118 vs 0.084 (old AFTER) and 0.094 (BEFORE 4-5). Cause not investigated; the counter includes insert traffic, and FINAL runs had more edges.
   - `fold_ref_intake`: FINAL 864 (703..1025) vs old AFTER 663 (564..821): higher, overlapping at 703..821; vs BEFORE 4-5 1030 (927..1134): lower, overlapping at 927..1025. The old AFTER advantage over BEFORE (0.73x) shrinks to 0.84x (DERIVED 864/1030) with n = 2.
   - `read-ahead wasted`: FINAL 67.5 (42..93) vs BEFORE 4-5 33 (27..39): disjoint, small absolute counts (0.2% of hits).
   - `round ms per 1000 edges`: FINAL 2453 (1923..2983), lowest of the four columns, all ranges overlap.
   - S30 janitor: FINAL 5.39 keys per DELETE request, 317 ms `namespace_cleanup`, max single 37 ms: same as `after-s30-r1` (5.71, 338, 49) and `after-s30-r2` (5.47, 319, 43); the 978 ms of the first `after-s30` remains the outlier. BEFORE is 1.00 keys per request, 430..494 ms.

## Concerns

- n = 2 per side for the quiet comparison; ranges of `fold_reduce` in the `during` class span more than 2x within any column.
- `DiskS3HeadObject` is process-wide and not attributed to GC; the higher FINAL figure is unexplained.
- Round counts are timer-driven; edges per run vary 28k..48k in `during`.
- Files: `tmp/gc-cuts-stand/seq.sh` (driver for this session), `confirm.py`, `c1.md`, `c2.md`. No tracked files changed (`git status --short utils/ca-soak/scenarios` unchanged: `RUN_HISTORY.md` modified as before, two untracked sidecars).
