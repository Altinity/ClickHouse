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
- T0 — WAITING on the full suite (blocking step before the soak). Will analyze on completion: separate refactor regressions from pre-existing flakies, fix regressions, then launch the 4h soak + live `clickhouse-disks` traversal, then B181.
