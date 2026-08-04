# Failed injected run (NOT the specimen) — seed 20260805

This directory is `python3 -m soak.run --seed 20260805 --phase 3 --duration 90m`, run for the T8
soak stage's criterion-4 (hold/anomaly injection) work. It is **not** the Stage-B general-soak
specimen — that is the clean rerun at seed `20260807` in the parent directory
(`general_soak_90m.log`), with no injections.

## What happened

At approximately minute 40 of this run, per the plan's criterion 4 ("inject one hold and one
anomaly during the soak; all delete families inert for those rounds; round still completes"), the
soak's own live table's `_ckpt` object (`cas/ns/state/081d0652ad0e0fee565484707cf30dfd/_ckpt`,
`ca_soak.ca_stress` on ch1) was deliberately overwritten with garbage bytes to trigger an anomaly.

This worked exactly as intended for the anomaly evidence: GC round 56
(`round_id=026949b3faed12ff26bd503f6f26f219`) read the corrupted checkpoint, classified it as an
anomaly + held namespace, and correctly suppressed every destructive family for that round
(`anomalies=1`, `tables_held=1`, `frontier_proven=9/10`, `objects_deleted=0`,
`entries_graduated=0` while `entries_condemned=6636` kept marking; every later phase —
`manifest_deletes`, `handoff_reclaim`, `ref_object_cleanup`, `orphan_sweep` — carried an explicit
`suppressed=1`). See the Stage-B RESULTS doc's `{#criterion-4-evidence}` (or the equivalent
section as landed) for the full write-up; this evidence is recorded there, not lost with this run.

The exact original `_ckpt` bytes were then restored (verified byte-identical via a hex diff
against the pre-injection backup). But `CASRefNeedsRecovery` — a counter that should always read
zero — went nonzero shortly after and **stayed nonzero for the rest of the run**, rather than
clearing once the bytes were good again. The soak's own workload then hit an entry-gate fsck
timeout (`WARNING [B146/B154]`), an fsck-settling flap (`WARNING [B152/B185]`), and finally a hard
`TRANSPORT FAILURE: timed out`, aborting the run at `op_id=55459`, ~59 minutes in (not the
requested 90). `soak.run` exited `rc=1`.

## Why this is not scored as a Stage-B failure

Corrupting a durable object under a live writer is outside CAS's supported fault model (the store
is trusted for durability; the design defends against LIST lies and races, not byte-rot under an
active lane) — a failed run under a deliberately injected fault of that kind does not fail Stage B.

## What IS carried forward as a named residual

Whether the ref-table lane can self-heal from transient checkpoint damage once the bytes are
restored to be byte-identical is a legitimate robustness question, NOT closed by this single
observation (one injected fault, outside the trusted-store model, not investigated further per
the controller's ruling). Recorded in the post-B residual list as: "checkpoint corruption under a
live lane: `CASRefNeedsRecovery` did not observably clear after a byte-identical `_ckpt` restore
(single observation, injected fault, outside the trusted-store model)" — with this directory as the
artifact.

## Contents

- `general_soak_90m_seed20260805_FAILED.log` — the full run log, including the failure JSON dump.
- `predown_ch1/`, `predown_ch2/` — the predown specimen dump the `run_soak.sh` wrapper captured
  automatically the instant the failed run returned, before any teardown (`cas_log.tsv`,
  `gc_log.tsv`, `part_log.tsv`, `errors.tsv`, `events.tsv`, trace extracts — per node).
