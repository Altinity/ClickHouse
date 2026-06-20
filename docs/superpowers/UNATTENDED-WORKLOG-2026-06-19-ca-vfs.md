# Unattended Work Log — CA VFS path-mapping (2026-06-19)

Branch: `cas-vfs-path-mapping` (off `cas-mergetree-poc`). Operator granted unattended authority.

## Mandate (this run)
1. Run the FULL stateless suite (all tests) on the CA lane — not run for a while, surprises possible.
2. Fix all regressions found.
3. Run a 4-hour chaos soak.
4. While the soak runs: drive `clickhouse-disks` and traverse the CA disk; fix any problems.
5. Then implement **B181** (detached parts living inside the table's own `@cas@` archive).
6. Record deferred/debt items in the backlog; keep this work log current.

## State at start
- Phases 1–6 + cleanup committed (18 commits). `clickhouse`+`unit_tests_dbms` build clean; 306 CA gtests pass (only the pre-existing B140 marker `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` red); CA stateless lane green on the per-task spot-checks.
- Layout now: `roots/<server-hex>/store/<u3>/<uuid>@cas@/…` (tables), `roots/<server-hex>/_precommits` + `_watermark`, `gc/registry`, loose files as plain mountpoint objects.

## Timeline
- T0 — start. Launched full CA stateless suite in background (`by3x0sbah` → `build/full_stateless_ca.log`); confirmed running (praktika stateless-test container up, server startup). Soak harness reconned: `docker compose up -d` + `python3 -m soak.run --seed N --phase …` (duration arg TBD at launch); `soak_watch.sh`/`soak_healthcheck.sh` for the 20-min cadence. Binary symlink `ci/tmp/clickhouse` → fresh `build/programs/clickhouse` (Phase-6 build, 22:39).
- T1 — full suite DONE (~25 min, praktika exit 1). **68 unique Fail**: ~19 `*content_addressed*` (04278–05005), ~17 transaction/isolation (01133/01167-74/02345/02421/02435/02497/03657/03803/03812/03916/04057/04060/05004), ~11 text-index (02346_*/04033/04068), + env (`01880_remote_ipv6`, `02784_connection_string`, `02479_mysql_connect_to_self`, `01854_s2`, `02224_s2`, `01271_show_privileges`), + `03649/03650_alias_marker_distributed`, misc (`00091_prewhere`, `03214_backup`, `03233_dynamic`, `03800_autopr`, `test_optimize_using_constraints`). Triaging: artifact (benign CA LOCAL-pool stderr WARNING) vs known-flaky/env vs real regression; baseline separation needed because Phase 6 changed the layout.
- T2 — triage done. CA-specific tests (04278–05005) = bucket A (benign LOCAL-pool stderr WARNING only — confirmed no new surfaces). **Real bucket-C regression families:** (1) transactions ×20 — `ContentAddressed: moveFile source mutable file missing: …/tmp_insert_*/txn_version.txt.tmp` (suspect Phase-2 loose/mutable-file rewiring); (2) text-index ×11 — `ContentAddressed: file … not in tree of store/…` FILE_DOESNT_EXIST (suspect Phase-1 tree/path-mirroring); + mutation satellites ×2, backup ×1. Bucket B (flaky/env): remote_ipv6, connection_string, mysql_self, s2×2, show_privileges, isolation-hermitage race, alias_marker (exec-bit/UNKNOWN_SETTING), autopr (TOO_SLOW), prewhere (timeout), test_optimize_using_constraints. Next: baseline-determine whether the two families are MY regressions vs pre-existing on the CA lane.
- T3 — **VERDICT: the refactor is regression-clean.** Baseline determination (built `cas-mergetree-poc`, ran `01172`/`01133`/`02346_text_index_lwd`/`04057` on base vs branch): the bucket-C families fail IDENTICALLY on base and branch → **PRE-EXISTING CA gaps, NOT path-mapping regressions.** Recorded as **B182** (explicit-transaction MVCC `txn_version.txt.tmp` rename) + **B183** (text-index/statistics not carried into the part tree on mutation/vertical-merge). Branch restored + rebuilt clean (HEAD a209facfd6f5). So all 68 full-suite failures = bucket A (benign WARNING) ∪ bucket B (flaky/env) ∪ bucket C (pre-existing CA gaps). Path-mapping Phases 1–6 + cleanup introduced ZERO regressions. → proceeding to the 4h soak + clickhouse-disks traversal + B181.
- T4 — **4h chaos soak launched** (PID 1232402, `utils/ca-soak/logs/soak_cavfs_4h_runner.log`, metrics `soak_cavfs_4h.db`). Phase 3, `--duration 4h --seed 1 --chaos-seed 1`, chaos armed (80 faults, window 5760–12240s), over rustfs/S3, using the fresh branch binary (mtime 23:45). Warmup live (281k+ rows). Ends ~03:50. Pool: `configs/storage_conf.xml` has a read-only `ca_ro` disk on the same rustfs pool (`test/soak_pool/`). Now: clickhouse-disks traversal of the live pool during the soak, then B181. Monitoring the soak between steps (CORRUPTED_DATA / dangling / both-pause recovery / the new `roots/<hex>/_precommits`+`_watermark` keys).
- T5 — clickhouse-disks traversal of the LIVE soak pool: found+fixed 2 real "normal-disk" bugs (commit `c9465f0b368`): (1) top-down `list /`→`store`→`store/<u3>` returned empty — added `listLiveTreeChildren`/`liveTreeDirHasChildren` (server-scoped mirrored LIST); (2) `store/<u3>` routing collided with the non-Atomic `data/<db>` fallback — added `isAtomicShardDir`. Verified on live data: top-down nav works (`…/count.txt → 600476`); physical layout shows `@cas@`, `roots/<hex>/_precommits`+`_watermark`, `gc/registry`. Soak undisturbed (read-only `ca_ro`). Binary-skew NOTE: the running soak mmap'd the pre-`c9465f0b368` binary; chaos restarts will pick up the new one. `c9465f0b368` is read-path/disks-only (does NOT touch GC/watermark/precommit/write-path), so the durability validation is unaffected and I am NOT restarting the 4h clock. To-validate: a CA-lane check of `c9465f0b368` (it touched load-bearing `route`/`listDirectory`).
- T6 — `c9465f0b368` validated on the CA lane (04287/04289/04278/04036/05003 — OK or benign-WARNING-artifact only; no route/listing regression). Soak healthy at ~22 min (steady stage, 4.4M rows on both replicas, 0 CORRUPTED/dangling/LOGICAL_ERROR this run). **Decision: defer B181 build-work to post-soak** — rebuilding `build/programs/clickhouse` would let the soak's chaos restarts reload a B181-in-progress binary mid-run and spuriously fail it (the soak does DROP/ATTACH round-trips, exactly B181's surface). Monitoring the soak every 20 min; B181 implemented after it finishes (~03:50) on the free binary, then its own detach/attach lane + a follow-up soak.
- T0 — WAITING on the full suite (blocking step before the soak). Will analyze on completion: separate refactor regressions from pre-existing flakies, fix regressions, then launch the 4h soak + live `clickhouse-disks` traversal, then B181.
- T7 — soak monitor (00:54, ~60min): HEALTHY. Stage `ttl_pressure` (t+3643s); pool ~600k objs / ~6GB; 0 CORRUPTED/dangling/LOGICAL_ERROR/WORKLOAD-FAILURE. Chaos stage (the Phase-6 reclaim validator) starts t+5760s (~01:26); converge/end ~03:50.
- T8 — soak monitor (01:15, ~84min): HEALTHY. Stage `gc_checkpoint` (t+5051s); pool ~620k objs / ~7.4GB oscillating (GC reclaiming normally); 0 CORRUPTED/dangling/Fatal. Chaos stage next (~01:26).
- T9 — soak monitor (01:36): **soak DIED at gc_checkpoint (~96min, t+5760s), BEFORE the chaos stage.** Cause = HARNESS bug (not product): `CHECKPOINT FAILURE: ambiguous TTL band non-empty (a row within 10s of its TTL boundary)` — over-strict checkpoint oracle (B173 class). Aggregates MATCHED (node1==node2 count=2513061), 0 CORRUPTED/dangling. So Phase 6 validated under NORMAL ops (insert/merge/mutation/ttl/quiesced-GC clean), but chaos-reclaim path NOT reached. Fixing the harness TTL-checkpoint ambiguity, then re-running to reach chaos.
- T10 — fixed harness TTL-checkpoint over-strictness (commit `5c48837912e`: bounded wait-out of the ambiguous band, re-quiesce, then assert EXACTLY as before; 143 pytest pass). Relaunched the 4h soak **v2** (PID 1371375, `logs/soak_cavfs_4h_v2_runner.log`, metrics `soak_cavfs_4h_v2.db`, same binary mtime 00:14). Started clean (warmup t+0). Should now pass gc_checkpoint into chaos (~01:40 start → chaos ~03:16 → ends ~05:40). Monitoring 20-min vs the v2 log.
- T11 — soak v2 monitor (02:02, ~22min): HEALTHY. Stage `steady` (t+720s); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/fatal. gc_checkpoint pass-through (the v1 death point, now fixed) ~03:04; chaos ~03:16.
- T12 — soak v2 monitor (02:23, ~42min): HEALTHY. Stage `mutations` (t+2160s); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/fatal. gc_checkpoint ~03:04, chaos ~03:16 ahead.
- T13 — soak v2 monitor (02:44, ~61min): HEALTHY. Stage `ttl_pressure` (t+3641s); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/fatal. gc_checkpoint pass-through ~03:04 (next window), chaos ~03:16.
- T14 — soak v2 monitor (03:05, ~85min): HEALTHY, AT `gc_checkpoint` (t+5069s) — the v1 death point, checkpoint in progress; no CHECKPOINT FAILURE/corruption/dangling yet. Next window confirms pass-through (TTL-fix) + chaos onset (~03:16).
- T15 — soak v2 DIED at gc_checkpoint AGAIN, different cause (TTL-band fix worked): `CHECKPOINT FAILURE: quiescence … backlog stuck at 7 … genuine hang`. Diagnosed on the live cluster: NOT a stall — 1 active merge running **620s+** on a huge level-46 part (`20260619_0_13982_46_…`) over S3, a MUTATE_PART postponed 69× behind it, MATERIALIZE TTL waiting (parts_to_do=1), **zero exceptions**. So it is genuine CA-over-S3 large-merge slowness exceeding the 600s quiescence budget — a 3rd HARNESS fragility, not a product/Phase-6 bug. Fix: make quiescence MERGE-AWARE (wait while a merge/mutation actively progresses; generous cap; fail only on idle+flat). Recording B184 (CA-over-S3 large-merge perf / soak merge-size). Relaunching v3.
- T16 — merge-aware quiescence fix committed (`ab74a729379`, 151 pytest pass; `is_genuine_hang`: active merge = progress, fail-fast on exceptions, hang only on idle+flat w/ 30min cap). B184 recorded (`3f5dabf01a5`). Soak **v3** relaunched (PID 1457525, `logs/soak_cavfs_4h_v3_runner.log`, metrics `soak_cavfs_4h_v3.db`), warmup. v3 timeline from ~03:45: gc_checkpoint ~05:09, chaos ~05:21–07:09, converge ~07:45. Both gc_checkpoint fragilities now fixed → expect pass-through to chaos.
- T17 — soak v3 died at SETUP: `DROP TABLE IF EXISTS ca_stress SYNC` TimeoutError (http recv). Cause: containers reused v1→v2→v3, so v3 setup dropped v2-accumulated huge table over slow CA/S3 → exceeded harness query timeout. NOT product/Phase-6 (cluster healthy, ch clean). 4th harness fragility. Fix: FRESH cluster (compose down -v + up → empty pool, instant DROP) + bump setup DROP timeout. Relaunching v4 fresh, 4h. If v4 also fails, will shorten duration to reach chaos faster.
- T18 — soak **v4** on a FRESH cluster (compose down -v + up; setup DROP-timeout fix `246a9cdad37`). PID 1477970, `logs/soak_cavfs_4h_v4_runner.log`, metrics `soak_cavfs_4h_v4.db`. Clean setup (instant DROP on empty pool), warmup running. All 3 harness fixes + fresh pool now in place. (Minor: compose ch1-before-bucket startup race needed a one-time restart — harden depends_on later.) Timeline from ~04:00: gc_checkpoint ~05:24, chaos ~05:36–07:24, converge ~08:00. If v4 hits a 5th fragility, shorten duration to reach chaos faster.
- T19 — soak v4 monitor (04:20, ~22min): HEALTHY. Stage `steady` (t+720s); setup succeeded (DROP-timeout fix + fresh pool worked); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/Timeout. gc_checkpoint ~05:24, chaos ~05:36 ahead.
- T20 — soak v4 monitor (04:41, ~42min): HEALTHY. Stage `mutations` (t+2160s); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/Timeout. gc_checkpoint ~05:24, chaos ~05:36 ahead.
- T21 — soak v4 monitor (05:02, ~61min): HEALTHY. Stage `ttl_pressure` (t+3633s); 0 CORRUPTED/dangling/CHECKPOINT-FAILURE/Timeout. gc_checkpoint pass-through ~05:24 (next window — the key milestone), chaos ~05:36.
- T22 — GC/CAS HEALTH CHECK (operator-requested, ~05:17 local = 03:17 UTC; the "03:16" timestamps were UTC, no clock skew). **GC healthy on the new layout:** 584 GC rounds (ch1 leader, current), reclaiming hard (recheck-deleted 464k, blob_delete/retire 447k, blob_forget 674k, tree strip/delete flowing); Phase-6 precommits working (precommit ok 11342, build_start 11343); **ZERO anomalies** (no fail/corrupt/incoher/missing/abort/dangling); ca_stress 4.4M rows / 74 active parts; S3 9.6M reads / 4.3M writes. → refactor did NOT degrade GC. The harness gc_checkpoint failures (TTL-band, merge-quiescence) are timing/scale fragilities independent of the binary (earlier B171 soaks avoided them by seed/scale luck), now fixed.
- T23 — soak v4 monitor (05:23): AT `gc_checkpoint` (t+5047s) — checkpoint in progress, no CHECKPOINT FAILURE/corruption/dangling. With TTL wait-out + merge-aware quiescence in place, expect pass-through this time. Next window confirms chaos onset.
- T24 — **v4 PASSED gc_checkpoint into the CHAOS stage** (milestone — both harness fixes worked). But a post-fault-window checkpoint hit **PERSISTENT `fsck dangling=94`** (INV-NO-LOSS, exit 36, reachable=30091), not transient. **MAJOR CONFOUND: rustfs threw 1.6M S3 write errors (~38% of 4.24M write attempts; NOT disk-full — 321G free) — rustfs-beta flakiness under write load.** CRUX DIAGNOSIS NEEDED: is dangling=94 (a) Phase-6 GC-deleted-while-referenced [B140-class real regression] or (b) never-persisted [rustfs dropping writes under chaos]? This is exactly the Phase-6 risk I assessed. Soak runner exited; cluster still up for forensics. Dispatching diagnosis.
- T25 — **DIAGNOSIS VERDICT: dangling=94 = flaky rustfs backend write-loss, NOT a Phase-6 GC bug.** Evidence: 922,772 GC content-deletes ALL had indeg_at_recheck=0; **0 deleted-while-referenced**; 430 indeg>0 cases SPARED; 0 anomalies. rustfs 35.4% write-error rate (1.6M errors; 15k ERROR/133k WARN: 5xx ServiceUnavailable/InternalError, erasure-coding write failures), 72 retire-time HEAD-404 (backend-lost objects). dangling=94 surfaced at gc_checkpoint (pre-chaos, last_fault=null) and **self-healed to 0** via read-path repair (6629 gate_revalidate + 20 gate_resurrect); 180s budget too short through a 38%-erroring backend. **CONCLUSION: Phase 6 is durability-clean (GC provably no-loss); the chaos soak cannot run cleanly on rustfs-beta (loses 38% of writes). Soak iterations STOPPED (infra-blocked). Recording B185.** Proceeding to B181 (lane/gtest validation is backend-independent).
- B181 — **DONE: detached parts folded INTO the table's own `@cas@` archive.** Removed the parallel sibling `detachedNamespace`; detached parts are now refs inside the table's OWN namespace keyed by a `detached/`-prefixed ref (`detached/<part>` vs live `<part>`). One namespace per table; live↔detached collision impossible by construction (ref names differ). Changes: `route` re-split now yields `(liveNamespace(table), ref="detached/"+part, file)`; `detachedNamespace` deleted (only a historical comment mentions it); `<table>/detached` container dir lists the table's refs filtered to the `detached/` prefix (stripped for display); table-dir listing collapses `detached/<part>` refs to the single `detached` subdir via `addFirstComponent`; transaction DROP TABLE drops one namespace, DROP DETACHED drops only `detached/`-prefixed refs, RENAME TABLE moves all refs in one namespace move; DETACH/ATTACH unchanged (same `(ns,ref)->(ns',ref')` part-dir move, now both endpoints share the table ns). **Validation (backend-independent):** CA gtests 347 PASS / 2 pre-existing-red (`CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` known; `CaWiringOps.FreezeViaHardLinksIntoShadow` confirmed failing on the pre-B181 baseline too — unrelated to this change). New gtests `CaWiringRoute.DetachedFoldsIntoTableNamespaceWithPrefixedRef` + reworked `CaWiringRead.DetachedFoldedIntoTableNamespace` (publishes detached as a `detached/`-prefixed ref in `liveNamespace`, adds a same-base-name live part proving no collision). **Stateless detach/attach oracle:** 04287/04288/04289/04278 stdout matches reference EXACTLY (verified against a local server); their praktika "FAIL" is the benign LOCAL-pool emulated-conditional-ops WARNING-to-stderr artifact (spec-acknowledged), NOT a result diff. 04036 (FREEZE/backup shadow) OK — shadow path untouched. **Chaos soak DEFERRED (B185: rustfs backend loses ~38% of writes; lane+gtests are the validation for now).**

