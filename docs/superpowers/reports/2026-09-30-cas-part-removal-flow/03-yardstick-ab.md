# CAS-83 yardstick A/B (`--quick`, alternating B A B A)

BEFORE `clickhouse-before-e2dd0f5d343`, AFTER `clickhouse-after-fa41049c383`. Tool unmodified, no errors, no FAILED cell, `unavailable: none`.
Runs (in order): B `20260930T162209Z`, A `20260930T162440Z`, B `20260930T162703Z`, A `20260930T162923Z` under `tmp/cas83/yardstick/runs/`.
Source of every figure: `<run>/summary.tsv` (`per_unit` column for wall/GET/PUT/DELETE/LIST; `median` column of the `ratio:cas/s3:*` rows). Aggregation script: `agg.py`; full tables below are its output (`AB_tables.md`).
Scale 0.25, 1 repeat per run: `cycle` = 48 cycles (8 workers x 6), so each run is a single sample. Spread = (max-min)/mean over the two runs of a side. Row counts of B/A: request counts are deterministic, wall is not.
Ratios with a zero denominator (`0 -> n`) are not numeric and are absent from the ratio table. LIST/HEAD rows are in `AB_tables.md`; LIST per unit did not move on any card.

Parts are removed in the measured window only by `cycle` (`DROP TABLE SYNC` each cycle). `mutation` also makes old parts obsolete, but its GET/PUT/DELETE per part are identical before and after, so the changed path is not exercised there in the window. `wide_insert`, `attach_200`, `lwd_concurrent`, `select_subset`, `dup_content` remove no parts in the window: they are the control.

| card/phase | metric | B run1 | B run2 | A run1 | A run2 | B mean | A mean | delta % | spread B % | spread A % |
|---|---|---|---|---|---|---|---|---|---|---|

**policy cas, per unit**

