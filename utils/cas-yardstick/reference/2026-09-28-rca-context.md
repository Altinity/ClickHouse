# Context: the CAS-223 RCA measurements (not a `CURRENT` reference)

These numbers come from the old ad-hoc rig, not from this tool. They explain why the `cycle` card exists.
Do not compare a yardstick run against them: workload, server and store differ (see `COMPAT_FIELDS` in `yardstick/report.py`).

## Requests per cycle

One cycle = `CREATE`, 3 one-part `INSERT`, `SELECT`, `DROP`, on one RustFS.
Source: `tmp/cas223/runs/req_cas.tsv`, `tmp/cas223/runs/req_plain.tsv`; table in `docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md` (RC3).

| | CAS | plain S3 |
|---|---|---|
| GET | 83 | 12 |
| PUT | 64 | 31 |
| LIST / HEAD / DeleteObjects | 7 / 5 / 1 | 0 / 0 / 2 |

The 800-cycle totals in `tests/fixtures/rca_2026_09_28_samples.tsv` are these counts times 800 (GET 66400 / 9600).

The yardstick's own cycle smoke measured 6 GET per cycle on plain S3 (48 GET for 8 cycles, `s3` in `tmp/cas321/smoke_task4_fix1.log`), against 12 above.
A likely cause, not verified: the old rig's plain-S3 process also had a CAS disk registered. Prefer the yardstick's number for plain S3.
The RCA does not state the GC setting of these two runs.

## Wall time

Plain S3 was 3.1x faster than CAS in release on the local repro after fixes A and B (8 x 1000 cycles: 449.7 s vs 143.9 s), and 2.8x under TSan.
Source: `docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md`, sections "What is proven vs hypothesis" and the run table.
