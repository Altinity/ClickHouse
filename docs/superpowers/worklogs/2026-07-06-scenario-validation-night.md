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

## S14 — restart with many refs {#s14}

**What S14 checks:** prefills many tables (many refs), cleanly restarts a node, then asserts all tables are queryable, startup reads scale with root METADATA (not total blob count — "startup does not list all blobs"), and no unknown-disk false positives.

**Dev-scale (200 tables, seed 20260707): PASS, 19/19 verdicts.** All green — notably: **all tables queryable after restart** + **no unknown-disk false positives** (the F1 `ca_ro` fix holds under a 200-table restart — a strong F1 re-validation); **startup does not list all blobs** (startup cost is metadata-bound, the S14 thesis, confirmed); replica agreement across 8 sampled tables; `fsck dangling=0`; `dryrun ⊆ unreachable`; event audit clean; GC no failed rounds; no unbounded leftovers. Peak startup metadata reads recorded (bounded).

**Full-scale (10000 tables, seed 20260707): PASS, 44/44 verdicts.** Clean at full scale — both servers clean-restarted with 10000 tables attached, the cluster came back, and: **all tables queryable after restart** + **no unknown-disk false positives** (the F1 `ca_ro` fix holds at 10000-table restart — the strongest F1 validation), **startup does not list all blobs** + startup root-metadata reads bounded (startup cost is metadata-bound, not blob-count-bound, confirmed at scale), first-query latency recorded, `fsck dangling=0`, replica agreement, event audit clean, GC no failed rounds. **S14 did NOT wedge on #3231** — bulk table creation is low-overwrite-churn (pool stayed at MB-scale metadata, no LIST storm). **This confirms the pattern: restart-heavy scenarios pass at full scale; only merge-CHURN-heavy scenarios (S13's 40 kill rounds) trigger the rustfs#3231 quiesce wedge.** S14 = clean PASS at full scale.

## S15 — GC target-shard comparison {#s15}

**What S15 checks:** runs the same workload under `gc_shards=1` (default) and `gc_shards=2` (gc_shards2 variant), asserts identical oracle checksums across shard counts, reducer memory doesn't balloon with shards, and `fsck dangling==0` under each.

