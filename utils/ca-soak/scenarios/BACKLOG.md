# Scenario suite backlog

> **TRIAGE SWEEP 2026-07-03** (night binary: queue + copy-forward hash fix + clamp suppression +
> guard/rebuild). Status of the recurring entry classes below:
> - **RESOLVED**: `GC-CONCURRENT-LEADER-LEAK` (attempt-scoped generations, marked inline);
>   `S31-*-dryrun-shard0` (previewDeletes iterates all target shards); `S18-*-UNFREEZE`
>   (`enable_system_unfreeze` in soak configs); `S13-*-quiescence`/replica-divergence class (the
>   card compared replicas BEFORE any sync — sync-gated oracle landed, S13 PASS 11/11);
>   `S01-*-RSS-384MiB` (streaming putBlob); `S02 TOO_LARGE_STRING` (card SQL fixed earlier).
> - **SUPERSEDED**: `S03/S04-*-residual-unreachable` findings — the fsck pipeline classification
>   (`pending-gc`/`awaiting-gc`/`unaccounted`) makes the two-phase deletion lag an expected state;
>   residual-settling loops keep the summary counter. `HARNESS-DRAIN-VERDICT-CONVERGENCE` — same.
> - **STILL OPEN**: `NEEDS-INFRA-S12/S22/S27`; the three `SCENARIO (proposed)` ack-floor cards
>   (SIGSTOP floor hold, kill-mid-burst fence-out, request-budget guard — now release-gate items,
>   see `docs/superpowers/cas/ROADMAP.md §release-gates-2026-07-03`); `S07` manifest-cap needs
>   ci/full scale; B206 settle-gate tuning; B207 fsck phantom-dangling race (RESTORED to the
>   roadmap as a release gate).
> - `PRODUCT BUG (S13 mount self-recovery)` — RESOLVED by self-remount (2026-07-02, marked inline).


Findings, anomalies, missing instrumentation, flaky/inconclusive cases, suspected bugs, and proposed
fixes discovered while building and running the content-addressed scenario suite. Newest at the
bottom. Each entry: a short id/title, the run it came from, what was observed, and a proposed action.

## S01-20260627T203522-1: S01 ran at a small dev blob size; the memory-materialization risk is best expose

- **Logged (UTC):** 2026-06-27T20:35:36
- **Severity:** finding
- **Run:** 20260627T203522_S01_seed7
- **Observed:** S01 ran at a small dev blob size; the memory-materialization risk is best exposed at >= 256 MiB (use --scale ci/full)

## S01-20260627T204416-1: S01 peak RSS grew 384 MiB during a 64 MiB blob upload — investigate Build::putBl

- **Logged (UTC):** 2026-06-27T20:44:45
- **Severity:** suspected-bug
- **Run:** 20260627T204416_S01_seed11
- **Observed:** S01 peak RSS grew 384 MiB during a 64 MiB blob upload — investigate Build::putBlob materializing BlobSource into a String

## S01-20260627T204416-2: S01 ran at a small dev blob size; the memory-materialization risk is best expose

- **Logged (UTC):** 2026-06-27T20:44:45
- **Severity:** suspected-bug
- **Run:** 20260627T204416_S01_seed11
- **Observed:** S01 ran at a small dev blob size; the memory-materialization risk is best exposed at >= 256 MiB (use --scale ci/full)

## S02-20260627T204445-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 131. DB::Exception: Too ma

- **Logged (UTC):** 2026-06-27T20:45:00
- **Severity:** suspected-bug
- **Run:** 20260627T204445_S02_seed11
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 131. DB::Exception: Too many times to repeat (1048576), maximum is: 1000000: while executing function repeat on arguments toString(modulo(__table1.number, 10_UInt8)) String String(size = 0), 1048576_UInt32 UInt32 Const(size = 0, UInt32(size = 1)). (TOO_LARGE_STRING_SIZE) (version 26.6.1.1) | sql=INSERT INTO s02_first SELECT number AS id, repeat(toString(number % 10), 1048576) AS payload FROM numbers(64)

## S03-20260627T204500-1: forced GC did not drain unreachable to 0: residual=8 (classify object class + pr

- **Logged (UTC):** 2026-06-27T20:45:45
- **Severity:** suspected-bug
- **Run:** 20260627T204500_S03_seed11
- **Observed:** forced GC did not drain unreachable to 0: residual=8 (classify object class + prove bounded/expected)

## S04-20260627T204545-1: forced GC did not drain unreachable to 0: residual=112 (classify object class + 

- **Logged (UTC):** 2026-06-27T20:46:20
- **Severity:** suspected-bug
- **Run:** 20260627T204545_S04_seed11
- **Observed:** forced GC did not drain unreachable to 0: residual=112 (classify object class + prove bounded/expected)

## GC-CONCURRENT-LEADER-LEAK: Explicit SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION on >1 replica concurrently permanently orphans blobs (reclaim leak)

- **Logged (UTC):** 2026-06-27T21:01:18
- **Severity:** suspected-bug / HIGH (storage leak; safety preserved)
- **Run:** diagnostic (manual repro on cas-gc-part-manifest-impl @ ae0cc27b1bf5, CH 26.6.1.1)
- **Observed:** REPRO: fresh pool; create 3-4 ReplicatedMergeTree tables on storage_policy='ca' (wide parts), insert, DROP TABLE ... SYNC all; then issue explicit `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca` on BOTH replicas concurrently (or alongside the 2s background scheduler). Result: a stable residual of UNREACHABLE content blobs + part-manifests is left FOREVER (e.g. 5 blobs/~413KB incl a 411KB payload .bin + 2 _manifests tagged reclaimable-pre-precommit). fsck dangling=0 throughout (NO data loss); ca-gc-dryrun lists the orphans as deletable (zeroInDegree); but GC rounds report candidates_marked=0 after the first generation completes, and never delete them. GC log shows many Finish rows with outcome=Error ('CAS gc fold: fold seal ... occupied by divergent bytes (concurrent leader); retry' / ABORTED 236) and NotALeader; gc_fold_begin=40 vs gc_fold_end ok=11 (most folds aborted). CONTRAST: the SAME drop drained to unreachable=0 within ~5s when reclaimed by BACKGROUND GC alone, and single-node SERIAL explicit GC drained 16->0 in one round. So the leak is triggered specifically by CONCURRENT GC LEADERS — explicit `SYSTEM ... GC` (runGarbageCollectionRoundNow) appears NOT to be lease-gated the way the background CasGcScheduler is, so it can run concurrently with the other replica's lease holder. The fold seal correctly aborts the divergent fold (safety: no over-delete, dangling=0), but the colliding/aborted round advances GC generation/cursor state past owner-removal events that were never folded, so those blobs' in-degree never reaches zero in the persistent snapshot and they are never retired (a fresh full re-fold via dryrun still sees them as zero-in-degree). Liveness/reclaim bug, not safety.
- **Proposed action:** MAINTAINER DECISION (design-sensitive, TLA+-proven GC core — NOT auto-fixed). Options: (a) make explicit runGarbageCollectionRoundNow acquire/respect the GC lease+fence exactly like the background scheduler, so a non-leader explicit round is a clean NotALeader no-op (no concurrent fold); (b) make the fold-abort path on fold-seal divergence NOT advance the generation/cursor past unfolded owner-removal events (so a later round re-folds them); (c) both. Add a TLA+ liveness/coverage check that every owner-removal event is eventually folded even under concurrent-leader aborts. HARNESS already mitigated: forced_gc_to_fixpoint + gc_drive_round now drive a SINGLE replica serially (commit on scenarios branch) so the suite does not manufacture the collision; scenario S33 added as a regression guard that deliberately drives concurrent explicit GC and asserts no reclaim leak.
- **RESOLVED 2026-06-28 (attempt-scoped generation).** Root cause was structural: each per-round `gc/gen/<gen>/…` write-once artifact (fold seal, completion seal, in-degree run, part-manifest-cleanup bundle) was keyed by generation ALONE, so a deposed leader wrote a FINAL-key seal before its lease-guarded `gc/state` CAS failed → orphaned write-once seal → every later round recomputed the same generation, hit divergent bytes, and threw `ABORTED` "concurrent leader" forever (`foldDeltasIntoGeneration` also ignored the `putIfAbsent` outcome → divergent run vs seal). Fix: every per-round artifact (incl. `retired`/`outcomes`, which `RetireView` LISTs writer-side) is now written under the folding leader's **attempt** (`= lease.seq`, fresh per round) at `gc/gen/<gen>/attempt/<a>/…`; one lease-guarded fold-adopt CAS sets `(snap_generation, snap_attempt)` (CAS #1) and the completion advance inherits it (CAS #2); ALL readers/resume/prune/RetireView resolve only the adopted `(snap_generation, snap_attempt)`. A deposed leader's artifacts land under its own unadopted attempt — invisible to every decision path — so concurrent leaders never collide on a final-key seal; the next honest round folds a fresh attempt and drains. Deterministic artifacts now go through `putDeterministicArtifact` (byte-equal-or-`CORRUPTED_DATA`); unadopted-attempt debris is reclaimed by the wholesale `gc/gen/<g>/` retention prune. Spec `docs/superpowers/specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md`; plan `docs/superpowers/plans/2026-06-28-cas-gc-attempt-scoped-generation.md`; worklog `docs/superpowers/worklogs/2026-06-28-cas-gc-attempt-scoped-generation-worklog.md`. TLA+ Gate A green (`INV_ONLY_ADOPTED_VIEWABLE` + R0; sabotage `SabotageDeposedLeaderWritesFinalGen` counterexamples; inertness exact) committed `cd27ac6`; 10 code commits `e9d898d`..`b4dde7e` on `cas-gc-part-manifest-impl`; unit regression `CasGcAttempt.DeposedFoldAttemptDoesNotWedge` + decoy tests (`NonAdoptedAttemptSealIgnored`, `NonAdoptedAttemptRetiredSetInvisible`) all green. **S33 LIVENESS verdict flipped: a nonzero residual is now a real regression, not an intended signal.** Still TODO: run S33 on a soak host to confirm end-to-end drain (needs docker RustFS + 2-node cluster — not run in the unattended dev env).

## S03-20260627T210357-1: forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible

- **Logged (UTC):** 2026-06-27T21:04:38
- **Severity:** suspected-bug
- **Run:** 20260627T210357_S03_seed12
- **Observed:** forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 8}