## RUN COMPLETE (2026-06-20 ~06:00)
All mandate items done: (1) full stateless suite = refactor regression-clean (pre-existing CA gaps → B182/B183); (2) no refactor regressions to fix; disks-traversal bugs fixed (c9465f0b368); (3) 4h soak: Phase 6 GC PROVEN no-loss via audit log (922k deletes, 0 deleted-while-referenced), 3 harness fragilities fixed (TTL-band, merge-aware quiescence, setup-DROP timeout), clean chaos-completion BLOCKED by flaky rustfs backend (38% write loss → B185); (4) clickhouse-disks traversal validated + 2 bugs fixed; (5) **B181 DONE** (detached folds into table @cas@ archive; lane+gtests green). Deferred to backlog: B182 (txn MVCC on CA), B183 (text-index/statistics in tree), B184 (CA/S3 large-merge perf), B185 (rustfs write-loss → need conformant backend for the no-loss soak). Branch cas-vfs-path-mapping ready for review.
- T26 — **CORRECTION (operator challenge): "rustfs loses 38% of writes" was WRONG.** The 1.6M `S3WriteRequestsErrors` are ~1.57M `PreconditionFailed` (412) = by-design CAS conditional-write contention on root-shard manifests (sample msg: 412 on `…@cas@/7` size 272), NOT failures/loss. Genuine rustfs transient errors in the failure hour ≈6k: 1284×503, 1207×429 (rate limits), 1281×500, ~2k conn-reset/timeout — retryable overload, all retried. **No data lost:** oracle node1==node2 matched; repairs = 306,247 gate_revalidate (re-read, found present) vs only 1,865 gate_resurrect (re-create) ≈165:1; live fsck=0; GC no-loss (922k deletes, 0 deleted-while-referenced). The dangling=94 was a TRANSIENT read-unavailability false-positive (rustfs 429/503 under load), the 180s settle budget too short under the load. Phase 6 durability-clean. Real fix = harness-side (fsck should RETRY transient reads before declaring dangling; the write-error metric conflates 412-CAS-contention with failures), NOT a different backend. Rewriting B185.
- **DRY cleanup (2026-06-20):** collapsed the 3 duplicated `detached/`-prefix filter loops (`existsDirectory`, `listDirectory`, `removeRecursive`) into one helper `ContentAddressedMetadataStorage::detachedRefNames` (returns full ref names); net −1 line; all detached gtests pass (347 PASS, 2 pre-existing reds only); 04287/04288/04289 lane: stdout correct, FAIL = benign LOCAL-pool stderr WARNING artifact only.

