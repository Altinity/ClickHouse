# Scenario suite run history

Every attempted scenario run is appended here (newest at the bottom). `run_dir` is relative to
`scenarios/runs/`. Status is the scenario's overall verdict (`pass` / `fail` / `inconclusive` /
`error`). See the per-run `report.md` for detail.

| started (UTC) | scenario | seed | scale | duration | status | git sha | run_dir | note |
|---|---|---|---|---|---|---|---|---|
| 2026-06-27T20:35:36 | S01 | 7 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T203522_S01_seed7 | S01 ran at a small dev blob size; the memory-materialization risk is best exposed at >= 256 MiB (use --scale ci/full) |
| 2026-06-27T20:44:45 | S01 | 11 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T204416_S01_seed11 | S01 peak RSS grew 384 MiB during a 64 MiB blob upload — investigate Build::putBlob materializing BlobSource into a String |
| 2026-06-27T20:45:00 | S02 | 11 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T204445_S02_seed11 | Node(localhost:8123) HTTP 500: Code: 131. DB::Exception: Too many times to repeat (1048576), maximum is: 1000000: while executing function repeat on arguments toString(modulo(__table1.number, 10_UInt8)) String String(size = 0), 1048576_UInt32 UInt32 Const(size = 0, UInt32(size = 1)). (TOO_LARGE_STRING_SIZE) (version 26.6.1.1) / sql=INSERT INTO s02_first SELECT number AS id, repeat(toString(number % 10), 1048576) AS payload FROM numbers(64) |
| 2026-06-27T20:45:45 | S03 | 11 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T204500_S03_seed11 | forced GC did not drain unreachable to 0: residual=8 (classify object class + prove bounded/expected) |
| 2026-06-27T20:46:20 | S04 | 11 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T204545_S04_seed11 | forced GC did not drain unreachable to 0: residual=112 (classify object class + prove bounded/expected) |
| 2026-06-27T21:03:29 | S01 | 12 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T210259_S01_seed12 |  |
| 2026-06-27T21:03:57 | S02 | 12 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T210329_S02_seed12 |  |
| 2026-06-27T21:04:38 | S03 | 12 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T210357_S03_seed12 | forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 8} |
| 2026-06-27T21:05:16 | S04 | 12 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T210438_S04_seed12 | forced GC left 104 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 36, '_manifests': 68} |
| 2026-06-27T21:11:20 | S03 | 13 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T211033_S03_seed13 | forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 8}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:16:45 | S01 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T211617_S01_seed20 |  |
| 2026-06-27T21:17:11 | S02 | 20 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T211645_S02_seed20 |  |
| 2026-06-27T21:17:53 | S03 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T211711_S03_seed20 |  |
| 2026-06-27T21:18:40 | S04 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T211753_S04_seed20 | forced GC left 112 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 40, '_manifests': 72}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:21:33 | S05 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T211840_S05_seed20 | forced GC left 225 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 225}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:22:17 | S06 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T212133_S06_seed20 | invalid literal for int() with base 10: '2026-06-27 21:June:59' |
| 2026-06-27T21:24:34 | S07 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T212217_S07_seed20 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-06-27T21:31:29 | S08 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T212434_S08_seed20 |  |
| 2026-06-27T21:32:00 | S09 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T213129_S09_seed20 |  |
| 2026-06-27T21:32:27 | S10 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T213200_S10_seed20 |  |
| 2026-06-27T21:33:28 | S11 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T213227_S11_seed20 | forced GC left 183 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 63, '_manifests': 120}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:33:29 | S12 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T213328_S12_seed20 | NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services |
| 2026-06-27T21:34:24 | S13 | 20 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T213329_S13_seed20 |  |
| 2026-06-27T21:38:19 | S14 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T213424_S14_seed20 | forced GC left 166 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 166}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:40:03 | S15 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T213819_S15_seed20 |  |
| 2026-06-27T21:40:20 | S16 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T214003_S16_seed20 | forced_gc_to_fixpoint() got an unexpected keyword argument 'max_rounds'. Did you mean 'max_seconds'? |
| 2026-06-27T21:40:48 | S17 | 20 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T214020_S17_seed20 |  |
| 2026-06-27T21:48:09 | S18 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T214048_S18_seed20 | S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Exception: Support for SYSTEM UNFREEZE query is disabled. You can enable it via 'enable_system_unfreeze' server setting. (SUPPORT_IS_DISABLED) (version 26.6.1.1) / sql=SYSTEM UNFREEZE WITH NAME 's18_snap_20' |
| 2026-06-27T21:48:38 | S19 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T214809_S19_seed20 |  |
| 2026-06-27T21:49:11 | S20 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T214838_S20_seed20 |  |
| 2026-06-27T21:49:38 | S21 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T214911_S21_seed20 |  |
| 2026-06-27T21:49:39 | S22 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T214938_S22_seed20 | NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) between ClickHouse and RustFS; not in the current compose (direct rustfs1 endpoint) |
| 2026-06-27T21:50:22 | S23 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T214939_S23_seed20 |  |
| 2026-06-27T21:50:23 | S24 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T215022_S24_seed20 | NOT RUN — requires a storage_conf disk config with a tiny dedup_cache_bytes; current compose mounts only the default (64 MiB) config — no small-cache variant |
| 2026-06-27T21:50:40 | S25 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T215023_S25_seed20 | Node(localhost:8124) HTTP 404: Code: 81. DB::Exception: Database s25db does not exist. (UNKNOWN_DATABASE) (version 26.6.1.1) / sql=CREATE TABLE s25db.s25_ordinary (id UInt64, payload String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/s25db_s25_ordinary','{replica}') |
| 2026-06-27T21:51:13 | S26 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T215040_S26_seed20 | forced GC left 296 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 63, '_manifests': 233}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:51:14 | S27 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T215113_S27_seed20 | NOT RUN — requires an instrumented object store / proxy that returns duplicate or unstable LIST pages for root-shard token listing; not available with the direct rustfs endpoint |
| 2026-06-27T21:51:42 | S28 | 20 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T215114_S28_seed20 |  |
| 2026-06-27T21:52:09 | S29 | 20 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T215142_S29_seed20 |  |
| 2026-06-27T21:53:03 | S30 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T215209_S30_seed20 | S30 confirmed checklist #6: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations even though no table stayed live — dropNamespace leaves a permanent GC registry entry (monotone fanout). Backlog: namespace registry needs a cleanup/deregister path. |
| 2026-06-27T21:53:48 | S31 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T215303_S31_seed20 | forced GC left 55 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 31, '_manifests': 24}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-06-27T21:54:17 | S32 | 20 | dev | 900s | pass | ae0cc27b1bf5 | 20260627T215348_S32_seed20 |  |
| 2026-06-27T21:54:37 | S33 | 20 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T215417_S33_seed20 | forced_gc_to_fixpoint() got an unexpected keyword argument 'max_rounds'. Did you mean 'max_seconds'? |
| 2026-06-27T22:09:40 | S06 | 21 | dev | 900s | inconclusive | ae0cc27b1bf5 | 20260627T220814_S06_seed21 |  |
| 2026-06-27T22:10:34 | S16 | 21 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T220940_S16_seed21 | GC log has 9 Failed (Error) finish row(s) |
| 2026-06-27T22:11:02 | S25 | 21 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T221034_S25_seed21 | GC log has 1 Failed (Error) finish row(s) |
| 2026-06-27T22:11:35 | S33 | 21 | dev | 900s | fail | ae0cc27b1bf5 | 20260627T221102_S33_seed21 | GC log has 11 Failed (Error) finish row(s) |
| 2026-06-27T22:15:30 | SOAK-4h-chaos | 20260628 | phase3 | 14400s | running | ae0cc27b1bf5 | tmp/soak_4h_20260628T001450.log | existing ca-soak phase-3 chaos soak, 86 faults; metrics in soak_scenario_4h_20260628T001450.db |
| 2026-06-27T22:48:35 | SOAK-4h-chaos | 20260628 | phase3-workers6 | 14400s | aborted | ae0cc27b1bf5 | tmp/soak_4h_20260628T001450.log | workers=6 attempt stopped pre-chaos at ~30min: roots/ grew ~2.4GB/min (scanner-off), would hit the 60GiB watchdog floor (~62min) BEFORE the chaos window starts (96min). Relaunched with workers=2. |
| 2026-06-27T22:48:35 | SOAK-4h-chaos | 20260628 | phase3-workers2 | 14400s | running | ae0cc27b1bf5 | tmp/soak_4h_20260628T004751.log | workers=2 to slow roots/ growth (~0.8GB/min) so the 4h timeline + chaos window fit the disk budget; metrics soak_scenario_4h_20260628T004751.db |
| 2026-06-28T00:48:14 | SOAK-4h-chaos | 20260628 | phase3-workers2 | 14400s | failed | ae0cc27b1bf5 | tmp/soak_4h_20260628T004751.log | ran ~106min: warmup->steady->mutations->ttl_pressure->gc_checkpoint(PASS dangling=0)->chaos(fault#1 rustfs restart). FAILED on soak TTL-band oracle ambiguity in the post-fault recovery checkpoint (row within 10s of TTL boundary; NOT a CA bug; dangling=0 throughout). Did not reach 4h / did not trip watchdog. Stack left up by trap. |
| 2026-06-29T21:54:29 | S01 | 42 | dev | 300s | inconclusive | 911fde499c22 | 20260629T215402_S01_seed42 |  |
| 2026-06-29T21:55:00 | S02 | 42 | dev | 300s | pass | 911fde499c22 | 20260629T215429_S02_seed42 |  |
| 2026-06-29T21:55:42 | S03 | 42 | dev | 300s | pass | 911fde499c22 | 20260629T215500_S03_seed42 |  |
| 2026-06-29T21:56:28 | S04 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T215542_S04_seed42 | GC log has 12 Failed (Error) finish row(s) |
| 2026-06-29T22:02:09 | S05 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T215628_S05_seed42 | GC log has 13 Failed (Error) finish row(s) |
| 2026-06-29T22:05:25 | S06 | 42 | dev | 300s | inconclusive | 911fde499c22 | 20260629T220209_S06_seed42 |  |
| 2026-06-29T22:07:43 | S07 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T220525_S07_seed42 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-06-29T22:14:23 | S08 | 42 | dev | 300s | inconclusive | 911fde499c22 | 20260629T220743_S08_seed42 |  |
| 2026-06-29T22:14:51 | S09 | 42 | dev | 300s | inconclusive | 911fde499c22 | 20260629T221423_S09_seed42 |  |
| 2026-06-29T22:15:18 | S10 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T221451_S10_seed42 | GC log has 2 Failed (Error) finish row(s) |
| 2026-06-29T22:16:10 | S11 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T221518_S11_seed42 |  |
| 2026-06-29T22:16:11 | S12 | 42 | dev | 300s | inconclusive | 911fde499c22 | 20260629T221610_S12_seed42 | NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services |
| 2026-06-29T22:24:43 | S13 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T221611_S13_seed42 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-06-29T22:31:45 | S14 | 42 | dev | 300s | fail | 911fde499c22 | 20260629T222443_S14_seed42 | GC log has 10 Failed (Error) finish row(s) |
| 2026-06-29T23:26:19 | S01 | 7 | dev | 300s | inconclusive | 911fde499c22 | 20260629T232551_S01_seed7 |  |
| 2026-06-29T23:26:48 | S02 | 7 | dev | 300s | pass | 911fde499c22 | 20260629T232619_S02_seed7 |  |
| 2026-06-29T23:27:30 | S03 | 7 | dev | 300s | pass | 911fde499c22 | 20260629T232648_S03_seed7 |  |
| 2026-06-29T23:28:18 | S04 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T232730_S04_seed7 | GC log has 13 Failed (Error) finish row(s) |
| 2026-06-29T23:34:31 | S05 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T232818_S05_seed7 | GC log has 16 Failed (Error) finish row(s) |
| 2026-06-29T23:36:29 | S06 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T233431_S06_seed7 | GC log has 1 Failed (Error) finish row(s) |
| 2026-06-29T23:38:37 | S07 | 7 | dev | 300s | inconclusive | 911fde499c22 | 20260629T233629_S07_seed7 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-06-29T23:45:18 | S08 | 7 | dev | 300s | inconclusive | 911fde499c22 | 20260629T233837_S08_seed7 |  |
| 2026-06-29T23:45:47 | S09 | 7 | dev | 300s | inconclusive | 911fde499c22 | 20260629T234518_S09_seed7 |  |
| 2026-06-29T23:46:18 | S10 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T234547_S10_seed7 | GC log has 1 Failed (Error) finish row(s) |
| 2026-06-29T23:47:08 | S11 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T234618_S11_seed7 |  |
| 2026-06-29T23:47:08 | S12 | 7 | dev | 300s | inconclusive | 911fde499c22 | 20260629T234708_S12_seed7 | NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services |
| 2026-06-29T23:55:41 | S13 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T234708_S13_seed7 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-06-30T00:01:56 | S14 | 7 | dev | 300s | fail | 911fde499c22 | 20260629T235541_S14_seed7 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-01T09:48:35 | S33 | 20260701 | dev | 600s | fail | d6604883f2ba | 20260701T094759_S33_seed20260701 | GC log has 1 Failed (Error) finish row(s) |
| 2026-07-01T09:50:21 | S04 | 20260701 | dev | 600s | fail | d6604883f2ba | 20260701T094933_S04_seed20260701 | GC log has 4 Failed (Error) finish row(s) |
| 2026-07-01T09:56:52 | S05 | 20260701 | dev | 600s | fail | d6604883f2ba | 20260701T095021_S05_seed20260701 | GC log has 15 Failed (Error) finish row(s) |
| 2026-07-01T09:57:51 | S03 | 20260701 | dev | 600s | pass | d6604883f2ba | 20260701T095652_S03_seed20260701 |  |
| 2026-07-01T09:58:40 | S11 | 20260701 | dev | 600s | pass | d6604883f2ba | 20260701T095751_S11_seed20260701 |  |
| 2026-07-01T10:17:24 | S33 | 20260701 | dev | 600s | pass | d6604883f2ba | 20260701T101634_S33_seed20260701 |  |
| 2026-07-01T13:36:14 | S04 | 20260701 | dev | 600s | fail | cb3aefb1a0eb | 20260701T133524_S04_seed20260701 |  |
| 2026-07-01T13:36:59 | S33 | 20260701 | dev | 600s | fail | cb3aefb1a0eb | 20260701T133614_S33_seed20260701 | GC log has 2 real (non-benign) Error finish row(s) |
| 2026-07-01T13:37:40 | S03 | 20260701 | dev | 600s | fail | cb3aefb1a0eb | 20260701T133659_S03_seed20260701 | GC log has 1 real (non-benign) Error finish row(s) |
| 2026-07-01T13:38:29 | S11 | 20260701 | dev | 600s | pass | cb3aefb1a0eb | 20260701T133740_S11_seed20260701 |  |
| 2026-07-01T13:51:56 | S05 | 20260701 | dev | 600s | pass | c7d94e518178 | 20260701T134627_S05_seed20260701 |  |
| 2026-07-01T14:13:47 | S04 | 20260701 | dev | 600s | pass | c7d94e518178 | 20260701T141253_S04_seed20260701 |  |
| 2026-07-01T14:20:01 | S05 | 20260701 | dev | 600s | pass | c7d94e518178 | 20260701T141347_S05_seed20260701 |  |
| 2026-07-01T14:20:52 | S33 | 20260701 | dev | 600s | pass | c7d94e518178 | 20260701T142001_S33_seed20260701 |  |
| 2026-07-01T14:21:33 | S03 | 20260701 | dev | 600s | pass | c7d94e518178 | 20260701T142052_S03_seed20260701 |  |
| 2026-07-01T22:50:16 | S30 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T224936_S30_seed20260702 |  |
| 2026-07-01T22:51:19 | S34 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T225016_S34_seed20260702 |  |
| 2026-07-01T22:51:58 | S35 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T225119_S35_seed20260702 |  |
| 2026-07-01T22:59:32 | S01 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T225905_S01_seed20260702 |  |
| 2026-07-01T22:59:57 | S02 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T225932_S02_seed20260702 |  |
| 2026-07-01T23:00:38 | S03 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T225957_S03_seed20260702 |  |
| 2026-07-01T23:01:17 | S04 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T230038_S04_seed20260702 |  |
| 2026-07-01T23:03:17 | S05 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T230117_S05_seed20260702 |  |
| 2026-07-01T23:04:47 | S06 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T230317_S06_seed20260702 |  |
| 2026-07-01T23:07:14 | S07 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T230447_S07_seed20260702 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-01T23:13:53 | S08 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T230714_S08_seed20260702 |  |
| 2026-07-01T23:14:19 | S09 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T231353_S09_seed20260702 |  |
| 2026-07-01T23:14:45 | S10 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T231419_S10_seed20260702 |  |
| 2026-07-01T23:15:30 | S11 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T231445_S11_seed20260702 |  |
| 2026-07-01T23:24:02 | S13 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T231530_S13_seed20260702 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-01T23:25:26 | S14 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T232402_S14_seed20260702 |  |
| 2026-07-01T23:31:26 | S15 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T232526_S15_seed20260702 |  |
| 2026-07-01T23:32:19 | S16 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T233126_S16_seed20260702 |  |
| 2026-07-01T23:32:45 | S17 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T233219_S17_seed20260702 |  |
| 2026-07-01T23:33:13 | S18 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T233245_S18_seed20260702 | S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Exception: Support for SYSTEM UNFREEZE query is disabled. You can enable it via 'enable_system_unfreeze' server setting. (SUPPORT_IS_DISABLED) (version 26.6.1.1) / sql=SYSTEM UNFREEZE WITH NAME 's18_snap_20260702' |
| 2026-07-01T23:33:39 | S19 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233313_S19_seed20260702 |  |
| 2026-07-01T23:34:08 | S20 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233339_S20_seed20260702 |  |
| 2026-07-01T23:34:35 | S21 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233408_S21_seed20260702 |  |
| 2026-07-01T23:35:18 | S23 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233435_S23_seed20260702 |  |
| 2026-07-01T23:35:46 | S24 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T233518_S24_seed20260702 |  |
| 2026-07-01T23:36:19 | S25 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T233546_S25_seed20260702 |  |
| 2026-07-01T23:36:45 | S26 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233619_S26_seed20260702 |  |
| 2026-07-01T23:37:11 | S28 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T233645_S28_seed20260702 |  |
| 2026-07-01T23:37:39 | S29 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260701T233711_S29_seed20260702 |  |
| 2026-07-01T23:38:16 | S30 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T233739_S30_seed20260702 |  |
| 2026-07-01T23:43:26 | S31 | 20260702 | dev | 900s | fail | fb5934de521b | 20260701T233816_S31_seed20260702 | cluster did not become healthy after reset |
| 2026-07-01T23:43:47 | S32 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T234326_S32_seed20260702 |  |
| 2026-07-01T23:44:17 | S33 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T234347_S33_seed20260702 |  |
| 2026-07-01T23:45:05 | S34 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T234417_S34_seed20260702 |  |
| 2026-07-01T23:45:47 | S35 | 20260702 | dev | 900s | pass | fb5934de521b | 20260701T234505_S35_seed20260702 |  |
| 2026-07-02T05:51:40 | S23 | 20260702 | dev | 900s | fail | fb5934de521b | 20260702T055056_S23_seed20260702 |  |
| 2026-07-02T05:53:37 | S23 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260702T055254_S23_seed20260702 |  |
| 2026-07-02T05:54:24 | S19 | 20260702 | dev | 900s | pass | fb5934de521b | 20260702T055355_S19_seed20260702 |  |
| 2026-07-02T05:55:00 | S20 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260702T055430_S20_seed20260702 |  |
| 2026-07-02T05:55:37 | S21 | 20260702 | dev | 900s | inconclusive | fb5934de521b | 20260702T055512_S21_seed20260702 |  |
| 2026-07-02T05:56:17 | S26 | 20260702 | dev | 900s | pass | fb5934de521b | 20260702T055551_S26_seed20260702 |  |
| 2026-07-02T06:01:31 | S31 | 20260702 | dev | 900s | fail | fb5934de521b | 20260702T055623_S31_seed20260702 | cluster did not become healthy after reset |
| 2026-07-02T06:03:53 | S31 | 20260702 | dev | 900s | fail | fb5934de521b | 20260702T060328_S31_seed20260702 | ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 0 but GC reclaimed ~40 (checklist #9). previewDeletes should iterate all target shards, not just shard 0. |
| 2026-07-02T06:12:51 | S13 | 20260702 | dev | 900s | fail | fb5934de521b | 20260702T060416_S13_seed20260702 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-02T06:14:56 | S10 | 20260702 | dev | 900s | fail | fb5934de521b | 20260702T061431_S10_seed20260702 |  |
| 2026-07-02T06:17:28 | S10 | 20260702 | dev | 900s | inconclusive | 3a054b9ffe67 | 20260702T061700_S10_seed20260702 |  |