| card/phase | metric | B run1 | B run2 | A run1 | A run2 | B mean | A mean | delta % | spread B % | spread A % |
|---|---|---|---|---|---|---|---|---|---|---|
| cycle/cold | wall | 0.03125 | 0.0293 | 0.01733 | 0.01663 | 0.03027 | 0.01698 | -43.9 | 6.4 | 4.1 |
| cycle/cold | GET | 83.08 | 83.15 | 47.1 | 47.15 | 83.11 | 47.12 | -43.3 | 0.1 | 0.1 |
| cycle/cold | PUT | 64.38 | 64.48 | 28.35 | 28.44 | 64.43 | 28.4 | -55.9 | 0.2 | 0.3 |
| cycle/cold | DELETE | 1 | 1 | 1 | 1 | 1 | 1 | +0.0 | 0.0 | 0.0 |
| cycle/cold | LIST | 7 | 7 | 7 | 7 | 7 | 7 | +0.0 | 0.0 | 0.0 |
| cycle/hot | wall | 0.02758 | 0.02821 | 0.01628 | 0.01518 | 0.0279 | 0.01573 | -43.6 | 2.3 | 7.0 |
| cycle/hot | GET | 83 | 83 | 47 | 47 | 83 | 47 | -43.4 | 0.0 | 0.0 |
| cycle/hot | PUT | 64 | 64 | 28 | 28 | 64 | 28 | -56.2 | 0.0 | 0.0 |
| cycle/hot | DELETE | 1 | 1 | 1 | 1 | 1 | 1 | +0.0 | 0.0 | 0.0 |
| cycle/hot | LIST | 7 | 7 | 7 | 7 | 7 | 7 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | wall | 0.0375 | 0.04365 | 0.03508 | 0.03255 | 0.04058 | 0.03382 | -16.7 | 15.1 | 7.5 |
| wide_insert/cold | GET | 5.008 | 5.008 | 5.008 | 5.008 | 5.008 | 5.008 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | PUT | 7 | 7 | 7 | 7 | 7 | 7 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | wall | 0.03748 | 0.03388 | 0.036 | 0.03331 | 0.03568 | 0.03465 | -2.9 | 10.1 | 7.8 |
| wide_insert/hot | GET | 5.008 | 5 | 5.008 | 5.008 | 5.004 | 5.008 | +0.1 | 0.2 | 0.0 |
| wide_insert/hot | PUT | 7.024 | 7.008 | 7.024 | 7.024 | 7.016 | 7.024 | +0.1 | 0.2 | 0.0 |
| wide_insert/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | wall | 0.001451 | 0.00132 | 0.001651 | 0.001373 | 0.001385 | 0.001512 | +9.2 | 9.5 | 18.4 |
| attach_200/cold | GET | 3.26 | 3.26 | 3.26 | 3.26 | 3.26 | 3.26 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | PUT | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | LIST | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | wall | 0.0002278 | 0.0002313 | 0.0002187 | 0.0002083 | 0.0002295 | 0.0002135 | -7.0 | 1.5 | 4.9 |
| attach_200/hot | GET | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | LIST | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | +0.0 | 0.0 | 0.0 |
| mutation/cold | wall | 0.04827 | 0.02175 | 0.03555 | 0.03267 | 0.03501 | 0.03411 | -2.6 | 75.8 | 8.5 |
| mutation/cold | GET | 7.04 | 7.04 | 7.04 | 7.04 | 7.04 | 7.04 | +0.0 | 0.0 | 0.0 |
| mutation/cold | PUT | 7.08 | 7.08 | 7.08 | 7.08 | 7.08 | 7.08 | +0.0 | 0.0 | 0.0 |
| mutation/cold | DELETE | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | +0.0 | 0.0 | 0.0 |
| mutation/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| mutation/hot | wall | 0.04685 | 0.04763 | 0.05149 | 0.03338 | 0.04724 | 0.04243 | -10.2 | 1.7 | 42.7 |
| mutation/hot | GET | 7.04 | 7.04 | 7.04 | 7.04 | 7.04 | 7.04 | +0.0 | 0.0 | 0.0 |
| mutation/hot | PUT | 7.08 | 7.08 | 7.08 | 7.08 | 7.08 | 7.08 | +0.0 | 0.0 | 0.0 |
| mutation/hot | DELETE | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | 0.04 | +0.0 | 0.0 | 0.0 |
| mutation/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/cold | wall | 0.0262 | 0.02589 | 0.02478 | 0.02807 | 0.02605 | 0.02642 | +1.5 | 1.2 | 12.4 |
| lwd_concurrent/cold | GET | 152.1 | 146.1 | 143.7 | 143.4 | 149.1 | 143.5 | -3.7 | 4.0 | 0.2 |
| lwd_concurrent/cold | PUT | 7.7 | 7.675 | 7.787 | 7.787 | 7.688 | 7.787 | +1.3 | 0.3 | 0.0 |
| lwd_concurrent/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/hot | wall | 0.02432 | 0.02426 | 0.02503 | 0.0228 | 0.02429 | 0.02392 | -1.5 | 0.3 | 9.3 |
| lwd_concurrent/hot | GET | 172.4 | 160.4 | 159.1 | 147.4 | 166.4 | 153.2 | -7.9 | 7.2 | 7.6 |
| lwd_concurrent/hot | PUT | 7.875 | 7.812 | 7.812 | 7.812 | 7.844 | 7.812 | -0.4 | 0.8 | 0.0 |
| lwd_concurrent/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | wall | 0.01604 | 0.03916 | 0.02417 | 0.01166 | 0.0276 | 0.01791 | -35.1 | 83.8 | 69.8 |
| select_subset/cold | GET | 50 | 50 | 50 | 50 | 50 | 50 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | wall | 0.01162 | 0.02487 | 0.01019 | 0.009565 | 0.01824 | 0.009879 | -45.9 | 72.6 | 6.4 |
| select_subset/hot | GET | 50 | 50 | 50 | 50 | 50 | 50 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | wall | 0.05464 | 0.05186 | 0.05623 | 0.05145 | 0.05325 | 0.05384 | +1.1 | 5.2 | 8.9 |
| dup_content/cold | GET | 17 | 17 | 17 | 17 | 17 | 17 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | PUT | 9.4 | 9.4 | 9.4 | 9.4 | 9.4 | 9.4 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | LIST | 5 | 5 | 5 | 5 | 5 | 5 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | wall | 0.04388 | 0.03275 | 0.04404 | 0.03706 | 0.03832 | 0.04055 | +5.8 | 29.1 | 17.2 |
| dup_content/hot | GET | 17 | 17 | 17 | 17 | 17 | 17 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | PUT | 9 | 9 | 9 | 9 | 9 | 9 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | LIST | 5 | 5 | 5 | 5 | 5 | 5 | +0.0 | 0.0 | 0.0 |

