# Criterion-4 hold-arm injection evidence — sourcing index

The anomaly arm of Stage-B criterion 4 was satisfied earlier (`crit4-injection-evidence/`,
`docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md` `{#criterion-4-evidence}`). This directory
holds the hold arm, run separately per the controller's ruling so the 90-minute clean specimen
stayed pristine.

## Run shape

`python3 -m soak.run --seed 20260901 --phase 3 --duration 20m --metrics
build/ca_soak_crit4_hold/metrics.sqlite`, from `utils/ca-soak` against the default
`docker-compose.yml` (same cluster shape as the anomaly arm and the Stage-B specimen). Started
2026-08-04 11:23 UTC. The run was terminated early (SIGTERM to the `soak.run` process, ~11:41 UTC)
once the evidence below was captured — this was never intended to be a preserved specimen, only a
short dedicated vehicle to get live GC rounds to inject into. It ran the harness's own chaos
schedule with a chaos window starting at t+480s; the injection (t+~265s) and every round discussed
below happened before that window opened, so nothing here is confounded by the harness's own
faults.

## Injection shape and why

`HoldReason::GapBelowWitness` (`CasFoldSealFormat.h:45`) — a 404 at the expected ref-log id with a
durable witness above it in the same epoch. Chosen over `WitnessDisappeared` /
`BodyUndecodable` / `CheckpointUndecodable`: those are byte-rot of a durable object under a live
writer, which the anomaly arm already showed is outside CAS's supported fault model (that
injection, on the checkpoint-corruption shape, wrecked its own run — see
`crit4-injection-evidence/README.md`). `GapBelowWitness` instead simulates a store losing or lying
about an object it still claims to hold — exactly the LIST-lie/race fault CAS's design defends
against — while leaving every other object, including the witness, untouched.

## Mechanism

RustFS stores each S3 object as its own directory (one `xl.meta` file per object-dir under
`_log/<id>.zst/`), so a single object can be removed and restored at the filesystem level via
`docker exec` into the `rustfs1` container without disturbing any neighboring key — this was
verified live (`inject_output.log`, `restore_output.log`) before trusting it for the real
namespace, and confirmed independently by a fresh `ls` right after each step (`656a` absent,
`6569`/`656b`/`656c` present and unaffected).

Target: namespace `865d3a88b5332a8e47c8b1463b483da3` (table `ca_soak.ca_stress` on `ch1`), epoch 1.
- Deleted (the position the fold expected next): `0000000000000001-000000000000656a`
- Durable witness above it, same epoch: `0000000000000001-000000000000656b` (and `...656c`
  further above), both untouched throughout

Cursor relation: the round immediately before injection (`399fbae5...`) resolved everything
committed as of its `fold_ref_intake` at 11:26:37 UTC (`logs_applied=10437`, `tables_held=0`). The
target was picked from a fresh listing taken at 11:29:20 UTC, three minutes of continued insert
traffic later, so it sat inside the still-unresolved backlog, not behind the already-folded cursor
— confirmed after the fact by the very next round reading forward into it (below).

Backup/restore round-trip, in order:
1. `tar -C <dir> -cf backup.tar <target>` inside the rustfs container, `docker cp` to host.
2. Extracted to a scratch path *inside the container* and `diff -r`'d against the still-live
   original — `SCRATCH_ROUNDTRIP_OK` — **before** the real delete (`inject_output.log`).
3. `rm -rf` on the single target object-dir only. Deleted 2026-08-04T11:29:24Z.
4. After capturing the held rounds: `docker cp` the same tar back in, `tar -x` over the original
   path, `diff -r` against the freshly-extracted tar contents — `RESTORE_ROUNDTRIP_OK`
   (`restore_output.log`). Restored 2026-08-04T11:34:41Z. The restored `xl.meta` is byte-for-byte
   the original (770 bytes, same content) — this is a restore, not a new write.

## What this experiment can and cannot isolate — read before the numbers

Every held round below carries `anomalies=1` at `Finish`. Since the gate is
`suppress_destructive = anomalies || carried_holds || !frontier_complete`, a reader is right to ask
whether the suppression is explained by the anomaly alone, leaving the hold arm proving nothing the
anomaly arm had not already proved. The answer, stated here rather than left to be discovered:

- **That `anomalies=1` IS the hold being recorded, not a second fault.** The injection introduced
  exactly one fault — one deleted object. `CasGc.cpp`, at the gate computation, says so in its own
  words: *"Term 2 is STRUCTURAL. Today every hold also records an anomaly, so term 1 happens to imply
  it -- but that is a property of the current code, not the invariant, and a gate that relies on a
  coincidence opens the day the coincidence stops holding."* The third term was lit by the same hold
  as well (`frontier INCOMPLETE ... unproven: held=1`). One injected fault lit all three terms.
- **So this experiment cannot isolate term 2 as the sole cause, and does not claim to.** Isolating it
  would require a hold that records no anomaly, which the current code does not produce.
- **What it does establish is the criterion's own wording**: with a hold present, every delete family
  is inert for those rounds, per family, and the round still completes. It additionally establishes
  the CLEARING behaviour, which the anomaly arm never showed — that injection never cleared
  (`CASRefNeedsRecovery` stayed nonzero for the rest of its run).

## Rounds observed — held, then cleared

All four round attempts' full `Start`/`Phase`/`Finish` rows (29-column, untruncated) are in
`gc_log_rows.tsv`, pulled directly from `system.cas_gc_log` on `ch1` — not retyped from this
prose.

**Three held rounds**, in order, all `outcome=Success`:

| round_id | fold_ref_intake | fold_reduce | tables_held | frontier_proven | absent_probes |
|---|---|---|---|---|---|
| `52c5216dbc5230c21d92d066a2e86375` | 11:33:01 | `suppress_destructive=1`, `frontier_complete=0` | 1 | 1 of 2 | 1 |
| `972ac3c2ad4e9d45f7262bbf3fea5490` | 11:33:48 | `suppress_destructive=1`, `frontier_complete=0` | 1 | 1 of 2 | 1 |
| `6c696cd55009bbbbcd4a084c8912b336` | (see tsv) | `suppress_destructive=1`, `frontier_complete=0` | 1 | 1 of 2 | 1 |

Every one of these carries `frontier_unprobed_budget=0` in `fold_ref_intake` — the hold is not a
budget skip, the fold genuinely read the position and found it absent with a witness above.

**Per delete family, all three held rounds, explicit zero work + suppressed tag** (verbatim from
`phase_metrics`, e.g. round `52c5216d...`):
- `manifest_deletes`: `{attempted:0, deleted:0, suppressed:1}`
- `handoff_reclaim`: `{generations_reclaimed:0, objects_reclaimed:0, suppressed:1}`
- `ref_object_cleanup`: `{suppressed:1, ...}` (no delete counter on this phase; `suppressed:1` is
  its own gate)
- `orphan_sweep`: `{deleted:0, listed:0, suppressed:1, ...}`
- `pending_deletes`: `{deleted:0, graduated:0, redeleted:0, replaced:0, ...}` (no explicit
  `suppressed` field on this phase, but zero across every actionable column)
- generation pruning (inside `round_commit`, no phase row of its own): `generations_visited:0`,
  `pruned_through:0` — no wholesale delete ran either

Server log, verbatim, once per held round (`text_log_cas_gc_lines.tsv`):
```
CAS GC ref intake: namespace ca_soak_ch1/store/309/3099fa90-3dd8-4740-a2ba-7dcc357c82cb@cas@
HELD at 0000000000000001-000000000000656a -- ref intake: expected next id absent below a
same-epoch witness -- contiguity says this cannot happen, so a durable record is missing. The
cursor stays at 0000000000000001-0000000000006569 and this namespace folds nothing further this
round.

CAS GC fold: destructive work SUPPRESSED this pass — 1 anomaly(ies), 1 held namespace(s),
frontier INCOMPLETE (1 of 2 namespace(s) proven; unproven: held=1). Graduations and pending
deletes are carried; nothing irreversible runs until a pass that clears all three.
```
(Both lines recur three times, timestamped 11:31:43/11:33:01, 11:33:29/11:33:48, and
11:34:20/11:34:26 — one pair per held round, matching the table above.)

The held namespace's own `entries_condemned`/`condemned` marking kept advancing across the three
held rounds (1679 → 1679 more → 5556) — condemning (a non-destructive bookkeeping step) is not
gated by the hold, only the irreversible deletes are, matching the anomaly arm's documented
behavior.

