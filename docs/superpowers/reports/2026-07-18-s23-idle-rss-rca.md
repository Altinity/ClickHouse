# S23 idle-RSS-growth FAIL — root-cause analysis

- **Run:** `utils/ca-soak/scenarios/runs/20260718T000459_S23_seed1/`
- **Branch/SHA:** `cas-gc-rebuild` @ `4420b5a3498b` (dirty)
- **Failing check:** *"memory flat over idle window"* — RSS grew 184.6 MiB (516 → 700 MiB) over an idle window, threshold 64 MiB.
- **Scope:** read-only RCA. No code, config, or cluster changes.

## Verdict

**Allocator retention / warmup settling + a marginal measurement design.** This is **not** a leak in background GC or log flushing. The failing metric is dominated by jemalloc dirty-page retention layered on fresh-boot warmup, sampled over an 86-second window that begins seconds after the server starts. Recommended action is **threshold/measurement recalibration**, not a product fix.

## Growth shape (the samples decide it)

The pool is **empty for the entire run** (0 blobs / 0 refs / 0 manifests; `gc/` holds 10 tiny state objects, 6 KiB total). Each of the 6 "idle minutes" is a single forced GC round over an empty universe: `wall_s = 0.01`, 12–24 object-store ops, `deleted_total = 0`, 40 CA-log event rows total. There is no CAS work that could accumulate 184 MiB of state.

`metrics.sqlite` RSS series (leader `ch1`, seconds relative to first sample):

```
 0s 515 | 11s 533 | 21s 545 | 32s 561 | 38s 662 | 43s 611 | 54s 642 | 65s 640 | 75s 719 | 81s 662 | 86s 671
```

This is **sawtooth / oscillating**, not monotonic: transient spikes at 38s (662) and 75s (719) each drop straight back on the next sample. `ch2` shows the identical pattern (dip at 65s, spike to 685 at 81s, back to 628 at 86s). A monotonic leak does not fall back between samples.

The decisive contrast is RSS vs `MemoryTracking` (the server's own tracked-allocation accounting, which is what a real server-side leak moves and is allocator-noise-free):

| node | RSS Δ | Tracking Δ | RSS−Tracking gap (allocator) |
|---|---|---|---|
| ch1 | +185 MiB | +56 MiB | ~130 MiB |
| ch2 | +237 MiB | +72 MiB | ~165 MiB |

Tracked memory climbs **smoothly and modestly** (~50–70 MiB) — the expected warmup of thread pools, background schedulers, and system-log buffers in the first ~90 s after boot. The much larger RSS growth is untracked resident dirty pages the allocator has not purged. `mem_before` (516 MiB) is a near-cold-boot baseline; `mem_after` (700 MiB) even exceeds every in-window sqlite sample (max 719 only momentarily), i.e. it caught the tail of a spike.

## Why this run is a 2x outlier vs history

Across 9 S23 runs, `idle_rss_growth` ranged −2 → 94 MiB (most ~30 MiB); this run is 184 MiB. In **every** run RSS growth exceeds Tracking growth (the gap is always allocator). The check has already flip-flopped pass/fail with no GC code changes (the 2026-06-27 run *failed* at only +90 MiB RSS / +8 MiB tracked). Two independent amplifiers explain this run:

1. **Elevated S3 error rates** this window (read 8.6%, write 17.2% on ch2 — flagged in the report). Retries allocate transient upload/download buffers; jemalloc keeps them resident as dirty pages, inflating RSS without inflating tracked memory. Both nodes spiked together, consistent with a shared object-store error episode.
2. **Fresh-boot warmup variance** — the baseline is captured seconds after `docker compose up`, before the server has finished settling.

Two measurement quirks make the check noisy on top of that: it compares `max(RSS)` **across nodes** before vs after (the two maxima can be different nodes, as in the 2026-07-13 "pass"), and the 64 MiB slack is tighter than the natural fresh-boot + allocator variance (~200 MiB here).

## Fix direction (recalibration, no product fix)

The evidence does not implicate GC or log flushing, so no product change is justified. Recalibrate the check in `utils/ca-soak/scenarios/cards/s23_s27_misc.py`:

- **Key the assertion off `MemoryTracking` growth, not RSS.** Tracked memory is what a real server-side CAS/log leak would move; it is free of allocator dirty-page noise and grew a benign ~50–70 MiB here. Keep RSS as an informational observation.
- If RSS must stay a gate: take the baseline **after a short settle delay** (or after an explicit `SYSTEM JEMALLOC PURGE` on each node) so it is not a cold-boot number; compare **per-node** deltas rather than max-across-nodes; and widen the slack to the observed fresh-boot variance (~200 MiB on a 2-node boot under S3 error load).
- Optionally record the S3 read/write error rates alongside the memory verdict so a retry-storm window is not misread as a leak.
