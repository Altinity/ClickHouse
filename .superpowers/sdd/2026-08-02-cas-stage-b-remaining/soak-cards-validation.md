# Soak cards S44/S45 validation {#soak-cards-validation}

Worktree: `/home/mfilimonov/workspace/ClickHouse/lane-g`, branch `laneg/soak-cards` (based on
`d7673bd9ede`). Validates the two draft soak scenario cards from `draft/t8` (T8's E4 deliverable,
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t8-draft-report.md` §E):
`s44_rebirth_namespace_file_readers.py` and `s45_decommission_hidden_removing.py`. Only the two card
files and the `cards/__init__.py` registration were pulled in (checked out from `draft/t8`, not
cherry-picked) — the rest of that commit (RESULTS skeleton, gtest edits, deferred-docs entry) is out
of scope for this validation pass.

## Static

- `python3 -m pytest utils/ca-soak/tests -q` — 290 passed, both before and after every fix below.
- `python3 -m scenarios.run --list` — both `S44` and `S45` enumerate.
- `python3 -c "from scenarios.cards import s44_rebirth_namespace_file_readers, s45_decommission_hidden_removing"` — imports clean.

## S44 — rebirth adversarial with concurrent namespace-file (mutation) readers/writers

**PASS on the first live run**, no fixes needed. Smoke: `python3 -m scenarios.run --scenario S44
--seed 1 --duration 5m` (scale `dev`; the card's own `param_table` drives cycle count, not
`--duration` — same idiom as `S34`/`S35`/`S43`, which also ignore `--duration`).

Log: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/soakcard_s44.log`. Run artifacts:
`utils/ca-soak/scenarios/runs/20260803T112117_S44_seed1/`.

5/5 verdicts passed:
- no unexpected mutation errors across incarnation boundaries (54 mutations applied across 6
  drop/recreate cycles, zero errors beyond the expected drop-window `UNKNOWN_TABLE`);
- recreate latency stable (first-half avg 2.349s, second-half avg 2.347s — not growing);
- `CasRefApplyPoisoned` and `CasRefRecoveryStreamHole` stayed at 0;
- final `fsck --detail` clean (`dangling=0`).

This confirms the card's own mutation-stream SQL-level proxy for a "namespace-file reader/writer in
flight across an incarnation boundary" mechanically works: the writer thread ran concurrently through
every churn cycle without interference. The T8 draft's own disclosed open question — whether this
proxy is a *fully faithful* stand-in for a raw namespace-file reader/writer, versus only exercising
the same `writeFile` code path from one call site — is a design judgment this validation pass does not
resolve; it confirms the card runs and its assertions execute, which is this pass's charge.

## S45 — decommission a victim member with hidden Removing catalog entries

**FAILED on the first live run** (real defects, not flakes) and needed three fix rounds before a
clean pass. Each fix is disclosed in the card's own docstring (`s45_decommission_hidden_removing.py`
top, "Gaps closed at validation time" items 2, 4, 5, 6); summarized here:

### Round 1 (static review, before any run)

The draft hardcoded `--config-file /etc/clickhouse-server/config.xml` for the `ca-drop-member`
invocation. Reading `configs/fsck_only_ca.xml` and `soak/fsck.py`'s own `_CLICKHOUSE_DISKS` showed the
`ca_ro` disk is defined ONLY in the standalone fsck-only config, deliberately kept out of the server's
own `config.d` (a `ca_ro` disk in the server config breaks table load on restart with `UNKNOWN_DISK`).
Fixed by importing `soak.fsck._CLICKHOUSE_DISKS` instead of duplicating (and mis-stating) the
invocation shape — this would have failed every run with `UNKNOWN_DISK` had it not been caught before
the first live attempt.

### Round 2 (seed 1 run — real failure)

