---
description: 'Which ca-soak scenario cards exercise which CAS persisted formats, and how strongly each is exercised.'
sidebar_label: 'ca-soak card format coverage'
sidebar_position: 96
slug: /superpowers/cas/ca-soak-card-format-coverage
title: 'ca-soak card to CAS format coverage'
doc_type: 'reference'
---

# ca-soak card → CAS format coverage {#ca-soak-card-cas-format-coverage}

Read-only analysis of `utils/ca-soak/scenarios/cards/*.py` (skipping `_common.py`/`__init__.py`)
against the 17 registered CAS formats (`src/.../ContentAddressed/Formats/CasFormat.cpp` `TRAITS`).
One row per card **file** (19 files map to 44 scenario IDs, S01–S45; S24 does not exist). Cross-checked
against the framework helpers (`checkpoint.py`, `gc.py`, `observe.py`, `lifecycle.py`, `cluster_boot.py`,
`assertions.py`, `sql.py`, `base.py`) and, where format write triggers were unclear from Python alone,
against the C++ ref-ledger/GC source (`CasRefLedger.cpp`, `CasGc.cpp`).

## Methodology / two facts that shape every row {#methodology-two-facts-that-shape-every-row}

**1. Every card that runs shares a mount+GC baseline.** `run_one()` (`framework/run.py`) hard-resets
the cluster (fresh pool, fresh CA-disk mount) before calling `scen.run()`, and almost every card ends
with `_common.standard_end()` → `checkpoint.end_checkpoint()`, which drives at least one real
`SYSTEM CAS GC RUN` and shells out to the production `clickhouse disks cas-fsck`/`cas-gc-dryrun`
binary (product code reading the pool, not the card's own parsing). Mechanically this means **seven
formats are exercised by every one of the 19 cards**, independent of workload:
`cas_pool_meta` (mount/pool bootstrap), `cas_owner`, `cas_epoch`, `cas_mount_lease` (server-root
singletons, written at mount and renewed every ~10s for the run's duration), `cas_gc_state` /
`cas_gc_hb` (round state + heartbeat, written on every GC round including a no-op deferred one), and
`cas_gc_maintenance_state` (see the correction below — every GC round, deferred or folding, reads it
via `Gc::runNamespaceJanitorPage` and often writes it too). Below, these seven are marked **[baseline]**
and not re-justified per row unless a card does something *extra* with them (a real owner/epoch/lease
**transition**, not just steady renewal).

**Correction to the format README's own framing of `cas_gc_maintenance_state`.** The registry's
`README.md` bucket-map lists this object's writer as "future janitor," which reads as "not wired to
anything yet." That is stale: `NamespaceJanitor::runOnePage` (`CasNamespaceJanitor.cpp`) unconditionally
calls `readGcMaintenanceState` (a real `backend.get` of the `gc/maintenance_state` key) as its first
step, and `Gc::runNamespaceJanitorPage` is called from **both** branches of every GC round in
`CasGc.cpp` — the DEFER early-return (`CasGc.cpp:579`, `suppress_destructive=true`, read-only in
practice since a suppressed page never advances) and the normal fold path
(`CasGc.cpp:1089`, unconditional, no feature flag gating it) — where a page with no ambiguity, no
suppression, and the fence still held also issues a real `casGcMaintenanceState` CAS-PUT, advancing the
`janitor_cursor`. Since `forced_gc_to_fixpoint` (inside `standard_end`, used by nearly every card) issues
at least one explicit `SYSTEM CAS GC RUN` on the leader and then lets background GC continue polling for
up to 240s, this format's read (and usually write) path is exercised by essentially every runnable card
in the suite — it belongs in **[baseline]**, not in "formats no card reaches." This was verified directly
against `CasGcMaintenanceState.cpp`'s `readGcMaintenanceState`/`casGcMaintenanceState` (both hit the real
key via `layout.gcMaintenanceStateKey()`), not inferred from the README text.

**2. A second tier is exercised by any card that creates ≥1 CA table and inserts ≥1 row**: `cas_ref_catalog`
(namespace admission on `CREATE TABLE`), `cas_ref_log` (writer-commit path — confirmed in
`CasRefLedger.cpp` via `publishCkptContribution`/append-lane calls on every commit), `cas_ref_ckpt`
(same call sites — the checkpoint is updated on essentially every writer commit, not just at GC fold,
contrary to a naive reading of the format README), `cas_part_manifest` (one manifest per part build),
and `cas_blob` (upload). Marked **[write-path]**. The one card with **zero** tables and **zero** inserts
is **S23** (deliberately idle empty pool) — it still gets the `[baseline]` seven via mount+GC rounds, but
none of `[write-path]`.

**`cas_fold_seal` / `cas_run` / `cas_gc_outcomes`** are written only when a GC round actually **folds**
new work (a real condemn/delete/replace), not on an idle deferred round (confirmed by S03's own
"Phase 4 Lever A skip-unchanged" commentary and cross-checked against `CasGc.cpp`). A card is marked as
reaching these three only when its own actions (`DROP TABLE`/`TRUNCATE`/`DROP PARTITION`/a mutation that
obsoletes a part version/an OFF-CA `MOVE`) mechanically guarantee unreachable content for GC to fold —
not merely "a GC round ran." Ordinary background merges *can* incidentally trigger the same effect (an
old pre-merge part's blobs become unreachable), but that depends on the MergeTree merge scheduler's
timing, which the card's code does not control or guarantee — those cases are called out as
**incidental/unconfirmed**, not counted as reached.

**`cas_blob_meta`** (the per-blob dedup/condemn sidecar) is marked reached only where a card's own design
produces byte-identical content across two writes/nodes/replicas and specifically checks the dedup
counters (`CASBlobPutDeduplicated`/`CASBlobBodyPutAvoided`) or the metadata-adoption counters
(`CASMetaAdoptBackfill`/`CASMetaResurrectClean`, S41 only).

**`cas_ref_snap`** (the periodic full-state snapshot compaction) is dispatched by a **read-triggered
latch** inside `CasRefLedger.cpp` (`CASRefSnapshotPublishDispatched`/`...Backoff`), not by any SQL
statement a card issues. No card explicitly names or asserts this counter. Its coverage by any given
card is therefore **UNDETERMINED from the Python code alone** — confirming it would require running the
scenario and reading `CASRefSnapshotPublishDispatched`/`CASRefSnapshotPutBytes` from `system.events`, or
instrumenting the C++ dispatch condition. It is *plausible* that high-ref-volume cards (S03–S05, S08,
S12, S14, S42) trigger it incidentally, but this is not proven and is called out per row as
`ref_snap? (undetermined)`.

**`cas_gc_maintenance_state`** — corrected above: reached by every card via **[baseline]** (every GC
round reads it, most write it). Not a coverage gap.

**A discrepancy worth flagging on its own:** the module docstrings for S22, S27, S44, and S45 describe
them as `needs_infra` ("declared `needs_infra` and runs inconclusive", "requires cas-drop-member wired
into the compose image"). None of the four actually sets a non-`None` `needs_infra` class attribute —
the base class treats `needs_infra: None` as **runnable**, and `run_one()` only skips `run()` for a
scenario whose attribute is truthy. All four cards therefore attempt their full `run()` against the
runner's real compose variant (S22/S27 self-probe their fault-proxy's `/healthz` and degrade to
`Verdict.inconclusive` only if genuinely unreachable; S44/S45 have no such probe at all). This survey
treats all four as **runnable now**, not skipped, and notes the stale docstrings inline.

Legend: **[baseline]** = the seven mount/GC-round formats reached by every card (including
`cas_gc_maintenance_state`, per the correction above). **[write-path]** = the
five formats reached by any CREATE+INSERT. `*` = the card's own Python code parses/writes persisted CAS
object **bytes** directly (`boto3` S3 GET/PUT + manual zstd/JSON decode of a `cas_*` object) — as
opposed to using the server's SQL/system-table interface or shelling out to the real `cas-fsck` binary.

## Coverage table {#coverage-table}

| card (file) | scenario ids | compose it needs | formats exercised | why |
|---|---|---|---|---|
| `s01_s02_huge_blob.py` | S01, S02 | default (2-replica) | S01: [baseline] + [write-path]; S02: [baseline] + [write-path] + `blob_meta` | S01: one bounded-insert→`OPTIMIZE FINAL` merge into a single huge part exercises `blob`/`part_manifest`/`ref_log` at scale, plus explicit mid-write `gc_round()` calls beyond the generic checkpoint (extra `gc_state`/`gc_hb` traffic). No drop → `fold_seal`/`run`/`gc_outcomes` NOT confirmed (nothing made unreachable). S02: two tables get byte-identical `generateRandom` content; asserts `CASBlobBodyPutAvoided>0`/`CASBlobPutDeduplicated>0` — direct, explicit `blob_meta` exercise. |
| `s03_s05_scale.py` | S03, S04, S05 | default | S03: [baseline] + [write-path], `ref_snap?` (undetermined); S04: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes`; S05: [baseline] + [write-path], `ref_snap?` (undetermined) | S03: one table, many inserts + periodic tiny "touch" inserts, dozens of explicit `gc_drive_round()` calls (heavy `gc_state`/`gc_hb`); never drops anything, so fold/run/outcomes are only incidental-via-background-merge (not confirmed). S04: explicitly `DROP TABLE`s most of ~100 tables, asserts `unreachable>0` post-drop then `assert_reclaimable_drained` — confirmed real fold/delete. S05: 10000 sparse tables, one insert each, never dropped — `ref_catalog` admission at scale is the point, but no reclaim is ever forced. |
| `s06_s08_manifest_parts.py` | S06, S07, S08 | default | S06: [baseline] + [write-path] (heavy `part_manifest` — 1000–10000 columns); S07: [baseline] + [write-path] (negative/`LIMIT_EXCEEDED` test on `part_manifest`); S08: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` | S06 is the manifest-cap/wide-part exercise itself — one blob per column plus one manifest per part; column-subset read also proves selective `CASBlobGet`. S07 is a negative card probing the same manifest caps (usually inconclusive at dev scale — caps unreachable via SQL) with `expect_exception=True`. S08 creates ~thousands of 1-row parts with merges stopped, then re-enables merges and explicitly measures "physical bytes converge toward referenced" after `OPTIMIZE`+GC — confirmed real merge-driven reclaim. |
| `s09_s11_mutations.py` | S09, S10, S11 | default | S09: [baseline] + [write-path] + `blob_meta`, `fold_seal`/`run`/`gc_outcomes`; S10: same + explicit `assert_reclaimable_drained`; S11: same + explicit `assert_reclaimable_drained` | S09: `ALTER ... UPDATE` (incl. an identity `c0=c0` update) mechanically obsoletes old part versions (reclaim confirmed) and explicitly asserts `CASBlobBodyPutAvoided>0`/`CASBlobPutDeduplicated>0` for the identity update (`blob_meta`). S10: lightweight/heavy `DELETE` + patch-part probing, mid-burst explicit `gc_drive_round()` calls, `assert_reclaimable_drained("obsolete patch content reclaimed")`. S11: `ALTER ... DELETE WHERE bucket=N` interleaved with `OPTIMIZE`, `assert_reclaimable_drained("deleted part content reclaimed")`. |
| `s12_s14_faults.py` | S12, S13, S14 | **S12: `tenreplicas`** (`docker-compose-10replicas.yml`, 10 nodes); S13, S14: default | S12: [baseline] + [write-path] + `blob_meta`; S13: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes`, owner/epoch/mount_lease **transition** (plausible); S14: [baseline] + [write-path], owner/epoch/mount_lease **transition** (plausible) | S12: 10 replicas write a byte-identical shared block concurrently — asserts `CASBlobBodyPutAvoided>0` (confirmed `blob_meta` under real concurrency). S13: repeatedly hard-kills ch1 (writer) and the last GC-leader node while inserting/mutating; the interleaved `ALTER ... DELETE` mechanically obsoletes parts (confirmed reclaim), and every kill/restart forces the killed node's CA-disk to re-mount — a genuine owner/epoch/lease transition, though the card verifies it only via CA-log lease-event *counts*, not the object bytes. S14: a full clean restart of both servers after prefilling many tables/parts — per S38's finding, even a *clean* restart mints a fresh writer epoch/seal, so this is also a plausible epoch transition, though S14 asserts only startup-latency/counter behavior, not the transition itself. |
| `s15_s18_shards_lifecycle.py` | S15, S16, S17, S18 | **S15 uniquely needs three composes**: default, `gc_shards2` (`docker-compose-gc_shards2.yml`), `gc_shards8` (`docker-compose-gc_shards8.yml`) — it resets the cluster itself, mid-run, across all three, then resets back to default at the end; S16–S18: default | S15: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` (×3 shard variants); S16: same + `blob_meta`; S17: same; S18: same, `ref_catalog`/`part_manifest` **conditionally** on `ALTER TABLE FREEZE` succeeding (the card documents this as a known-risk B3 item that may simply fail) | S15 compares GC-shard fanout across gc_shards=1/2/8 after `DROP PARTITION`s — confirmed reclaim, and it does its own raw filesystem **directory-count** probe (`find .../gc/*/blob_target -type d`) which counts shard dirs but never decodes an object body (not `*`). S16: insert→`TRUNCATE`→condemn→re-insert cycles with deterministic content are the resurrect/dedup invariant itself — confirmed `blob_meta`, explicit `blob_reuse_resurrect` event check. S17: `DETACH`/`ATTACH`/`DROP DETACHED PARTITION` — confirmed reclaim of the dropped-detached content via `assert_reclaimable_drained`. S18: `FREEZE`/drop-live/`UNFREEZE` — if `FREEZE` fails (which the card explicitly anticipates and handles), only the ordinary live table's formats are touched; if it succeeds, the shadow's own manifest/ref-catalog exposure is asserted only via `fsck dangling`, not confirmed at the format level. |
| `s19_s22_clone_fetch.py` | S19, S20, S21, S22 | S19, S20, S21: default; **S22: `s3faultproxy`** (`docker-compose-s3faultproxy.yml`) | S19: [baseline] + [write-path] (heavy `part_manifest`/`ref_log` republish, no reclaim confirmed); S20: same + `blob_meta`, mount_lease **transition** (plausible); S21: [baseline] + [write-path] only (read-heavy, no reclaim); S22: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` | S19: `MOVE PARTITION`/`REPLACE PARTITION` republish manifests/refs without re-uploading bodies (checked directly: `CASBlobPut` stays small); a gated cross-disk `MOVE` is asserted to fail with zero blob writes. S20: stops/starts the follower (ch2) around a leader-only insert window, so the follower's later fetch/relink is the point — explicit `CASBlobPutDeduplicated`/`CASBlobBodyPutAvoided` check on the follower (`blob_meta`), and the stop/start is a real per-node mount cycle (plausible lease transition, not asserted at the object level). S21: pure read-path caching card, no `DROP`/mutation anywhere. S22: fault-injected `OPTIMIZE TABLE ... FINAL` under an armed S3 proxy forces real merges → confirmed reclaim as a side effect. |
| `s23_s27_misc.py` | S23, S25, S26, S27 (no S24) | S23, S25, S26: default; **S27: `s3listproxy`** (same `docker-compose-s3faultproxy.yml`, LIST-anomaly mode) | S23: [baseline] **only** — zero tables, zero inserts, explicit `SELECT count() FROM system.tables` check that the pool stays empty; S25: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes`, plus a **non-Atomic (`Ordinary`) database** namespace-path variant (often inconclusive — `Ordinary` is deprecated and frequently refused by the build); S26: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes`; S27: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` | S23 is the idle-pool baseline — dozens of explicit `gc_drive_round()` calls with nothing to reclaim, so it is the strongest single-card evidence for the `[baseline]` claim (`gc_state`/`gc_hb`/`gc_maintenance_state`/`pool_meta`/`owner`/`epoch`/`mount_lease` all reached with **zero** other formats). S25 attempts the full lifecycle (insert/detach/attach/freeze/mutate/drop-partition/drop-table/drop-database) under an `Ordinary` DB — confirmed reclaim on the final drop, *if* `CREATE DATABASE ... Ordinary` is accepted by the build (recorded honestly as inconclusive otherwise). S26 explicitly proves table-level verbatim `_files` (mutation/dedup-log entries — NOT one of the 17 formats, a raw passthrough per the format README) are cleaned by owner-path drop rather than `CASBlobDelete`, and separately confirms identical-insert dedup (`blob_meta`) and a real drop+GC at the end. S27 drops half of many tables under LIST-anomaly injection targeting `cas/refs/` and asserts the dropped content still fully reclaims — confirmed reclaim under fault. |
| `s28_s33_corner.py` | S28, S29, S30, S31, S32, S33 | S28, S29, S30, S32, S33: default; **S31: `gc_shards2`** | S28: [baseline] + [write-path]; S29: [baseline] + [write-path] (heavy inline-payload zone of `part_manifest` — the `PayloadHybrid` non-blob file path); S30: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` + `ref_catalog` (incarnation/tombstone specifically); S31: same as S30 + `gc_shards2`; S32: same + TTL-driven reclaim; S33: same, deliberately adversarial | S28: concurrent wide inserts stressing scratch, no drop — no reclaim confirmed. S29: a data-skipping-index/statistics part is the one card explicitly designed to make the manifest's inline-payload banner zone (not `.bin`/marks/`primary.idx`) large — direct `part_manifest` PayloadHybrid-path exercise. S30: rapid create/insert/drop of many distinct table names is precisely the `ref_catalog` namespace-tombstone/monotone-fanout regression guard (D1), with per-batch explicit GC rounds and a raw **directory-count** probe (`find roots/ -maxdepth 1 -type d`, again a dir count, not a body decode — not `*`). S31: same churn pattern plus `cas-gc-dryrun` completeness under `gc_shards=2`. S32: `TTL ... DELETE` expiry + `MATERIALIZE TTL`/`OPTIMIZE FINAL`, explicit `assert_reclaimable_drained`. S33: deliberately fires `SYSTEM CAS GC RUN` concurrently on both replicas (the one place in the whole suite that manufactures the two-leader fold-seal collision) — the strongest single-card exercise of `fold_seal`'s attempt-scoped-generation mechanics. |
| `s34_s35_d1_churn.py` | S34, S35 | default | S34: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` + `ref_catalog` (D1 tombstone); S35: same, plus incarnation-monotonicity specifically | S34 is S30's identical pattern re-run as a regression-**win** guard (D1 fixed the fanout), same raw directory-count probe (not `*`). S35 hammers the SAME table name through hundreds of create/insert/DROP cycles — the single strongest card for `ref_catalog`'s per-`(ns,shard)` incarnation-monotonicity property specifically (a wrong-incarnation revival would surface as a bad CA-log event, which the card asserts stays at zero). |
| `s36_s37_disk_move.py` | S36, S37 | **`multidisk`** (`docker-compose-multidisk.yml`) for both | S36: [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` + `blob_meta`, owner/epoch/mount_lease **transition** (chaos kill); S37: same, plus a clean full-cluster restart | S36: explicit `MOVE PART`/`MOVE PARTITION TO DISK 'ca'` (publishes via the normal build path — `part_manifest`/`blob`/`ref_log`) then back `TO DISK 'local1'` (drops the CAS refs, explicit "GC reclaims the vacated CA content" residual==0 check — confirmed reclaim), a dedicated dedup A/B differential (`blob_meta`, confirmed), and a chaos leg that hard-kills ch1 mid-`MOVE PART` (a genuine mount/epoch transition on ch1's restart, verified only via row-count/checksum atomicity, not the lease object itself). S37: policy-routed placement + `TTL ... TO VOLUME` moves (same TO-CA/OFF-CA reclaim pattern) plus both a clean full-cluster restart and a second chaos-kill leg mid-policy-move — two more transition events. |
| `s38_late_put_injection.py` | S38 | **`s38`** (`docker-compose-s38.yml`, published RustFS port 18121) | [baseline] + [write-path] + `ref_log`\* (asserted), owner/epoch/mount_lease **transition** (confirmed, logged) | **This is one of only two `*` cards.** It hard-kills ch1 mid append-storm, restarts it, then uses `boto3` to GET a real `cas_ref_log` object the server wrote, zstd-decompress + re-encode its `txn_seq`/`txn_epoch` JSON meta line by hand, and PUTs it back both at the sealed slot (expects a `412`/`409` conditional-create refusal) and above the seal (expects the object to be structurally unreachable). It also greps `system.text_log` for the literal "stale-looking mount lease" line, i.e. it specifically asserts the mount-claim observation happens on the unclean restart — the clearest single-card evidence of a real `cas_mount_lease`/epoch transition anywhere in the suite (short of S39). |
| `s39_lease_fault_tolerance.py` | S39 | **`s3faultproxy`** | [baseline] + [write-path], `cas_mount_lease` **(the prime target, deep transition/renewal exercise)** | The **only** card that targets `cas_mount_lease` specifically: it arms the fault proxy with `path_substring` equal to the exact key `gc/server-roots/<srid>/mount`, injecting isolated 503s at that one path to drive bounded renewal-retry pulses (asserted via `system.cas_mounts` `lifecycle/state/renewal_sequence` and `system.cas_log` `event_type='watermark_renew'`), then a past-TTL sustained outage forcing exactly one lease-loss generation and a full "whole-chain remount" recovery. All of this is via SQL/system-table queries, not raw byte decode of the mount-lease object itself — so it is heavy, targeted **exercise**, not `*`. |
| `s40_insert_dedup_outage.py` | S40 | default | [baseline] + [write-path], owner/epoch/mount_lease **transition** (plausible, ch2 kill) | Despite the title, "dedup" here is ClickHouse's **replicated-insert block dedup** (Keeper `block_id`/znode), the `renameParts` durability fix — **not** CAS content-level dedup, so `cas_blob_meta` is not confirmed by this card's design (though every row shares one fixed payload string, so incidental content-level dedup is plausible but not asserted). RustFS is `docker pause`d past the 90s CAS write budget while ch2 is killed and restarted mid-outage (a real mount transition on ch2, not independently verified against the lease object). `expect_exception=True` — CA-log `exception` rows are expected from the outage-induced insert failures. |
| `s41_wide_insert_baseline.py` | S41 | **`s41`** (isolated single-node `ca-s41` compose project; `--no-reset` required if `ca-soak` is also running) | [baseline] + [write-path] + `blob_meta` **(deepest exercise of this format's full state-transition set in the suite)** | Single-node perf-measurement card: fresh vs. duplicate-insert legs on both a plain-S3 policy (control) and the `ca` policy, explicitly protocol-checking that a **fresh** blob issues exactly one `HEAD` (miss) + one metadata "clean create" + one body publish, and a **duplicate** issues one `HEAD` (hit) + one metadata GET + zero body publish/zero create — this is a direct, assertion-level exercise of `cas_blob_meta`'s `CASMetaCreateClean`/`CASMetaCompareSwap`/`CASMetaAdoptBackfill`/`CASMetaResurrectClean` transitions, via `system.query_log` ProfileEvents (not raw byte decode — a `json.loads`/`boto3` grep hit on this file is a false positive: those are literal substrings in a stack-symbol bucket list and `S3GetObject`/`DiskS3GetObject` ProfileEvent names, not actual S3-client or JSON-decode code). The card explicitly `SYSTEM CAS GC STOP`s GC for the whole measurement window (restored at the end), so it does **not** exercise `gc_state`/`gc_hb`/`fold_seal`/`run`/`gc_outcomes` during its main body — only the final `standard_end` touches them generically. Being single-node, it has no replica/owner-transition story. |
| `s42_alloc_faults.py` | S42 | default | [baseline] + [write-path], owner/epoch/mount_lease **transition** (both nodes restart) | Arms `memory_tracker_fault_probability` on query threads specifically because the ref-append lane (`CasRefLedger::appendRefOps`) runs on the caller's thread — i.e. this card targets `cas_ref_log`'s write path under allocation failure more precisely than any other card, though its assertions are all counter/consistency-based (`CASRefNeedsRecovery==0`, pre/post-restart view equality), not a byte-level check. Ends with a full clean restart of both servers to rebuild the ref view from the durable journal (another epoch/mount transition, unasserted at the object level). |
| `s43_same_uuid_recreation.py` | S43 | **`s38`** (reuses the same published-port compose as S38) | [baseline] + [write-path] + `ref_log`\* (asserted, same mechanism as S38) + `ref_catalog` (fresh-incarnation-on-same-uuid) + `pool_meta` (bootstrap-refusal exercise) + `cas_owner`/`cas_mount_lease` (`SYSTEM CAS FORGET` unmount) | **The second `*` card.** Imports S38's own `boto3`/zstd/restamp helpers to GET a real donor `cas_ref_log` object, restamp it as a "survivor" transaction at `{epoch=1,seq=2}`, and PUT it directly into a pool it has just wiped clean (deleting every object under the prefix via raw `s3.delete_objects`, including `_pool_meta`) before restarting the servers. It asserts the servers **refuse to boot** (`CasPool.cpp`'s "refusing to bootstrap over residual data over a missing `_pool_meta`") by grep'ing the server's own stderr log for that exact string — strong `cas_pool_meta` exercise, though via log-grep rather than decoding the pool_meta bytes itself (so `pool_meta` is exercised, not asterisked). It also runs `SYSTEM CAS FORGET 'ca'` on both nodes and reads `system.cas_mounts` for a `vanished(...)` lifecycle string before wiping the prefix — a genuine, explicit unmount of `cas_owner`/`cas_mount_lease`. Finally it recreates the same table UUID and asserts the catalog allocates a **distinct opaque life id** — `ref_catalog` incarnation exercise. |
| `s44_rebirth_namespace_file_readers.py` | S44 | default (docstring says `needs_infra`; class attribute is `None` — it runs) | [baseline] + [write-path] + `fold_seal`/`run`/`gc_outcomes` + `ref_catalog` (rapid rebirth) | Rapid create/insert/`DROP TABLE ... SYNC` cycles with a **concurrent mutation writer** (`ALTER ... UPDATE`) running throughout — the mutation entry is itself a namespace-scoped file appended via the same `writeFile` life-keyed path, used here as the harness's only available SQL-level proxy for "a namespace-file reader/writer in flight across an incarnation boundary" (the card is explicit that this is a proxy, not a direct hook). Bypasses `_common.standard_end` entirely: calls `gc_mod.forced_gc_to_fixpoint` and `soak.fsck.run_fsck` directly. No `janitor`/`maintenance_state` reference anywhere despite the docstring's informal use of "janitor" (colloquial, not the `cas_gc_maintenance_state` mechanism). |
| `s45_decommission_hidden_removing.py` | S45 | default (docstring says `needs_infra`; class attribute is `None` — it runs) | [baseline] + [write-path] + `ref_catalog` **(the only card exercising the `Removing` catalog state / `cas-drop-member` decommission path)** + `cas_owner` (retirement) + `cas_mount_lease` (liveness-gate check) | The **only** card in the whole suite that decommissions a pool member: it drops the victim's own per-replica tables and `docker kill`s the victim *before* its own GC/janitor would condemn them (deliberately leaving `NsState::Removing` catalog rows "hidden"), then runs the real `cas-drop-member <srid>` CLI tool from a survivor and asserts its `namespaces_removed` count accounts for them. `cas-drop-member` itself polls the victim's `cas_mount_lease` `expires_at_ms` before proceeding ("pool member is alive or contended") and, on success, is the one place in the suite that plausibly sets a `cas_owner` `retired_at_ms`. Like S44, it bypasses `_common.standard_end`, calling `gc_mod.forced_gc_to_fixpoint` + `fsck_mod.run_fsck` directly. No `janitor`/`maintenance_state` literal reference either. |

## Formats no card reaches at all {#formats-no-card-reaches-at-all}

**None, after correcting one mistaken assumption.** All 17 registered formats are reached (in the weak
"touched on the live path" sense of this survey) by at least one card, and 7 of them
(`cas_pool_meta`/`cas_owner`/`cas_epoch`/`cas_mount_lease`/`cas_gc_state`/`cas_gc_hb`/
`cas_gc_maintenance_state`) are reached by essentially every card via the **[baseline]** mount+GC-round
mechanics (see the correction above for `cas_gc_maintenance_state` specifically — an earlier pass in
this same investigation initially misclassified it as unreached, taking the format README's "future
janitor" wording at face value instead of checking `CasGc.cpp`; `NamespaceJanitor::runOnePage` reads it
on every round, deferred or not, and writes it on most). The only format whose coverage is **not**
confirmed either way is the one below.

**`cas_ref_snap` — not confirmed reached by any card, but not confirmed absent either (UNDETERMINED).**
As explained above, its publication is dispatched by an internal read-triggered latch in
`CasRefLedger.cpp`, not by anything a card's SQL controls or asserts. No card names or checks
`CASRefSnapshotPublishDispatched`/`CASRefSnapshotPublishBackoff`/`CASRefSnapshotPutBytes`. Determining
real coverage would require running a high-ref-volume card (S03/S04/S05/S08/S12/S14/S42 are the best
candidates by insert/read volume) and reading those counters from `system.events`, or reading the exact
dispatch condition in `CasRefLedger.cpp` to determine what volume/read pattern triggers it.

## Minimal covering set {#minimal-covering-set}

Given the baseline/write-path facts above, 16 of the 17 formats (all but the undetermined `cas_ref_snap`)
are reached by any ordinary CREATE+INSERT+DROP card, since `cas_gc_maintenance_state` now joins the
`[baseline]` tier reached by mount+GC-round mechanics alone (see correction above) rather than needing a
dedicated card. The greedy cover is therefore short — it is driven entirely by the handful of formats
that need a **specific** fault/lifecycle shape, not by the bulk of the matrix:

1. **S16** (`s15_s18_shards_lifecycle.py`) — picked first because its insert→`TRUNCATE`→condemn→
   re-insert design is the single densest card: it hits every `[baseline]` and `[write-path]` format,
   plus `cas_blob_meta` (dedup/resurrect) and `cas_fold_seal`/`cas_run`/`cas_gc_outcomes` (real,
   repeated reclaim), all in one scenario. Contributes: `pool_meta`, `owner`, `epoch`, `mount_lease`,
   `gc_state`, `gc_hb`, `gc_maintenance_state`, `ref_catalog`, `ref_log`, `ref_ckpt`, `part_manifest`,
   `blob`, `blob_meta`, `fold_seal`, `run`, `gc_outcomes` — 16 of 16 confirmed-reached formats in one
   card (any other card running to its `standard_end`/GC-round baseline would do equally well for the
   `[baseline]` seven; S16 is picked for also covering every `[write-path]`-and-beyond format at once).
2. **S39** (`s39_lease_fault_tolerance.py`) — added because it is the *only* card that targets
   `cas_mount_lease` with real fault injection at the mount-lease key path and asserts its
   renewal/loss/remount transitions; S16 only touches this format via the trivial steady-state mount
   renewal. Contributes: `cas_mount_lease` (as a genuine transition, not just baseline presence).
3. **S45** (`s45_decommission_hidden_removing.py`) — added because it is the *only* card exercising
   `cas_ref_catalog`'s `Removing`/decommission state machine and the `cas-drop-member` path, and the
   only card with any evidence of `cas_owner`'s `retired_at_ms` retirement field. S16 only exercises
   `ref_catalog`'s ordinary admission/removal, not the decommission-specific state. Contributes: the
   `Removing`-state / decommission-specific facet of `ref_catalog`, plus `cas_owner` retirement.

**{S16, S39, S45}** covers all 16 confirmed-reached formats (`cas_gc_maintenance_state` included, via the
`[baseline]` correction above) except `cas_ref_snap` (undetermined by construction — no card's assertions
pin it down, so no card can be credited with "covering" it in a defensible way). A fourth card is
arguably worth adding for
completeness rather than strict minimality: **S41** is the only card that protocol-verifies `cas_blob_meta`'s
full `CASMetaCreateClean`/`CASMetaCompareSwap`/`CASMetaAdoptBackfill`/`CASMetaResurrectClean` transition
set (S16's dedup check is coarser — it only confirms `blob_reuse_resurrect` fires, not the metadata
sidecar's own state machine). If the goal is "smallest set that touches every format at all," {S16, S39,
S45} suffices; if the goal is "smallest set where every format's *interesting* transitions are actually
exercised," add **S41** for `blob_meta`.

## Cards that parse persisted bodies themselves (`*`) {#cards-that-parse-persisted-bodies-themselves}

Exactly two cards decode/construct real CAS object bytes in their own Python code, both for
**`cas_ref_log`** only — no other format is ever parsed by a card:

- **`s38_late_put_injection.py` (S38)** — GETs a real `cas_ref_log` object via `boto3`, zstd-decompresses
  it, edits the JSON meta line's `txn_seq`/`txn_epoch` fields, re-encodes and zstd-recompresses it, and
  PUTs it back both as a conditional-create (expecting a store-level refusal) and as a raw unconditional
  PUT above an epoch seal (expecting the object to be unreachable). Any change to `cas_ref_log`'s header
  line, meta-line field names (`txn_seq`, `txn_epoch`, `namespace`, `!prev_epoch`, `!prev_seq`), the
  `{"n":...}` trailer shape, or the `.zst` compression policy would break this card at the Python level,
  not just at the product level.
- **`s43_same_uuid_recreation.py` (S43)** — imports S38's exact same helpers
  (`_restamp_ref_log_txn`/`_zstd_decompress`/`_render_ref_txn_id`/`_s3_client`) to build and inject a
  "survivor" transaction into a freshly-wiped pool. Same wire-key exposure as S38, since it is literally
  the same decode/encode code.

No card decodes `cas_pool_meta`, `cas_owner`, `cas_epoch`, or `cas_mount_lease` bytes directly (S39/S43
read their *state* only via `system.cas_mounts`/`system.cas_log` SQL, and S43's pool-wipe/bootstrap-
refusal check is via a server-log grep, not a body decode). No card decodes `cas_part_manifest`,
`cas_blob`, `cas_blob_meta`, `cas_gc_state`, `cas_gc_hb`, `cas_gc_outcomes`, `cas_fold_seal`, `cas_run`,
`cas_ref_snap`, or `cas_ref_ckpt` bytes either — every other card's evidence is SQL/system-table/
ProfileEvents-based, or (S15/S30/S34) a raw filesystem **directory listing** (`find ... -type d`) that
never opens or interprets an object's body.


## How much of this was checked independently {#how-much-of-this-was-checked-independently}

This survey was produced by reading every card, and its row count matches the nineteen card files on
disk. Its most load-bearing claims were then re-verified against the source.

The verification is worth describing, because the first attempt at it was wrong in an instructive
way. An earlier draft of this document said `cas_gc_maintenance_state` was reached by no card, and
that claim was "confirmed" by searching every card for `maintenance_state` and `GcMaintenanceState`
and finding nothing. The search was accurate; the conclusion did not follow. A card does not have to
NAME a format to reach it — the server reaches it while executing what the card asks for. Here
`readGcMaintenanceState` is called unconditionally by `NamespaceJanitor::runOnePage`
(`CasNamespaceJanitor.cpp:13`), and `Gc::runNamespaceJanitorPage` runs from BOTH branches of every GC
round (`CasGc.cpp:579` on the defer path and `CasGc.cpp:1089` on the normal fold path). Since almost
every card drives a real GC round in its end checkpoint, that format is part of the universal
baseline rather than a gap. Naming and reaching are different questions, and a grep answers only the
first.

What does hold after re-verification:

- **Only S38 and S43 decode persisted object bytes in their own code**, both for `cas_ref_log` only.
  Searching every card for `zstd` and `decompress` returns exactly those two files. That is the whole
  surface on which a wire-key rename fails loudly instead of round-tripping silently.
- **`cas_ref_snap` coverage is UNDETERMINED**, and should be read as a gap rather than as coverage.
  Its publication is dispatched by a read-triggered latch in `CasRefLedger.cpp` that no card's SQL
  controls, and no card checks its counters. Settling it needs a run with
  `CASRefSnapshotPublishDispatched` read from `system.events`, not more code reading.

The remaining per-format attributions were not re-derived one by one.
