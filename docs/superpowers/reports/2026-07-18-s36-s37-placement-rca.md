# RCA: S36 / S37 disk-placement failures (R5 sweep, ci scale, 2026-07-18)

## Verdict

**Card scale-parameter bug — NOT a product routing regression and NOT an environment/compose problem.**
Both failures are the same root cause: at `ci` (and `full`) scale the per-part byte size the cards
generate **exceeds the `max_data_part_size_bytes = 4 MiB` routing threshold** on the `hot` volume of
the `ca_local` / `ca_local3` policies, so ClickHouse correctly routes those parts straight to the
`ca` volume instead of the local disk the placement checks assume. Only the `dev` scale keeps parts
under 4 MiB, and every prior GREEN run of these cards was at `dev` scale. The R5 sweep is the first
time either card ran at `ci` scale.

## Evidence

Routing threshold (`configs/storage_conf_multidisk_ch{1,2}.xml`, both policies): `max_data_part_size_bytes = 4194304` (4 MiB).

| scale | S36 prefill part = `rows_per_part*payload_bytes` | S37 mixed part = `mixed_rows*mixed_payload_bytes` |
|-------|--------------------------------------------------|---------------------------------------------------|
| dev   | 300*1024 = **0.29 MiB** → local1 ✓               | 60*65536 = **3.75 MiB** → local1/local2 ✓         |
| ci    | 3000*2048 = **5.86 MiB** → **ca** ✗              | 120*131072 = **15.0 MiB** → **ca** ✗              |
| full  | 20000*4096 = **78 MiB** → **ca** ✗               | 400*262144 = **100 MiB** → **ca** ✗               |

- **S36** (run `20260718T002431_S36_seed1`): prefill part `0_0_0_0` at 5.86 MiB routed to `ca`; the
  "initial parts land on the local disk (below the routing threshold)" check FAILED, then the first
  `MOVE PART ... TO DISK 'ca'` threw `Code 479 ... Part '0_0_0_0' is already on disk 'ca'`.
- **S37** (run `20260718T002448_S37_seed1`, `report.json`): the multidisk config was **definitely
  applied** — `system.disks` reported `local1`, `local2`, `ca` with sane space, and **leg 1 passed**
  (small 2 MiB part → `local1`, big 20 MiB part → `ca`). Only leg 4 ("round-robin JBOD spreads
  source parts across local1 AND local2") failed: all six 15 MiB mixed parts landed on `ca` because
  each exceeds the 4 MiB threshold. The scale observation in the same report already prints
  `mixed_bytes_per_part = 15728640, route_threshold_bytes = 4194304`.

Prior verdicts in `RUN_HISTORY.md`: every green S36 (07-17 00:43, 09:57) and S37 (07-17 10:57,
11:01, 17:38) ran at `dev`; rows 415-416 (the two failures) are the first `ci`-scale runs.

## The exact divergence

Not a config, storage.xml, policy-name, or disk-order difference — the multidisk policy is identical
and correct across runs (routing verified working). The divergence is **scale**: `dev` → `ci`. The
`ci`/`full` param rows in `cards/s36_s37_disk_move.py` (`param_table`) were never made
threshold-aware:
- S36 `param_table["ci"|"full"]` `rows_per_part*payload_bytes` (5.86 / 78 MiB) > 4 MiB.
- S37 `param_table["ci"|"full"]` `mixed_rows*mixed_payload_bytes` (15 / 100 MiB) > 4 MiB.

The S36 dev comment ("well under the 4 MiB routing threshold, so everything lands on local1") and the
S37 dev comment ("~3.75 MiB/part") document the invariant the ci/full rows silently violate.

## Non-causes ruled out

- **Compose/variant**: run.log shows `docker-compose-multidisk.yml up -d`; S37 `system.disks`
  confirms local1/local2/ca were live. The `config/` snapshot in the run dir lists default
  (`storage_conf.xml`/`docker-compose.yml`) names — that is a harness snapshot that always captures a
  fixed default set regardless of variant (cosmetic; worth fixing separately), not proof of the wrong
  config.
- **Concurrent sweep clobber**: the live `ca-soak-ch1-1` is currently a *different* sweep's
  default-config container (only `ca`+`default` policies, `rustfs1` paused). All variants share the
  unpinned `ca-soak` docker project + host ports 8123/9000, so concurrent sweeps *can* stomp each
  other — a real latent hazard, but not this failure: S36/S37 both saw the correct multidisk policy.
- **Product routing regression**: refuted — S37 leg 1 shows `max_data_part_size_bytes` routing works
  exactly right (2 MiB→local1, 20 MiB→ca).

## Fix direction

Make the two placement checks scale-invariant, in `cards/s36_s37_disk_move.py`:
1. Keep the "must land on local first" payloads **under `ROUTE_THRESHOLD_BYTES` (4 MiB) per part at
   every scale** — cap `rows_per_part*payload_bytes` (S36 prefill) and `mixed_rows*mixed_payload_bytes`
   (S37 leg 4) below 4 MiB in the `ci`/`full` rows (scale up part *count*, not per-part *bytes*), OR
2. Raise `max_data_part_size_bytes` on the `hot` volumes so the ci/full per-part sizes stay under it
   (less preferred — it weakens the S37 leg-1 routing test that deliberately relies on the 4 MiB line).

Recommend option 1. Separately (lower priority): pin a per-variant `COMPOSE_PROJECT_NAME` so parallel
sweeps stop sharing containers/ports, and fix the run-dir `config/` snapshot to capture the actual
variant files.