## S04-20260627T210438-1: forced GC left 104 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:05:16
- **Severity:** suspected-bug
- **Run:** 20260627T210438_S04_seed12
- **Observed:** forced GC left 104 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 36, '_manifests': 68}

## GC-CONCURRENT-LEADER-LEAK-ROOTCAUSE: Root-cause (read-only investigation) of the concurrent-leader reclaim leak

- **Logged (UTC):** 2026-06-27T21:05:34
- **Severity:** diagnosis / MAINTAINER DECISION (design-sensitive, TLA+-proven core)
- **Observed:** Call path: InterpreterSystemQuery::runContentAddressedGarbageCollection (InterpreterSystemQuery.cpp:2165) -> ContentAddressedMetadataStorage::runGarbageCollectionRoundNow (ContentAddressedMetadataStorage.cpp:282) -> CasGcScheduler::runOneRoundNow (CasGcScheduler.cpp:159) -> Cas::Gc::runRegularRound (CasGc.cpp:71). Explicit GC DOES acquire the lease (CasGc.cpp:76 acquireOrRenewLease) but a lease window still admits two concurrent folds. MECHANISM: fold() computes new_generation=snap_generation+1, putIfAbsent's the fold_seal (CasGc.cpp:441); on PreconditionFailed it compares bytes and THROWS ABORTED on divergence (CasGc.cpp:444-446) BEFORE updating snap_generation + CAS'ing gc/state (CasGc.cpp:450-451). So the loser's gc/state never advances to N+1. fold_seal and gc/state are NOT atomic. Next round reads cursors from generation N (CasGc.cpp:215 / readSealedCursors CasGc.cpp:1129); if N has no seal, parent_cursors is EMPTY and the fold restarts from cursor=0, but the durable fold_seal(N+1) written by the winner is never re-adopted into gc/state, so owner-removal events folded in the divergent N+1 fold are never re-folded -> those blobs' in-degree never reaches 0 -> never retired. A fresh full re-fold (ca-gc-dryrun previewDeletes) still sees them at zero in-degree, hence the dryrun-vs-actual discrepancy. Relates to TLA+ MonotoneGC cursor invariant (CaGcRootLocalPartManifestCore.tla:1112-1114, 208-214).
- **Proposed action:** MAINTAINER DECISION — all options touch the fold-seal divergence check that exists to catch concurrency violations; re-run the TLA+ model after any change. Ranked options from the investigation: (1) LOCALIZED (lowest-risk, ~4 lines at CasGc.cpp:441-447): on divergent/PreconditionFailed fold_seal, ADOPT the durable fold_seal (decode existing bytes into result.fold_seal) instead of throwing, then proceed to the gc/state CAS (CasGc.cpp:450-451) which still serializes a single winner via ABORTED — caveat: this weakens a deliberate concurrency-violation tripwire, so it MUST be re-checked against the proven safety invariants. (2) reverse write order: CAS gc/state (snap_generation=N+1) BEFORE writing fold_seal, so the pointer never lags the seal (medium risk; changes crash-recovery in tryResumeIncompleteRound CasGc.cpp:1345). (3) full atomic fold_seal+snap_generation transaction (design change, high risk). RECOMMEND maintainer evaluate (1) with a TLA+ liveness/coverage extension proving every owner-removal event is eventually folded even under concurrent-leader aborts. Suite mitigations already in place: single-leader GC driving (gc_drive_round/forced_gc_to_fixpoint) + S33 regression guard.

## S03-20260627T211033-1: forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible

- **Logged (UTC):** 2026-06-27T21:11:20
- **Severity:** suspected-bug
- **Run:** 20260627T211033_S03_seed13
- **Observed:** forced GC left 8 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 8}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S04-20260627T211753-1: forced GC left 112 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:18:40
- **Severity:** suspected-bug
- **Run:** 20260627T211753_S04_seed20
- **Observed:** forced GC left 112 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 40, '_manifests': 72}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S05-20260627T211840-1: forced GC left 225 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:21:33
- **Severity:** suspected-bug
- **Run:** 20260627T211840_S05_seed20
- **Observed:** forced GC left 225 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 225}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S06-20260627T212133-1: scenario raised: invalid literal for int() with base 10: '2026-06-27 21:June:59'

- **Logged (UTC):** 2026-06-27T21:22:17
- **Severity:** suspected-bug
- **Run:** 20260627T212133_S06_seed20
- **Observed:** scenario raised: invalid literal for int() with base 10: '2026-06-27 21:June:59'

## S07-20260627T212217-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-06-27T21:24:34
- **Severity:** finding
- **Run:** 20260627T212217_S07_seed20
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S11-20260627T213227-1: forced GC left 183 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:33:28
- **Severity:** suspected-bug
- **Run:** 20260627T213227_S11_seed20
- **Observed:** forced GC left 183 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 63, '_manifests': 120}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S12-20260627T213328-1: NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool tes

- **Logged (UTC):** 2026-06-27T21:33:29
- **Severity:** finding
- **Run:** 20260627T213328_S12_seed20
- **Observed:** NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services

## S14-20260627T213424-1: forced GC left 166 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:38:19
- **Severity:** suspected-bug
- **Run:** 20260627T213424_S14_seed20
- **Observed:** forced GC left 166 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 166}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S16-20260627T214003-1: scenario raised: forced_gc_to_fixpoint() got an unexpected keyword argument 'max

- **Logged (UTC):** 2026-06-27T21:40:20
- **Severity:** suspected-bug
- **Run:** 20260627T214003_S16_seed20
- **Observed:** scenario raised: forced_gc_to_fixpoint() got an unexpected keyword argument 'max_rounds'. Did you mean 'max_seconds'?

## S18-20260627T214048-1: S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Except

- **Logged (UTC):** 2026-06-27T21:48:09
- **Severity:** finding
- **Run:** 20260627T214048_S18_seed20
- **Observed:** S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Exception: Support for SYSTEM UNFREEZE query is disabled. You can enable it via 'enable_system_unfreeze' server setting. (SUPPORT_IS_DISABLED) (version 26.6.1.1) | sql=SYSTEM UNFREEZE WITH NAME 's18_snap_20'

## S22-20260627T214938-1: NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) be

- **Logged (UTC):** 2026-06-27T21:49:39
- **Severity:** finding
- **Run:** 20260627T214938_S22_seed20
- **Observed:** NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) between ClickHouse and RustFS; not in the current compose (direct rustfs1 endpoint)

## S24-20260627T215022-1: NOT RUN — requires a storage_conf disk config with a tiny dedup_cache_bytes; cur

- **Logged (UTC):** 2026-06-27T21:50:23
- **Severity:** finding
- **Run:** 20260627T215022_S24_seed20
- **Observed:** NOT RUN — requires a storage_conf disk config with a tiny dedup_cache_bytes; current compose mounts only the default (64 MiB) config — no small-cache variant

## S25-20260627T215023-1: scenario raised: Node(localhost:8124) HTTP 404: Code: 81. DB::Exception: Databas

- **Logged (UTC):** 2026-06-27T21:50:40
- **Severity:** suspected-bug
- **Run:** 20260627T215023_S25_seed20
- **Observed:** scenario raised: Node(localhost:8124) HTTP 404: Code: 81. DB::Exception: Database s25db does not exist. (UNKNOWN_DATABASE) (version 26.6.1.1) | sql=CREATE TABLE s25db.s25_ordinary (id UInt64, payload String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/s25db_s25_ordinary','{replica}')
ORDER BY (id)
SETTINGS storage_policy='ca', min_bytes_for_...(74 more chars)

## S26-20260627T215040-1: forced GC left 296 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:51:13
- **Severity:** suspected-bug
- **Run:** 20260627T215040_S26_seed20
- **Observed:** forced GC left 296 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 63, '_manifests': 233}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S27-20260627T215113-1: NOT RUN — requires an instrumented object store / proxy that returns duplicate o

- **Logged (UTC):** 2026-06-27T21:51:14
- **Severity:** finding
- **Run:** 20260627T215113_S27_seed20
- **Observed:** NOT RUN — requires an instrumented object store / proxy that returns duplicate or unstable LIST pages for root-shard token listing; not available with the direct rustfs endpoint

## S30-20260627T215209-1: S30 confirmed checklist #6: GC per-round fanout (roots/<ns> dir count and/or Cas

- **Logged (UTC):** 2026-06-27T21:53:03
- **Severity:** suspected-bug
- **Run:** 20260627T215209_S30_seed20
- **Observed:** S30 confirmed checklist #6: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations even though no table stayed live — dropNamespace leaves a permanent GC registry entry (monotone fanout). Backlog: namespace registry needs a cleanup/deregister path.

## S30-20260627T215209-2: forced GC left 100 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possib

- **Logged (UTC):** 2026-06-27T21:53:03
- **Severity:** suspected-bug
- **Run:** 20260627T215209_S30_seed20
- **Observed:** forced GC left 100 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 43, '_manifests': 57}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S31-20260627T215303-1: forced GC left 55 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-06-27T21:53:48
- **Severity:** suspected-bug
- **Run:** 20260627T215303_S31_seed20
- **Observed:** forced GC left 55 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 31, '_manifests': 24}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S33-20260627T215417-1: scenario raised: forced_gc_to_fixpoint() got an unexpected keyword argument 'max

- **Logged (UTC):** 2026-06-27T21:54:37
- **Severity:** suspected-bug
- **Run:** 20260627T215417_S33_seed20
- **Observed:** scenario raised: forced_gc_to_fixpoint() got an unexpected keyword argument 'max_rounds'. Did you mean 'max_seconds'?

## S16-20260627T220940-1: GC log has 9 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-27T22:10:34
- **Severity:** suspected-bug
- **Run:** 20260627T220940_S16_seed21
- **Observed:** GC log has 9 Failed (Error) finish row(s)

## S25-20260627T221034-1: GC log has 1 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-27T22:11:02
- **Severity:** suspected-bug
- **Run:** 20260627T221034_S25_seed21
- **Observed:** GC log has 1 Failed (Error) finish row(s)

## S33-20260627T221102-1: GC log has 11 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-27T22:11:35
- **Severity:** suspected-bug
- **Run:** 20260627T221102_S33_seed21
- **Observed:** GC log has 11 Failed (Error) finish row(s)

## SUITE-RUN-2026-06-28: Full 33-scenario suite run (seed 20, dev scale) — results summary