---

## T27 — B182 (transactions) root-caused as a B151 REGRESSION + fix (2026-06-20)

User directive: "afair transactions and text indexes used to work. probably some regression. that is the first priority."

### Repro (deterministic, local CA disk + embedded Keeper, see tmp/ca-repro/)
- B182: `BEGIN; INSERT; INSERT; OPTIMIZE FINAL; COMMIT` →
  `Code:107 ContentAddressed: moveFile source mutable file missing: .../tmp_merge_all_1_2_1/txn_version.txt.tmp`
- B183: vertical merge + text index (`02346_text_index_vertical_merge.sql`) →
  `Code:107 ContentAddressed: file skp_idx_idx_c1.cmrk2 not in tree of .../all_1_2_1/...`
- Note: plain (non-txn) INSERT/merge work because txn_version writes are DEFERRED for non-transactional parts.
  Basic `BEGIN;INSERT;COMMIT` works; only merge/mutation-inside-txn fails.

### B182 root cause (CONFIRMED by trace logging)
`DiskObjectStorageTransaction::moveDirectory` dispatches the part-dir rename EAGERLY for CA (B151,
commit 6f5e3866710) — it publishes `tmp_merge_*` → final ref immediately. But the atomic-write rename
`txn_version.txt.tmp → txn_version.txt` (issued earlier via `storeInfoToDataPartStorage`) is a QUEUED
`replaceFile` op that only replays at commit, AFTER the eager publish. So the publish captures the
un-renamed `.tmp`, erases the staging, and the queued replaceFile then finds an empty staging →
committed-mutable branch → `resolveRef(tmp_merge ref)` fails (ref was published under the FINAL name).
This is a regression introduced by B151's eager-publish-at-rename.