`ca-drop-member` refused immediately after `docker kill`:
```
DB::Exception: CAS decommission 'ca_soak_ch2': pool member is alive or contended — mount lease held
by uuid=... epoch=1 pid=1 hostname=... (expires_at_ms=...). Refusing (no FORCE variant exists; stop
the server or wait for its lease to lapse).
```
Root cause (confirmed by reading `CasPool.h`/`CasMountRuntime.h`: `mount_lease_ttl_ms{30000}`,
`mount_renew_period{10000}`, and `CasPool.cpp`'s `mountWritable`): `docker kill` does not shorten the
lease the victim already renewed before dying, and the decommission mount path
(`openForDecommission` → `mountWritable`) takes one unbounded-wait-free snapshot compare against
`expires_at_ms` — there is no bounded-wait variant on this call path (only the crash-recovery mount
path has one). `soak/chaos.py`'s `FREEZE_LONG` fault documents the same constraint from the other
side: a frozen replica must be held past TTL (30s) + GC fence margin (15s) ≈ 45s before a GC leader
will fence it. Fixed by replacing the fixed 2s sleep with a bounded poll loop
(`_run_drop_member_after_lease_lapses`, 60s bound / 5s interval) that only retries on the specific
"alive or contended" refusal — any other failure returns immediately, so a real defect cannot be
masked as a timing flake.

Log: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/soakcard_s45.log`.

### Round 3 (seed 2 run — real failure)

`ca-drop-member` now exited 0, but reported `namespaces_removed=0` against 3 victim tables — nothing
of the victim's own was found to sweep. `sql.create_ca_table` had only been called against `node1`;
a `ReplicatedMergeTree` materializes per-replica (each replica runs its own `CREATE` against the
shared zk path), so the victim (`node2`) never had a local table at all, hence no local namespace
under its own srid. Fixed by creating each table on both replicas.

Log: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/soakcard_s45_v2.log`.

### Round 4 (seed 3 run — real failure, exposed by round 3's fix)

With the table now created on both replicas, `SYSTEM SYNC REPLICA` on the victim raised
`UNKNOWN_TABLE` — the victim's own replica hadn't been created before the sync/drop sequence ran
(a stale reference to `node`-only creation still present at that point in the card). This surfaced
the deeper issue: the `DROP TABLE ... SYNC` calls were also `node1`-only, and a `ReplicatedMergeTree`
`DROP` is per-replica, not automatically cluster-wide — the victim's own local replica would have
stayed `Live` (ATTACHED), never entering `Removing`, which defeats the entire premise of "hidden
Removing entries." Fixed by dropping on both `node` and `victim`, with the victim's own drop's return
awaited before the kill.

Log: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/soakcard_s45_v3.log`.

### Round 5 (seed 4 run — clean PASS)

3/3 verdicts passed:
- `ca-drop-member` exit code 0;
- `namespaces_removed=3` (matches `victim_tables=3` exactly — the hidden `Removing` rows were
  genuinely swept, not a zero-vs-zero coincidence like round 3's failure);
- final `fsck` clean (`dangling=0`).

The tool's own stdout also surfaced a real, benign finding worth recording: several `CAS orphan
sweep: retained ...` warnings, each citing "epoch 1's closing seal is not consumed: the sealed
cursor is at ..., so a grant naming this build may still be unfolded above it" — the decommission
tool correctly declines to unconditionally delete manifests from a still-open epoch, leaving that to
the normal GC path rather than the destructive `ca-drop-member` sweep. `fsck` showed nonzero
`unreachable` (the expected pre-GC state those warnings describe) alongside `dangling=0`, i.e. no
loss.

Log: `/home/mfilimonov/workspace/ClickHouse/lane-g/build/soakcard_s45_v4.log`. Run artifacts:
`utils/ca-soak/scenarios/runs/20260803T112949_S45_seed4/`.

## Disclosed scope limits

- Only a 5-minute-`dev`-scale smoke of each card was run (per the dispatch — "a SMOKE of the cards'
  machinery, not the T8 soak"), not the full `ci`/`full`-scale run T8's own soak plan calls for. S44's
  actual run time was ~28s (`dev` scale, 6 cycles); S45's was ~75s (`dev` scale, dominated by the
  60s lease-wait bound).
- The victim `server_root_id` (`ca_soak_ch2`) is still hardcoded in S45; a different compose variant
  needs that constant updated (disclosed in the card's own docstring, not fixed here — out of scope
  for a default-compose smoke).
- S44's SQL-level mutation-proxy faithfulness question (is it truly equivalent to a raw
  namespace-file reader/writer, not just another `writeFile` call site) is disclosed, not resolved,
  per above.