**Clearing round**: `ae46f97f746073e093943a84da1408ce` (Start 11:34:39, after the restore completed
at 11:34:41). Its `fold_ref_intake` (11:35:38) shows `tables_held=0, absent_probes=0,
frontier_proven=2` (both namespaces) — the fold actually re-read `656a` and got the object back,
which is what `absent_probes=0` proves; it did **not** clear by observing some other absent
(`CasFoldSealFormat.h:58-60`: a hold clears only by folding through `offending_position`). Its
`fold_reduce` shows `suppress_destructive=0, frontier_complete=1, graduated=5000`, and every
destructive family resumed real work in the same round: `manifest_deletes
{attempted:33428, deleted:33428, suppressed:0}`, `ref_object_cleanup {suppressed:0}`, `orphan_sweep
{suppressed:0, cursor_advanced:1}`, `handoff_reclaim {suppressed:0}`, and generation pruning
resumed too (`round_commit`: `generations_visited:3, pruned_through:3`, both 0 in every held
round). `outcome=Success`.

**Hold duration**: 3 rounds held (11:28:59 Start through 11:34:29 Finish, ~5.5 minutes wall time —
most of it the fold walking a multi-hundred-thousand-record backlog, not idle), cleared on the 4th
round attempted after the restore.

## Safety counters

Read via `SELECT event, value FROM system.events WHERE event IN (...) SETTINGS
system_events_show_zero_values = 1` (required — see `soak/signals.py`'s own comment on why the
setting is load-bearing) on both nodes, both before injection and after clearance:
`CASRefNeedsRecovery=0`, `CASGCUnappliedFoldedTransactions=0`, `CASRefRecoveryStreamHole=0` —
never nonzero, unlike the anomaly arm's checkpoint-corruption injection which left
`CASRefNeedsRecovery` stuck nonzero for the rest of that run. The harness's own metrics ticker
(`soak_run.log`, not copied here — see `build/ca_soak_crit4_hold/soak_run.log` on the machine that
ran this) independently tracked `CASGCClampSuppressedPasses` rising 1 → 2 → 3 across the same
window and holding at 3 through clearance, then the pool's physical byte total resumed shrinking
once destructive work resumed (`pool drain probe: pool_bytes=2076212934 sample=3
trajectory=[2505957993, 2302963831, 2076212934]`), corroborating the `manifest_deletes` resumption
independently of `system.cas_gc_log`.

## Contents

- `gc_log_rows.tsv` — every `Start`/`Phase`×15-18/`Finish` row for the four round attempts
  discussed above, all columns, `SELECT * FROM system.cas_gc_log WHERE round_id IN (...)`.
- `text_log_cas_gc_lines.tsv` — every `system.text_log` row matching `%CAS GC%` in the injection
  window (11:28–11:37 UTC), including the three verbatim HELD/SUPPRESSED line pairs quoted above.
- `inject_output.log` — the injection script's own output: pre/post listings, the scratch
  round-trip proof, the delete.
- `restore_output.log` — the restore script's own output: absence re-confirmation, the restore, the
  post-restore round-trip proof.
- `final_fsck_ch1.log` — a live `clickhouse disks --disk ca_ro --query "cas-fsck --detail"` against
  the still-running cluster after clearance, launched as a closing health check. **Not required by
  the criterion** (the round-level `system.cas_gc_log` evidence above already proves the per-family
  suppression and clearance on its own); included as a health check only. It completed with
  `dangling=0 chain_broken=0 lifeless_keys=0` — the injection left no data-loss or chain-corruption
  finding — but `stale_edge=3279` nonzero (the summary line's own tail: `reachable=24527 dangling=0
  unreachable=210051 pending_gc=85804 awaiting_gc=14423 unaccounted=3181 stale_edge=3279
  corrupted_runs=0 chain_broken=0 lifeless_keys=0`). This run was killed with `SIGTERM` right after
  capturing the clearing round, without ever reaching the harness's own write-quiescing checkpoint
  the way the Stage-B specimen does before every `cas-fsck` it trusts for criterion 2 — so this fsck
  ran against a pool with a large, still-draining insert/backlog in flight, not a quiesced one. That
  is a plausible, not confirmed, explanation for the nonzero `stale_edge`; it was not investigated
  further, since criterion 4's own bar (per-family suppression during the hold, resumed work after
  clearance) does not depend on this fsck and is already proven above from `system.cas_gc_log`
  directly. Flagged here rather than left implicit.

## What this does NOT cover

This run used one namespace, one epoch, one witness gap. It says nothing about multiple
simultaneous holds, a hold spanning an epoch crossing, or a hold on the `_ckpt`/catalog path rather
than a `_log` record — those remain untested by this evidence.