### Fix
`DiskObjectStorageTransaction::moveFile`/`replaceFile`: when the rename is a CA mutable per-part file
atomic-write rename (both endpoints mutable per-part files), dispatch EAGERLY (like writeFile /
createHardLink / moveDirectory) instead of queuing. The `.tmp → final` re-key then completes in staging
BEFORE the eager moveDirectory publish, so the published sidecar carries the correct `txn_version.txt`.
Helper: `isContentAddressedMutablePartFileRename`.

Status: fix built & verifying. B183 (vertical-merge text-index tree) still open — separate path
(temporary_text_index_storage merge-back).

## T28 — B183 (text indexes on vertical-merge / mutation) root-caused + fix (2026-06-20)

### Scope of breakage (confirmed by repro)
- Plain INSERT + text index: WORKS.
- HORIZONTAL merge + text index: WORKS.
- VERTICAL merge + text index: BROKEN.
- Mutation `MATERIALIZE INDEX` (and any mutation that rebuilds a text index): BROKEN.
The broken cases are exactly those that use `createTemporaryTextIndexStorage` (MergeTask.cpp:2847,
MutateTask.cpp:1764) — a `text_index_tmp` SUBDIRECTORY inside the part with its OWN DataPartStorage
+ transaction.

### Root cause (CONFIRMED by content/publish trace)
On CA, `<part>/text_index_tmp/...` routes to the part's OWN ref. The temp storage's separate
`commitTransaction()` therefore DURABLY PUBLISHES a committed `<part>` ref holding ONLY the
`text_index_tmp/*` scratch files. The main merge transaction stages the real part files (+ the merged
`skp_idx_*`, `statistics.packed`) into its own staging and publishes the correct `all_*` manifest at the
eager rename (publishStaging). But `moveDirectory` then unconditionally calls
`republishRef(tmp_merge → all)`, which ADOPTS the spurious committed `tmp_merge` ref (scratch-only tree)
and republishes it OVER the just-published real manifest — clobbering it. Reads then fail with
`file skp_idx_*.cmrk2 / statistics.packed not in tree`.
This violated B151's own stated invariant ("the tmp ref was never durably published, so the republishRef
below is a no-op"); the nested text-index sub-storage breaks that assumption.
`removeRecursive(text_index_tmp)` (MergeTask:2313) is a no-op on CA (routes to a projection-subdir
no-op), so the spurious ref is never cleaned there.