**policy s3, per unit**

| card/phase | metric | B run1 | B run2 | A run1 | A run2 | B mean | A mean | delta % | spread B % | spread A % |
|---|---|---|---|---|---|---|---|---|---|---|
| cycle/cold | wall | 0.01964 | 0.01621 | 0.02051 | 0.01986 | 0.01793 | 0.02019 | +12.6 | 19.1 | 3.2 |
| cycle/cold | GET | 6 | 6 | 6 | 6 | 6 | 6 | +0.0 | 0.0 | 0.0 |
| cycle/cold | PUT | 31 | 31 | 31 | 31 | 31 | 31 | +0.0 | 0.0 | 0.0 |
| cycle/cold | DELETE | 2 | 2.083 | 2.083 | 2.167 | 2.042 | 2.125 | +4.1 | 4.1 | 3.9 |
| cycle/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| cycle/hot | wall | 0.01983 | 0.01596 | 0.01967 | 0.02007 | 0.01789 | 0.01987 | +11.0 | 21.6 | 2.0 |
| cycle/hot | GET | 6 | 6 | 6 | 6 | 6 | 6 | +0.0 | 0.0 | 0.0 |
| cycle/hot | PUT | 31 | 31 | 31 | 31 | 31 | 31 | +0.0 | 0.0 | 0.0 |
| cycle/hot | DELETE | 1.917 | 1.917 | 2 | 1.833 | 1.917 | 1.917 | -0.0 | 0.0 | 8.7 |
| cycle/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | wall | 0.02265 | 0.02142 | 0.02281 | 0.02929 | 0.02204 | 0.02605 | +18.2 | 5.6 | 24.9 |
| wide_insert/cold | GET | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | PUT | 10 | 10 | 10 | 10 | 10 | 10 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | wall | 0.02225 | 0.0262 | 0.02315 | 0.03242 | 0.02422 | 0.02779 | +14.7 | 16.3 | 33.4 |
| wide_insert/hot | GET | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | PUT | 10 | 10 | 10 | 10 | 10 | 10 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | wall | 0.000527 | 0.0005468 | 0.000769 | 0.0005702 | 0.0005369 | 0.0006696 | +24.7 | 3.7 | 29.7 |
| attach_200/cold | GET | 9.02 | 9.02 | 9.02 | 9.02 | 9.02 | 9.02 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | wall | 0.000206 | 0.0001726 | 0.000221 | 0.0002116 | 0.0001893 | 0.0002163 | +14.3 | 17.6 | 4.3 |
| attach_200/hot | GET | 9.02 | 9.02 | 9.02 | 9.02 | 9.02 | 9.02 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| mutation/cold | wall | 0.01885 | 0.01244 | 0.00791 | 0.01874 | 0.01565 | 0.01333 | -14.8 | 41.0 | 81.3 |
| mutation/cold | GET | 3 | 3 | 3 | 3 | 3 | 3 | +0.0 | 0.0 | 0.0 |
| mutation/cold | PUT | 11.04 | 11.04 | 11.04 | 11.04 | 11.04 | 11.04 | +0.0 | 0.0 | 0.0 |
| mutation/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| mutation/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| mutation/hot | wall | 0.02361 | 0.03742 | 0.01371 | 0.01302 | 0.03051 | 0.01336 | -56.2 | 45.3 | 5.2 |
| mutation/hot | GET | 2 | 2 | 2 | 2 | 2 | 2 | +0.0 | 0.0 | 0.0 |
| mutation/hot | PUT | 11.04 | 11.04 | 11.04 | 11.04 | 11.04 | 11.04 | +0.0 | 0.0 | 0.0 |
| mutation/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| mutation/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/cold | wall | 0.01192 | 0.009019 | 0.01185 | 0.009896 | 0.01047 | 0.01087 | +3.9 | 27.7 | 18.0 |
| lwd_concurrent/cold | GET | 104.2 | 99.61 | 103.1 | 103.2 | 101.9 | 103.1 | +1.2 | 4.5 | 0.0 |
| lwd_concurrent/cold | PUT | 19.55 | 19.76 | 19.76 | 19.55 | 19.66 | 19.66 | +0.0 | 1.1 | 1.1 |
| lwd_concurrent/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/hot | wall | 0.01157 | 0.00936 | 0.01168 | 0.01079 | 0.01046 | 0.01123 | +7.4 | 21.1 | 7.9 |
| lwd_concurrent/hot | GET | 130.5 | 116.5 | 120.7 | 118.3 | 123.5 | 119.5 | -3.2 | 11.4 | 2.0 |
| lwd_concurrent/hot | PUT | 19.34 | 19.55 | 19.98 | 19.34 | 19.44 | 19.66 | +1.1 | 1.1 | 3.2 |
| lwd_concurrent/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | wall | 0.09874 | 0.03613 | 0.07801 | 0.02305 | 0.06743 | 0.05053 | -25.1 | 92.8 | 108.8 |
| select_subset/cold | GET | 100 | 100 | 100 | 100 | 100 | 100 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | wall | 0.009494 | 0.008019 | 0.01412 | 0.009608 | 0.008757 | 0.01187 | +35.5 | 16.8 | 38.1 |
| select_subset/hot | GET | 50 | 50 | 50 | 50 | 50 | 50 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | PUT | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | wall | 0.04442 | 0.03209 | 0.04442 | 0.03924 | 0.03825 | 0.04183 | +9.3 | 32.2 | 12.4 |
| dup_content/cold | GET | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | PUT | 11 | 11 | 11 | 11 | 11 | 11 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | wall | 0.04235 | 0.03148 | 0.04044 | 0.03863 | 0.03691 | 0.03953 | +7.1 | 29.5 | 4.6 |
| dup_content/hot | GET | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | PUT | 11 | 11 | 11 | 11 | 11 | 11 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | DELETE | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | LIST | 0 | 0 | 0 | 0 | 0 | 0 | +0.0 | 0.0 | 0.0 |

