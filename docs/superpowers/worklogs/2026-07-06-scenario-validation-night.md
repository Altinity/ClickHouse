---
description: 'Unattended night scenario-validation run of the CAS MergeTree feature — objective findings (correctness, anomalies, resource/S3 budget, performance) per scenario, from Task 6 (mount-lease live validation) and S12 onward.'
sidebar_label: 'Scenario validation night 2026-07-06'
sidebar_position: 99
slug: /superpowers/cas/worklogs/2026-07-06-scenario-validation-night
title: 'CAS scenario-validation night (2026-07-06 → 07)'
doc_type: 'guide'
---

# CAS scenario-validation night (2026-07-06 → 2026-07-07) {#night}

Binary under test: HEAD `ee63c36740e` (P3.1 mount-lease fence recovery + lease/view-sync decouple + Phase-4 Lever A GC round skip-unchanged, all landed). Stand: local `utils/ca-soak` (bind-mounts `build/programs/clickhouse`). Objective: anomalies in the WHOLE feature, not just the last increment — correctness, resource use, unjustified S3 budget, performance. No sugarcoating.

## Task 6 — mount-lease fence-recovery live validation {#task6}

**What it validates:** the P3.1 fix — a crash-killed writer whose mount lease is GC-fenced must recover as a FRESH incarnation (higher `writer_epoch`) on restart, never wedge with the "foreign writer" `LOGICAL_ERROR` (exit 49) that was the original P1 bug.

### Finding F1 (blocker, found first) — `ca_ro` read-only disk breaks table load on restart {#f1}

Before the fence-recovery could be end-to-end validated, a **general, severe stand bug** surfaced and had to be fixed to run ANY restart-based scenario (S13/S14 included):

- **Symptom:** a fresh, simple `MergeTree` table (1000 rows, no merge) fails to reload after a **graceful** server restart: `Part all_1_1_0 was found on disk 'ca_ro' which is not defined in the storage policy 'ca'` → `UNKNOWN_DISK` → `ASYNC_LOAD_WAIT_FAILED`. Reproduced on both hard-kill (t6) and graceful restart (t_probe). The server comes up (ping Ok) but the table is stuck in failed-load.
- **Mechanism:** the default `storage_conf_ch{1,2}.xml` defined a `ca_ro` read-only CA disk over the SAME pool + same `server_root_id` as the writable `ca`. MergeTree part discovery finds every part on BOTH disks; since `ca_ro` isn't in policy `ca`, load fails. This is a genuine ClickHouse × CAS × two-same-pool-disks interaction, **already a known ROADMAP prod-gate item** ("Read-only fsck shadow disk breaks table load on restart", hit on the GCS stand 2026-07-03).
- **Why it wasn't caught:** `ca_ro` was embedded in the RustFS server config only on 2026-07-03 02:04 (`cbe0ffb7608`); the passing S13 runs predate it. It answers that ROADMAP row's open triage question — the RustFS stand DOES hit it; it was just never re-run. (I initially mis-blamed the phase-1 soak's earlier occurrence on a "stale leftover" table — wrong; it is this, on any restart.)
- **Fix (stand workaround, the GCS pattern propagated):** `ca_ro` removed from `storage_conf_ch{1,2}.xml`; moved to standalone `configs/fsck_only_ca.xml` mounted at `/etc/clickhouse-server/fsck-only.xml` (outside `config.d`, so the server ignores it); `soak/fsck.py` points `clickhouse disks -C` there; `docker-compose.yml` mounts it on both nodes. **Smoke-tested:** post-fix `system.disks` = `{ca, default}` (no ca_ro on the server), a fresh table survives a graceful restart (1000 rows), and fsck via the standalone config works (exit=0, dangling=0). The PRODUCT fix (part discovery skipping readonly same-pool disks) remains OPEN in the ROADMAP; the `10replicas`/`gc_shards2`/`awss3` configs still embed `ca_ro` and need the same treatment before their restart scenarios run.
- **Severity:** stand-config blocker (fixed for the default stand); underlying product interaction = prod-gate, tracked in ROADMAP.

### Fence-recovery cycle (crash-kill → fence → recover) — PASS

On the pre-F1-fix stand (the fence-recovery path itself is independent of F1):
- Baseline: ch1 mount `writer_epoch=1, seq=7, state=live`, TTL 30s.
- Hard-kill ch1 (`docker kill -s KILL`, lease left to expire), wait 90s. ch2 stole GC leadership and **fenced ch1's expired mount**: `gc_fenced=1, state='fenced'`, `fence_outs=1` over the window, honest floor reason. The live gc log also showed the **Phase-4 skip-unchanged DEFER firing in production** (`outcome=deferred` "re-adopting the sealed generation") on the idle rounds while R1's ack-floor fence-out still ran every round — confirming the fence-out happens pre-DEFER.
- Restart ch1 → **back in 2s, recovered as `writer_epoch=2, seq=1, gc_fenced=0, state=live`** (a fence costs an epoch). **Zero** "foreign writer" / exit-49 / self-remount-failed lines dated after the kill (the P1 wedge is gone; the 13 stale "foreign writer" hits in the bind-mounted err.log are all dated 2026-07-03/05 — yesterday's pre-fix runs). Data-integrity check was blocked by F1 (t6 load) — re-validated after the F1 fix via S13.

### S13 full-scale run

(pending — running on the F1-fixed stand)