- **Logged (UTC):** 2026-06-27T22:14:36
- **Severity:** summary
- **Observed:** PASS (5): S02 huge-dup-blob, S13 process-loss, S17 detached, S28 concurrent-scratch, S32 TTL-reclaim. INCONCLUSIVE (12): S01 (RSS not attributable at dev scale — rerun ci/full), S03/S07/S08/S09/S18/S29, and needs-infra S12/S22/S24/S27. FAIL (16). Two distinct buckets: (A) GC-CONCURRENT-LEADER-LEAK recurrence — reclaimable residual (blobs/_manifests) after forced GC in S04,S05,S11,S14,S26,S30,S31 (all drive explicit GC under a drop/merge workload, colliding with background GC). by_prefix examples: S04 {blobs:40,_manifests:72}, S26 {blobs:63,_manifests:233}. (B) GC 'Error' finish rows (same root cause, the divergent-fold abort symptom) tripping 'GC no Failed rounds' in S16,S25,S33. S33 is the intentional regression guard (designed to FAIL until the core bug is fixed). Card bugs found+fixed during validation (NOT product bugs): harness _now_str used '%M' (ClickHouse month name, not minutes) corrupting since_event_time for all cards' log queries — fixed to toString(now()); S06 int() on the timestamp; S16/S33 called forced_gc_to_fixpoint(max_rounds=) after the param rename to max_seconds; S25 created the Ordinary DB on one node only. Other real-verdict fails to triage later: S10 (lightweight-delete/patch-part oracle), S19 (gated cross-disk move not failing closed), S20 (follower-refs), S21 (read-path thresholds), S23 (idle GC op/mem budget) — may be conservative card thresholds or secondary findings.
- **Proposed action:** All failures in buckets (A)+(B) are the single known GC-CONCURRENT-LEADER-LEAK (see that backlog item) — the suite reproduces it across 7+ workloads that follow the README 'drive explicit GC at checkpoints' contract. Triage S10/S19/S20/S21/S23 individually (likely card-threshold tuning). Rerun S01 at --scale ci/full to attribute the Build::putBlob memory risk.

## GC-CONCURRENT-LEADER-DANGLING-SUSPECTED: S16 hot-cycle left fsck dangling=1 at quiescence (SUSPECTED over-delete under concurrent GC leaders) — NOT robustly reproduced

- **Logged (UTC):** 2026-06-27T22:14:36
- **Severity:** suspected-bug / SAFETY (unconfirmed)
- **Run:** S16 seed21 run; manual repro attempt came back dangling=0
- **Observed:** S16 (hot insert/truncate/GC-retire/re-insert resurrection cycle, driving explicit GC that collided with background GC — 9 GC Error rounds) ended with final QUIESCED fsck dangling=1 (a live ref to missing content) + unreachable=0. If real, this would ESCALATE GC-CONCURRENT-LEADER-LEAK from a liveness/reclaim leak to a SAFETY over-delete (a concurrent-leader round deleting a blob a freshly-republished resurrection ref still needs). HOWEVER a focused manual repro (6 hot cycles with deliberate concurrent GC on both nodes, then quiesce+fsck) returned dangling=0 — so this is rare/flaky or a near-quiescence measurement artifact (a blob upload mid-publish at the final re-insert), NOT confirmed.
- **Proposed action:** Maintainer: investigate whether concurrent GC leaders can over-delete a resurrected blob (would be a safety bug). Reproduce by tightening timing: hammer concurrent explicit GC continuously WHILE a resurrection re-insert publishes, across many iterations, asserting dangling==0 at quiescence each time. The background-GC-only path (the production default + the soak) keeps a single leader and is NOT expected to hit this. Tracked under the same root cause as GC-CONCURRENT-LEADER-LEAK.

## GC-CONCURRENT-LEADER-DANGLING-FORENSICS: S16 dangling=1 — full blob lifetime reconstructed from the saved system.content_addressed_log dump

- **Logged (UTC):** 2026-06-27T22:44:22
- **Severity:** diagnosis (downgrades the suspected safety finding)
- **Run:** scenarios/runs/20260627T220940_S16_seed21/raw/ca_events_summary_*.tsv
- **Observed:** Mined the saved full CA-log dump (the per-run raw extract). All 5 real content blobs in the hot cycle follow the CORRECT incarnation-token resurrection pattern, e.g. blob 21b5e7f2..: blob_put(fresh, tok ad4f..) -> fold +1/-1 edges -> indeg_zero -> blob_retire(tok ad4f..) -> blob_delete(exact tok ad4f..) -> recheck deleted; re-insert -> blob_put(fresh, tok 67d8..) -> ... -> blob_delete(exact tok 67d8..); re-insert -> blob_put(fresh, tok 7945..) -> blob_reuse_adopt(tok 7945.., reason "observed token not condemned; adopted the live incarnation"). Every revival is a FRESH re-upload with a NEW token; every delete is exact-token; every adopt verifies the token is NOT condemned. NO condemned-token adoption, NO resurrect-invariant violation. Therefore the final-checkpoint dangling=1 was a TRANSIENT delete->reupload (or manifest mid-publish) window captured a hair before quiescence settled, NOT a durable over-delete.
- **Proposed action:** Downgrade GC-CONCURRENT-LEADER-DANGLING-SUSPECTED: no evidence of an over-delete safety violation; the resurrection path is correct. The remaining real issue stays the LIVENESS leak (GC-CONCURRENT-LEADER-LEAK). Harness now auto-captures forensics/object_lifetimes.json + forensics/fsck_detail_by_class.json on ANY dangling/unreachable at a checkpoint, so a future recurrence carries its full story automatically. To fully rule out a rare transient dangling, the S16 quiesce could add a short post-reinsert publish-fence before the final fsck.

## SOAK-4H-DISK-INFEASIBLE-AT-HIGH-OPRATE: Literal 4h active-workload chaos soak needs reduced workers on this host (RustFS scanner-off roots/ retention)

- **Logged (UTC):** 2026-06-27T22:48:35
- **Severity:** harness/infra finding
- **Observed:** At workers=6 the shared pool's roots/ grew ~2.4GB/min because RustFS runs scanner+heal OFF (rustfs.env) — every manifest CAS overwrite is retained as a distinct on-disk version, never compacted. With ~225GB headroom above the disk_watchdog 60GiB floor that fills in ~60-90min, BEFORE the phase-3 chaos window even starts (96min into a 4h timeline). So a 4h run at high op-rate gets watchdog-stopped pre-chaos.
- **Proposed action:** Use WORKERS=2 (the documented rustfs.env multi-hour strategy) so growth (~0.8GB/min) fits a 4h timeline + chaos. The real fix is op-count/manifest-rewrite-rate reduction (B157) or a compacting object store, not harness config. The scenario suite does NOT hit this (fresh pool + short per-card runs).

## SOAK-DISK-GROWTH-ROOT-CAUSE-CORRECTED: Disk growth under soak = ClickHouse inactive-part backlog (x replicas) of manifests, NOT GC inefficiency or root-shard version retention

- **Logged (UTC):** 2026-06-27T22:57:07
- **Severity:** analysis (corrects earlier framing)
- **Observed:** Measured the workers=2 pool live: total 2.1G (blobs 1.3G, roots 794M of which _manifests=256MB in 38,224 objects; mutable shard/_watermark/_files objects negligible). CH local dirs ~1G (off the CA disk). system.parts: only 22 ACTIVE parts but 18,344 INACTIVE parts on node1 (ClickHouse holds merged-away parts for old_parts_lifetime before removal). Manifests are IMMUTABLE, one per part, and each replica publishes its OWN — so ~18K parts x2 replicas ~= 38,224 manifest objects. CA GC is reclaiming manifests fast and ACCELERATING: manifests_deleted/round 375->679->1078->2367->4761, 10,522 deleted in 14 rounds; objects_deleted(blobs)=0 because blobs stay referenced until their manifests drop (blob reclaim trails manifest reclaim). blob xl.meta is large because RustFS INLINES small blobs into xl.meta (617MB xl.meta ~= inlined content, not metadata bloat). The earlier rustfs.env note framed the growth as per-object VERSION retention of mutable roots/ objects — that is NOT the dominant factor here; it is the COUNT of immutable manifest objects tracking the inactive-part backlog x replicas.
- **Proposed action:** GC is efficient; the growth is bounded by ClickHouse inactive-part retention + merge churn, drained by GC as parts are removed (at a quiesced checkpoint manifests drop back to ~live-parts level — matches the controlled insert+OPTIMIZE test draining to unreachable=0). The runaway at high op-rate (workers=6, 234GB) is create-rate (parts/manifests) outpacing ClickHouse part-cleanup + CA GC + the scanner-off backend not compacting tombstones — a throughput/backend artifact, not a GC correctness/efficiency bug. Levers: lower merge/insert churn (fewer workers), shorter old_parts_lifetime, or B157 op-rate reduction; a compacting object store removes the physical-retention tail.

## SOAK-INACTIVE-PARTS-NOT-STUCK: 18K inactive parts is in-grace working set under high churn, NOT a removal failure

- **Logged (UTC):** 2026-06-27T23:00:17
- **Severity:** analysis
- **Observed:** old_parts_lifetime=480, cleanup_delay_period=30, max_part_removal_threads=auto(32). Of 14,072 inactive parts: 12,781 are <8min (within grace), only 1,291 are >8min with the OLDEST just 609s (~10min) — i.e. pinned at grace + a ~2min cleanup-batch lag, not aging unbounded. RemovePart=4069 in the last 5min (~814/min) so removal actively keeps pace; 32 removal threads not saturated. Same 5min: NewPart=2614, MergeParts=870, DownloadPart=2572 (~6k part-events/min) — very high churn for workers=2, dominated by cross-replica part FETCH traffic. So the inactive backlog = creation_rate x 8min grace (bounded steady state), and CA GC reclaims each manifest as its part is removed (10.5k manifests reclaimed in 14 rounds, keeping pace).
- **Proposed action:** No removal bug: removal is cheap and tracks the 8min grace. The disk working-set is bounded by churn x grace and collapses at quiescence (controlled test -> unreachable=0). To shrink the working set: lower part-churn (fewer/larger inserts, fewer workers), shorten old_parts_lifetime, or reduce cross-replica DownloadPart churn. The unbounded physical tail only appears on the scanner-off test backend (no tombstone compaction).