**ratios (median column)**

| card/phase | ratio | B run1 | B run2 | A run1 | A run2 | B mean | A mean | delta % | spread B % | spread A % |
|---|---|---|---|---|---|---|---|---|---|---|
| cycle/cold | wall | 1.591 | 1.808 | 0.8447 | 0.8372 | 1.699 | 0.841 | -50.5 | 12.8 | 0.9 |
| cycle/cold | GET | 13.85 | 13.86 | 7.851 | 7.858 | 13.85 | 7.854 | -43.3 | 0.1 | 0.1 |
| cycle/cold | PUT | 2.077 | 2.08 | 0.9147 | 0.9173 | 2.078 | 0.916 | -55.9 | 0.2 | 0.3 |
| cycle/hot | wall | 1.391 | 1.768 | 0.8275 | 0.7565 | 1.579 | 0.792 | -49.9 | 23.9 | 9.0 |
| cycle/hot | GET | 13.83 | 13.83 | 7.833 | 7.833 | 13.83 | 7.833 | -43.4 | 0.0 | 0.0 |
| cycle/hot | PUT | 2.065 | 2.065 | 0.9032 | 0.9032 | 2.065 | 0.9032 | -56.2 | 0.0 | 0.0 |
| wide_insert/cold | wall | 1.656 | 2.038 | 1.538 | 1.112 | 1.847 | 1.325 | -28.3 | 20.7 | 32.2 |
| wide_insert/cold | PUT | 0.7 | 0.7 | 0.7 | 0.7 | 0.7 | 0.7 | +0.0 | 0.0 | 0.0 |
| wide_insert/hot | wall | 1.685 | 1.293 | 1.555 | 1.027 | 1.489 | 1.291 | -13.3 | 26.3 | 40.9 |
| wide_insert/hot | PUT | 0.7024 | 0.7008 | 0.7024 | 0.7024 | 0.7016 | 0.7024 | +0.1 | 0.2 | 0.0 |
| attach_200/cold | wall | 2.753 | 2.414 | 2.147 | 2.408 | 2.583 | 2.278 | -11.8 | 13.1 | 11.5 |
| attach_200/cold | GET | 0.3614 | 0.3614 | 0.3614 | 0.3614 | 0.3614 | 0.3614 | +0.0 | 0.0 | 0.0 |
| attach_200/hot | wall | 1.106 | 1.34 | 0.9899 | 0.9844 | 1.223 | 0.9871 | -19.3 | 19.1 | 0.6 |
| attach_200/hot | GET | 0.006652 | 0.006652 | 0.006652 | 0.006652 | 0.006652 | 0.006652 | +0.0 | 0.0 | 0.0 |
| mutation/cold | wall | 2.56 | 1.748 | 4.494 | 1.743 | 2.154 | 3.119 | +44.8 | 37.7 | 88.2 |
| mutation/cold | GET | 2.347 | 2.347 | 2.347 | 2.347 | 2.347 | 2.347 | +0.0 | 0.0 | 0.0 |
| mutation/cold | PUT | 0.6413 | 0.6413 | 0.6413 | 0.6413 | 0.6413 | 0.6413 | +0.0 | 0.0 | 0.0 |
| mutation/hot | wall | 1.984 | 1.273 | 3.754 | 2.564 | 1.629 | 3.159 | +94.0 | 43.7 | 37.7 |
| mutation/hot | GET | 3.52 | 3.52 | 3.52 | 3.52 | 3.52 | 3.52 | +0.0 | 0.0 | 0.0 |
| mutation/hot | PUT | 0.6413 | 0.6413 | 0.6413 | 0.6413 | 0.6413 | 0.6413 | +0.0 | 0.0 | 0.0 |
| lwd_concurrent/cold | wall | 2.198 | 2.871 | 2.092 | 2.836 | 2.535 | 2.464 | -2.8 | 26.5 | 30.2 |
| lwd_concurrent/cold | GET | 1.459 | 1.466 | 1.393 | 1.39 | 1.463 | 1.392 | -4.9 | 0.5 | 0.3 |
| lwd_concurrent/cold | PUT | 0.3939 | 0.3884 | 0.3941 | 0.3983 | 0.3911 | 0.3962 | +1.3 | 1.4 | 1.1 |
| lwd_concurrent/hot | wall | 2.103 | 2.591 | 2.144 | 2.113 | 2.347 | 2.129 | -9.3 | 20.8 | 1.5 |
| lwd_concurrent/hot | GET | 1.322 | 1.378 | 1.318 | 1.246 | 1.35 | 1.282 | -5.0 | 4.2 | 5.6 |
| lwd_concurrent/hot | PUT | 0.4072 | 0.3996 | 0.3911 | 0.404 | 0.4034 | 0.3976 | -1.5 | 1.9 | 3.2 |
| select_subset/cold | wall | 0.1625 | 1.084 | 0.3098 | 0.5059 | 0.6231 | 0.4078 | -34.6 | 147.9 | 48.1 |
| select_subset/cold | GET | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | +0.0 | 0.0 | 0.0 |
| select_subset/hot | wall | 1.224 | 3.101 | 0.7217 | 0.9955 | 2.163 | 0.8586 | -60.3 | 86.8 | 31.9 |
| select_subset/hot | GET | 1 | 1 | 1 | 1 | 1 | 1 | +0.0 | 0.0 | 0.0 |
| dup_content/cold | wall | 1.23 | 1.616 | 1.266 | 1.311 | 1.423 | 1.289 | -9.5 | 27.1 | 3.5 |
| dup_content/cold | PUT | 0.8545 | 0.8545 | 0.8545 | 0.8545 | 0.8545 | 0.8545 | +0.0 | 0.0 | 0.0 |
| dup_content/hot | wall | 1.036 | 1.04 | 1.089 | 0.9595 | 1.038 | 1.024 | -1.4 | 0.4 | 12.6 |
| dup_content/hot | PUT | 0.8182 | 0.8182 | 0.8182 | 0.8182 | 0.8182 | 0.8182 | +0.0 | 0.0 | 0.0 |