- **First run: FAIL (8/10)** — `no dangling after GC (gc_shards2)` = `None`. **Root-caused as MY OWN F1-fix regression, NOT a CAS bug:** the F1 fix pointed `soak/fsck.py` at `/etc/clickhouse-server/fsck-only.xml` globally, but I'd only mounted that file (and stripped `ca_ro`) in the DEFAULT compose — the **gc_shards2 compose still lacked the fsck-only mount**, so `clickhouse disks -C fsck-only.xml` found no config → fsck returned no summary → `dangling=None` → verdict fail. The real correctness verdicts PASSED even in this run: **identical oracle checksum default vs gc_shards2** (2400 rows, `12815430402022094066` both) and `orphan backlog drained (gc_shards2)`=0.
- **Fix (F1 propagation):** stripped `ca_ro` from `storage_conf_gc_shards2_ch{1,2}.xml` and mounted `fsck_only_ca.xml` in `docker-compose-gc_shards2.yml` (both nodes). Re-run: **INCONCLUSIVE (9/10)** — both dangling verdicts now PASS (`default=0`, `gc_shards2=0`); the single remaining INCONCLUSIVE is `gc_shards=8 comparison` = unavailable (no gc_shards8 compose exists; only 1 and 2). That's a scale/infra gap, not a failure.
- **Verdict: sharded-GC correctness VALIDATED** — identical results at gc_shards 1 and 2, dangling=0 both, reducer memory flat (0.60 → 0.61 GB), orphan backlog drained. The gc_shards=8 arm is unavailable (would need a gc_shards8 compose). No CAS defect. (Reminder for later: `10replicas`/`awss3` server configs still embed `ca_ro` + lack the fsck-only mount — same F1 propagation needed before their scenarios' fsck works.)

## S16 — hot content cycle with GC {#s16}

**Dev-scale (seed 20260707): INCONCLUSIVE 9/10 — effectively PASS.** 9 verdicts pass (all safety: `fsck dangling=0`, replica agreement, orphan drained, event audit clean, GC no failed rounds, no unbounded leftovers); the 1 INCONCLUSIVE is `resurrection counters recorded` — an observability gate (the condemned-blob revival/W-REVALIDATE counter wasn't exercised in the short dev window, like S13's precommit-reclaim gate). **No dangling, no wedge** — the hot-content overwrite churn at dev scale stayed under the #3231 threshold. (Full-scale hot-content churn would be #3231-limited like S13; validated at dev.) Note: BACKLOG had a historical S16 `dangling=1` forensics investigation (GC-CONCURRENT-LEADER-DANGLING) — **not reproduced here** (dangling=0).

## S17 — detached, attach, drop detached {#s17}

**Dev-scale (seed 20260707): PASS, 13/13 verdicts.** Clean — detach/attach/drop-detached lifecycle correct; `dangling=0`, replica agreement, event audit clean, no unbounded leftovers. No CAS defect. (Attach/detach-focused, low overwrite-churn → not #3231-sensitive.)

## S18 — freeze and unfreeze shadows {#s18}

**Dev-scale (seed 20260707): FAIL 7/9, but the core freeze correctness PASSED — the FAIL is finding F3 (a dryrun-vs-GC reachability divergence for shadows).**

- **PASS (core):** `frozen content survives a live-table drop` (`fsck dangling=0` after dropping the live table AND after forced-GC-to-fixpoint, 3 rounds) + `unfreeze releases shadow refs` (reclaimable unreachable=0). So a freeze snapshot correctly keeps its blobs alive across a live-table drop, and the real GC did NOT over-delete them.
- **FAIL → finding F3 (real, MEDIUM-HIGH, needs code investigation):** the FINAL `ca-gc-dryrun` (post-forced-GC) proposed deleting **132 blobs** (e.g. `soak_pool/blobs/01/01072cc50e...`) that fsck classifies **reachable** (`unreachable=0`). The gc_log confirms the real GC condemned/`blob_retire`d other (legitimately-dead, dropped-live-table) blobs but — since `dangling=0` after 3 rounds — did NOT delete the frozen 132. **So `ca-gc-dryrun`'s reachability DIVERGES from the real GC + fsck for shadow/frozen-referenced blobs: dryrun over-proposes them for deletion.** This is the same discover-root divergence class as review-finding #3 (fsck/GC/dryrun compute reachability from different roots). Observed-safe here (real GC spares the frozen blobs), BUT it's a red flag: (a) the operator-facing `ca-gc-dryrun` is misleading on any pool with frozen data (over-reports deletions), (b) it breaks the `dryrun ⊆ unreachable` soak oracle for freeze scenarios, and (c) it must be code-confirmed that the REAL GC robustly protects shadow refs on ALL paths (not just the timing here). **Backlog: align `ca-gc-dryrun` reachability with the real GC/fsck for shadow/frozen references; audit whether GC's reachability universe (`discoverUniverse` over `cas/refs/`) includes frozen part-manifests' blob edges or relies on a shadow-walk fsck does but dryrun doesn't.**
- Minor: `no unbounded leftovers` INCONCLUSIVE — residual=164 but the fsck detail wasn't available to classify by prefix (harness detail-availability gap, not a leak signal; `unfreeze releases shadow refs`=0 covers the reclaimable check).

## S19 — clone and partition movement {#s19}

**Dev-scale (seed 20260707): FAIL 13/14 — but the FAIL is a strict-atomicity verdict with a SAFE outcome (finding F4, MINOR / known triage).**

- **PASS (core):** `clone moves metadata only (no body re-upload)` (CasBlobPut for MOVE/REPLACE = 0 — clone republishes refs, doesn't copy blobs ✓), `moved partition lands in dst` (8 rows ✓), `no dangling after clone ops` (0), `fsck dangling`=0, `no unbounded leftovers`=0.
- **FAIL → F4 (MINOR, matches BACKLOG "gated cross-disk move not failing closed" triage):** `ALTER TABLE s19_src MOVE PARTITION 2 TO DISK 'default'` correctly **failed closed** (raised `Code:479 UNKNOWN_DISK: No such disk 'default' in storage policy 'ca'`), but the verdict is strict ("publishes NO partial ref") and observed **2 `CasRootCas` ops during the rejected attempt** → the move does partial ref work before the destination-disk-not-in-policy check rejects it (not perfectly atomic). **Outcome is SAFE — `fsck dangling=0`, no leftovers** → the 2 CAS left no dangling ref. So: a minor atomicity/ordering concern (the CA move path publishes ref CAS before validating the target disk is in the policy), NOT data loss. Fix direction: validate the destination disk is in the storage policy BEFORE any ref CAS in the MOVE path. Backlog.

## S20 — replicated fetch and relink {#s20}

**Dev-scale (seed 20260707): INCONCLUSIVE 11/12 — effectively PASS.** 11 pass (all safety: `dangling=0`, no leftovers, event audit clean, relink zero-copy behavior); the 1 INCONCLUSIVE is `follower publishes its own refs` — the known BACKLOG S20 "follower-refs" observability gate (couldn't confirm the follower's own ref-publish in the dev window), not a failure. No CAS defect.

## S21 — read-heavy many-ref workload {#s21}

**Dev-scale (seed 20260707): INCONCLUSIVE 12/13 — effectively PASS.** 12 pass (all safety: `dangling=0`, no leftovers, event audit clean, read-path correctness); the 1 INCONCLUSIVE is `column-subset fetches only required blobs` — a read-path column-pruning observation gate (scale-gated at dev; BACKLOG S21 read-path-thresholds triage). No CAS defect.

## S22 / S27 — NEEDS-INFRA (skipped) {#s22-s27}

**S22** (object-store throttling & retry budget) and **S27** (backend LIST pagination ambiguity) require a **fault-injecting S3 proxy** (503/429/slow/connection-close for S22; duplicate/unstable LIST pages for S27) interposed between ClickHouse and RustFS — not available on this stand (direct rustfs1 endpoint). NOT RUN (unchanged from prior campaigns). NOTE: S22's proxy is also what would let Task-6's mount-lease decouple be validated under *induced* S3 latency, and would let the rustfs#3231/503 wedge (F2) be exercised deliberately — a high-value dedicated infra build (BACKLOG estimate ~3h: toxiproxy + HTTP-status injector).

## S23 — idle shared pool baseline {#s23}

**Dev-scale (seed 20260707): INCONCLUSIVE 14/16 — effectively PASS.** 14 pass (all safety: `dangling=0`, no leftovers, event audit clean); the 2 INCONCLUSIVE are `1-server idle baseline` + `10-server idle baseline` — recorded-only idle-GC-cost observations (no fixed budget; the 10-server arm needs the unavailable 10-replica infra). Corroborates the documented S3-BUDGET idle-GC-cost item; no CAS defect.

## S24 — small dedup-cache capacity {#s24}

**Dev-scale (seed 20260707, small_dedup_cache variant, F1-fixed): FAIL 9/10 — but it's a HARNESS-TIMING false-FAIL, NOT a CAS bug.** The `S24 replica agreement` verdict saw ch1=280 rows / ch2=272 at check time (8-row gap). **Live re-check (post-run): both replicas 280 rows with IDENTICAL checksum `8663329780789566770`, queue_size=0, absolute_delay=0 — fully converged.** So ch2 was transiently mid-replication when the S24 card checked agreement (the card checks agreement WITHOUT a preceding `SYSTEM SYNC REPLICA`, unlike S13/S14 which use `sync_replica_with_readonly_retry`). Data is consistent; the small dedup cache (a correctness-neutral hint) caused no divergence. 9 other verdicts pass, `dangling=0`, residual=0. **Harness fix (minor): add a pre-agreement SYNC to the S24 card.** No CAS defect. (The small_dedup_cache variant also got the F1 fix — ca_ro out of server config + fsck-only mount — before this run.)

## S25 — non-Atomic (Ordinary) database paths {#s25}

**Dev-scale (seed 20260707): FAIL 8/10 — SAME as finding F3 (dryrun reachability gap), now confirmed SYSTEMATIC.** Core correctness passes: `part files content-addressed under non-Atomic db` (10 blobs), `S25 non-Atomic replica agreement` (400/400, identical checksum `15759365460278066692`), `fsck dangling=0`, `non-Atomic path cleanup fsck clean`=0. The FAIL is again `dryrun ⊆ unreachable`: **10 blobs fsck calls reachable** (incl. the all-zeros sentinel `soak_pool/blobs/00/00000000000000000000000000000000`) proposed by `ca-gc-dryrun`; real GC keeps them (`dangling=0`).

**→ F3 upgraded to SYSTEMATIC:** the `ca-gc-dryrun` reachability under-counts vs fsck/real-GC across MULTIPLE roots — shadow/frozen refs (S18) AND non-Atomic DB paths + the all-zeros sentinel blob (S25). The real GC is safe in both (dangling=0). This is now clearly a **dryrun-tool reachability defect** (not scenario-specific): it proposes deleting blobs that are actually reachable. Two failure roots pinned. **Backlog priority raised — `ca-gc-dryrun` must use the SAME reachability walk as the real GC/fsck** (or the dryrun⊆unreachable oracle will keep false-failing, and an operator trusting dryrun would think GC deletes live data). Confirm the real GC's reachability never regresses to the dryrun's narrower view. `no unbounded leftovers` inconclusive (residual=10 = the same F3 blobs, detail-classification unavailable).

## S26 — table-level verbatim file churn {#s26}

**Dev-scale (seed 20260707): FAIL 11/13 — F3 AGAIN (3rd occurrence).** Core passes: `verbatim churn fsck clean`=0, `fsck dangling=0`, replica agreement. FAIL is `dryrun ⊆ unreachable`: 63 blobs fsck calls reachable, real GC keeps (`dangling=0`). **F3 now confirmed at 3 independent scenarios (S18 shadow / S25 non-Atomic / S26 verbatim churn), always over-proposing reachable BLOBS, real GC always safe** — this is a broad `ca-gc-dryrun` reachability under-count, not an edge case. (Effectively these three scenarios "pass" on all safety verdicts; only the dryrun-oracle false-fails.) No new info beyond F3; strengthens the backlog priority to fix `ca-gc-dryrun`'s reachability.

## S28 — concurrent wide/large insert scratch pressure {#s28}

**Dev-scale (seed 20260707): PASS 12/12.** Clean — concurrent wide/large-insert scratch pressure handled; `dangling=0`, no leftovers, event audit clean. No CAS defect (the whole-part scratch-spill resource concern is a documented separate item, not a correctness fail).

## S29 — large non-direct-blob file memory spike {#s29}

**Dev-scale (seed 20260707): INCONCLUSIVE 9/10 — effectively PASS.** 9 safety verdicts pass (`dangling=0`, no leftovers, event audit clean); the 1 INCONCLUSIVE is `RSS growth during finalize not ~ non-direct-blob file size` — a memory-observation gate best exercised at full scale (streaming write-path behavior already established by the S01 putBlob-memory work). No CAS defect.

## S30 — repeated create/drop namespace churn {#s30}

**Dev-scale (30 create/insert/drop iterations, seed 20260707): FAIL 6/8 — finding F5 (GC per-round fanout growth vs the D1 bounding goal).**
- **PASS:** `fsck dangling=0`, `root_dirs 2 -> 2` (dir count bounded), replica/audit clean. No data loss.
- **FAIL → F5 (real, MEDIUM, backlog + D1 follow-up):** `GC fanout bounded across ever-created namespaces (D1 registry removal)` — **`CasRootGet` per-round grew 75 -> 190 across the 30 create/drop cycles even though no table stayed live**. D1 (registry removal + dropped-shard reclaim) was meant to keep GC per-round work bounded across create/drop churn; this shows per-round GETs still growing. Residual=43 "other" (dropped-namespace remnants). `dangling=0` so it's a GC-EFFICIENCY / fanout issue, not correctness/data-loss. **Caveats to resolve in investigation:** (a) confirm at full scale (1000 iters) whether the growth is truly unbounded/linear or levels off; (b) the growth may be partly inflated by concurrent-leader `gc/state moved ... retry next round` retries (2-node, both GC-enabled) rather than pure dropped-namespace re-reads — separate the two; (c) audit dropped-namespace ref-shard reclaim completeness (are dropped `cas/refs/<ns>/*` fully removed, or lingering for `discoverUniverse` to re-read each round?). Relates to BACKLOG S30 "other"/dropNamespace-registry item + [[project_d1_shard_incarnation_registry_removal]]. `no unbounded leftovers` INCONCLUSIVE (residual=43, unclassified).

## S31 — ca-gc-dryrun completeness under gc_shards>1 {#s31}

**Dev-scale (seed 20260707, gc_shards2 variant): INCONCLUSIVE 9/10 — effectively PASS.** 9 pass — the dryrun COMPLETENESS verdicts (dryrun does not MISS dead blobs under gc_shards>1) hold, `dangling=0`. The 1 INCONCLUSIVE is the recurring `no unbounded leftovers` classification-detail gap. Note: S31 (completeness = dryrun catches all dead) is the OPPOSITE direction from F3 (dryrun proposes EXTRA reachable blobs) — so the dryrun under-count in F3 is not a completeness miss. No CAS defect.

## S32 — TTL expiry reclaim {#s32}

**Dev-scale (seed 20260707): PASS 12/12.** Clean — TTL-expired parts reclaimed correctly; `dangling=0`, no leftovers, event audit clean. No CAS defect.

## S33 — concurrent explicit GC leaders (reclaim-leak regression guard) {#s33}

**Dev-scale (6 concurrent-GC collision rounds, seed 20260707): FAIL 8/10 — but the CORE guard PASSED; the FAIL is F3 (oracle variant).**
- **PASS (the point of S33 — the 2026-06-27 GC-CONCURRENT-LEADER-LEAK guard):** `SAFETY: no dangling under concurrent GC leaders`=0 (no over-delete, no data loss) + `LIVENESS: reclaimable drains to 0 after concurrent leaders + recovery`=0 + `fsck dangling=0`; `not_a_leader=8` (the lease correctly gated non-leaders). **The concurrent-leader reclaim leak stays FIXED under live concurrent explicit GC.**
- **FAIL = F3 (4th occurrence), oracle variant:** `dryrun ⊆ unreachable` — 34 candidates; but here fsck shows `unreachable=34 pending_gc=34` (the 34 are legitimately-condemned blobs IN the deletion pipeline), and the oracle's "unreachable" set excludes `pending-gc`. So this variant is the oracle being too strict (`dryrun ⊆ unreachable` should be `dryrun ⊆ (unreachable ∪ pending-gc)`), distinct from S18/S25/S26 where fsck called the proposed blobs fully reachable. Same root: the `dryrun ⊆ unreachable` oracle + dryrun reachability/classification don't align with fsck. No data loss. Folds into F3's backlog.

## S34 — create/drop churn: D1 bounded GC fanout {#s34}

**Dev-scale (40 create/insert/drop iterations, seed 20260707): FAIL 7/9 — CONFIRMS F5 (D1 fanout not bounded), correctness intact.**
- **The D1-goal check FAILED (headline):** `per-round GC fanout bounded` — **`CasRootGet` grew 32 → 248 monotonically across the 40 create/drop iterations** while `root_dirs` stayed flat at **2**. So the *live* root set is bounded, but per-round GC GET count scales with **tables-ever-created**, not live tables. D1 (`[[project_d1_shard_incarnation_registry_removal]]`) was meant to eliminate exactly this monotone namespace registry — the reclaim of dropped-namespace state is **incomplete**: something enumerated per historical table (dropNamespace tombstones / retired-generation runs) still accumulates and is re-read every round.
- **Correctness intact:** `fsck dangling`=0, `dropped content reclaimed to 0 (D1 reclaimable drain)` reclaimable=0 → no data loss, no over-delete, dropped content fully reclaims. This is a **GC-efficiency / D1-completeness defect, not a safety defect**.
- `no unbounded leftovers` = inconclusive (residual=33, fsck prefix-detail unavailable to classify — same harness gap as prior scenarios; content itself reclaims per the drain check).
- **This is the clean controlled corroboration of F5** (S30 showed 75→190 under mixed churn; S34 isolates it: root_dirs flat, CasRootGet linear in iterations). Same root cause; folded into the F5 backlog entry. **Design-sensitive** (D1 registry-reclaim path, same TLA+-gated area as the 2026-06-27 concurrent-leader leak) → backlog for code investigation, NOT an inline fix.

## S35 — rapid same-name rotation: D1 incarnation monotonicity {#s35}

**Dev-scale (30 tight `CREATE t; INSERT; DROP t` cycles on the same name `s35_rotation`, seed 20260707): effectively GREEN — 13/14 pass, the 1 inconclusive is the benign harness leftovers-classification gap.**
- **Resurrect invariant holds under speed (the point of S35):** `no dangling after rapid same-name rotation`=0 and `rotation residual reclaimed to 0 (D1 reclaimable drain)`=0 — reclaim racing recreate on the same name (greater incarnation) and the revive-races-reclaim window at speed produced **no dangling, no leak, no revived-condemned object**. `[[feedback_ca_resurrect_invariant]]` validated live.
- **Correctness:** `final recreated table queryable`=1, `S35 final-table replica agreement` identical checksum both nodes, `no bad CA-log events`=0, `no CREATE errors`=0, `no INSERT errors`=0, `event audit`=0, `GC no Failed rounds`=0.
- **F3 did NOT fire here:** `dryrun ⊆ unreachable` = 0 candidates / 0 unreachable → PASS. Confirms F3 is *not* universal — it only manifests when dead-but-misclassified blobs exist at the dryrun snapshot (S18/S25/S26 reachable-variant, S33 pending-gc-variant); when the store is clean at snapshot time, dryrun is correct.
- `no unbounded leftovers` = inconclusive (residual=43, fsck prefix-detail unavailable) — same harness gap as S33/S34, not a defect; the reclaimable-drain check separately confirms content reclaims to 0.
- S3 error rates (info): read max 10.4% / write max 4.2% — rustfs under mild pressure at this cadence, retries absorbed, no functional impact.

## Out-of-band notes / review comments {#notes}

**CI: new CAS S3 functional-test lane has no RustFS provisioning (P1) — recorded 2026-07-06.**

> The patch adds a new functional-test lane that depends on RustFS, but the startup code requires an unprovisioned binary in `ci/tmp`, causing that lane to fail deterministically in a clean CI workspace.
>
> Review comment:
> - **[P1] Provision RustFS before starting the new S3 CAS job** — `ci/jobs/scripts/clickhouse_proc.py:173-176`
>   When the `content_addressed` s3 storage functional-test job runs, `start()` now calls `CH.start_rustfs()`, but this method only checks for an already-existing `ci/tmp/rustfs` binary and returns `False` if it is missing. There is no tracked setup code that downloads or extracts this binary (unlike `setup_minio.sh`, which downloads MinIO), so the new CI lane will fail during environment startup before running any tests unless the runner happens to contain this per-workspace temp file.

**Verified (2026-07-06):** confirmed accurate. `clickhouse_proc.py:173-176` = `rustfs_bin = f"{temp_dir}/rustfs"; if not Path(rustfs_bin).is_file(): print("rustfs binary not found"); return False` — the comment above it says the binary is "extracted from rustfs/rustfs:1.0.0-beta.8" but NO tracked script does that extraction (grep across `ci/` for any rustfs download/wget/curl/install/extract/tar → none; `setup_minio.sh` by contrast downloads MinIO). `functional_tests.py:575` calls `CH.start_rustfs()` for the CAS s3 lane. The local `ci/tmp/rustfs.log` exists only because this workspace was provisioned by hand on 2026-06-13; a clean CI runner has no `ci/tmp/rustfs`. Fix direction: add a tracked `setup_rustfs.sh` (mirror `setup_minio.sh`) that pulls/extracts the `rustfs/rustfs:1.0.0-beta.8` static binary into `ci/tmp/rustfs`, invoked before the CAS s3 job's `start()`. Not fixed here (recording per request).

**Review findings (recorded 2026-07-06; reviewer said High, user + my verification agree these are MINOR / worth-attention, not High — narrowing nuance each):**

- **`read_only` `Store::open` still calls `PoolMeta::createOrValidate` (can write `_pool_meta`).** `CasStore.cpp:122` skips only the capability *probe* for `read_only`; `CasStore.cpp:134` calls `PoolMeta::createOrValidate` unconditionally, and `CasPoolMeta.cpp:129` does `casPut(..., expected=nullopt)` = create-if-missing. So a read-only mount over an EMPTY/wrong prefix either mutates storage or (with `<readonly>true</readonly>` → S3 write rejected) throws a write-permission error instead of cleanly reporting "pool absent". **Nuance (→ Minor):** for the real fsck use (`ca_ro` over a LIVE pool) `_pool_meta` exists → `createOrValidate` only validates, no write — verified by the F1 fsck smoke (exit=0, no error). Only bites read-only-over-empty. Fix: split create vs validate, or `create_if_missing=false` when `read_only`. (Only `gtest_cas_store.cpp:132` covers reopen-after-writable-create, missing this case.)

- **`ObjectStorageBackend::list` token kind mismatch in `EmulatedSingleProcess` mode.** `list` fills `lk.token` from `child->metadata->etag` (`CasObjectStorageBackend.cpp:781-783`) while emulated `head`/`get`/`putOverwrite`/`deleteExact` all use `emuObserveToken` (`:410,474,521,543,623,662`). A consumer using `listed.token` for `deleteExact` (orphan-manifest sweep, `CasOrphanManifestSweep.cpp:300`) can get a token mismatch → GC debris not deleted, in that mode. **Nuance (→ Minor / test-fidelity):** `EmulatedSingleProcess` is a "unit tests only" mode (`:240`); PRODUCTION uses `Native` S3 where both `list` and `head` return the S3 ETag, so they match — the bug is a test-fidelity gap (the emulated mode doesn't faithfully exercise the production token path), not a production delete-failure. Fix: emulated `list` returns `emuObserveToken(lk.key)` (under `emu_mutex`), or `supportsListTokens()==false` for that mode.

- **fsck under-reports orphan manifest bodies for ref-less namespaces.** The manifest-debris pass iterates `store.listNamespaces` (`CasFsck.cpp:371`), which discovers namespaces from `cas/refs/` + `roots/` only (`CasStore.cpp:1640`), NOT `cas/manifests/`. A namespace whose only remnant is a `cas/manifests/...` body (staged pre-`precommitAdd`, then a best-effort `Build::abandon` cleanup that missed it, `CasBuild.cpp:958`) is not enumerated → fsck won't flag it unreachable. **Nuance (→ Minor / diagnostics):** the GC orphan sweep scans `cas/manifests/` DIRECTLY, so GC still reclaims these — this is a fsck *diagnostics* under-report (weakens fsck as the authoritative consistency oracle), not a leak. Fix: have the fsck manifest pass also enumerate namespaces from `cas/manifests/`.

*Assumption (reviewer's, reasonable): `read_only` is strictly observe-only (both `PoolConfig` and `Store::open` comments say skip mutating probes, reads only).*

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

### S13 — process loss during write and GC

**What S13 checks:** hard-kills + restarts a writer (and the recent GC leader) repeatedly during finalize/publish windows while inserting + mutating; then verifies at quiescence: replica agreement, `fsck dangling==0`, bounded abandoned-precommit residual (==0 after forced GC), GC lease churn / no-failed-rounds, dryrun ⊆ unreachable, event-audit clean, memory + S3-budget observations.

**Dev-scale smoke (seed 20260707, F1-fixed stand) — 11/12 PASS, INCONCLUSIVE overall:**
- All safety verdicts PASS: `fsck dangling=0`; replica agreement (ch1/ch2 identical row-count+checksum on both tables); `no unbounded leftovers=0`; `abandoned-precommit residual bounded` = 0 after forced GC; `GC no Failed rounds` (0 + 1 benign concurrency-retry); `dryrun ⊆ unreachable`; `event audit` 0 bad rows; forced-GC drained `unreachable=0` in 1 round.
- The 1 INCONCLUSIVE is an **observability gate**, not a safety failure: "abandoned precommits reclaimed" wanted to see `precommit_reclaim/removed` EVENTS in-window (added=3123, reclaimed-in-window=0) — at dev scale the 4 kills missed the publish windows; the authoritative bounded-residual safety check (==0) passed. Expected to resolve at full scale (more kills).
- Resource/S3: peak MemoryResident 0.84 GB (no budget); S3 error rates read max 3.9%, **write max 16.4%** under chaos (store-dependent RustFS retry territory — recorded, watch at full scale).
- **No `UNKNOWN_DISK`, no "foreign writer", no exit-49** across the kill rounds — the F1 fix + P3.1 fence-recovery hold under chaos.

**Full-scale run (seed 20260707, 40 kill rounds, 20k×64KB inserts, 4 tables):** ran all 40 kill rounds, then **WEDGED in the end-checkpoint quiesce** — did NOT reach a verdict. Diagnosed objectively:

- **Correctness held under the chaos (the point of Task 6):** across 40 rapid crash-restarts of ch1/ch2, ch1 reached `writer_epoch=26` (≈24 clean fence-recoveries), `gc_fenced=0`, `state=live` — **zero exit-49 wedge, zero "foreign writer"** in the run window. That is exactly the P1 trigger (rapid crash-restart), so the P1 fix is validated at scale. GC-round churn logged benign `gc/state moved (another leader advanced it); retry next round` (expected concurrent-leader CAS serialization).
- **The wedge is an infra/scale limit, NOT a CAS correctness bug — finding F2:** S13's `randomString(65536)` payloads don't dedup, so the pool outgrew GC reclaim; near disk-full (94%), **rustfs returned `503 Service Unavailable`** (err.log 21:43:50, on the merge's blob upload). The CAS merge correctly retried the upload, so 6 `MERGE_PARTS` on `s13_churn_0` stuck at `progress=1.0` (finalize can't commit) → the replication queue never drained → `SYSTEM SYNC REPLICA` (issued by the quiesce, no client timeout) blocked indefinitely (`do_poll`) → the runner hung ~6+ min with the cluster otherwise healthy (replicas read-write, Keeper fine).
- **Secondary observation (F2b, CAS-relevant):** while wedged, the stuck merges' finalize-retries **re-uploaded blobs on every attempt**, growing the pool +47 GB in 8 min despite the workload being stopped — a retry-amplification/disk-leak under a persistently-503ing store. Worth a backlog note (a bounded/backoff-capped finalize-retry, or aborting the merge when the store is hard-down, would avoid amplifying disk under a failing object store).
- **Action taken:** killed the hung runner + `docker compose down -v` (reclaimed the 273 GB pool; disk 94%→73%). This confirms the user's rustfs-leak warning live. **Re-planning: re-run S13 at a payload size that fits the host's disk headroom** (reduce `payload_bytes` so the pool stays well under free space and rustfs doesn't 503), keeping the 40-round chaos — a full-chaos run within infra limits. Full-scale-with-64KB-payloads on THIS host (1.8 TB fs shared with builds) is infra-bound; a bigger-disk host or the S22 fault-proxy would be needed to push 64KB payloads at 40 rounds.

**Fitted re-run (payload_bytes=8192, 40 rounds, disk 74%/459 GB free): WEDGED AGAIN in quiesce** — same signature (8 merges stuck at `progress=1.0`, `SYNC REPLICA` hang, 77 rustfs `503`s). This DISPROVED the "near-disk-full triggers 503" theory and pinned the real root cause:

- **F2 refined root cause = rustfs#3231 (known upstream), NOT disk/payload.** `configs/rustfs.env` documents it: overwriting a >128 KiB object in an UN-VERSIONED bucket leaks the previous incarnation's data-dir; every `casPut` of a `cas/refs/<shard>` body leaks a uuid dir, and the metacache walk over the accumulated dirs produces `walk_dir timeout 5000ms` / `list_merged Io(timeout)` LIST storms → `503 Service Unavailable`. S13's **40-round ref-shard OVERWRITE CHURN** (4 tables × mutations × 40 rounds) is what accumulates the leaked dirs — independent of blob payload size or disk fullness (reproduced at 64 KB/94%-disk AND 8 KB/74%-disk). CAS-side mitigations already in the config (`root_shards=64` to keep shard bodies inline <128 KiB; journal batching) are insufficient at 40-round full-chaos. The merge-finalize retry + the quiesce's no-timeout `SYNC REPLICA` turn the transient 503 storm into an indefinite hang + a disk-leak (re-uploads).
- **This is an INFRA / upstream (rustfs beta.8) limit, not a CAS correctness defect.** It caps merge-heavy full-scale runs' quiesce on THIS stand and, by extension, the unattended 4h chaos soak — those need a rustfs without #3231 (or the fixed upstream) or the S22 fault-proxy stand. Backlogged as the dominant scale blocker.

**Verdict for Task 6 & S13:** the P3.1 mount-lease fix + F1 fix are **validated** — fence-recovery PASS (manual cycle + 40-round S13 chaos, `writer_epoch=26`, no wedge) and S13 dev-scale 11/12 (all safety verdicts green: dangling=0, replica agreement, bounded precommit residual). **CAS correctness under S13 process-loss chaos is confirmed.** The full-scale QUIESCE is infra-limited by rustfs#3231 (F2), not a CAS defect — two attempts (64 KB, 8 KB) both wedged identically; not re-running further (per "backlog complex infra, move on"). P1 backlog marked RESOLVED. **Operating adjustment for the rest of the night: run merge-heavy scenarios at a scale whose overwrite-churn stays under the #3231 LIST-storm threshold (dev / reduced rounds); a clean full-scale + 4h soak are gated on a #3231-free store.**