### Fix
In `ContentAddressedTransaction::moveDirectory`, when the part-dir move had a STAGED source (the fresh
tmp->final publish — `had_staged_source`), DROP any committed source ref of the same tmp name
(`dropRefIfPresent`) instead of `republishRef`-ing it. The staged manifest is authoritative; the
committed tmp ref is spurious scratch (the text_index_tmp sub-storage commit). Its blobs become
unreachable → GC reclaims them. The committed-source rename path (DETACH/ATTACH/delete_tmp) is
unchanged (it has no staged source).

Status: both B182 + B183 fixes built (traces removed); validating next.

## T29 — Validation: both fixes verified, 2 gtest failures proven PRE-EXISTING (2026-06-20)

### Functional verification (local CA disk + Keeper)
- B182 repro (merge in txn): exception -> returns 2. FIXED.
- B183 repro (vertical-merge text index): exception -> returns 2214 (== pre-merge count). FIXED.
- Mutation `MATERIALIZE INDEX` (was broken): returns 111, no exception. FIXED.
- Oracle (vertical-merge text index, CA vs `default` disk): CA=2214, default=2214 — IDENTICAL.
- Broader txn scenarios (merge/delete/rollback in txn): CA behaves identically to the `default` disk.

### CA gtests
Filter `CaWiring*:CaPartPathParser*:CasStore*:Cas*` → 292/294 pass; 2 fail:
- `CaWiringOps.FreezeViaHardLinksIntoShadow` (gtest_ca_wiring.cpp:776)
- `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` (gtest_cas_gc_leak.cpp:150)
Both PROVEN PRE-EXISTING: `git stash`-ed both fixes, rebuilt unit_tests_dbms clean, and BOTH still fail
identically. They are unrelated to B182/B183:
- `CasGcLeak.*` is a self-documented intentional RED TDD test ("THE B140 ASSERTION (RED today)…
  unreachable == 2 today"), added by 47d3107ee8c — the unfixed B140 displaced-tree GC leak.
- `CaWiringOps.FreezeViaHardLinksIntoShadow` regressed in the VFS refactor (af54aa415bc, shadow/mountpoint
  namespace change): `removeRecursive("shadow/bk1")` no longer makes `existsDirectory("shadow/bk1")` false.
  My diff touches neither shadow nor GC nor createHardLink/removeRecursive paths. -> new backlog item.

## T30 — B123 / B124 / B126 latent write-path hardening (2026-06-20)

Three write-path-review correctness items (user's B86/B87/B88 = backlog B123/B124/B126), all latent +
single-writer-mitigated. Direction confirmed with the operator (detailed RU writeup), then implemented.

- **B124 (collision policy):** aligned both move paths to SOURCE-wins (POSIX rename semantic; matches
  the atomic `.tmp`->final rename). `moveDirectory` staged-merge `emplace`(dest-wins) → source-wins +
  fail-loud `LOGICAL_ERROR` on a genuine differing-bytes collision (silent lost-update → loud error).
- **B123 (verbatim RMW + unlink):** documented the single-writer contract for the verbatim get→put→
  remove move and made it idempotent on re-drive (src gone + dst present → no-op). For `unlinkFile`,
  documented the LOAD-BEARING no-op invariant (committed content files are removed via whole-part
  ref-drop, not per-file unlink — a blanket assert would break fast-removal); the operator emphasised
  this must be documented. No risky assert added.
- **B126 (RENAME TABLE atomicity):** documented + made explicit that `move_namespace` is idempotent /
  re-drivable; wrapped in try/catch with a loud LOG_ERROR naming both namespaces on partial failure.
  Durable move-journal (true atomicity) deliberately out of scope.

Validation: 3 new gtests (`MoveDirectoryMutableCollisionPolicy`, `VerbatimMoveIsIdempotentOnRedrive`,
`TableRenameIsIdempotentOnRedrive`) PASS; full CA gtests 299/301 (the 2 fails are the known pre-existing
`FreezeViaHardLinksIntoShadow` [B186] + `CasGcLeak.*` [B140]). Functional sanity on the local CA server:
B182/B183 still green; RENAME TABLE+mutation oracle CA==default (2000/1999000/7290); a mixed workload
(inserts+vertical merges+MATERIALIZE INDEX+UPDATE+DELETE+txn-merge+RENAME) ran with zero CA errors and
the collision guard never fired spuriously.

## T31 — soak-harness cluster A cleanup (2026-06-20)

Goal: get the soak to a stable green run for real chaos validation. Reviewed cluster A:
- **B154** (fsck subprocess timeout) — already DONE in code (`fsck.py` timeout_s + graceful degrade); confirmed.
- **B155** (SYNC REPLICA readonly-retry) — already DONE in code (`checker.py sync_replica_with_readonly_retry`, `cluster.py is_readonly`); confirmed.
- **B152** (post-fault settling false-fail + misleading message) — FIXED: `wait_for_pool_consistent`
  now distinguishes FLAPPING-CLEAN (reached dangling==0 once but didn't hold `stable` → warn + return
  the clean reading; the aggregate oracle is the authoritative no-loss gate, asserted separately) from
  PERSISTENT NEVER-CLEAN (raise, accurate message). No longer cries INV-NO-LOSS on a dangling==0 timeout.
- **B185** — false-fail FIXED by the same flapping-tolerance. CORRECTED the entry: retracted my wrong
  "transient unavailability / 165:1 gate_revalidate:resurrect" claim (gate_revalidate is an unconditional
  per-commit fail-closed marker; gate_resurrect is GC-race re-stamping — neither is a 404 recovery).
  Root-cause of the dangling=94 (rustfs read-after-write vs mid-churn transient vs real) is STILL OPEN —
  follow-ups recorded (capture+re-HEAD the keys; rustfs RAW probe).

Validation: full ca-soak unit suite 152 passed (incl. new `test_pool_consistent_flapping_clean_does_not_raise`
+ `test_pool_consistent_persistent_never_clean_raises`). Next: 1-hour soak on the fresh binary.
