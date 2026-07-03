# Unattended night worklog — 2026-07-03

Plan (user, verbatim order): stop soak + summary → finish shard-mutation-queue → implement
gc-rebuild plan → config changes (root_shards=8 default, content_addressed_log default-on,
trim body under 128 KB) → 1h soak + triage → scenarios + repair → docker image.

## 1. Soak summary (copy-forward + scanner run, seed 20260703, phase 3, stopped at ~T+3h50m)

Run purpose: (a) validate copy-forward + self-remount under kill chaos, (b) test whether the
RustFS scanner cleans the overwrite leak.

**Findings:**

1. **Scanner experiment: NEGATIVE, root cause identified = rustfs#3231** (already documented +
   reverted in `configs/rustfs.env`, commit `188e0d3c92e`): overwrite of a >128 KiB object in an
   un-versioned bucket leaks the previous data dir. cas/refs = 39 GB of 45 GB pool at T+1h;
   167 GB at T+2h; per-key data dirs 1600 → 4088. `list_merged`/`walk_dir` LIST timeouts
   reproduced at ~600k files. Scanner reclaimed ZERO in >2h (bring-up works though — the old
   blocker was the unpinned mc).
2. **S3 stats** (per replica, T+1h): CasBlobHead 8.7M (gate revalidation — 57 HEAD per blob PUT);
   CasOtherCas 380k landed shard casPuts + 257k conflict attempts (40% avg, 92% peak);
   CasOtherGet 1.29M. Shard bodies 250-340 KB = journal tail (3157 events, 95% of body; live
   refs only 53 = 5 KB). Trim works but is fold-cadence-bound; trims stop entirely under LIST
   storms. → motivates shard-mutation-queue + trim-size config change (tonight's tasks).
3. **`mount_beat` introspection works**: 26-27 view-advance events per replica.
4. **BUG FOUND (mine, in copy-forward, S13-fix): FALSE `CORRUPTED_DATA` on real copy-forward.**
   `Build::copyForwardFromCondemned` verifies the payload by ONE-SHOT `CityHash128`, but the pool
   convention is the STREAMING `HashingWriteBuffer` hash (chunked CityHash128, block = 2048 B —
   `DBMS_DEFAULT_HASHING_BLOCK_SIZE`). Any payload > 2048 B diverges ⇒ verify falsely refuses ⇒
   attach brick returned: ch1 `ca_stress` readonly at 22:48:27 with
   `copyForwardFromCondemned: object .../a496d0c0... payload does not verify (payload hash
   9cf6e90d..., header hash a496d0c0..., expected a496d0c0...)`. Unit repro passed because test
   payloads were < 2048 B (single block = plain hash). Positive side: the copy-forward TRIGGER
   scenario reproduced in the wild (first real condemned-evidence reuse), and the fail-closed
   path behaved exactly as designed — the verdict was just wrong.
   **FIX (tonight, simple):** recompute via `HashingReadBuffer` over the payload + the
   `getHexUIntLowercase` → `hexToU128` chain the write path uses; RED test with a multi-block
   payload whose BlobId is minted by the streaming convention.
5. Oracle checkpoints passed until the brick (count 1.79M rows matched); `dangling=0` in the one
   fsck that completed; later fscks timed out (>180 s — the leak-bloated LIST, known B146/B154).
6. Chaos: 20+ faults fired; kills/restarts survived until finding 4 bricked ch1's attach.

Archive: `utils/ca-soak/logs/archive_copyforward_scanner_run_20260703T0130/` (ch logs 195 MB +
driver log). Pool destroyed (`down -v`).

## 2. Copy-forward hash fix — DONE (`9f92b3f5e92`)

`poolContentHash` (HashingReadBuffer through the write path's hex chain) replaces the one-shot
CityHash128 in `copyForwardFromCondemned`. Multi-block regression test red->green; the existing
verification tests re-minted with the shared `streamingHexOf` helper (plain `idOf` is a test-local
convention the verifier rightly refuses). Cas* 434/434 (+4 expected-red CasShardQueue).

## 3. Shard-mutation-queue (spec 2026-07-03-cas-shard-mutation-queue)

DONE: queue `20e21d96f8a`, metrics+docs `11ef5ec5146`. Cas* 438/438; CasShardQueue 5/5 x8.
Two real races found by the stress test during implementation:
- slow-exiting waiter erased a SUCCESSOR queue by key => two leaders => conflicts; erase is now
  identity-checked (map entry must still be OUR queue);
- test-harness: counters had to be filtered to cas/refs keys (bootstrap adopts + mount renewals
  also casPut), and co-batching made deterministic via a queue-depth probe.

## 4. GC rebuild (spec+plan 2026-07-03-cas-gc-rebuild)

- Task 1 TLA+ gate DONE (`bd16aff956d`): GRebuild + witness + 3 sabotages; SabotageRebuildDropEdge
  needed a `lostRefs` ghost set (dropping the folded ref also dropped the invariant's witness).
  Stage-1 clean; all old cfgs re-verified.
- Task 2 guard DONE (`6bd21f28c78`): per-shard trim-evidence refusal + (б) absent-seal audit.
  First cut keyed the audit on coverage EMPTINESS and tripped 9 tests — a present seal with empty
  per_ns_shard is a legitimate empty-universe generation; re-keyed on OBJECT absence.
- Task 3 rebuildBaseline DONE (`24b3362bfb8`): engine = existing bricks; health check gates FORCE
  (gen-0 bootstrap over trim-proving journals is NOT healthy); REAL FIND during testing —
  **pipeline blindness**: a blob with zero rebuilt edges has no run row and never transitions to
  zero => never reclaimed; the rebuild now condemns zero-edge physical blobs itself (head-captured
  tokens, minted round, floor-gated graduation). CasGcRebuild 7/7, Cas* 448/448.
- Tasks 4-5 (SYSTEM + clickhouse-disks wiring + docs) dispatched to a sonnet subagent.

## 5. Config changes — DONE (`cbe0ffb7608`)

content_addressed_log default-ON in programs/server/config.xml (experimental => keep enabled
recommendation); soak root_shards 64 -> 8 (16 files); gc_trim_body_soft_limit 8 MiB -> 96 KiB
(bodies stay under the RustFS 128 KiB inline threshold => rustfs#3231 leak sidestepped).

- Tasks 4-5 DONE by the subagent (`44d06a1e57a`, `fcf630bded7`): SYSTEM CONTENT ADDRESSED GC
  REBUILD [FORCE] (reuses the GC access type; refusal => BAD_ARGUMENTS with the reason),
  clickhouse-disks ca-gc-rebuild (read-only open + fresh gc_id + key=value report), operator
  runbook + 04-gc-protocol §gc-rebuild + ROADMAP row. CasGcRebuild+Guard 11/11; full link x2.
  Deviation noted: no dedicated gc-round-log row for rebuild (LOG_INFO + the gc_rebuild
  content_addressed_log event carry the audit); follow-up candidate.

## 6. 1h validation soak (seed 20260704) — THE BIG SAFETY FIND + queue validation

**Queue validated in production**: 200,675 mutations / 87,594 casPuts = 2.3x compression;
intra-server refs conflicts **11 total** (vs 257k/h on the previous binary); CasOtherCas ==
flushes exactly. Self-remounts + chaos kills survived.

**Config-physics finding (root_shards=8 + 96 KiB trim cap):** the cap cannot bind the body at 8
shards — trim only cuts BELOW the sealed cursor, and the tail-above-cursor is round-cadence-bound
(5-9 min of 8x-concentrated traffic = 300-600 KB). Observed body 588 KB; 8,471 leaked rustfs#3231
dirs on ONE key; refs plane 64 GB in 30 min. CONCLUSION for morning: at 8 shards the only body
lever is round cadence; either keep more shards on soak until the upstream fix, or accept the
leak, or drive rounds faster. (The cap is still right for healthy pools.)

**THE SAFETY BUG (fixed tonight, `c47d10d01ec`): clamped shards break the ack-floor graduation
lemma.** Chaos checkpoint went `persistent-dangling`: **31 dangling blobs + 1 dangling manifest**
(committed refs -> absent objects) — INV-NO-LOSS violated on the pre-fix binary. Forensics
(archive: `logs/archive_night_validation_20260703/`, full event+gc logs both replicas): 11 shards
CLAMPED continuously rounds 25-51; blob df30f113 condemned r32 -> pending r34 -> DELETED r35 while
its landed +1 sat UNFOLDED behind a clamp; the +1 folded at r52 => dangle. 'landed before the cut
=> folded before graduation' simply does not hold for clamped shards — the model's
SabotageSkipChangedShard counterexample realized (the sanctioned clamp path IS an unconsumed-
landed-events skip; nothing compensated).
**Fix (conservative, restores the lemma's precondition):** a pass whose fold recorded ANY clamp
anomaly is destruction-suppressed — no graduations, no redeletes (pending entries carry
UNCHANGED); condemn/spare continue; deletes resume on the first clamp-free pass. LOG_WARNING +
CasGcClampSuppressedPasses counter. Regression test reconstructs the incident exactly: RED without
the fix (blob deleted by round ~3), GREEN with it. Cas* 450/450.
**Morning follow-ups:** (1) why 11 clamps persisted 30 min (clamp REASONS are in the archived
server logs — true-removal-missing-body vs bodiless precommit attribution); (2) liveness under
long clamps (suppression stalls deletes — clamp-unsticking may need its own work); (3) TLA+ model
extension for clamps+suppression (currently argued, not machine-checked).

## 7. Docker image — DONE

`cas-nightly:20260703` (1.72 GB): stock clickhouse-server:25.8 base + the night's binary
(strip-debug, 926 MB) with symlinked server/client/disks. Smoke: `clickhouse-server --version`
= 26.6.1.1 (our build). Dockerfile in tmp/docker-cas-nightly/.

## 8. Scenario sweep (S01-S35, dev scale, fixed binary) — DONE

**Zero real failures.** 8 PASS (S02 S14 S17 S19 S24 S28 S32 S33); the ONE fail (S13 replica
divergence) was a CARD bug — the replica-agreement oracle ran BEFORE any sync, comparing replicas
with replication still draining after kill chaos; with the sync-gated oracle S13 re-ran PASS 11/11
(=> no data loss under kill chaos on the night binary). All seven scenarios that FAILED on the
previous binary (S10 S19 S20 S21 S23 S26 S31) are clean. Remaining inconclusives are honest gates:
scale-gated verdicts ("rerun at ci/full") and infra gates (S12 needs a multi-node Cluster
abstraction; S22/S27 need a fault-injecting / LIST-instrumented S3 proxy) — recorded in BACKLOG.
Framework improvements landed: replication_queue+replicas in standard extracts; S18 unblocked by
enable_system_unfreeze (7/8 now, was hard-error).

## 9. Night close-out

- Docker image `cas-nightly:20260703` REBUILT after the clamp-suppression fix (the safety fix is in).
- Branches: `cas-copy-forward` -> `cas-shard-mutation-queue` -> `cas-gc-rebuild` (linear; night work
  on the last one from `9f92b3f5e92` through this commit).
- Harness tests 148/148; Cas* unit suite 450/450; full binary link clean.
- MORNING QUEUE (complex/deferred): (1) clamp attribution — why 11 shards stayed clamped for 30 min
  (server logs archived; suspect true-removal-missing-body after mf_cleanup vs ABA re-fold);
  (2) clamp liveness — suppression stalls deletes while clamps persist; clamp-unsticking work;
  (3) TLA+ model extension for clamps+suppression; (4) root_shards-vs-leak physics decision for
  soak (8 shards concentrates rustfs#3231 8x; body is cursor-age-bound, not trim-bound);
  (5) rebuild follow-ups: dedicated gc-round-log row; fsck Orphan-class test gap;
  (6) S12/S22/S27 infra (multi-node Cluster, S3 fault proxy).