## SOAK-TTL-BAND-CHECKPOINT-FAILURE: 4h chaos soak failed on TTL-band oracle ambiguity during post-fault recovery (NOT a CA correctness bug)

- **Logged (UTC):** 2026-06-28T00:48:14
- **Severity:** soak-harness/oracle limitation
- **Run:** tmp/soak_4h_20260628T004751.log + failure.json
- **Observed:** Soak progressed through all pre-chaos stages and INTO chaos (fault#1 = rustfs restart 13s). The gc_checkpoint passed clean: fsck dangling=0, dryrun_count=0, count=1,734,614. After the rustfs-restart fault the recovery checkpoint failed with "ambiguous TTL band still non-empty after 6 waits (a row stays within 10s of its TTL boundary; genuinely stuck scheduling)". This is the soak deterministic oracle's known TTL-edge limitation: a row sitting within +/-10s of its TTL DELETE boundary cannot be predicted exactly, and the rustfs-restart fault delayed TTL materialization so the band never cleared in 6x11s. dangling=0 held throughout; no data loss, no GC-leak/over-delete. Also note: at ~150GB pool the entry/post-GC fsck timed out (>180s, B146/B154), so checkpoint pool-consistent gates were skipped (best-effort).
- **Proposed action:** Not a CA bug. To get a clean multi-hour chaos result on this host: (a) avoid the TTL-edge oracle ambiguity (disable TTL in the soak table for long chaos runs, or widen the ambiguous-band wait, or skip the exact aggregate assert when chaos delayed TTL materialization); (b) the O(pool) fsck timeout at large pool needs the streaming/sharded fsck or a smaller pool; (c) the scanner-off backend retains tombstones so the pool grows under sustained churn. A 4h literal run needs a compacting object store + a streaming fsck + a TTL-robust oracle. The CHAOS correctness that WAS exercised (gc_checkpoint + 1 fault) showed dangling=0.

## S01-PUTBLOB-MEMORY-BLOWUP-CONFIRMED: CA blob upload uses ~6.5x part size in peak memory (CA adds ~4.5x over local), linear+unbounded — large parts OOM

- **Logged (UTC):** 2026-06-28T05:23:23
- **Severity:** suspected-bug / HIGH (memory; README first-investigation-target)
- **Observed:** Measured INSERT peak memory_usage (system.query_log) for single big parts on storage_policy=ca, max_insert_threads=1: 256MiB->1.63GiB, 512MiB->3.25GiB, 1GiB->6.50GiB, 2GiB->13.00GiB — LINEAR at ~6.5x part size. CONTROL on a local (non-CA) MergeTree disk, same parts: 1GiB->2.01GiB, 2GiB->4.01GiB (~2x = normal insert block overhead). So the CA path adds ~4.5x the blob size in EXTRA peak memory (1GiB:+4.5GiB, 2GiB:+9.0GiB), linear and unbounded. S3 multipart IS used (DiskS3CreateMultipartUpload>0), so the blow-up is the CA layer materializing the staged blob in memory BEFORE the already-streaming S3 upload, not the upload. At the 28GiB/node mem_limit this OOMs at ~4GiB parts; the spec 100GiB S01 target would need ~650GiB. CH 26.6.1.1, branch cas-gc-part-manifest-impl.
- **Proposed action:** README first-investigation-target: Build::putBlob materializes the staged BlobSource into a String before putIfAbsentStream. Fix = stream the blob from its staged temp file into putIfAbsentStream (and ensure the hash-before-upload pass also streams), keeping peak ~buffer-sized regardless of part size. Localized write-path I/O change; does NOT touch GC/TLA+ safety invariants. Candidate for a TDD+review fix. The ~4.5x (not 1x) suggests several simultaneous full copies (staged String + hash buffer + put buffer) to eliminate.

## S01-PUTBLOB-MEMORY-FIXED: FIXED+VERIFIED: Build::putBlob now streams the staged source instead of materializing it in a String

- **Logged (UTC):** 2026-06-28T05:33:12
- **Severity:** fix (verified, uncommitted on cas-gc-part-manifest-impl)
- **Observed:** Fix: uploadFromSource signature String->const BlobSource& (re-invocable); putBlob dropped the WriteBufferFromString pre-materialization and streams source.write_payload (caller already re-reads the staged temp file) straight into putIfAbsentStream; size check via sink count()-delta. INV-1 preserved (re-upload re-reads the writers own temp file across the whole retry window — the temp file lives until cleanupPendingTempFiles after putBlob returns; never GETs the dying object). HEAD-first dedup still skips the body. New unit test CasBuild.PutBlobStreamsSourceOnceNoFullMaterialization + existing INV-1/condemned/vanish gtests pass; ninja clickhouse + unit_tests_dbms exit 0. VERIFIED (independent re-measure): 2GiB INSERT peak memory_usage 13GiB -> 4.33GiB (~2.15x, = local-disk baseline); 1GiB 6.5->2.33GiB; 256MiB 1.63GiB->787MiB. The unbounded ~4.5x linear blob copy is eliminated (ratio converges to ~2x as part grows). fsck dangling=0 after drop+GC; read-back correct.
- **Proposed action:** Minor residual follow-up (NOT the blocker): the rare condemned-displacement putOverwrite path still materializes on-demand (backend putOverwrite is whole-body-only, no streaming variant) — only hit on an INV-1 revival/displacement, not the common fresh upload. A streaming putOverwrite would remove that last full copy. Fix is left UNCOMMITTED for maintainer review (commit when ready, branch not master).

## S01-MEMORY-RESIDUAL-2X-IS-BLOCK-BOUNDED-NOT-CA: Post-fix residual ~2x peak is generic max_block_size buffering (tunable to O(block)), NOT a CA cost or a floor

- **Logged (UTC):** 2026-06-28T05:41:13
- **Severity:** analysis
- **Observed:** After the putBlob streaming fix, a 2GiB single-part CA insert peaks ~4.33GiB (~2x) under DEFAULT settings REGARDLESS of row shape (512x4MiB and 32768x64KiB both 4.33GiB), because the source read block (max_block_size, default 65505 rows) holds the whole part when its rows fit in one block. Forcing bounded blocks (max_block_size=1024, min_insert_block_size_bytes=32MiB) drops the SAME 2GiB CA insert to 358MiB (~12x less) — O(block), constant in part size. So CAs upload sink streams (no whole-body buffer); the residual is generic ClickHouse insert read-block buffering, identical on a local disk, tunable, and NOT part-proportional in general.
- **Proposed action:** No further CA work needed for the memory blocker: CA contributes O(block) not O(part). For an extreme huge-single-file ingest, cap max_block_size / min_insert_block_size_bytes to keep peak flat (~hundreds of MiB) regardless of part size. The S01 card could add a verdict that CA peak tracks block size (set a small max_block_size and assert peak stays bounded as blob size grows) to lock this in as a regression guard.

## S01-MEMORY-PROFILER-ATTRIBUTION: Memory profiler (trace_log MemorySample) attribution of the post-fix CA insert peak

- **Logged (UTC):** 2026-06-28T05:58:32
- **Severity:** analysis (evidence)
- **Observed:** 1GiB CA part insert, peak memory_usage=2.33GiB, memory_profiler_sample_probability=1, min_allocation_size=1MiB, symbolized via allow_introspection_functions. Top allocation sites by bytes: (1) 2048MiB / n=1 — FunctionRandomString::executeImpl -> PODArray::reallocPowerOfTwoElements: the ColumnString holding the randomString output; 1GiB of data rounds to a 2GiB power-of-two capacity in a SINGLE live block = the peak. (2) 1008MiB / n=63 — WriteBufferFromS3::allocateBuffer: S3 multipart upload part-buffers (~16MiB each, allocation CHURN freed per completed part, not peak-resident; peak concurrent bounded by inflight-parts). (3..) <=62MiB: S3 resize, Ca*WriteBuffer, compression, ReadBufferFromFile — negligible. CRITICAL: there is NO source_bytes / WriteBufferFromString / BlobSource whole-blob allocation anywhere — the putBlob String materialization is ELIMINATED at the allocation level (before the fix it would be a blob-sized String alloc here). Confirms: peak is the insert column block (block-bounded; drops to 358MiB with small max_block_size), partly a randomString test-data artifact; CA->S3 adds only bounded multipart buffers (churn), not an O(part) copy.
- **Proposed action:** CA memory blocker fully resolved + evidence-backed. If an absolute floor matters for huge single-file ingest: cap max_block_size (shrinks the column block) and tune s3 multipart inflight/part-size (shrinks the WriteBufferFromS3 churn). The randomString 2x-via-power-of-two is a generic ColumnString property + a test-data artifact, not CA. No further CA action needed.

## GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS: GC root-shard discovery does a recursive, re-enumerated-per-page LIST over all of roots/ -> ~O(N^2) in the manifest backlog

- **Logged (UTC):** 2026-06-28T06:06:30
- **Severity:** suspected-bug / perf-scalability (correctness-safe)
- **Observed:** S3 ListObjectsV2 is recursive/flat by default (no delimiter). Gc::listRootShardTokens (CasGc.cpp:1168) lists layout.rootsPrefix() with backend.list(prefix,cursor,1000) and filters non-shard keys in-process. But ObjectStorageBackend::list (CasObjectStorageBackend.cpp:564) calls object_storage->listObjects(prefix, children, max_keys=0) = enumerate ALL keys under roots/ into memory + std::sort, then returns a 1000-key window after cursor. So the paging loop RE-ENUMERATES + RE-SORTS the entire roots/ prefix on EVERY page: ~N/1000 page-calls x a full N-key S3 enumeration each = ~O(N^2/1000) S3 LIST round-trips + O(N) mem per page, N = all objects under roots/ (dominated by the manifest backlog, 38k+ in the soak; only ~128 are actual shard objects). Recursion pulls _manifests/_files/watermarks/shadow too. This is the mechanism behind the B146/B154 fsck/GC timeouts at large pool: discovery cost explodes super-linearly with the manifest count -> gc_checkpoint fsck timed out (>180s) at ~150GB -> no reclaim -> pool grew. Correctness-safe (registry is the universe authority; LIST is only a token-diff accelerator).
- **Proposed action:** Two independent fixes: (1) ObjectStorageBackend::list must NOT re-enumerate the whole prefix per page — paginate at the source (carry a real continuation token) or enumerate once per sweep and window in memory. (2) Do not recurse over all of roots/ to find shards: either list each namespace shard container roots/<ns>/store/<uuid>@cas@/ with Delimiter="/" (returns the numeric shard leaf objects as Contents and _manifests/_files as CommonPrefixes -> O(shards), no manifest enumeration), or drive the accelerator from the registry-known (ns,shard) set with batched HEADs/scoped lists. Either keeps discovery O(shards) instead of O(all roots objects). README surprise-checklist #5, now with the max_keys=0 re-list-per-page wrinkle.

## IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST: IDEA: move shard objects to one common flat prefix + stable cached @cas@ reference -> GC discovery = a single LIST

- **Logged (UTC):** 2026-06-28T06:09:53
- **Severity:** design-idea (proposed fix for GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS)
- **Observed:** Root cause of the quadratic GC discovery: the GC-hot mutable shard objects live INSIDE each table tree (roots/<ns>/store/<uuid>@cas@/<shard>), interleaved with _manifests/ and _files/, so the token-diff accelerator must recursively LIST all of roots/ (enumerating the 38k+ manifest backlog to find ~128 shards). PROPOSAL: relocate every shard object into ONE common flat prefix (all namespaces shards together, key encodes (ns,shard) e.g. shards/<ns_id>/<shard_idx>); the table tree keeps @cas@ as a STABLE, CACHEABLE reference (pointer) to its shards rather than their physical home. Because namespaces are UUID-keyed the table->shards mapping never churns, so the reference is resolved once and cached — and if the mapping is made deterministic it is a pure function (no stored indirection object).
- **Proposed action:** BENEFIT: GC per-round "what changed" probe becomes a SINGLE LIST over the one shard prefix -> returns exactly (ns,shard)->token(ETag), O(shards), no manifest/_files noise, lines up 1:1 with the registry universe. _manifests/_files stay per-table and are never enumerated by GC (read on demand per owner transition). SCOPE: a CasLayout change (shard-key derivation + mutateShard/precommit/promote write to the common prefix; per-table @cas@ becomes the stable read-side reference). Still pair with the max_keys=0 re-list-per-page fix, but now over a tiny prefix. Pre-release / no persisted data => layout change is free (no migration). Open Qs: shard-key encoding of (ns,shard); whether the reference is a stored object or a computed mapping; interaction with gc_shards>1 blob_target sharding (orthogonal — this is root-shard discovery, not blob reduce).

## ZZ-RESUME-STATE-2026-06-28: RESUME INDEX — where we left off (open items + uncommitted artifacts) before the TLA+ sidetrack

- **Logged (UTC):** 2026-06-28T06:18:54
- **Severity:** resume-pointer
- **Observed:** Scenario suite (utils/ca-soak/scenarios/: framework + 33 cards S01-S33) built+run 2026-06-27/28 on branch cas-gc-part-manifest-impl. ALL UNCOMMITTED. Per-run artifacts in runs/ (gitignored); RUN_HISTORY.md + this BACKLOG committed-side.
- **Proposed action:** OPEN, by priority:
  [1] GC-CONCURRENT-LEADER-LEAK (HIGH, correctness/liveness) -> TLA+ design decision (the sidetrack). Root cause GC-...-ROOTCAUSE (non-atomic fold_seal/gc_state CasGc.cpp:441-451; divergent-fold abort drops owner-removal from cursor). Recommended: adopt-divergent-fold_seal + a TLA+ liveness invariant proving every owner-removal is eventually folded under concurrent-leader aborts.
  [2] GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS (HIGH perf) -> clean sub-fix: max_keys=0 re-list-per-page in CasObjectStorageBackend.cpp:564; bigger win: IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST.
  [3] COMMIT: the scenarios/ suite AND the Build::putBlob memory fix (S01-PUTBLOB-MEMORY-FIXED: CasBuild.cpp/.h + gtest_cas_build.cpp) — both verified, uncommitted on the branch.
  [4] TRIAGE secondary card fails S10/S19/S20/S21/S23 (likely card thresholds vs real).
  [5] needs-infra: S12/S22/S24/S27; inconclusive cap-test: S07.
  [6] soak follow-ups: TTL-band oracle (SOAK-TTL-BAND-CHECKPOINT-FAILURE) + 4h infeasibility on this host (SOAK-4H-DISK-INFEASIBLE).
CLOSED/explained (no action): S16 dangling=racy-fsck FP; disk-growth=inactive-part backlog x replicas; residual 2x memory=block-bounded. Soak stack may be left up (docker compose down -v to reclaim).

## S04-20260629T215542-1: GC log has 12 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T21:56:28
- **Severity:** suspected-bug
- **Run:** 20260629T215542_S04_seed42
- **Observed:** GC log has 12 Failed (Error) finish row(s)

## S04-20260629T215542-2: forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-06-29T21:56:28
- **Severity:** suspected-bug
- **Run:** 20260629T215542_S04_seed42
- **Observed:** forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 32, 'other': 56}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S05-20260629T215628-1: GC log has 13 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T22:02:09
- **Severity:** suspected-bug
- **Run:** 20260629T215628_S05_seed42
- **Observed:** GC log has 13 Failed (Error) finish row(s)

