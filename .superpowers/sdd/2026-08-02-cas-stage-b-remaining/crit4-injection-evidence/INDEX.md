# Criterion-4 anomaly-arm injection evidence — sourcing index

The narrative and verdict live in `docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md` under
`{#criterion-4-evidence}`. This directory holds the underlying evidence that prose cites, pulled
out of the (gitignored) `logs_archive/` tree so a future reader does not have to trust the prose
alone. The big raw dumps (58 MB harness log, 12 GB `cas_log.tsv`, etc.) stay on disk under
`utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/failed_injected_run/` — not copied here —
only the small, high-value extracts are duplicated into git.

## Contents

- `README.md` — verbatim copy of the archive's own README, written at the time of the ruling:
  what happened, why it is not a Stage-B blocker, what residual it feeds.
- `round56_gc_log_rows.tsv` — every row (`Start`/`Phase`×18/`Finish`) for GC round 56
  (`round_id=026949b3faed12ff26bd503f6f26f219`) from ch1's predown `gc_log.tsv` dump, all 29
  columns including the full `phase_metrics` map per phase — not a summary. Re-extracted directly
  from the archived predown dump (`grep -a <round_id> predown_ch1/gc_log.tsv`), not retyped from
  the RESULTS.md prose, so it is independently checkable against that file's own numbers.
- `casrefneedsrecovery_trajectory.txt` — every `CAS SIGNALS` / `CASRefNeedsRecovery` line the
  soak harness log contains for this run (13 lines total, the harness's full occurrence count):
  the clean baseline (`CASRefNeedsRecovery=0` on both nodes at preflight), then the counter's
  first nonzero reading at metrics tick #33 (`ts=1785814819`) through the last tick captured before
  the run's terminal failure (tick #42, `ts=1785815457`) — it never returns to 0 in this run.

## What is NOT re-derivable here

The exact verbatim server-log line quoted in RESULTS.md (`"CAS GC fold: destructive work
SUPPRESSED this pass — ..."`) was read live during the session via a direct `system.text_log`
query against the running cluster. It does not appear in any of the predown dumps captured for
this run (checked `text_log_error_shapes.tsv`, `errors.tsv`, `events.tsv`, and the raw harness
log — none contain it; the predown script's `text_log` extract only captures error-shaped rows,
and this line is an `INFORMATION`-level trace, not an error). The cluster was reset before this
gap was noticed, so the row itself can no longer be re-queried. The line is preserved only as
prose in the committed RESULTS.md — flagged here explicitly rather than silently treated as
independently verified, per the "cite the SYMBOL/state what a fence does not cover" discipline for
this campaign. The round-56 phase evidence above (`fold_reduce.suppress_destructive=1`, every
later phase's `suppressed=1`, zero deletes) is independently verified from the predown dump and
stands on its own without that line.
