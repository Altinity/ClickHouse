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
| 2026-07-03T01:24:28 | S01 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012359_S01_seed20260703 |  |
| 2026-07-03T01:24:52 | S02 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T012428_S02_seed20260703 |  |
| 2026-07-03T01:25:32 | S03 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012452_S03_seed20260703 |  |
| 2026-07-03T01:26:14 | S04 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012532_S04_seed20260703 |  |
| 2026-07-03T01:27:34 | S05 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012614_S05_seed20260703 |  |
| 2026-07-03T01:28:54 | S06 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012734_S06_seed20260703 |  |
| 2026-07-03T01:31:00 | S07 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T012854_S07_seed20260703 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-03T01:37:45 | S08 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T013100_S08_seed20260703 |  |
| 2026-07-03T01:38:12 | S09 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T013745_S09_seed20260703 |  |
| 2026-07-03T01:38:38 | S10 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T013812_S10_seed20260703 |  |
| 2026-07-03T01:39:23 | S11 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T013838_S11_seed20260703 |  |
| 2026-07-03T01:39:23 | S12 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T013923_S12_seed20260703 | NOT RUN — docker-compose-10replicas.yml (ch1..ch10) exists; remaining gap: soak/cluster.py Cluster is hardcoded to 2 nodes — needs a multi-node abstraction to address ch3..ch10 (see BACKLOG NEEDS-INFRA-S12) |
| 2026-07-03T01:42:13 | S13 | 20260703 | dev | 900s | fail | 80ab8b69abf3 | 20260703T013923_S13_seed20260703 |  |
| 2026-07-03T01:43:29 | S14 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T014213_S14_seed20260703 |  |
| 2026-07-03T01:45:16 | S15 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014329_S15_seed20260703 |  |
| 2026-07-03T01:46:24 | S16 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014516_S16_seed20260703 |  |
| 2026-07-03T01:46:50 | S17 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T014624_S17_seed20260703 |  |
| 2026-07-03T01:47:23 | S18 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014650_S18_seed20260703 |  |
| 2026-07-03T01:47:51 | S19 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T014723_S19_seed20260703 |  |
| 2026-07-03T01:48:21 | S20 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014751_S20_seed20260703 |  |
| 2026-07-03T01:48:45 | S21 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014821_S21_seed20260703 |  |
| 2026-07-03T01:48:46 | S22 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014845_S22_seed20260703 | NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) between ClickHouse and RustFS; not in the current compose (direct rustfs1 endpoint) |
| 2026-07-03T01:49:27 | S23 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014846_S23_seed20260703 |  |
| 2026-07-03T01:49:53 | S24 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T014927_S24_seed20260703 |  |
| 2026-07-03T01:50:26 | S25 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T014953_S25_seed20260703 |  |
| 2026-07-03T01:50:57 | S26 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015026_S26_seed20260703 |  |
| 2026-07-03T01:50:58 | S27 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015057_S27_seed20260703 | NOT RUN — requires an instrumented object store / proxy that returns duplicate or unstable LIST pages for root-shard token listing; not available with the direct rustfs endpoint |
| 2026-07-03T01:51:23 | S28 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T015058_S28_seed20260703 |  |
| 2026-07-03T01:51:49 | S29 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015123_S29_seed20260703 |  |
| 2026-07-03T01:52:30 | S30 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015149_S30_seed20260703 |  |
| 2026-07-03T01:53:08 | S31 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015230_S31_seed20260703 |  |
| 2026-07-03T01:53:32 | S32 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T015308_S32_seed20260703 |  |
| 2026-07-03T01:54:08 | S33 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T015332_S33_seed20260703 |  |
| 2026-07-03T01:54:53 | S34 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015408_S34_seed20260703 |  |
| 2026-07-03T01:55:36 | S35 | 20260703 | dev | 900s | inconclusive | 80ab8b69abf3 | 20260703T015453_S35_seed20260703 |  |
| 2026-07-03T02:00:37 | S13 | 20260703 | dev | 900s | pass | 80ab8b69abf3 | 20260703T015746_S13_seed20260703 |  |
| 2026-07-03T03:24-03:56 | S01-S35 sweep | 20260703 | dev | 900s | 8 pass / 1 fail (S13) / rest inconclusive (scale- and infra-gated) | night binary (queue+copy-forward-hashfix+clamp-suppression+guard/rebuild) | logs/scenarios_night_sweep.log | ALL seven previously-FAILing scenarios (S10 S19 S20 S21 S23 S26 S31) no longer fail; S19 full PASS |
| 2026-07-03T04:00 | S13 | 20260703 | dev | 900s | pass (11/11) | night binary | logs/s13_retest.log | after the sync-gated oracle fix: the sweep's S13 'divergence' was replication-in-flight (oracle ran before any sync) — no data loss under kill chaos |
| 2026-07-05T17:49:01 | S01 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260705T174845_S01_seed20260703 | Node(localhost:8123) HTTP 500: Code: 241. DB::Exception: (total) memory limit exceeded: would use 128.27 GiB (attempt to allocate chunk of 128.00 GiB), current RSS: 245.26 MiB, maximum: 25.20 GiB. OvercommitTracker decision: Query was selected to stop by OvercommitTracker: while executing 'FUNCTION randomString(8388608_UInt32 :: 2) -> randomString(8388608_UInt32) String : 0'. (MEMORY_LIMIT_EXCEEDED) (version 26.6.1.1) / sql=INSERT INTO s01_huge SELECT 0 + number AS id, randomString(8388608) AS payload FROM numbers(12800) |
| 2026-07-05T19:12:54 | S01 | 20260703 | full | 1200s | pass | 8e6e68504b3e | 20260705T190214_S01_seed20260703 |  |
| 2026-07-05T19:23:03 | S02 | 20260703 | full | 1200s | pass | 8e6e68504b3e | 20260705T192200_S02_seed20260703 |  |
| 2026-07-05T19:46:49 | S02 | 20260703 | full | 1200s | pass | 8e6e68504b3e | 20260705T193807_S02_seed20260703 |  |
| 2026-07-05T20:16:24 | S03 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260705T195416_S03_seed20260703 |  |
| 2026-07-05T20:35:43 | S04 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260705T202340_S04_seed20260703 |  |
| 2026-07-05T21:52:57 | S05 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260705T205639_S05_seed20260703 |  |
| 2026-07-05T21:58:42 | S06 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260705T215757_S06_seed20260703 | S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED |
| 2026-07-05T22:08:16 | S06 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260705T220753_S06_seed20260703 | S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED |
| 2026-07-05T22:39:51 | S06 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260705T222603_S06_seed20260703 |  |
| 2026-07-05T22:49:04 | S07 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260705T224846_S07_seed20260703 | Node(localhost:8123) HTTP 400: Code: 62. DB::Exception: Max query size exceeded (can be increased with the `max_query_size` setting): Syntax error: failed at position 262144 (UI): UI. . (SYNTAX_ERROR) (version 26.6.1.1) / sql=CREATE TABLE s07_capwide (k UInt64, c0 UInt32, c1 UInt32, c2 UInt32, c3 UInt32, c4 UInt32, c5 UInt32, c6 UInt32, c7 UInt32, c8 UInt32, c9 UInt32, c10 UInt32, c11 UInt32, c12 UInt32, c13 UInt32, c14 UI...(288932 more chars) |
| 2026-07-06T01:31:25 | S08 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260705T232733_S08_seed20260703 |  |
| 2026-07-06T01:44:12 | S09 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260706T014159_S09_seed20260703 |  |
| 2026-07-06T02:21:20 | S09 | 20260703 | full | 1200s | pass | 8e6e68504b3e | 20260706T022016_S09_seed20260703 |  |
| 2026-07-06T02:30:46 | S10 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260706T022603_S10_seed20260703 |  |
| 2026-07-06T02:53:39 | S10 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260706T024801_S10_seed20260703 |  |
| 2026-07-06T02:56:25 | S11 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260706T025607_S11_seed20260703 | Node(localhost:8123) HTTP 500: Code: 252. DB::Exception: Too many partitions for single INSERT block (more than 100). The limit is controlled by 'max_partitions_per_insert_block' setting. Large number of partitions is a common misconception. It will lead to severe negative performance impact, including slow server startup, slow INSERT queries and slow SELECT queries. Recommended total number of partitions for a table is under 1000..10000. Please note, that partitioning is not intended to speed up SELECT queries (ORDER BY key is sufficient to make range queries fast). Partitions are intended for data manipulation (DROP PARTITION, etc). (TOO_MANY_PARTS) (version 26.6.1.1) / sql=INSERT INTO s11_buckets SELECT 0 + number AS id, randomString(2048) AS payload, (number % 256) AS bucket FROM numbers(10000) |
| 2026-07-06T05:15:46 | S13 | 20260703 | full | 1200s | fail | 8e6e68504b3e | 20260706T044329_S13_seed20260703 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-06T06:03:45 | S14 | 20260703 | full | 1200s | pass | 8e6e68504b3e | 20260706T052819_S14_seed20260703 |  |
| 2026-07-06T07:00:41 | S15 | 20260703 | full | 1200s | inconclusive | 8e6e68504b3e | 20260706T065553_S15_seed20260703 |  |
| 2026-07-06T08:47:29 | S03 | 20260706 | dev | 900s | pass | f7912a5ed0a3 | 20260706T084644_S03_seed20260706 |  |
| 2026-07-06T21:04:57 | S13 | 20260707 | dev | 900s | inconclusive | 1cab0a2698be | 20260706T210210_S13_seed20260707 |  |
| 2026-07-06T22:25:12 | S14 | 20260707 | dev | 900s | pass | 4bbf68b478b5 | 20260706T222403_S14_seed20260707 |  |
| 2026-07-06T23:01:52 | S14 | 20260707 | full | 1200s | pass | 4bbf68b478b5 | 20260706T222641_S14_seed20260707 |  |
| 2026-07-06T23:04:37 | S15 | 20260707 | dev | 900s | fail | a3fc07430f5d | 20260706T230317_S15_seed20260707 |  |
| 2026-07-06T23:08:29 | S15 | 20260707 | dev | 900s | inconclusive | a3fc07430f5d | 20260706T230657_S15_seed20260707 |  |
| 2026-07-06T23:10:39 | S16 | 20260707 | dev | 900s | inconclusive | 6a4dfebbe5d7 | 20260706T230941_S16_seed20260707 |  |
| 2026-07-06T23:12:18 | S17 | 20260707 | dev | 900s | pass | 0552b7032282 | 20260706T231157_S17_seed20260707 |  |
| 2026-07-06T23:13:50 | S18 | 20260707 | dev | 900s | fail | e97858e63ebc | 20260706T231321_S18_seed20260707 | GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/01/01072cc50e01979bd48c985b2719ee8c', 'soak_pool/blobs/01/01c16d4da5bf1ada12a2024ca8591c4c', 'soak_pool/blobs/06/06d01b256bb15321515b1c38254ff56e', 'soak_pool/blobs/06/06eebc04b7f90340adf03dbc86868b02', 'soak_pool/blobs/07/0717efb8c793beebddb325cba8d076da', 'soak_pool/blobs/0f/0fca7b1e1f16c9752ba3f714aecb3c2c', 'soak_pool/blobs/12/12d68cf72c2f6217b3ca85ffb2fae4fe', 'soak_pool/blobs/14/14efb9d2dfe01430a62cd064e40fc318', 'soak_pool/blobs/15/151ef3fcaa9bd70cf26a36132b2432a8', 'soak_pool/blobs/1d/1d60ba4b3f5540694e218b5902602f41'] |
| 2026-07-06T23:17:58 | S19 | 20260707 | dev | 900s | fail | 6518efddb5f5 | 20260706T231738_S19_seed20260707 |  |
| 2026-07-06T23:20:13 | S20 | 20260707 | dev | 900s | inconclusive | b88125257280 | 20260706T231948_S20_seed20260707 |  |
| 2026-07-06T23:21:15 | S21 | 20260707 | dev | 900s | inconclusive | 43e06f832d45 | 20260706T232055_S21_seed20260707 |  |
| 2026-07-06T23:22:51 | S23 | 20260707 | dev | 900s | inconclusive | dbe0556e14e4 | 20260706T232213_S23_seed20260707 |  |
| 2026-07-06T23:24:15 | S24 | 20260707 | dev | 900s | fail | dbe0556e14e4 | 20260706T232353_S24_seed20260707 |  |
| 2026-07-06T23:26:27 | S25 | 20260707 | dev | 900s | fail | e5d6aa785cbf | 20260706T232602_S25_seed20260707 | GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/00/00000000000000000000000000000000', 'soak_pool/blobs/07/07596c79b6ee9d57c99a5e7272902c3f', 'soak_pool/blobs/0d/0d6b60b1f3397793f1c5f54f78326e1d', 'soak_pool/blobs/1b/1b243a06671e1270cd076b0a901ad65a', 'soak_pool/blobs/38/38aa643fcf9332594e0166ac106170b9', 'soak_pool/blobs/a3/a3af5524c8b55aa3cb374c923706ae39', 'soak_pool/blobs/c7/c7dedcceb2f845ee7ffe17e48ce96c0f', 'soak_pool/blobs/e1/e19d3c9977e508b0824410174ef10166', 'soak_pool/blobs/fb/fb85b48a48b6dbb3617b8ec2e460483b', 'soak_pool/blobs/fd/fd082a9a2007ea9bb93102b15e1a8f33'] |
| 2026-07-06T23:28:39 | S26 | 20260707 | dev | 900s | fail | 141030b27936 | 20260706T232811_S26_seed20260707 | GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/0c/0cb03f16cfebaccc7750d4ca40ebc188', 'soak_pool/blobs/0f/0f2b0b701c916b38c32dcbd42bfd1be1', 'soak_pool/blobs/12/121a73e8a6b09205e6fb7fa75e5bf273', 'soak_pool/blobs/14/14174d2098c21591e0d4781382e7ce35', 'soak_pool/blobs/17/17147dcd91c5dcccb77fc50fe576ada1', 'soak_pool/blobs/1f/1fe03c0fb542471b31f32b62a54917c4', 'soak_pool/blobs/20/200d0e4f3db020618ce4eaca85fa3006', 'soak_pool/blobs/20/20225823b00d4d034be7ed1125075e28', 'soak_pool/blobs/21/21d8e7ce195f9b2f875908e0746e800b', 'soak_pool/blobs/25/25892a5e81965b3a3c2e9e17868966c1'] |
| 2026-07-06T23:30:04 | S28 | 20260707 | dev | 900s | pass | 6d701167c8a7 | 20260706T232943_S28_seed20260707 |  |
| 2026-07-06T23:31:21 | S29 | 20260707 | dev | 900s | inconclusive | 2fadf5a6bce9 | 20260706T233101_S29_seed20260707 |  |
| 2026-07-06T23:32:38 | S30 | 20260707 | dev | 900s | fail | b059d6edc181 | 20260706T233201_S30_seed20260707 | S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated. |
| 2026-07-06T23:34:54 | S31 | 20260707 | dev | 900s | inconclusive | 121562406653 | 20260706T233410_S31_seed20260707 |  |
| 2026-07-06T23:36:01 | S32 | 20260707 | dev | 900s | pass | 44338b6ba967 | 20260706T233542_S32_seed20260707 |  |
| 2026-07-06T23:37:39 | S33 | 20260707 | dev | 900s | fail | 716462053388 | 20260706T233703_S33_seed20260707 | GC dry-run proposed deleting 34 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/07/070323332e252eb0620007c0728aa372', 'soak_pool/blobs/10/1000348b3e9635e7adef8c91774d6747', 'soak_pool/blobs/19/19fe4b0496717c2cd3cdbe977451fd62', 'soak_pool/blobs/1a/1a742de0de639e55306b48fba511985d', 'soak_pool/blobs/20/200961efba8a14242a2d97a83c2fdfb2', 'soak_pool/blobs/24/24a908bed8ddfacfc5aaf7cc96a8f01d', 'soak_pool/blobs/2f/2f5e04d458d0eebfeea76204b7228ea2', 'soak_pool/blobs/35/35cb2108cb4974f86801b513f7e33b08', 'soak_pool/blobs/3a/3ac9f384f1f74e0a14be1aa360c192a4', 'soak_pool/blobs/44/440730c92b5561bda171664ea355263a'] |
| 2026-07-06T23:39:55 | S34 | 20260707 | dev | 900s | fail | 288e39b51431 | 20260706T233911_S34_seed20260707 | S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRootGet first=32 -> last=248, root_dirs 2 -> 2) — D1 should have eliminated the monotone namespace registry; investigate dropNamespace / tombstone GC reclaim path |
| 2026-07-06T23:44:31 | S35 | 20260707 | dev | 900s | inconclusive | 043db564345c | 20260706T234353_S35_seed20260707 |  |
| 2026-07-07T06:14:00 | S34 | 20260707 | dev | 900s | fail | 8682956258e1 | 20260707T061202_S34_seed20260707 | S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRootGet first=0 -> last=214, root_dirs 2 -> 2) — D1 should have eliminated the monotone namespace registry; investigate dropNamespace / tombstone GC reclaim path |
| 2026-07-07T06:16:21 | S34 | 20260707 | dev | 900s | inconclusive | 8682956258e1 | 20260707T061536_S34_seed20260707 |  |
| 2026-07-07T06:19:10 | S34 | 20260707 | dev | 900s | inconclusive | 8682956258e1 | 20260707T061822_S34_seed20260707 |  |
| 2026-07-07T06:34:05 | S12 | 20260707 | dev | 900s | fail | 6bb7ae5caa04 | 20260707T063228_S12_seed20260707 |  |
| 2026-07-07T06:39:53 | S12 | 20260707 | dev | 900s | pass | 6bb7ae5caa04 | 20260707T063816_S12_seed20260707 |  |
| 2026-07-07T06:48:32 | S22 | 20260707 | dev | 900s | fail | 04ac62b12ca7 | 20260707T064805_S22_seed20260707 | Node(localhost:8123) HTTP 500: Code: 246. DB::Exception: Build: blob object soak_pool/blobs/f2/f2123bb7f1630af47810ea1a47068929 size 0 is below the pool blob header length 256. (CORRUPTED_DATA) (version 26.6.1.1) / sql=INSERT INTO s22_t0 SELECT 0 + number AS id, randomString(4096) AS payload FROM numbers(750) |
| 2026-07-07T06:54:09 | S22 | 20260707 | dev | 900s | pass | 04ac62b12ca7 | 20260707T065316_S22_seed20260707 |  |
| 2026-07-07T06:58:16 | S27 | 20260707 | dev | 900s | inconclusive | 74c8764fc6c7 | 20260707T065708_S27_seed20260707 |  |
| 2026-07-07T07:17:19 | S13 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T071428_S13_seed20260707 | forced GC left 1 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 1}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently. |
| 2026-07-07T07:18:34 | S14 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T071719_S14_seed20260707 |  |
| 2026-07-07T07:20:13 | S15 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T071834_S15_seed20260707 |  |
| 2026-07-07T07:21:16 | S16 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072013_S16_seed20260707 |  |
| 2026-07-07T07:21:44 | S17 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T072116_S17_seed20260707 |  |
| 2026-07-07T07:22:17 | S18 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T072144_S18_seed20260707 | GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/01/01072cc50e01979bd48c985b2719ee8c', 'soak_pool/blobs/01/01c16d4da5bf1ada12a2024ca8591c4c', 'soak_pool/blobs/06/06d01b256bb15321515b1c38254ff56e', 'soak_pool/blobs/06/06eebc04b7f90340adf03dbc86868b02', 'soak_pool/blobs/07/0717efb8c793beebddb325cba8d076da', 'soak_pool/blobs/0f/0fca7b1e1f16c9752ba3f714aecb3c2c', 'soak_pool/blobs/12/12d68cf72c2f6217b3ca85ffb2fae4fe', 'soak_pool/blobs/14/14efb9d2dfe01430a62cd064e40fc318', 'soak_pool/blobs/15/151ef3fcaa9bd70cf26a36132b2432a8', 'soak_pool/blobs/1d/1d60ba4b3f5540694e218b5902602f41'] |
| 2026-07-07T07:22:43 | S19 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T072217_S19_seed20260707 |  |
| 2026-07-07T07:23:13 | S20 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072243_S20_seed20260707 |  |
| 2026-07-07T07:23:39 | S21 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072313_S21_seed20260707 |  |
| 2026-07-07T07:24:21 | S23 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072339_S23_seed20260707 |  |
| 2026-07-07T07:24:48 | S24 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T072421_S24_seed20260707 |  |
| 2026-07-07T07:25:18 | S25 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T072448_S25_seed20260707 | GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/00/00000000000000000000000000000000', 'soak_pool/blobs/0d/0d6b60b1f3397793f1c5f54f78326e1d', 'soak_pool/blobs/1b/1b243a06671e1270cd076b0a901ad65a', 'soak_pool/blobs/2f/2f47273814e4e7c29145a9e1543e52fa', 'soak_pool/blobs/9b/9bb486c1ee93987ad634bc7792f24bb3', 'soak_pool/blobs/c7/c7dedcceb2f845ee7ffe17e48ce96c0f', 'soak_pool/blobs/c8/c89a7d919795d0202f37af4ed5930700', 'soak_pool/blobs/d9/d94fa6eb80490d0867c25e78ee8ef02d', 'soak_pool/blobs/fb/fb85b48a48b6dbb3617b8ec2e460483b', 'soak_pool/blobs/fd/fd082a9a2007ea9bb93102b15e1a8f33'] |
| 2026-07-07T07:25:50 | S26 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T072518_S26_seed20260707 | GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/04/040fa185ea949400f8f8f13f41e7a6eb', 'soak_pool/blobs/09/09eceded07bc90a7a9c054998a757811', 'soak_pool/blobs/0a/0a2c77daea60b234a72cd951fecf1fc3', 'soak_pool/blobs/0a/0a8b602963ffd45c68488dbe0db13ee1', 'soak_pool/blobs/0b/0b7d1c9998e7f02fcd3abb72bdf4094a', 'soak_pool/blobs/0f/0f2b0b701c916b38c32dcbd42bfd1be1', 'soak_pool/blobs/11/118b5356bdf4d4b87fc1feab72929d4a', 'soak_pool/blobs/12/121a73e8a6b09205e6fb7fa75e5bf273', 'soak_pool/blobs/16/160493b5223359bae725615333faff0e', 'soak_pool/blobs/18/182467fb900cf2494daae8acee7eab48'] |
| 2026-07-07T07:26:15 | S28 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T072550_S28_seed20260707 |  |
| 2026-07-07T07:26:39 | S29 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072615_S29_seed20260707 |  |
| 2026-07-07T07:27:19 | S30 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T072639_S30_seed20260707 | S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated. |
| 2026-07-07T07:28:08 | S31 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T072719_S31_seed20260707 |  |
| 2026-07-07T07:28:33 | S32 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T072808_S32_seed20260707 |  |
| 2026-07-07T07:31:59 | S30 | 20260707 | dev | 900s | inconclusive | 07d8c37efe45 | 20260707T073118_S30_seed20260707 |  |
| 2026-07-07T07:34:45 | S13 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T073159_S13_seed20260707 |  |
| 2026-07-07T07:35:11 | S19 | 20260707 | dev | 900s | fail | 07d8c37efe45 | 20260707T073445_S19_seed20260707 |  |
| 2026-07-07T07:37:50 | S19 | 20260707 | dev | 900s | pass | 07d8c37efe45 | 20260707T073725_S19_seed20260707 |  |
| 2026-07-07T07:50:22 | S18 | 20260707 | dev | 900s | inconclusive | 228743fab9f9 | 20260707T074948_S18_seed20260707 |  |
| 2026-07-07T07:50:53 | S25 | 20260707 | dev | 900s | inconclusive | 228743fab9f9 | 20260707T075022_S25_seed20260707 |  |
| 2026-07-07T07:51:25 | S26 | 20260707 | dev | 900s | inconclusive | 228743fab9f9 | 20260707T075053_S26_seed20260707 |  |
| 2026-07-07T07:52:14 | S31 | 20260707 | dev | 900s | inconclusive | 228743fab9f9 | 20260707T075125_S31_seed20260707 |  |
| 2026-07-07T07:52:56 | S33 | 20260707 | dev | 900s | inconclusive | 228743fab9f9 | 20260707T075214_S33_seed20260707 |  |
| 2026-07-07T08:56:24 | S25 | 20260707 | dev | 900s | pass | ef6a43a59369 | 20260707T085402_S25_seed20260707 |  |
| 2026-07-07T09:00:58 | S30 | 20260707 | dev | 900s | fail | bf6b8dc32a63 | 20260707T085740_S30_seed20260707 | forced GC left 3 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'blobs': 3}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-07T09:02:09 | S34 | 20260707 | dev | 900s | pass | bf6b8dc32a63 | 20260707T090058_S34_seed20260707 |  |
| 2026-07-07T12:08:27 | S30 | 1 | dev | 900s | fail | 6da55fce2a0d | 20260707T120511_S30_seed1 | forced GC left 1 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 1}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-08T13:21:57 | S30 | 1 | dev | 900s | pass | 7c06bcfdde21 | 20260708T132053_S30_seed1 |  |
| 2026-07-08T13:25:12 | S30 | 2 | dev | 900s | pass | 7c06bcfdde21 | 20260708T132400_S30_seed2 |  |
| 2026-07-08T13:26:30 | S30 | 3 | dev | 900s | pass | 7c06bcfdde21 | 20260708T132512_S30_seed3 |  |
| 2026-07-08T13:27:47 | S30 | 4 | dev | 900s | pass | 7c06bcfdde21 | 20260708T132630_S30_seed4 |  |
| 2026-07-08T16:06:54 | S15 | 1 | dev | 900s | pass | f5fd7c0ead35 | 20260708T160443_S15_seed1 |  |
| 2026-07-11T03:53:13 | S30 | 20260711 | dev | 600s | pass | aa57013a86a4 | 20260711T035241_S30_seed20260711 |  |
| 2026-07-11T03:53:42 | S33 | 20260711 | dev | 600s | pass | aa57013a86a4 | 20260711T035313_S33_seed20260711 |  |
| 2026-07-11T04:36:22 | S33 | 20260711 | dev | 480s | pass | 0868f9d360a6 | 20260711T043558_S33_seed20260711 |  |
| 2026-07-11T10:03:55 | S30 | 20260711 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100323_S30_seed20260711 |  |
| 2026-07-11T10:04:32 | S30 | 2 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100355_S30_seed2 |  |
| 2026-07-11T10:04:59 | S01 | 20260711 | dev | 480s | inconclusive | eceacc2ad1d6 | 20260711T100432_S01_seed20260711 |  |
| 2026-07-11T10:05:30 | S25 | 20260711 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100459_S25_seed20260711 |  |
| 2026-07-11T10:06:11 | S34 | 20260711 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100530_S34_seed20260711 |  |
| 2026-07-11T10:08:16 | S15 | 20260711 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100611_S15_seed20260711 |  |
| 2026-07-11T10:08:45 | S33 | 20260711 | dev | 480s | pass | eceacc2ad1d6 | 20260711T100816_S33_seed20260711 |  |
| 2026-07-11T10:15:34 | S12 | 20260711 | dev | 480s | pass | ffc993a1d85b | 20260711T101423_S12_seed20260711 |  |
| 2026-07-11T11:55:50 | S01 | 20260711 | ci | 300s | pass | 4b104c25649e | 20260711T115530_S01_seed20260711 |  |
| 2026-07-11T15:45:50 | S04 | 20260711 | dev | 240s | inconclusive | c5a7c0409fb5 | 20260711T154517_S04_seed20260711 |  |
| 2026-07-11T15:46:15 | S02 | 20260711 | dev | 240s | pass | c5a7c0409fb5 | 20260711T154550_S02_seed20260711 |  |
| 2026-07-11T15:46:41 | S09 | 20260711 | dev | 240s | pass | c5a7c0409fb5 | 20260711T154615_S09_seed20260711 |  |
| 2026-07-11T15:49:30 | S13 | 20260711 | dev | 240s | pass | c5a7c0409fb5 | 20260711T154641_S13_seed20260711 |  |
| 2026-07-13T16:49:15 | S01 | 42 | dev | 900s | fail | f51fbab60bc0 | 20260713T164402_S01_seed42 | cluster did not become healthy after reset |
| 2026-07-13T16:51:26 | S01 | 42 | dev | 900s | inconclusive | 84011674682b | 20260713T165106_S01_seed42 |  |
| 2026-07-13T16:52:21 | S02 | 42 | dev | 900s | pass | 84011674682b | 20260713T165156_S02_seed42 |  |
| 2026-07-13T16:53:16 | S03 | 42 | dev | 900s | inconclusive | 84011674682b | 20260713T165230_S03_seed42 |  |
| 2026-07-13T16:54:32 | S04 | 42 | dev | 900s | inconclusive | 84011674682b | 20260713T165348_S04_seed42 |  |
| 2026-07-13T16:56:13 | S05 | 42 | dev | 900s | inconclusive | 84011674682b | 20260713T165449_S05_seed42 |  |
| 2026-07-13T16:59:31 | S06 | 42 | dev | 900s | fail | 84011674682b | 20260713T165702_S06_seed42 |  |
| 2026-07-13T17:08:34 | S07 | 42 | dev | 900s | fail | 029c05c6553c | 20260713T170326_S07_seed42 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-13T17:15:59 | S08 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T170920_S08_seed42 |  |
| 2026-07-13T17:16:47 | S09 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T171619_S09_seed42 |  |
| 2026-07-13T17:17:20 | S10 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T171654_S10_seed42 |  |
| 2026-07-13T17:18:29 | S11 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T171735_S11_seed42 |  |
| 2026-07-13T17:20:01 | S12 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T171845_S12_seed42 |  |
| 2026-07-13T17:27:15 | S13 | 42 | dev | 900s | fail | dcdaf479cd2c | 20260713T172032_S13_seed42 | forced GC left 2 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 2}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-13T17:29:44 | S14 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T172829_S14_seed42 |  |
| 2026-07-13T17:31:57 | S15 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T172954_S15_seed42 |  |
| 2026-07-13T17:33:13 | S16 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173208_S16_seed42 |  |
| 2026-07-13T17:33:49 | S17 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173322_S17_seed42 |  |
| 2026-07-13T17:34:48 | S18 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173358_S18_seed42 |  |
| 2026-07-13T17:35:24 | S19 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173459_S19_seed42 |  |
| 2026-07-13T17:36:02 | S20 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T173533_S20_seed42 |  |
| 2026-07-13T17:36:42 | S21 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T173617_S21_seed42 |  |
| 2026-07-13T17:38:10 | S22 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173702_S22_seed42 |  |
| 2026-07-13T17:39:16 | S23 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T173823_S23_seed42 |  |
| 2026-07-13T17:39:58 | S24 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T173931_S24_seed42 |  |
| 2026-07-13T17:40:39 | S25 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174008_S25_seed42 |  |
| 2026-07-13T17:41:18 | S26 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174048_S26_seed42 |  |
| 2026-07-13T17:42:15 | S27 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174128_S27_seed42 |  |
| 2026-07-13T17:42:59 | S28 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174223_S28_seed42 |  |
| 2026-07-13T17:43:33 | S29 | 42 | dev | 900s | inconclusive | dcdaf479cd2c | 20260713T174308_S29_seed42 |  |
| 2026-07-13T17:44:26 | S30 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174349_S30_seed42 |  |
| 2026-07-13T17:45:15 | S31 | 42 | dev | 900s | fail | dcdaf479cd2c | 20260713T174441_S31_seed42 | ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 23 but GC reclaimed ~78 (checklist #9). previewDeletes should iterate all target shards, not just shard 0. |
| 2026-07-13T17:46:01 | S32 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174536_S32_seed42 |  |
| 2026-07-13T17:46:44 | S33 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174614_S33_seed42 |  |
| 2026-07-13T17:47:38 | S34 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174656_S34_seed42 |  |
| 2026-07-13T17:48:23 | S35 | 42 | dev | 900s | pass | dcdaf479cd2c | 20260713T174747_S35_seed42 |  |
| 2026-07-13T18:16:47 | S06 | 44 | dev | 900s | fail | 2174a893f33d | 20260713T181600_S06_seed44 |  |
| 2026-07-13T18:20:34 | S07 | 44 | dev | 900s | fail | 2174a893f33d | 20260713T181937_S07_seed44 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-13T18:25:27 | S13 | 44 | dev | 900s | pass | 2174a893f33d | 20260713T182111_S13_seed44 |  |
| 2026-07-13T18:30:51 | S06 | 45 | dev | 900s | inconclusive | 7a22bc5b700a | 20260713T182958_S06_seed45 |  |
| 2026-07-13T18:32:12 | S07 | 45 | dev | 900s | inconclusive | 7a22bc5b700a | 20260713T183115_S07_seed45 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-13T18:50:44 | S01 | 46 | full | 900s | pass | 7a22bc5b700a | 20260713T184953_S01_seed46 |  |
| 2026-07-13T19:00:12 | S07 | 46 | full | 900s | inconclusive | 7a22bc5b700a | 20260713T185131_S07_seed46 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-13T19:56:40 | S08 | 46 | ci | 900s | inconclusive | 7a22bc5b700a | 20260713T192931_S08_seed46 |  |
| 2026-07-13T20:00:17 | S21 | 46 | ci | 900s | inconclusive | 7a22bc5b700a | 20260713T195730_S21_seed46 |  |
| 2026-07-13T20:02:50 | S29 | 46 | full | 900s | inconclusive | 7a22bc5b700a | 20260713T200118_S29_seed46 |  |
| 2026-07-13T20:05:10 | S02 | 46 | full | 900s | pass | 7a22bc5b700a | 20260713T200357_S02_seed46 |  |
| 2026-07-13T20:09:08 | S03 | 46 | full | 900s | fail | 7a22bc5b700a | 20260713T200548_S03_seed46 | Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/89c/89ccd094-87f6-4932-94d5-cbdeef131f2a@cas@/0000000000000001-00000000000009b4/000001.proto' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId. (ABORTED) (version 26.6.1.1) / sql=INSERT INTO s03_live SELECT 12550000 + number AS id, randomString(512) AS payload FROM numbers(50000) |
| 2026-07-14T11:52:10 | S38 | 42 | dev | 900s | fail | c1693936a3ff | 20260714T115007_S38_seed42 |  |
| 2026-07-14T12:20:57 | S38 | 42 | dev | 900s | fail | c1693936a3ff | 20260714T115429_S38_seed42 | quiescence failed: timed out |
| 2026-07-14T13:41:38 | S38 | 42 | dev | 900s | fail | c1693936a3ff | 20260714T131226_S38_seed42 | quiescence failed: timed out |
| 2026-07-14T13:54:49 | S13 | 42 | dev | 900s | pass | 2d3d57c549a5 | 20260714T134837_S13_seed42 |  |
| 2026-07-14T13:58:15 | S15 | 42 | dev | 900s | pass | 2d3d57c549a5 | 20260714T135545_S15_seed42 |  |
| 2026-07-14T13:59:21 | S18 | 42 | dev | 900s | pass | 2d3d57c549a5 | 20260714T135815_S18_seed42 |  |
| 2026-07-15T10:30:19 | S03 | 1 | dev | 900s | inconclusive | 47ce91e05eb9 | 20260715T102739_S03_seed1 |  |
| 2026-07-16T18:20:48 | S36 | 1 | dev | 900s | fail | e3a165dfa15d | 20260716T181544_S36_seed1 | cluster did not become healthy after reset |
| 2026-07-16T20:05:05 | S36 | 1 | dev | 900s | fail | e3a165dfa15d | 20260716T200002_S36_seed1 | cluster did not become healthy after reset |
| 2026-07-16T20:19:22 | S36 | 1 | dev | 900s | fail | e3a165dfa15d | 20260716T201906_S36_seed1 | Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: promote: ref 'moving' already names a different committed manifest — refusing to overwrite (unique-ref invariant; use republishRef for an intended repoint). (ABORTED) (version 26.6.1.1) / sql=ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca' |
| 2026-07-16T20:39:15 | S37 | 1 | dev | 900s | fail | 03f6da7a1ec3 | 20260716T203706_S37_seed1 |  |
| 2026-07-17T00:25:03 | S36 | 1 | dev | 900s | fail | e114f8c88e9c | 20260717T002305_S36_seed1 |  |
| 2026-07-17T00:43:01 | S36 | 1 | dev | 900s | pass | 26590e4aa55f | 20260717T004047_S36_seed1 |  |
| 2026-07-17T00:45:32 | S37 | 1 | dev | 900s | fail | 26590e4aa55f | 20260717T004323_S37_seed1 |  |
| 2026-07-17T00:50:22 | S37 | 1 | dev | 900s | fail | 93bb65eb8e82 | 20260717T005005_S37_seed1 | Node(localhost:8124) HTTP 400: Code: 36. DB::Exception: Table doesn't have any table TTL expression, cannot remove. (BAD_ARGUMENTS) (version 26.6.1.1) / sql=ALTER TABLE s37_ttl REMOVE TTL |
| 2026-07-17T00:54:37 | S37 | 1 | dev | 900s | fail | a2c420d1fd26 | 20260717T005228_S37_seed1 |  |
| 2026-07-17T01:51:43 | S39 | 1 | dev | 900s | fail | 58b4d1b8759c | 20260717T015047_S39_seed1 | Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS write could not be committed (stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/c94/c945a7a9-4578-4b78-bdd0-6ec0e42ead78@cas@/0000000000000001-000000000000000b/000001.zst' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId); retrying later. (NETWORK_ERROR) (version 26.6.1.1) / sql=INSERT INTO s39_lease SELECT 4000 + number AS id, randomString(512) AS payload FROM numbers(500) |
| 2026-07-17T02:02:35 | S39 | 1 | dev | 900s | fail | 3104f162abd4 | 20260717T020015_S39_seed1 | Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS write could not be committed (stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/2a7/2a740e92-6007-4b67-b9f5-20062b1be4a7@cas@/0000000000000001-000000000000000d/000001.zst' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId); retrying later. (NETWORK_ERROR) (version 26.6.1.1) / sql=INSERT INTO s39_lease SELECT 8000 + number AS id, randomString(512) AS payload FROM numbers(500) |
| 2026-07-17T02:11:50 | S39 | 1 | dev | 900s | fail | 82bec8e77655 | 20260717T020400_S39_seed1 | quiescence failed: Node(localhost:8124) HTTP 404: Code: 60. DB::Exception: Table default.s39_lease does not exist. (UNKNOWN_TABLE) (version 26.6.1.1) / sql=SYSTEM SYNC REPLICA s39_lease |
| 2026-07-17T02:21:07 | S39 | 1 | dev | 900s | pass | 8e39ea2fba4d | 20260717T021323_S39_seed1 |  |
| 2026-07-17T02:23:46 | S37 | 1 | dev | 900s | fail | 8e39ea2fba4d | 20260717T022127_S37_seed1 |  |
| 2026-07-17T03:35:04 | S01 | 1 | ci | 900s | fail | cdac5ce8409c | 20260717T033430_S01_seed1 | S01 peak RSS grew 531 MiB during a 512 MiB blob upload — investigate Build::putBlob materializing BlobSource into a String before putIfAbsentStream (README known first investigation target) |
| 2026-07-17T03:35:44 | S02 | 1 | ci | 900s | pass | cdac5ce8409c | 20260717T033504_S02_seed1 |  |
| 2026-07-17T03:40:19 | S03 | 1 | ci | 900s | inconclusive | cdac5ce8409c | 20260717T033545_S03_seed1 |  |
| 2026-07-17T03:42:09 | S04 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T034019_S04_seed1 |  |
| 2026-07-17T03:50:30 | S05 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T034209_S05_seed1 |  |
| 2026-07-17T03:53:07 | S06 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T035030_S06_seed1 |  |
| 2026-07-17T03:55:49 | S07 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T035307_S07_seed1 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-17T04:11:42 | S09 | 1 | ci | 900s | pass | 314489f00c5d | 20260717T041049_S09_seed1 |  |
| 2026-07-17T04:12:42 | S10 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T041142_S10_seed1 |  |
| 2026-07-17T04:19:49 | S11 | 1 | ci | 900s | inconclusive | 314489f00c5d | 20260717T041242_S11_seed1 |  |
| 2026-07-17T04:22:20 | S12 | 1 | ci | 900s | pass | 314489f00c5d | 20260717T041949_S12_seed1 |  |
| 2026-07-17T09:14:10 | S40 | 1 | ci | 900s | pass | 77484196b0d5 | 20260717T090957_S40_seed1 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-17T09:43:29 | dl_probe | - | - | ~300s | pass | 01b5ec7deeb5 | build/test_dl_probe_postfix.log | acked-then-lost regression gate (T4 Step 1, original reproducer, tracked in tools/); submitted=2466 acked=2466 PRESENT=2466 LOST(acked-but-absent)=0 (pre-fix baseline was ~198/1314 lost); ch2 exited post-pause (known B200, tolerated, does not affect LOST) |
| 2026-07-17T09:49:38 | S39 | 1 | ci | 900s | fail | 01b5ec7deeb5 | 20260717T094921_S39_seed1 | leg A's fault window must be shorter than the renew period so it can overlap AT MOST one renewal beat -- a window >= the renew period can fault two consecutive beats and (correctly) near the lease deadline, which is leg B's job, not leg A's |
| 2026-07-17T09:54:31 | S39 | 1 | dev | 900s | pass | 01b5ec7deeb5 | 20260717T095134_S39_seed1 |  |
| 2026-07-17T09:57:12 | S36 | 1 | dev | 900s | pass | e08fb29bc7d3 | 20260717T095448_S36_seed1 |  |
| 2026-07-17T09:59:21 | S37 | 1 | dev | 900s | fail | e08fb29bc7d3 | 20260717T095712_S37_seed1 |  |
| 2026-07-17T09:59:29 | soak-phase3 | 42 | full | 1200s | pass | e08fb29bc7d3 | build/test_soak_postfix.log | T4 Step 4: 20m row-count-oracle soak, sync inserts, seed=42 (the chaos recipe that originally exposed R4's loss); recovery + final converge checkpoints all OK (count=813264, fsck reachable=974 unreachable=0 dangling=0 dryrun_count=0); checkpoint model==observed, no deficit; ABORTED-retried INSERT attempts=0 (node_down=81 driver-retried, all recovered) |
| 2026-07-17T10:53:40 | S37 | 1 | dev | 900s | fail | 70b360471b47 | 20260717T105322_S37_seed1 | Node(localhost:8123) HTTP 500: Code: 384. DB::Exception: Cannot move part 'all_0_0_0' because it's participating in background process. (PART_IS_TEMPORARILY_LOCKED) (version 26.6.1.1) / sql=ALTER TABLE s37_ttl MOVE PARTITION ID 'all' TO VOLUME 'hot' |
| 2026-07-17T10:57:01 | S37 | 1 | dev | 900s | pass | 9926f38ba23d | 20260717T105454_S37_seed1 |  |
| 2026-07-17T11:01:16 | S37 | 1 | dev | 900s | pass | 9926f38ba23d | 20260717T105908_S37_seed1 |  |
| 2026-07-17T17:36:12 | S39 | 1 | ci | 900s | pass | 34c2d615874c | 20260717T173254_S39_seed1 |  |
| 2026-07-17T17:38:31 | S37 | 1 | dev | 900s | pass | 34c2d615874c | 20260717T173612_S37_seed1 |  |
| 2026-07-17T21:26:28 | S01 | 1 | ci | 900s | pass | 7a9627cd0fab | 20260717T212549_S01_seed1 |  |
| 2026-07-17T21:27:08 | S02 | 1 | ci | 900s | pass | 7a9627cd0fab | 20260717T212628_S02_seed1 |  |
| 2026-07-17T21:32:16 | S03 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T212708_S03_seed1 |  |
| 2026-07-17T21:35:20 | S04 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T213216_S04_seed1 |  |
| 2026-07-17T21:56:44 | S06 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T215021_S06_seed1 |  |
| 2026-07-17T22:03:09 | S07 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T215644_S07_seed1 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-17T22:19:02 | S09 | 1 | ci | 900s | pass | 7a9627cd0fab | 20260717T221809_S09_seed1 |  |
| 2026-07-17T22:20:01 | S10 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T221902_S10_seed1 |  |
| 2026-07-17T22:27:24 | S11 | 1 | ci | 900s | inconclusive | 7a9627cd0fab | 20260717T222001_S11_seed1 |  |
| 2026-07-17T22:29:54 | S12 | 1 | ci | 900s | pass | 7a9627cd0fab | 20260717T222724_S12_seed1 |  |
| 2026-07-17T23:51:15 | S14 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T234138_S14_seed1 |  |
| 2026-07-17T23:54:44 | S15 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T235115_S15_seed1 |  |
| 2026-07-17T23:57:13 | S16 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T235444_S16_seed1 |  |
| 2026-07-17T23:57:56 | S17 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T235713_S17_seed1 |  |
| 2026-07-17T23:58:49 | S18 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T235756_S18_seed1 |  |
| 2026-07-17T23:59:28 | S19 | 1 | ci | 900s | pass | 4420b5a3498b | 20260717T235849_S19_seed1 |  |
| 2026-07-18T00:00:11 | S20 | 1 | ci | 900s | inconclusive | 4420b5a3498b | 20260717T235928_S20_seed1 |  |
| 2026-07-18T00:03:07 | S21 | 1 | ci | 900s | inconclusive | 4420b5a3498b | 20260718T000011_S21_seed1 |  |
| 2026-07-18T00:04:59 | S22 | 1 | ci | 900s | fail | 4420b5a3498b | 20260718T000307_S22_seed1 |  |
| 2026-07-18T00:07:29 | S23 | 1 | ci | 900s | fail | 4420b5a3498b | 20260718T000459_S23_seed1 |  |
| 2026-07-18T00:08:20 | S24 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T000729_S24_seed1 |  |
| 2026-07-18T00:09:02 | S25 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T000820_S25_seed1 |  |
| 2026-07-18T00:09:47 | S26 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T000902_S26_seed1 |  |
| 2026-07-18T00:13:39 | S27 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T000947_S27_seed1 |  |
| 2026-07-18T00:15:10 | S28 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T001339_S28_seed1 |  |
| 2026-07-18T00:15:53 | S29 | 1 | ci | 900s | inconclusive | 4420b5a3498b | 20260718T001510_S29_seed1 |  |
| 2026-07-18T00:17:53 | S30 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T001553_S30_seed1 |  |
| 2026-07-18T00:19:08 | S31 | 1 | ci | 900s | fail | 4420b5a3498b | 20260718T001753_S31_seed1 | ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 72 but GC reclaimed ~406 (checklist #9). previewDeletes should iterate all target shards, not just shard 0. |
| 2026-07-18T00:19:46 | S32 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T001908_S32_seed1 |  |
| 2026-07-18T00:20:54 | S33 | 1 | ci | 900s | pass | 4420b5a3498b | 20260718T001946_S33_seed1 |  |
| 2026-07-18T00:22:55 | S34 | 1 | ci | 900s | pass | b4196a7017f7 | 20260718T002054_S34_seed1 |  |
| 2026-07-18T00:24:31 | S35 | 1 | ci | 900s | pass | b3fe10fa3fba | 20260718T002255_S35_seed1 |  |
| 2026-07-18T00:24:48 | S36 | 1 | ci | 900s | fail | b3fe10fa3fba | 20260718T002431_S36_seed1 | Node(localhost:8123) HTTP 500: Code: 479. DB::Exception: Part '0_0_0_0' is already on disk 'ca'. (UNKNOWN_DISK) (version 26.6.1.20000.altinityantalya) / sql=ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca' |
| 2026-07-18T00:26:59 | S37 | 1 | ci | 900s | fail | b3fe10fa3fba | 20260718T002448_S37_seed1 |  |
| 2026-07-18T00:33:21 | S38 | 1 | ci | 900s | fail | b3fe10fa3fba | 20260718T002659_S38_seed1 |  |
| 2026-07-18T00:36:37 | S39 | 1 | ci | 900s | pass | b3fe10fa3fba | 20260718T003321_S39_seed1 |  |
| 2026-07-18T00:40:39 | S40 | 1 | ci | 900s | pass | b3fe10fa3fba | 20260718T003637_S40_seed1 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-18T00:49:02 | S05 | 1 | ci | 900s | inconclusive | b0da1f60f429 | 20260718T004039_S05_seed1 |  |
| 2026-07-18T01:13:08 | S08 | 1 | ci | 900s | inconclusive | 4d457ec378af | 20260718T004902_S08_seed1 |  |
| 2026-07-18T01:40:34 | S23 | 1 | ci | 900s | fail | 4d457ec378af | 20260718T013739_S23_seed1 |  |
| 2026-07-18T01:41:47 | S31 | 1 | ci | 900s | pass | 4d457ec378af | 20260718T014034_S31_seed1 |  |
| 2026-07-18T01:44:04 | S36 | 1 | ci | 900s | pass | 4d457ec378af | 20260718T014147_S36_seed1 |  |
| 2026-07-18T01:46:16 | S37 | 1 | ci | 900s | pass | 4d457ec378af | 20260718T014404_S37_seed1 |  |
| 2026-07-18T01:52:35 | S38 | 1 | ci | 900s | fail | 4d457ec378af | 20260718T014616_S38_seed1 |  |
| 2026-07-18T03:13:16 | S02 | 1 | dev | 900s | pass | b27ec0816de8 | 20260718T031236_S02_seed1 |  |
| 2026-07-18T16:17:03 | S22 | 1 | ci | 900s | pass | 08ea8d1200e4 | 20260718T161452_S22_seed1 |  |
| 2026-07-18T16:45:59 | S13 | 3 | ci | 3600s | pass | 08ea8d1200e4 | 20260718T161945_S13_seed3 |  |
| 2026-07-18T16:49:04 | S23 | 1 | ci | 900s | inconclusive | 08ea8d1200e4 | 20260718T164643_S23_seed1 |  |
| 2026-07-18T21:07:50 | S01 | 1 | ci | 900s | pass | 426b3dbce2b8 | 20260718T210704_S01_seed1 |  |
| 2026-07-18T21:08:35 | S02 | 1 | ci | 900s | pass | 426b3dbce2b8 | 20260718T210750_S02_seed1 |  |
| 2026-07-18T21:13:11 | S03 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T210836_S03_seed1 |  |
| 2026-07-18T21:15:00 | S04 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T211311_S04_seed1 |  |
| 2026-07-18T21:23:24 | S05 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T211500_S05_seed1 |  |
| 2026-07-18T21:25:58 | S06 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T212324_S06_seed1 |  |
| 2026-07-18T21:28:46 | S07 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T212558_S07_seed1 | S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs. |
| 2026-07-18T21:44:41 | S09 | 1 | ci | 900s | pass | 426b3dbce2b8 | 20260718T214346_S09_seed1 |  |
| 2026-07-18T21:45:35 | S10 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T214441_S10_seed1 |  |
| 2026-07-18T21:52:59 | S11 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T214535_S11_seed1 |  |
| 2026-07-18T21:55:32 | S12 | 1 | ci | 900s | pass | 426b3dbce2b8 | 20260718T215259_S12_seed1 |  |
| 2026-07-18T22:35:30 | S13 | 1 | ci | 900s | pass | 426b3dbce2b8 | 20260718T221424_S13_seed1 |  |
| 2026-07-18T23:01:31 | S08 | 1 | ci | 900s | inconclusive | 426b3dbce2b8 | 20260718T223640_S08_seed1 | quiescence failed: quiesce initial: 1 replication-queue entries carry a real last_exception — genuine error |
| 2026-07-18T23:15:44 | S14 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T230652_S14_seed1 |  |
| 2026-07-18T23:19:13 | S15 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T231544_S15_seed1 |  |
| 2026-07-18T23:21:43 | S16 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T231913_S16_seed1 |  |
| 2026-07-18T23:22:26 | S17 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T232143_S17_seed1 |  |
| 2026-07-18T23:23:26 | S18 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T232227_S18_seed1 |  |
| 2026-07-18T23:24:04 | S19 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T232326_S19_seed1 |  |
| 2026-07-18T23:24:48 | S20 | 1 | ci | 900s | inconclusive | 35faaae182c5 | 20260718T232405_S20_seed1 |  |
| 2026-07-18T23:27:30 | S21 | 1 | ci | 900s | inconclusive | 35faaae182c5 | 20260718T232448_S21_seed1 |  |
| 2026-07-18T23:29:46 | S22 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T232730_S22_seed1 |  |
| 2026-07-18T23:32:14 | S23 | 1 | ci | 900s | inconclusive | 35faaae182c5 | 20260718T232946_S23_seed1 |  |
| 2026-07-18T23:33:07 | S24 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T233214_S24_seed1 |  |
| 2026-07-18T23:33:50 | S25 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T233307_S25_seed1 |  |
| 2026-07-18T23:34:35 | S26 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T233350_S26_seed1 |  |
| 2026-07-18T23:38:26 | S27 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T233436_S27_seed1 |  |
| 2026-07-18T23:40:33 | S28 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T233826_S28_seed1 |  |
| 2026-07-18T23:41:15 | S29 | 1 | ci | 900s | inconclusive | 35faaae182c5 | 20260718T234033_S29_seed1 |  |
| 2026-07-18T23:43:15 | S30 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234115_S30_seed1 |  |
| 2026-07-18T23:44:31 | S31 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234315_S31_seed1 |  |
| 2026-07-18T23:45:08 | S32 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234431_S32_seed1 |  |
| 2026-07-18T23:46:11 | S33 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234508_S33_seed1 |  |
| 2026-07-18T23:48:11 | S34 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234611_S34_seed1 |  |
| 2026-07-18T23:49:53 | S35 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234811_S35_seed1 |  |
| 2026-07-18T23:52:10 | S36 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T234953_S36_seed1 |  |
| 2026-07-18T23:54:22 | S37 | 1 | ci | 900s | pass | 35faaae182c5 | 20260718T235210_S37_seed1 |  |
| 2026-07-19T00:00:44 | S38 | 1 | ci | 900s | fail | 35faaae182c5 | 20260718T235423_S38_seed1 |  |
| 2026-07-19T00:03:59 | S39 | 1 | ci | 900s | pass | 35faaae182c5 | 20260719T000044_S39_seed1 |  |
| 2026-07-19T00:08:02 | S40 | 1 | ci | 900s | pass | 35faaae182c5 | 20260719T000359_S40_seed1 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-19T00:31:55 | S08 | 1 | ci | 900s | inconclusive | 35faaae182c5 | 20260719T000802_S08_seed1 |  |
| 2026-07-23T18:41:56 | S41 | 1 | dev | 900s | fail | 29c98dcfd05c | 20260723T184127_S41_seed1 |  |
| 2026-07-23T18:45:41 | S41 | 1 | dev | 900s | pass | 29c98dcfd05c | 20260723T184508_S41_seed1 |  |
| 2026-07-23T18:49:18 | S41 | 1 | dev | 900s | pass | 29c98dcfd05c | 20260723T184846_S41_seed1 |  |
| 2026-07-23T18:55:50 | S41 | 1 | full | 900s | pass | 29c98dcfd05c | 20260723T185256_S41_seed1 |  |
| 2026-07-24T10:54:58 | S41 | 1 | full | 900s | pass | a9449127f724 | 20260724T105212_S41_seed1 |  |
| 2026-07-25T16:45:35 | S42 | 1 | dev | 900s | fail | 830c4997a73f | 20260725T164254_S42_seed1 |  |
| 2026-07-25T16:51:56 | S42 | 1 | dev | 900s | inconclusive | 830c4997a73f | 20260725T164929_S42_seed1 |  |
| 2026-07-26T21:22:46 | S42 | 42 | ci | 900s | fail | ef3347160aad | 20260726T205952_S42_seed42 | GC log has 2 real (non-benign) Error finish row(s) |
| 2026-07-26T21:49:03 | S42 | 43 | ci | 900s | fail | 7c8a302393f8 | 20260726T212653_S42_seed43 |  |
| 2026-07-29T10:49:50 | S38 | 20260729 | dev | 900s | fail | 352d80a75657 | 20260729T104557_S38_seed20260729 | forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 20}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-29T10:50:13 | S43 | 20260729 | dev | 900s | fail | 352d80a75657 | 20260729T104950_S43_seed20260729 | 'utf-8' codec can't decode byte 0xb5 in position 1: invalid start byte |
| 2026-07-29T10:53:35 | S33 | 20260729 | dev | 900s | fail | 352d80a75657 | 20260729T105013_S33_seed20260729 | forced GC left 87 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 87}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-29T10:56:58 | S30 | 20260729 | dev | 900s | fail | fc7416bd8a96 | 20260729T105335_S30_seed20260729 | S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated. |
| 2026-07-29T11:01:17 | S38 | 20260729 | dev | 900s | fail | fc7416bd8a96 | 20260729T105718_S38_seed20260729 | forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 20}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
| 2026-07-29T11:06:04 | S43 | 20260729 | dev | 900s | fail | fc7416bd8a96 | 20260729T110117_S43_seed20260729 | quiescence failed: <urlopen error [Errno 111] Connection refused> |
| 2026-07-29T11:11:40 | S38 | 20260729 | dev | 900s | fail | ea33e7c80998 | 20260729T110740_S38_seed20260729 | forced GC left 45 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 45}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events. |