## S07-20260629T220525-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-06-29T22:07:43
- **Severity:** suspected-bug
- **Run:** 20260629T220525_S07_seed42
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S07-20260629T220525-2: GC log has 2 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T22:07:43
- **Severity:** suspected-bug
- **Run:** 20260629T220525_S07_seed42
- **Observed:** GC log has 2 Failed (Error) finish row(s)

## S10-20260629T221451-1: GC log has 2 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T22:15:18
- **Severity:** suspected-bug
- **Run:** 20260629T221451_S10_seed42
- **Observed:** GC log has 2 Failed (Error) finish row(s)

## S12-20260629T221610-1: NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool tes

- **Logged (UTC):** 2026-06-29T22:16:11
- **Severity:** finding
- **Run:** 20260629T221610_S12_seed42
- **Observed:** NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services

## S13-20260629T221611-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-06-29T22:24:43
- **Severity:** suspected-bug
- **Run:** 20260629T221611_S13_seed42
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S14-20260629T222443-1: GC log has 10 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T22:31:45
- **Severity:** suspected-bug
- **Run:** 20260629T222443_S14_seed42
- **Observed:** GC log has 10 Failed (Error) finish row(s)

## S04-20260629T232730-1: GC log has 13 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T23:28:18
- **Severity:** suspected-bug
- **Run:** 20260629T232730_S04_seed7
- **Observed:** GC log has 13 Failed (Error) finish row(s)

## S04-20260629T232730-2: forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-06-29T23:28:18
- **Severity:** suspected-bug
- **Run:** 20260629T232730_S04_seed7
- **Observed:** forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 32, 'other': 53}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S05-20260629T232818-1: GC log has 16 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T23:34:31
- **Severity:** suspected-bug
- **Run:** 20260629T232818_S05_seed7
- **Observed:** GC log has 16 Failed (Error) finish row(s)

## S06-20260629T233431-1: GC log has 1 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T23:36:29
- **Severity:** suspected-bug
- **Run:** 20260629T233431_S06_seed7
- **Observed:** GC log has 1 Failed (Error) finish row(s)

## S07-20260629T233629-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-06-29T23:38:37
- **Severity:** finding
- **Run:** 20260629T233629_S07_seed7
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S10-20260629T234547-1: GC log has 1 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-06-29T23:46:18
- **Severity:** suspected-bug
- **Run:** 20260629T234547_S10_seed7
- **Observed:** GC log has 1 Failed (Error) finish row(s)

## S12-20260629T234708-1: NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool tes

- **Logged (UTC):** 2026-06-29T23:47:08
- **Severity:** finding
- **Run:** 20260629T234708_S12_seed7
- **Observed:** NOT RUN — compose provides only 2 replicas (ch1/ch2); 10-replica shared-pool test requires a new docker compose with 10 ClickHouse services

## S13-20260629T234708-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-06-29T23:55:41
- **Severity:** suspected-bug
- **Run:** 20260629T234708_S13_seed7
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S14-20260629T235541-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-06-30T00:01:56
- **Severity:** suspected-bug
- **Run:** 20260629T235541_S14_seed7
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S33-20260701T094759-1: GC log has 1 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-07-01T09:48:35
- **Severity:** suspected-bug
- **Run:** 20260701T094759_S33_seed20260701
- **Observed:** GC log has 1 Failed (Error) finish row(s)

## S04-20260701T094933-1: GC log has 4 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-07-01T09:50:21
- **Severity:** suspected-bug
- **Run:** 20260701T094933_S04_seed20260701
- **Observed:** GC log has 4 Failed (Error) finish row(s)

## S04-20260701T094933-2: forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-07-01T09:50:21
- **Severity:** suspected-bug
- **Run:** 20260701T094933_S04_seed20260701
- **Observed:** forced GC left 32 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'blobs': 32, 'other': 56}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S05-20260701T095021-1: GC log has 15 Failed (Error) finish row(s)

- **Logged (UTC):** 2026-07-01T09:56:52
- **Severity:** suspected-bug
- **Run:** 20260701T095021_S05_seed20260701
- **Observed:** GC log has 15 Failed (Error) finish row(s)

## S33-20260701T133614-1: GC log has 2 real (non-benign) Error finish row(s)

- **Logged (UTC):** 2026-07-01T13:36:59
- **Severity:** suspected-bug
- **Run:** 20260701T133614_S33_seed20260701
- **Observed:** GC log has 2 real (non-benign) Error finish row(s)

## S03-20260701T133659-1: GC log has 1 real (non-benign) Error finish row(s)

- **Logged (UTC):** 2026-07-01T13:37:40
- **Severity:** suspected-bug
- **Run:** 20260701T133659_S03_seed20260701
- **Observed:** GC log has 1 real (non-benign) Error finish row(s)


## NEEDS-INFRA-S12: S12 ten-replica harness wiring — compose exists, Cluster abstraction missing

