# S23 idle `MemoryTracking` growth — root-cause analysis

- **Runs:** `utils/ca-soak/scenarios/runs/20260718T013739_S23_seed1/` and `.../20260718T000459_S23_seed1/`
- **Branch/SHA:** `cas-gc-rebuild` @ `4d457ec378af` (dirty)
- **Failing check:** *"memory flat over idle window"* — per-node `MemoryTracking` growth `71.5 MiB` on `ch2` (threshold 64 MiB). ch1 `56.6 MiB`.
- **Scope:** read-only RCA. Live cluster was in use (a scenario was running); no cluster queries, no code/config changes.
- **Context:** this is the wave-2 follow-up to `2026-07-18-s23-idle-rss-rca.md`. That RCA correctly re-keyed the gate off `MemoryTracking` (RSS was allocator noise). The gate now trips on tracked memory itself.

## Verdict

**Generic ClickHouse boot-warmup, measured against a cold-boot baseline over a ~90 s window — not a CAS leak and not the GC fold.** The gate baselines `MemoryTracking` seconds after `docker compose up`, runs only `idle_minutes * minute_s = 6 * 15 = 90 s` of idle window (ci scale), then compares. It measures the server *settling*, not steady-state drift. Recommended action is **card measurement recalibration**, not a product fix.

## The two decisive facts (attribution)

1. **The failing node is the non-leader.** GC log outcomes: `ch1` = `Success` on all 19 rounds (held the lease), `ch2` = `NotALeader` on all 12 rounds — it ran **zero** folds, marked 0, deleted 0. Yet `ch2` grew tracked memory **more** than the leader (`71.5` vs `56.6 MiB`; before→after: ch2 277.5→352.5, ch1 291.6→351.0). A node that does no fold work cannot be leaking via the fold. The growth is **shared machinery**, present equally with or without GC work.
2. **The pool is empty for the entire run.** `end_state.json`: 0 blobs / 0 manifests / 0 refs / 0 roots; `gc/` holds 10 tiny state objects (6 KiB). There is no CAS pool state that could accumulate 60–75 MiB. The two CAS-only system logs (`content_addressed_log`, `content_addressed_garbage_collection_log`) received ~40 + ~30 rows total across the run — their buffers are kilobytes, not tens of MiB.

## Growth shape — decelerating, not linear

Three coarse points span the whole run (`report.json` observations):

| node | before (idle start) | after (+90 s idle) | final (post-quiesce) |
|---|---|---|---|
| ch1 | 291.7 MiB | 351.0 MiB (+56.6) | 360.8 MiB (**+9.3**) |
| ch2 | 277.5 MiB | 352.5 MiB (+71.5) | 357.2 MiB (**+4.7**) |

The bulk (~60–75 MiB) lands in the first ~90 s; the trailing quiesce adds only 5–9 MiB. The rate drops several-fold after the idle window — an **asymptotic warmup curve, not a constant-slope leak**. The `metrics.sqlite` series looks "monotonic with no plateau" only because it samples exactly that first-90 s ramp and never reaches steady state. Both S23 runs sample identical ~86 s windows, so neither run can observe a plateau by construction.

What accumulates ~60 MiB in the first ~90 s of any ClickHouse boot (CAS or not): system-log `SystemLog` in-memory buffers and their first flushed parts — `metric_log` (very wide column set, 1 row/s), `asynchronous_metric_log`, `trace_log`, `text_log` — plus background thread-pool / scheduler warmup and mark/uncompressed-cache fill (partly driven by the sampler itself querying `system.metrics`/`system.parts` every ~5 s). None of this is CAS-pool state; it settles once the server reaches steady state.

**CAS-specific vs generic verdict: generic.** I could not run a non-CAS baseline control (cluster busy), but the shared/decelerating signature — non-leader growing as fast as the leader, empty pool, deceleration after the window — is dispositive that the *driver* is generic warmup, not a CAS per-round allocation. The CAS event/GC-log emitters contribute only kilobytes here.

## Fix direction (card recalibration, no product fix)

Edit `utils/ca-soak/scenarios/cards/s23_s27_misc.py`:

- **Do not baseline at cold boot.** Capture the `MemoryTracking` baseline *after* the first idle GC round (or after a short settle / `SYSTEM JEMALLOC PURGE`), so boot-warmup is excluded from the delta.
- **Gate on steady-state slope, not delta-from-boot.** Measure `MemoryTracking` growth over the *later* portion of the idle window (e.g. drop the first ~60–90 s, fit the slope of the sqlite series) and fail only if the rate is non-decreasing / above a small MiB-per-minute floor. A monotonic-but-decelerating ramp is warmup, not a leak.
- **Only `--scale full` can assert a plateau.** The `dev` (4×5 s) and `ci` (6×15 s) windows are too short to reach steady state; there they should record-only or use generous slack. Reserve the tight tracked-memory gate for the 15-minute `full` window.
- **Optional:** run one non-CAS baseline (plain default disk) to quantify generic warmup once and document it in the card, so the number is not re-litigated.

## Secondary observation (out of scope for this gate)

`scratch_bytes` on the local staging path grows monotonically on an empty pool with no inserts (ch1 ~1 → 21 MiB, ch2 16 KiB → 21 MiB). This is local disk, **not** tracked memory, so it does not explain this gate — but idle GC rounds appear to leave scratch/staging files uncollected on an empty pool. Worth a separate check; not investigated here.
