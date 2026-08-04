# Step-3c cost-inventory evidence — sourcing index

Small, high-value extracts backing the Step-3c cost inventory and six-criteria tables in
`docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md` (`{#step-3c-cost-inventory}`,
`{#six-result-criteria}`, `{#t6b-budget-watch}`). The full specimen (predown dumps, harness log,
metrics.sqlite, ~8.3 GB) lives on disk under
`utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/general_soak_90m_run3_seed20260808_specimen/`
(gitignored soak output, not a durable record on its own) — these files are the durable, checkable
subset, pulled directly from `system.cas_gc_log` queried live against the still-up cluster before
teardown.

## Contents

- `ch1_phase_rows.tsv`, `ch2_phase_rows.tsv` — every `Phase`-type row for the six inventoried
  phases (`pending_deletes`, `manifest_deletes`, `orphan_sweep`, `ref_object_cleanup`,
  `namespace_cleanup`, `round_commit`), full `phase_metrics` and `ProfileEvents` maps, per node,
  for the specimen window (2026-08-04 04:53–06:35 UTC). The `sumMap()` aggregates reported in the
  RESULTS.md table are directly re-derivable from these files (e.g. `awk`/`clickhouse-local` over
  the `phase_metrics`/`ProfileEvents` columns) rather than trusted from the prose alone.
- `ch1_start_finish_rows.tsv`, `ch2_start_finish_rows.tsv` — every `Start`/`Finish` row (`round`,
  `outcome`, `duration_ms`, `anomalies`, `entries_condemned`/`graduated`/`redeleted`,
  `objects_deleted`) for the same window — backs the rounds-to-fixpoint row and criterion 3's
  backlog-drains-and-stays-zero evidence (the round-105→108 sequence cited in the doc is rows in
  `ch2_start_finish_rows.tsv`).
- `final_fsck_summary_both_nodes.txt` — a `cas-fsck` (summary, not `--detail`) run against both
  nodes immediately before teardown: `dangling=0 stale_edge=0 unreachable=0` on both, confirming
  criterion 2 held all the way to the end. `reachable` reads 20 here versus 840 in the
  `ca-fsck --detail` run captured minutes earlier in the RESULTS.md prose — this is GC continuing
  to drain reclaimable objects after the workload stopped generating new data, not a discrepancy;
  the number that matters for criterion 2 (`dangling`/`stale_edge`/`unreachable`) is unchanged at
  zero across both readings.