- **Logged (UTC):** 2026-07-02
- **Severity:** infra / harness gap (not a CA product bug)
- **Observed:** `docker-compose-10replicas.yml` now defines ch1..ch10 sharing one CA pool on RustFS + Keeper with serialized probe gating (each node waits for the previous to be healthy to avoid the `_probe/token` CAS race). Per-node configs (`storage_conf_10replicas_ch{3..10}.xml`, `macros_node{3..10}.xml`) are committed. Remaining gap: `soak/cluster.py` `Cluster` is hardcoded to exactly two nodes (`node1`/`node2`, ports 8123/8124) via `_DEFAULTS`. The `run.py` runner creates `Cluster()` with no node-count argument; `cluster_boot.wait_healthy`, `_prep_log_dirs`, `archive_server_logs` all assume exactly ch1/ch2. Wiring S12 to run a real 10-node workload requires all of the following:
  1. Make `Cluster` variadic: accept a list of `(host, port, container)` tuples or a node-count and derive them. Keep the default 2-node path unchanged (additive).
  2. Make `_prep_log_dirs` and `archive_server_logs` in `cluster_boot.py` enumerate the actual node list from the variant (or accept a `nodes` argument) instead of hardcoding `ch1`/`ch2`.
  3. Make `wait_healthy` poll all nodes in the cluster (already calls `cluster.nodes()` — only needs the Cluster to carry all 10 nodes).
  4. Implement S12's `run()`: create a `ReplicatedMergeTree` table on all 10 nodes, run parallel inserts from all 10, assert pool bytes ≈ unique content (not 10x), assert replica convergence across all 10, assert only one GC leader makes progress per round.
- **Variant registered:** `"tenreplicas"` in `_VARIANT_FILE` → `docker-compose-10replicas.yml`. S12's `needs_infra` reflects the remaining harness gap.
- **Why deferred:** rewriting `Cluster` risks breaking the 2-node default path shared by all 35 scenarios. The correct approach is additive: extend `Cluster.__init__` to accept an optional `nodes` list; keep the existing 2-node defaults when not supplied. The compose + per-node configs are the larger authoring work and are now done.

## NEEDS-INFRA-S22: S22 object-store throttling — fault-injecting S3 proxy needed

- **Logged (UTC):** 2026-07-02
- **Severity:** infra gap (not a CA product bug)
- **Observed:** S22 ("object-store throttling and retry budget") requires a fault-injecting proxy between ClickHouse's `ca` disk endpoint and RustFS that can inject bounded transient faults: `503 SlowDown`, `429 Too Many Requests`, artificial latency, and mid-response TCP resets. The current compose wires the `ca` disk endpoint directly at `http://rustfs1:11121/test/soak_pool/` with no interposer.
- **Implementation sketch:**
  1. Add a `toxiproxy` sidecar service to a new `docker-compose-throttle_proxy.yml` (image: `ghcr.io/shopify/toxiproxy`). The proxy listens on a stable port (e.g. `11122`) and forwards to `rustfs1:11121`.
  2. Override the `ca` disk `endpoint` in a new `storage_conf_throttle_proxy_ch{1,2}.xml` to point at `http://toxiproxy:11122/test/soak_pool/` (no other config changes needed).
  3. Register a `"throttleproxy"` variant in `_VARIANT_FILE`.
  4. Implement S22's `run()`: use the toxiproxy HTTP management API (`POST /proxies/rustfs/toxics`) to add/remove `latency`, `slow_close`, and a custom HTTP-status injector; verify `DiskS3*RetryableErrors` counters rise, retry budget is respected (bounded attempt counts), and the replica-agreement oracle + `fsck dangling==0` hold throughout.
  - Note: toxiproxy injects TCP-level faults (latency, slow_close, connection reset) but does NOT inject HTTP-level 503/429 status codes. For HTTP-status injection a thin Python WSGI proxy (e.g. using the stdlib `http.server`) running as a sidecar would be needed, or a custom mitm container. The toxiproxy path covers latency + connection-close (the higher-risk faults); 503/429 need a separate HTTP-level shim.
- **Why deferred:** adding a proxy service is a new Docker image dependency + new compose + new storage config + new scenario code — a self-contained but non-trivial unit of work. The fault injection for latency/reset alone (toxiproxy) would be ~2h of authoring; the HTTP-status injection layer adds another ~1h. S22 is P1 priority and should be picked up as a dedicated session.

## NEEDS-INFRA-S27: S27 LIST pagination ambiguity — instrumented LIST proxy needed

- **Logged (UTC):** 2026-07-02
- **Severity:** infra gap (not a CA product bug)
- **Observed:** S27 ("backend list pagination ambiguity") requires an object-store proxy that deliberately returns duplicate keys across pages, drops the continuation token mid-listing, or returns unstable per-key list-tokens between two listings of the same unchanged shard — exercising the conservative reread path in `listRootShardTokens`. The direct RustFS endpoint serves stable, well-ordered pages and cannot produce these anomalies.
- **Implementation sketch:**
  1. Write a small Python HTTP proxy (`scenarios/proxies/list_fault_proxy.py`, ~150 lines) that sits in front of RustFS and implements the S3 `ListObjectsV2` API. On `roots/`-prefix listings it injects configurable anomalies: (a) duplicate a random key across two consecutive pages; (b) omit the `NextContinuationToken` mid-listing to simulate a truncated page; (c) return a slightly-different `ETag`/list-token for an otherwise-unchanged key. Requests for other prefixes (blobs, GC state) are forwarded transparently.
  2. Run this proxy as a Docker service (e.g. a `python:3.12-slim` container running the script) in a `docker-compose-list_fault_proxy.yml`, with the `ca` disk `endpoint` pointing at the proxy.
  3. Register a `"listfaultproxy"` variant; implement S27's `run()` using a control knob (an environment variable or a POST to the proxy's management endpoint) to enable/disable the anomaly injection per phase, and assert that `CasRootGet` increases (conservative rereads) while `CasBlobDelete` stays safe and `fsck dangling==0`.
- **Why deferred:** the proxy requires a small but new Python HTTP service — new Docker service, new compose, ~150-line proxy shim, new storage config, new scenario code. The proxy's S3 ListObjectsV2 re-implementation must handle authentication (or bypass it), paging tokens, and fault scheduling correctly. This is a self-contained ~3h authoring session; S27 is P2 and is lower priority than S22.

## HARNESS-DRAIN-VERDICT-CONVERGENCE: "forced GC drives unreachable -> 0" reads a mid-run transient, not the converged state

- **Logged (UTC):** 2026-07-01
- **Severity:** harness / test-precision (NOT a product bug — GC reclaim is correct)
- **Observed:** The "forced GC drives unreachable -> 0" verdict (repeated across ~8 cards: `s28_s33_corner.py`, `s15_s18_shards_lifecycle.py`, etc.) reads the residual from a MID-RUN bounded-round `forced_gc_to_fixpoint` snapshot (`drain_residual_unreachable`), not the CONVERGED end-checkpoint state. Under concurrent GC leaders the mid-run residual can be transiently >0 while the pool converges to 0 by the end checkpoint. S04 (2026-07-01, fixed binary `cb3aefb`): verdict read `drain_residual=27` → FAIL, but `gc_residual=0` and `fsck_final.unreachable=0` (converged in 1 round). A false FAIL.
- **Also — make the residual verdicts PREFIX-AWARE.** Raw `fsck.unreachable` / `gc_residual_unreachable` include non-reclaimable "other" bookkeeping (namespace registry / root-shard / gc-state objects). S05 (2026-07-01, fixed binary) settles at `unreachable=240` but `by_prefix={'other':240, blobs:0, _manifests:0}` — the reclaimable classes drained to 0; the 240 "other" is the known S30 `dropNamespace` monotone-registry growth (200 create/drop cycles), NOT a content leak. A raw-count drain assertion would wrongly FAIL S05. Assert on RECLAIMABLE prefixes (`blobs`, `_manifests`) only; track "other" growth separately against S30.
- **Proposed action:** (1) key the drain verdict on the CONVERGED `fsck_final` after the end-checkpoint fixpoint, not the mid-run snapshot; (2) scope it to reclaimable prefixes. Touches ~8 cards + `framework/checkpoint.py` + `framework/assertions.py`.
- **Related:** S30 (dropNamespace monotone registry fanout — the "other" residual); edge-set in-degree fix `55a766e..cb3aefb` (resolved the actual undercount; the benign-error classifier in `framework/observe.py` was broadened for fold/fence/recheck retry variants).
## S07-20260701T230447-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-01T23:07:14
- **Severity:** finding
- **Run:** 20260701T230447_S07_seed20260702
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S13-20260701T231530-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-01T23:24:02
- **Severity:** suspected-bug
- **Run:** 20260701T231530_S13_seed20260702
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S18-20260701T233245-1: S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Except

- **Logged (UTC):** 2026-07-01T23:33:13
- **Severity:** finding
- **Run:** 20260701T233245_S18_seed20260702
- **Observed:** S18 SYSTEM UNFREEZE failed: Node(localhost:8123) HTTP 500: Code: 344. DB::Exception: Support for SYSTEM UNFREEZE query is disabled. You can enable it via 'enable_system_unfreeze' server setting. (SUPPORT_IS_DISABLED) (version 26.6.1.1) | sql=SYSTEM UNFREEZE WITH NAME 's18_snap_20260702'

## S31-20260701T233816-1: scenario raised: cluster did not become healthy after reset

- **Logged (UTC):** 2026-07-01T23:43:26
- **Severity:** suspected-bug
- **Run:** 20260701T233816_S31_seed20260702
- **Observed:** scenario raised: cluster did not become healthy after reset

## S31-20260702T055623-1: scenario raised: cluster did not become healthy after reset

- **Logged (UTC):** 2026-07-02T06:01:31
- **Severity:** suspected-bug
- **Run:** 20260702T055623_S31_seed20260702
- **Observed:** scenario raised: cluster did not become healthy after reset

## S31-20260702T060328-1: ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under

- **Logged (UTC):** 2026-07-02T06:03:53
- **Severity:** suspected-bug
- **Run:** 20260702T060328_S31_seed20260702
- **Observed:** ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 0 but GC reclaimed ~40 (checklist #9). previewDeletes should iterate all target shards, not just shard 0.

## S13-20260702T060416-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-02T06:12:51
- **Severity:** suspected-bug
- **Run:** 20260702T060416_S13_seed20260702
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>


## PRODUCT BUG (found 2026-07-02, S13) — CA mount ownership has no crash self-recovery
Symptom: after `docker kill -s KILL` of a CA server, it exits 236 on restart:
"Content-addressed disk cannot start: server_root_id '<srid>' is actively mounted by another server."
Root cause: the mount owner anchor (`gc/server-roots/<srid>/owner`, CaServerRoot) is a hard CAS claim with
no stale-owner recovery / lease-expiry / self-takeover, so the SAME server restarting after a crash sees its
own prior incarnation's claim and refuses to mount. Any unclean shutdown (crash/OOM/kill-9) permanently
wedges that server_root_id until the anchor is manually cleared.
Why deferred: DESIGN-SENSITIVE (must be split-brain safe — distinguish a crashed prior incarnation from a
live peer). Touches the TLA+-proven mount protocol (CaCasMountCore.tla) → needs a design + model extension
(e.g. mount lease with fenced-epoch takeover), not an unattended fix. NOT a D1 regression.
Impact: real deployments cannot auto-restart a crashed CA server. High priority for the mount protocol.

## SCENARIO (proposed): SIGSTOP a writer holds the ack floor, SIGCONT releases it (ack-floor round)

- **Severity:** scenario-proposal (ack-floor liveness/safety)
- **Observed:** N/A (proposed regression guard for the one-pass ack-floor GC round, `cas-gc-ack-floor-fence`).
- **Proposed action:** Steps: fresh pool, one writer + one GC leader; publish then drop a ref so a blob is
  condemned. `SIGSTOP` the writer (its heartbeat stops renewing, but its lease has NOT yet expired, so the
  floor still counts its `observed_gc_round`). Drive GC rounds: assert the condemned entry NEVER graduates
  (its `observed_gc_round` is stuck below the condemn round → `min_ack` pins the floor), the blob survives,
  and `CasGcFloorHeldByStaleAck` fires once the lag exceeds 2. `SIGCONT` the writer, let it beat once so its
  `observed_gc_round` catches up, then assert graduation resumes and the blob drains to absent. Verifies the
  floor is genuinely held by a live-but-paused writer's ack and released cleanly — the SIGSTOP-not-KILL path
  that fence-out does NOT reclaim.

## SCENARIO (proposed): hard-KILL a writer mid-burst → fence-out after TTL → no dangle in fsck

- **Severity:** scenario-proposal (ack-floor fence-out safety)
- **Observed:** N/A (proposed regression guard for the heartbeat fence-out path).
- **Proposed action:** Steps: fresh pool, two writers + one GC leader; both writers insert/burst concurrently.
  `docker kill -s KILL` one writer mid-burst (its lease stops renewing; it holds a stale `observed_gc_round`).
  Wait past `mount_lease_ttl_ms + skew_margin` and drive GC: assert the round FENCES OUT the dead writer
  (`RoundReport::fence_outs == 1`, a `gc_fence_out` audit row, its mount body `gc_fenced = true`), the floor
  advances past its stale ack, and condemned blobs drain. Then run `clickhouse-disks fsck`: assert
  `dangling == 0` throughout — the fence-out must never let GC delete a blob a still-live writer references
  (the surviving writer's fresh incarnations are spared). Guards the safety half of fence-out.

## SCENARIO (proposed): round request-budget regression guard — O(delta)+O(servers)

- **Severity:** scenario-proposal (ack-floor cost regression)
- **Observed:** N/A (proposed regression guard; the ack-floor redesign's headline property is per-round
  request count no longer scaling with the object universe).
- **Proposed action:** Instrument a soak run's per-round S3 request count (the `CasGc*` ProfileEvents or the
  instrumented backend op log). Drive a pool to a large object universe, then a quiescent series of rounds
  with a small per-round owner-event delta and a handful of servers. Assert the per-round request count stays
  O(delta) + O(servers) — specifically that it does NOT grow with total object count (the fence+recheck
  design's ~2×O(universe) GET + O(universe) CAS-PUT is gone). Fail the guard if a round's request count
  correlates with universe size rather than delta size. Pairs with the ROADMAP "Ack-floor round soak
  validation" item.
## S07-20260703T012854-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-03T01:31:00
- **Severity:** finding
- **Run:** 20260703T012854_S07_seed20260703
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S12-20260703T013923-1: NOT RUN — docker-compose-10replicas.yml (ch1..ch10) exists; remaining gap: soak/

- **Logged (UTC):** 2026-07-03T01:39:23
- **Severity:** finding
- **Run:** 20260703T013923_S12_seed20260703
- **Observed:** NOT RUN — docker-compose-10replicas.yml (ch1..ch10) exists; remaining gap: soak/cluster.py Cluster is hardcoded to 2 nodes — needs a multi-node abstraction to address ch3..ch10 (see BACKLOG NEEDS-INFRA-S12)

## S22-20260703T014845-1: NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) be

- **Logged (UTC):** 2026-07-03T01:48:46
- **Severity:** finding
- **Run:** 20260703T014845_S22_seed20260703
- **Observed:** NOT RUN — requires a fault-injecting S3 proxy (503/429/slow/connection-close) between ClickHouse and RustFS; not in the current compose (direct rustfs1 endpoint)

## S27-20260703T015057-1: NOT RUN — requires an instrumented object store / proxy that returns duplicate o

- **Logged (UTC):** 2026-07-03T01:50:58
- **Severity:** finding
- **Run:** 20260703T015057_S27_seed20260703
- **Observed:** NOT RUN — requires an instrumented object store / proxy that returns duplicate or unstable LIST pages for root-shard token listing; not available with the direct rustfs endpoint

## S01-20260705T174845-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 241. DB::Exception: (total

- **Logged (UTC):** 2026-07-05T17:49:01
- **Severity:** suspected-bug
- **Run:** 20260705T174845_S01_seed20260703
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 241. DB::Exception: (total) memory limit exceeded: would use 128.27 GiB (attempt to allocate chunk of 128.00 GiB), current RSS: 245.26 MiB, maximum: 25.20 GiB. OvercommitTracker decision: Query was selected to stop by OvercommitTracker: while executing 'FUNCTION randomString(8388608_UInt32 :: 2) -> randomString(8388608_UInt32) String : 0'. (MEMORY_LIMIT_EXCEEDED) (version 26.6.1.1) | sql=INSERT INTO s01_huge SELECT 0 + number AS id, randomString(8388608) AS payload FROM numbers(12800)


---

## RESOURCE — CAS write path spills the WHOLE part to local scratch (hash-before-upload)
- **Logged (UTC):** 2026-07-05 (campaign full-scale S01)
- **Severity:** resource-bug (scalability + local-disk amplification)
- **Observed:** building/uploading a large part spills the ENTIRE object to `disks/ca/cas_scratch`
  before the S3 upload — 24 GiB part -> ~24 GiB scratch; 100 GiB part -> 93 GiB scratch (measured).
  The S3 upload itself streams (RSS stayed 115 MiB for a 100 GiB merge — memory is fine), but local
  free disk must be >= part size or the write cannot complete. A part larger than local scratch is
  unwritable even though nothing is held in RAM.
- **Root cause hypothesis:** `HashingWriteBuffer` in the CA write path computes the content hash by
  writing the full object to a local temp first, then uploads. The hash should be computed IN-STREAM
  (hash the bytes as they are uploaded to S3) so no full local spill is needed — the streaming
  HashingWriteBuffer already sees every byte on the way out.
- **Impact:** caps max part/blob size at local-disk free space; on the campaign host a 100 GiB
  single-blob merge exhausted the disk during OPTIMIZE FINAL (see worklog 2026-07-03-scenarios).
- **Fix direction:** in-stream hash-while-upload (no full scratch spill), or a bounded chunked
  hash that never materializes the whole object locally. Verify against the streaming-hash
  convention (chunked CityHash128 over 2048-B blocks — chunking already exists, the spill is the issue).

## RESOURCE — replicated OPTIMIZE re-merges + re-spills on every replica (shared pool)
- **Logged (UTC):** 2026-07-05 (campaign full-scale S01)
- **Severity:** resource-bug (duplicated work + local-disk amplification on shared pool)
- **Observed:** ch2 (a replica on the SHARED CAS pool) independently ran the same OPTIMIZE FINAL
  merge and spilled its OWN ~93 GiB scratch; dedup then keeps ONE pool blob. 186 GiB of local
  scratch across two replicas to produce one deduped 100 GiB blob; both replicas burn CPU/IO
  re-merging identical content.
- **Fix direction:** a shared-pool replica should ADOPT the leader's already-uploaded merged blob
  (the content hash is identical by construction) instead of re-merging + re-hashing + re-spilling
  locally. Ties into the general "replicas share pool blobs, don't re-upload" design — the merge
  path apparently does not take that shortcut.

## S3-BUDGET — idle GC has a high fixed per-round cost on a large static pool
- **Logged (UTC):** 2026-07-05 (campaign S03 full 20M rows/400 parts)
- **Severity:** s3-budget / efficiency
- **Observed:** 161 idle-GC rounds over ~15 min on a STATIC pool: ~1362 CasGcGet + ~643 CasBlobHead
  + ~457 CasRootGet PER ROUND (~2500 S3 ops/round) with nothing changing. GC memory is bounded
  (1.57 GB) but S3 op volume is not idle-cheap — each round re-reads the generation runs and HEADs
  candidate blobs regardless of change.
- **Components:** (a) the fold re-reads prior-generation runs every round even when the journal has
  no new transitions; (b) B148 HEAD-storm-at-retire (per-candidate blob HEAD instead of stored token,
  ~643/round). (b) is already a ROADMAP item; (a) is the bigger idle cost.
- **Fix direction:** short-circuit a GC round when the ack-floor + journal show zero new transitions
  since the last sealed generation (skip the re-fold entirely — "nothing to do" round is ~O(1) reads,
  not O(generation)); land B148 stored-token retire to kill the per-round blob HEADs.
- **Note:** prod `gc_interval_sec=60` reduces round COUNT 6x vs the soak's 10 s, but per-round cost
  is unchanged — a large idle pool still burns steady S3 ops.

## S3-BUDGET/SCALABILITY — GC round duration is O(ref universe): ~93 s at 10000 tables
  - **UPDATE (S08, 100000 tiny parts):** the same O(pool-object-count) scaling reached **398 s
    (6.6 min) for a SINGLE GC fold round** at ~100k parts (one round deleted 24392 manifests). Data
    points: 87 ms @ 400 parts (S03) -> 93 s @ 10k tables (S05) -> 398 s @ 100k parts (S08). The
    end-checkpoint settle_fsck cannot stabilize because each multi-minute round bulk-mutates the pool.
    Correctness holds (manifests drain, no errors), but a 6.6-min GC round is a hard scalability wall.

- **Logged (UTC):** 2026-07-05 (campaign S05 "10000 sparse tables" full)
- **Severity:** scalability / s3-budget (latency)
- **Observed:** GC fold rounds took **92.6 s and 93.9 s** each on a 10000-table pool (each table is a
  namespace; discoverUniverse LISTs cas/refs/ across ~10000 namespaces × shards and the fold reads
  the generation). Compare S03 (400 parts, one namespace): p95 87 ms. So round time scales with the
  ref-universe size, reaching ~1.5 min/round at 10k tables.
- **Consequences:** (1) with `gc_interval_sec=10` a 93 s round cannot keep cadence -> continuous
  back-to-back GC; (2) `settle_fsck` cannot stabilize (background GC mutates the pool faster than fsck
  can snapshot it — history oscillated 22415->22103->21212 reachable, dangling stayed 0 so correctness
  is intact); (3) the forced-GC-to-fixpoint end-phase is very slow.
- **Fix direction:** (a) skip-unchanged-namespace in discoverUniverse/fold (a namespace with no new
  journal transitions since the last sealed generation should cost ~O(1), not a full re-read) —
  the token-diff discovery should prune untouched namespaces; (b) incremental/partitioned fold so a
  round is bounded regardless of total universe; (c) raise the default gc_interval for very large
  universes so rounds don't overlap. Ties to the S03 "idle GC high per-round cost" item — same root:
  the fold re-reads the whole universe every round.
- **Correctness:** unaffected (dangling=0 throughout); this is cost/latency, not a data bug.
## S06-20260705T215757-1: S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED

- **Logged (UTC):** 2026-07-05T21:58:42
- **Severity:** suspected-bug
- **Run:** 20260705T215757_S06_seed20260703
- **Observed:** S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED

## S06-20260705T220753-1: S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED

- **Logged (UTC):** 2026-07-05T22:08:16
- **Severity:** suspected-bug
- **Run:** 20260705T220753_S06_seed20260703
- **Observed:** S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED

## S07-20260705T224846-1: scenario raised: Node(localhost:8123) HTTP 400: Code: 62. DB::Exception: Max que

- **Logged (UTC):** 2026-07-05T22:49:04
- **Severity:** suspected-bug
- **Run:** 20260705T224846_S07_seed20260703
- **Observed:** scenario raised: Node(localhost:8123) HTTP 400: Code: 62. DB::Exception: Max query size exceeded (can be increased with the `max_query_size` setting): Syntax error: failed at position 262144 (UI): UI. . (SYNTAX_ERROR) (version 26.6.1.1) | sql=CREATE TABLE s07_capwide (k UInt64, c0 UInt32, c1 UInt32, c2 UInt32, c3 UInt32, c4 UInt32, c5 UInt32, c6 UInt32, c7 UInt32, c8 UInt32, c9 UInt32, c10 UInt32, c11 UInt32, c12 UInt32, c13 UInt32, c14 UI...(288932 more chars)


## S3-BUDGET/RESOURCE — wide part = O(columns) S3 ops; 20000-column merge exhausts ephemeral ports
- **Logged (UTC):** 2026-07-06 (campaign S07 full, 20000 columns)
- **Severity:** s3-budget / resource (connection churn)
- **Observed:** OPTIMIZE FINAL on a 20000-column wide part stalled at progress=0 for 4+ min. Server
  log: `Poco::Net Cannot assign requested address: 172.19.0.2:11121` (errno 99) — the CH container
  EXHAUSTED its ephemeral TCP port range connecting to RustFS. The S3 client retried (attempt 7/501)
  with backoff, so the merge crawled (S3PutObject +5 ops / 5 s). Each column file is a separate CAS
  object → a HEAD/GET/PUT per column → ~20000 object-store ops in a burst for ONE part merge.
- **Root cause:** wide-part operations issue O(columns) CAS ops; a burst of tens of thousands of
  connections outpaces keep-alive reuse and exhausts local ports (TIME_WAIT accumulation).
- **Fix direction:** (a) stronger connection pooling / keep-alive reuse in the CAS S3 path so a
  wide-part op reuses a small connection set instead of churning per column; (b) batch per-column
  HEAD/GET/PUT (the manifest already groups columns — read/write column blobs in batched requests);
  (c) consider inlining small column files into the manifest (already happens for tiny ones — raise
  the threshold so 20000 tiny columns don't each become a separate object). Also a deployment note:
  raise net.ipv4.ip_local_port_range / tune S3 max connections for very wide tables.
- **Correctness:** unaffected (the 20000-column part COMMITTED on insert; only the subsequent merge
  stalls on port exhaustion). This is cost/latency/resource, not a data bug.
## S11-20260706T025607-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 252. DB::Exception: Too ma

- **Logged (UTC):** 2026-07-06T02:56:25
- **Severity:** suspected-bug
- **Run:** 20260706T025607_S11_seed20260703
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 252. DB::Exception: Too many partitions for single INSERT block (more than 100). The limit is controlled by 'max_partitions_per_insert_block' setting. Large number of partitions is a common misconception. It will lead to severe negative performance impact, including slow server startup, slow INSERT queries and slow SELECT queries. Recommended total number of partitions for a table is under 1000..10000. Please note, that partitioning is not intended to speed up SELECT queries (ORDER BY key is sufficient to make range queries fast). Partitions are intended for data manipulation (DROP PARTITION, etc). (TOO_MANY_PARTS) (version 26.6.1.1) | sql=INSERT INTO s11_buckets SELECT 0 + number AS id, randomString(2048) AS payload, (number % 256) AS bucket FROM numbers(10000)


## S3-BUDGET — partitioned-table INSERT is O(partitions) CAS commits (256-partition insert ~10 s)
- **Logged (UTC):** 2026-07-06 (campaign S11 full, PARTITION BY bucket, 256 buckets)
- **Severity:** s3-budget / latency
- **Observed:** each INSERT into a 256-partition table took ~10 s (only 4-5 inserts/min). A single
  INSERT splits into one part-piece per partition, and each part-piece is a separate CAS commit
  (manifest + ref + blob round-trip) -> ~256 S3 commit sequences per INSERT. On a non-CAS MergeTree
  the per-partition parts are cheap local writes; CAS turns them into S3 round-trips, so
  partition-heavy ingestion is latency-bound on S3 op count.
- **Fix direction:** batch the per-partition part commits of a single INSERT into one shard-queue
  flush / one manifest CAS where the parts share a namespace shard (the flat-combining queue already
  coalesces same-shard mutations — verify it covers multi-partition single-INSERT part-pieces).
- **Correctness:** unaffected — same O(N)-on-S3 family as the GC-round / wide-part findings.
## S13-20260706T044329-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-06T05:15:46
- **Severity:** suspected-bug
- **Run:** 20260706T044329_S13_seed20260703
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>


## PRODUCT BUG (availability, HIGH) — mount-lease self-adoption fails closed under rapid crash-restart
- **Logged (UTC):** 2026-07-06 (campaign S13 full, process-loss chaos, round 22)
- **Severity:** CORRECTNESS/AVAILABILITY (the first non-budget bug of the campaign)
- **Symptom:** S13 kills ch1 repeatedly (~every 40 s, 6 s down). At round 22 ch1 failed to restart and
  stayed down (Exited 49); all downstream verdicts (health, replica agreement, fsck) went unavailable.
- **Error chain (ch1 err log):**
  1. While RUNNING (pre-kill): `CasMountLeaseKeeper: background renewal failed ... key
     'soak_pool/gc/server-roots/ca_soak_ch1/mount' was touched by a FOREIGN WRITER — failing closed,
     never re-minting. (LOGICAL_ERROR)`.
  2. On RESTART: `CAS mount-lease: key '...ca_soak_ch1/mount' was touched while adopting our own mount
     slot — failing closed. (LOGICAL_ERROR)` -> exit 49, node never comes up.
- **Why it's a real bug (not a test artifact):** the mount slot is `.../server-roots/ca_soak_ch1/mount`
  — ONLY ch1 should ever write its own slot. Something ELSE ("foreign writer") touched it while ch1
  was alive AND during ch1's self-adopt on restart. Prime suspect: the GC leader (possibly ch2, or a
  ch1 incarnation's delayed lease-renewal landing after the kill) writes/CASes another server's mount
  slot during the heartbeat/liveness scan. The self-remount path (meant to re-adopt "my own stale
  mount after a crash", landed 2026-07-02) cannot distinguish a genuine foreign owner from a
  concurrent touch of its own slot under rapid restart, so it fails closed PERMANENTLY — the node
  needs manual `mount`-object deletion to recover. Fail-closed is right for a true foreign owner, but
  wedging a node forever on self-restart is an availability defect.
- **Root-cause questions:** (1) who is the foreign writer of `ca_soak_ch1/mount`? (GC scan writing
  peer mount slots? a delayed pre-kill renewal?) — instrument the mount-slot writers. (2) the
  self-adopt CAS should treat "current value is MY OWN uuid's stale lease" as adoptable even if the
  ETag/token changed between read and CAS (re-read and compare uuid, not just token). (3) does a
  killed incarnation's async lease write land after restart and race the adopt?
- **Repro:** S13 at full scale reliably (round 22). Also relevant to production: a pod that
  crash-loops or restarts within the lease TTL could wedge itself out of its own pool.
- **NOTE vs prior:** the ledger's "S13 mount self-recovery — RESOLVED (self-remount 2026-07-02)"
  handled the simple stale-self case; this is a RACE gap in that path under rapid repeated kills.

## INTROSPECTION FOLLOW-UP — first-open mount claim is invisible to the audit-event trail
- **Logged (UTC):** 2026-07-06 (fix-plan Phase 2 Task 6 live validation)
- **Severity:** follow-up (compensated, not blocking)
- **Observed:** the `mount_claim`/`mount_conflict` audit events (Phase 2) cannot capture the claim
  made DURING `Store::open` — the `CasEventSink` is installed after `open` returns. A kill-restart
  cycle showed only `mount_beat` rows while the plain server log proved the reclaim fired
  ("a stale mount lease is held by uuid=...; waiting for it to lapse, then reclaiming").
- **Compensation in place:** the three keeper refusal exception messages now carry the observed
  holder identity (uuid/hostname/pid/epoch/seq/expires), so err.log names the toucher at first-open.
- **Fix direction:** synthesize one `mount_claim` event describing the open-time claim as soon as
  the sink is installed (the lease body is at hand), or install the sink before `Store::open`'s
  mount step. Small; touches `ContentAddressedMetadataStorage` startup wiring only.
