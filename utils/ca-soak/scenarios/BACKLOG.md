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

## GC-CONCURRENT-LEADER-LEAK: Explicit SYSTEM CONTENT ADDRESSED GC RUN on >1 replica concurrently permanently orphans blobs (reclaim leak)

- **Logged (UTC):** 2026-06-27T21:01:18
- **Severity:** suspected-bug / HIGH (storage leak; safety preserved)
- **Run:** diagnostic (manual repro on cas-gc-part-manifest-impl @ ae0cc27b1bf5, CH 26.6.1.1)
- **Observed:** REPRO: fresh pool; create 3-4 ReplicatedMergeTree tables on storage_policy='ca' (wide parts), insert, DROP TABLE ... SYNC all; then issue explicit `SYSTEM CONTENT ADDRESSED GC RUN ca` on BOTH replicas concurrently (or alongside the 2s background scheduler). Result: a stable residual of UNREACHABLE content blobs + part-manifests is left FOREVER (e.g. 5 blobs/~413KB incl a 411KB payload .bin + 2 _manifests tagged reclaimable-pre-precommit). fsck dangling=0 throughout (NO data loss); ca-gc-dryrun lists the orphans as deletable (zeroInDegree); but GC rounds report candidates_marked=0 after the first generation completes, and never delete them. GC log shows many Finish rows with outcome=Error ('CAS gc fold: fold seal ... occupied by divergent bytes (concurrent leader); retry' / ABORTED 236) and NotALeader; gc_fold_begin=40 vs gc_fold_end ok=11 (most folds aborted). CONTRAST: the SAME drop drained to unreachable=0 within ~5s when reclaimed by BACKGROUND GC alone, and single-node SERIAL explicit GC drained 16->0 in one round. So the leak is triggered specifically by CONCURRENT GC LEADERS — explicit `SYSTEM ... GC` (runGarbageCollectionRoundNow) appears NOT to be lease-gated the way the background CasGcScheduler is, so it can run concurrently with the other replica's lease holder. The fold seal correctly aborts the divergent fold (safety: no over-delete, dangling=0), but the colliding/aborted round advances GC generation/cursor state past owner-removal events that were never folded, so those blobs' in-degree never reaches zero in the persistent snapshot and they are never retired (a fresh full re-fold via dryrun still sees them as zero-in-degree). Liveness/reclaim bug, not safety.
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

- **RESOLVED 2026-06-29..07-01 (verified against code 2026-07-06).** BOTH mechanisms of this entry
  are gone — the fixes landed as a side-effect of the Phase-1 relocation + D1 registry removal and
  this entry was never retired (it misled a 2026-07-06 re-read):
  1. Discovery no longer walks `roots/`. `Gc::discoverUniverse` (`CasGc.cpp:1236`) and
     `Gc::listRootShardTokens` (`CasGc.cpp:1281`) LIST `layout.casRefsPrefix()` = `cas/refs/`, a FLAT
     one-object-per-`(ns,shard)` prefix (`cas/refs/<ns>/<shard>`); there is NO `rootsPrefix()` in any
     GC discovery path. Landed `f5f96dce01a` (2026-06-29, relocate ref shards → `cas/refs/`) +
     `644eb7c6ade` (2026-07-01, D1: `discoverUniverse` = `LIST(cas/refs/)`, `gc/registry` deleted).
  2. The per-page re-enumeration is gone. `ObjectStorageBackend::list` (`CasObjectStorageBackend.cpp:715`)
     uses a lazy `object_storage->iterate(prefix, max_keys=0, start_after=cursor)` on the native/S3
     path — S3 honors `start_after`, each page = ONE LIST request, linear walk. The old
     `listObjects(max_keys=0)` materialize-and-slice survives ONLY in the EmulatedSingleProcess (test)
     branch. Landed `b15f1ef9d28` (2026-06-29, "paginate backend list with object iterator").
  Release gate #16 is closed. **What this entry did NOT cover, and is still open:** the round is still
  O(universe) per round because discovery does TWO full `cas/refs/` LISTs every round + re-reads the
  generation, regardless of delta — see `S3-BUDGET — idle GC …` and `S3-BUDGET/SCALABILITY — GC round
  duration is O(ref universe)` below. That is the Phase-4 "fold/discover skip-unchanged" item, distinct
  from this now-fixed discovery-PLACEMENT quadratic. (Note: the per-shard fold READ already skips
  unchanged shards via token-diff — `computeDiscoverDecisions`/`fold`, `CasGc.cpp:637-660,1379`.)
- **Logged (UTC):** 2026-06-28T06:06:30
- **Severity:** suspected-bug / perf-scalability (correctness-safe)
- **Observed:** S3 ListObjectsV2 is recursive/flat by default (no delimiter). Gc::listRootShardTokens (CasGc.cpp:1168) lists layout.rootsPrefix() with backend.list(prefix,cursor,1000) and filters non-shard keys in-process. But ObjectStorageBackend::list (CasObjectStorageBackend.cpp:564) calls object_storage->listObjects(prefix, children, max_keys=0) = enumerate ALL keys under roots/ into memory + std::sort, then returns a 1000-key window after cursor. So the paging loop RE-ENUMERATES + RE-SORTS the entire roots/ prefix on EVERY page: ~N/1000 page-calls x a full N-key S3 enumeration each = ~O(N^2/1000) S3 LIST round-trips + O(N) mem per page, N = all objects under roots/ (dominated by the manifest backlog, 38k+ in the soak; only ~128 are actual shard objects). Recursion pulls _manifests/_files/watermarks/shadow too. This is the mechanism behind the B146/B154 fsck/GC timeouts at large pool: discovery cost explodes super-linearly with the manifest count -> gc_checkpoint fsck timed out (>180s) at ~150GB -> no reclaim -> pool grew. Correctness-safe (registry is the universe authority; LIST is only a token-diff accelerator).
- **Proposed action:** Two independent fixes: (1) ObjectStorageBackend::list must NOT re-enumerate the whole prefix per page — paginate at the source (carry a real continuation token) or enumerate once per sweep and window in memory. (2) Do not recurse over all of roots/ to find shards: either list each namespace shard container roots/<ns>/store/<uuid>@cas@/ with Delimiter="/" (returns the numeric shard leaf objects as Contents and _manifests/_files as CommonPrefixes -> O(shards), no manifest enumeration), or drive the accelerator from the registry-known (ns,shard) set with batched HEADs/scoped lists. Either keeps discovery O(shards) instead of O(all roots objects). README surprise-checklist #5, now with the max_keys=0 re-list-per-page wrinkle.

## IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST: IDEA: move shard objects to one common flat prefix + stable cached @cas@ reference -> GC discovery = a single LIST

- **REALIZED / superseded 2026-06-29 (verified 2026-07-06).** The core of this idea already shipped:
  ref (root-shard) objects live in ONE flat prefix `cas/refs/<ns>/<shard>` since the Phase-1
  relocation (`f5f96dce01a`), so GC discovery IS a single paged `LIST(cas/refs/)` returning
  `(ns,shard)->token`, O(shards), no manifest/_files noise. `_manifests/`/`_files` stay per-table and
  are never enumerated by GC. Nothing further to relocate. The remaining per-round O(universe) cost is
  the Phase-4 skip-unchanged item, not a placement problem.
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
- **TRIAGED 2026-07-27: ENVIRONMENT, not a bug.** Both Error rows are
  `Code: 499 ... Timeout ... (S3_ERROR)` on objects of **268 bytes** and **4,606 bytes**. A 268-byte GET
  timing out is a store that is not answering, not a GC defect. The host was still recovering from an
  aborted `--scale full` attempt (load average 22-48 at launch), which also produced 23,561
  `UNCERTAIN (retry budget exhausted)` client failures in the same run. The seed-43 repeat on a quiet host
  had ZERO environmental failures. Downgrading from `suspected-bug`; see BACKLOG
  `{#s42-ci-verdict}`. The genuine finding from seed 43 is the stale-edge defect, root-caused in
  `docs/superpowers/reports/2026-07-26-s42-stale-edge-repro/PHASE1.md`.

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
  advances past its stale ack, and condemned blobs drain. Then run `clickhouse-disks ca-fsck`: assert
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
- **RESOLVED (idle / small-delta case) 2026-07-06, `436714d80f0`..`3cba4f812f8` (Phase 4 Lever A,
  "GC round skip-unchanged").** A round that makes no destructive decision now DEFERs: it re-adopts
  the sealed in-degree generation instead of rebuilding it from a full snapshot read, so an idle round
  no longer re-reads the prior-generation runs. Mechanism: `shouldDeferRound` (config
  `gc_fold_threshold`, default 1) decides DEFER vs FOLD from cheap pre-fold signals
  (`changedShardCount`, `graduationDue`); `graduationDue` force-folds before a bounded deferral window
  elapses (`gc_fold_max_defer_rounds`, default 8), so no candidate is starved of eventual fold. Safety
  gated by the TLA+ model `CaGcRoundDeferCore` (`NoOverDelete` + `EventuallyFolded`). Verified by a
  soak-harness ops-budget assertion (`S03`, `utils/ca-soak/scenarios/cards/s03_s05_scale.py`): an
  isolated idle round's `CasGcGet` delta must be `< 50` (was ~1362) with `fsck dangling == 0`. **Not
  resolved:** the large-delta case remains O(universe) per round — see Lever B below (incremental
  point-updatable in-degree), still open.
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
- **RESOLVED (idle / small-delta case) 2026-07-06, `436714d80f0`..`3cba4f812f8` (Phase 4 Lever A,
  "GC round skip-unchanged").** Same mechanism/fix as the `S3-BUDGET — idle GC …` entry above: a
  round with no destructive decision DEFERs (re-adopts the sealed generation) instead of re-reading
  the whole ref universe, with `graduationDue` force-folding within a bounded window so nothing is
  starved. **Not resolved:** a round with a large delta (many tables actually changing in the same
  round — the S05/S08 scaling this entry measured) is still O(universe) per fold, because a single
  FOLD still rebuilds the whole in-degree generation. That is Lever B (incremental point-updatable
  in-degree, so even a non-idle round is O(delta) not O(universe)) — still open, tracked in
  `docs/superpowers/cas/ROADMAP.md` under "GC round is O(universe) not O(delta)".
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
- **LIVE-VALIDATED 2026-07-06 (night):** manual fence-recovery cycle (crash-kill ch1 → GC-fenced → restart recovers as `writer_epoch=2` in 2s, no "foreign writer"/exit-49) AND S13 full-scale chaos (40 rapid crash-restarts of ch1/ch2 → ch1 reached `writer_epoch=26`, `state=live`, **zero exit-49 wedge, zero "foreign writer"** — the exact rapid-crash-restart trigger of this bug). P1 is fixed. (The S13-full run then wedged in the end-checkpoint quiesce on an INFRA limit — rustfs 503 near disk-full stalling a merge finalize — NOT this bug; see worklog 2026-07-06-scenario-validation-night finding F2.)
- **RESOLVED 2026-07-06 (code complete + reviewed; live soak validation = the one remaining step).**
  Root cause (P3.1): the "foreign writer" of `ca_soak_ch1/mount` is the GC leader's **legitimate**
  fence-out of a lease that EXPIRED while ch1 was alive — the renewal thread also ran the S3-heavy
  retired-view refresh (`refreshViewForBeat`, exclusive `view_gate` + per-shard GETs) synchronously
  before the lease PUT, so under RustFS retry storms (~19% read errors, backoff to minutes) the
  renewal was pushed past the 30 s TTL. The PERMANENT wedge (exit 49 on restart) is that fence landing
  inside the keeper's non-atomic adopt GET→CAS window during `Store::open`, which had no retry.
  Two-part fix on `cas-gc-rebuild`, TLA+-gated (`FenceCostsEpoch` + `NoPermanentWedge`;
  `CaCasMountCore`):
  - **Fence recovery** ("a fence costs an epoch"): `FencedSelf` claim outcome + typed
    `MountFencedException`, renewal mismatch classified by BODY (a GC fence of our own expired lease is
    not a "foreign writer"), and a bounded fence-recovery loop in `Store::open` (re-alloc a fresh
    `writer_epoch` and re-claim). Commits `d76d4e75e8e`..`4000161a2ab` (+ `cdf02bfd67b` ErrorCodes→typed).
  - **Lease/view-sync decouple** (removes the CAUSE): the renewal reads the installed round in-memory
    (`Store::observedGcRound`) and no longer runs the refresh; a dedicated syncer thread
    (`syncRetiredView`, formerly `refreshViewForBeat`) advances the view off the renewal path, so a
    slow object store can no longer block a renewal past its TTL. Commits `afb89a730bb`..`245b8ffd30e`.
    (The `mount_beat` audit event referenced elsewhere in this backlog is renamed `retired_view_advance`.)
  Unit suite `Cas*` green throughout. **Still TODO (Task 6):** live soak validation of the
  fence-recovery cycle under induced S3 latency — renewal cadence stays ≤ period while the syncer
  lags, the lease never expires on a live node, no spurious `gc_fence_out`, and a genuine fence-out
  recovers in place as a fresh incarnation (higher `writer_epoch`, no "foreign writer" wedge). See
  `docs/superpowers/specs/2026-07-06-cas-mount-lease-fence-recovery-design.md` +
  `docs/superpowers/specs/2026-07-06-cas-lease-view-sync-decouple-design.md`.
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
  cycle showed only `retired_view_advance` (formerly `mount_beat`) rows while the plain server log proved the reclaim fired
  ("a stale mount lease is held by uuid=...; waiting for it to lapse, then reclaiming").
- **Compensation in place:** the three keeper refusal exception messages now carry the observed
  holder identity (uuid/hostname/pid/epoch/seq/expires), so err.log names the toucher at first-open.
- **Fix direction:** synthesize one `mount_claim` event describing the open-time claim as soon as
  the sink is installed (the lease body is at hand), or install the sink before `Store::open`'s
  mount step. Small; touches `ContentAddressedMetadataStorage` startup wiring only.

## ADAPTIVE-GC-CADENCE: GC frequency tuning — journal-pressure-triggered fold, not fixed interval
- **Logged (UTC):** 2026-07-06
- **Severity:** design / s3-budget (efficiency; follow-up to Phase 4 Lever A skip-unchanged)
- **Insight:** the dominant cost of running GC RARELY is NOT S3 storage of dead (condemned-but-not-
  yet-reclaimed) data — that is nearly free short-term (~$0.02/GB/mo; minutes of garbage negligible).
  It is **journal growth**. Every writer mutation (publish/drop/precommit/promote) RMW-rewrites the
  WHOLE root-shard body (live-refs + journal tail); flat-combining batches only concurrent mutations.
  The journal tail is trimmed (B12) only up to the FOLD cursor, which GC advances — so **no fold ⇒ no
  trim ⇒ the body grows and every mutation's CAS gets more expensive**. Without trim a shard's write
  cost is O(mutations-since-fold) per mutation ⇒ **quadratic** in mutations over the interval. That is
  the real feedback: rare GC → fat journals → slow writers.
- **Tradeoff shape:** total S3 ops/sec ≈ A/interval + B·interval, where A = O(universe) snapshot
  read+write per fold (fewer folds when interval grows) and B = writer amplification (journal tail
  ∝ rate × time-since-fold, grows with interval). ⇒ **sqrt-optimal interval ∝ √U / r** (U = blob
  universe, r = hottest-shard mutation rate): bigger pool → longer optimal interval; hotter writes →
  shorter. HARD CEILING regardless of the optimum: the hottest shard's body must stay under the
  object-store inline threshold (~128 KiB RustFS; 8 MiB `gc_trim_body_soft_limit` backstop) or RMW
  goes pathological (rustfs#3231). Night data: 32 shards, hot table ~25 KB healthy, ~165 KB @ 10 min
  under a storm (already over 128 KiB) ⇒ ceiling on a hot pool is single-digit minutes, not tens.
- **KEY design point (for Phase 4 and beyond):** the fold trigger should key on **per-shard journal
  pressure (event count / body size / age)**, NOT on changed-shard count. One hot shard mutated 10k
  times = "1 changed shard" — a shard-count threshold would defer and let its journal explode. Journal
  pressure is the correct signal for when a fold (and thus a trim) is actually necessary.
- **Relation to Phase 4 Lever A (skip-unchanged, in progress):** with the default `gc_fold_threshold
  = 1` (fold on any change), Lever A is already **journal-safe** — an active shard is folded/trimmed
  every round it is touched, journals stay tiny; idle rounds DEFER (cheap). What Lever A does NOT do
  is reduce O(universe) folds on an ACTIVE pool. Reducing active-pool fold frequency (tolerating
  fatter journals to save folds) is THIS item — bounded by the hot-shard ceiling, and best driven by
  a journal-pressure trigger.
- **Prod direction:** the aggressive every-few-seconds GC is a TEST instrument; for prod use a modest
  `gc_interval_sec` (~30–60 s — idle DEFER makes idle pools ~free) plus a journal-pressure fold
  trigger (self-tuning: cold shards never force a fold, a hot shard forces one before its body crosses
  the threshold). Constants (A, B, U, r) are pool-specific.
- **Proposed action:** its own brainstorm + spec AFTER Phase 4 Lever A lands. Needs a cheap per-shard
  journal-size signal (not from LIST — LIST gives the token, not the body size; the writer knows it
  post-write, or GC reads hot shards' bodies which it does at fold anyway) and a **measurement soak**:
  sweep `gc_interval_sec` (and/or a journal-size trigger), plot hot-shard CAS body size + writer
  amplification vs GC ops/sec, find the knee. Do NOT block Lever A (which is journal-safe at default).
## S18-20260706T231321-1: GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['so

- **Logged (UTC):** 2026-07-06T23:13:50
- **Severity:** suspected-bug
- **Run:** 20260706T231321_S18_seed20260707
- **Observed:** GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/01/01072cc50e01979bd48c985b2719ee8c', 'soak_pool/blobs/01/01c16d4da5bf1ada12a2024ca8591c4c', 'soak_pool/blobs/06/06d01b256bb15321515b1c38254ff56e', 'soak_pool/blobs/06/06eebc04b7f90340adf03dbc86868b02', 'soak_pool/blobs/07/0717efb8c793beebddb325cba8d076da', 'soak_pool/blobs/0f/0fca7b1e1f16c9752ba3f714aecb3c2c', 'soak_pool/blobs/12/12d68cf72c2f6217b3ca85ffb2fae4fe', 'soak_pool/blobs/14/14efb9d2dfe01430a62cd064e40fc318', 'soak_pool/blobs/15/151ef3fcaa9bd70cf26a36132b2432a8', 'soak_pool/blobs/1d/1d60ba4b3f5540694e218b5902602f41']

## S25-20260706T232602-1: GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soa

- **Logged (UTC):** 2026-07-06T23:26:27
- **Severity:** suspected-bug
- **Run:** 20260706T232602_S25_seed20260707
- **Observed:** GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/00/00000000000000000000000000000000', 'soak_pool/blobs/07/07596c79b6ee9d57c99a5e7272902c3f', 'soak_pool/blobs/0d/0d6b60b1f3397793f1c5f54f78326e1d', 'soak_pool/blobs/1b/1b243a06671e1270cd076b0a901ad65a', 'soak_pool/blobs/38/38aa643fcf9332594e0166ac106170b9', 'soak_pool/blobs/a3/a3af5524c8b55aa3cb374c923706ae39', 'soak_pool/blobs/c7/c7dedcceb2f845ee7ffe17e48ce96c0f', 'soak_pool/blobs/e1/e19d3c9977e508b0824410174ef10166', 'soak_pool/blobs/fb/fb85b48a48b6dbb3617b8ec2e460483b', 'soak_pool/blobs/fd/fd082a9a2007ea9bb93102b15e1a8f33']

## S26-20260706T232811-1: GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soa

- **Logged (UTC):** 2026-07-06T23:28:39
- **Severity:** suspected-bug
- **Run:** 20260706T232811_S26_seed20260707
- **Observed:** GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/0c/0cb03f16cfebaccc7750d4ca40ebc188', 'soak_pool/blobs/0f/0f2b0b701c916b38c32dcbd42bfd1be1', 'soak_pool/blobs/12/121a73e8a6b09205e6fb7fa75e5bf273', 'soak_pool/blobs/14/14174d2098c21591e0d4781382e7ce35', 'soak_pool/blobs/17/17147dcd91c5dcccb77fc50fe576ada1', 'soak_pool/blobs/1f/1fe03c0fb542471b31f32b62a54917c4', 'soak_pool/blobs/20/200d0e4f3db020618ce4eaca85fa3006', 'soak_pool/blobs/20/20225823b00d4d034be7ed1125075e28', 'soak_pool/blobs/21/21d8e7ce195f9b2f875908e0746e800b', 'soak_pool/blobs/25/25892a5e81965b3a3c2e9e17868966c1']

## S30-20260706T233201-1: S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGe

- **Logged (UTC):** 2026-07-06T23:32:38
- **Severity:** suspected-bug
- **Run:** 20260706T233201_S30_seed20260707
- **Observed:** S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated.

## S33-20260706T233703-1: GC dry-run proposed deleting 34 key(s) NOT classified unreachable by fsck: ['soa

- **Logged (UTC):** 2026-07-06T23:37:39
- **Severity:** suspected-bug
- **Run:** 20260706T233703_S33_seed20260707
- **Observed:** GC dry-run proposed deleting 34 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/07/070323332e252eb0620007c0728aa372', 'soak_pool/blobs/10/1000348b3e9635e7adef8c91774d6747', 'soak_pool/blobs/19/19fe4b0496717c2cd3cdbe977451fd62', 'soak_pool/blobs/1a/1a742de0de639e55306b48fba511985d', 'soak_pool/blobs/20/200961efba8a14242a2d97a83c2fdfb2', 'soak_pool/blobs/24/24a908bed8ddfacfc5aaf7cc96a8f01d', 'soak_pool/blobs/2f/2f5e04d458d0eebfeea76204b7228ea2', 'soak_pool/blobs/35/35cb2108cb4974f86801b513f7e33b08', 'soak_pool/blobs/3a/3ac9f384f1f74e0a14be1aa360c192a4', 'soak_pool/blobs/44/440730c92b5561bda171664ea355263a']

## S34-20260706T233911-1: S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRo

- **Logged (UTC):** 2026-07-06T23:39:55
- **Severity:** suspected-bug
- **Run:** 20260706T233911_S34_seed20260707
- **Observed:** S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRootGet first=32 -> last=248, root_dirs 2 -> 2) — D1 should have eliminated the monotone namespace registry; investigate dropNamespace / tombstone GC reclaim path


## SOAK-LONG-CHAOS-HARNESS-REMEDIATION (2026-07-07, from the 4h chaos soak) — MEDIUM, harness-only

A clean multi-hour chaos soak is currently blocked by THREE non-CA limits (all reconfirmed by the
2026-07-07 4h run; CA correctness stayed GREEN — perfect replica agreement through the chaos fault,
`dangling=0` while fsck worked). To enable a meaningful long chaos soak on this host, the harness/infra
needs (independent, ranked):
1. **Relax the TTL-band checkpoint oracle for long chaos runs** (`soak/run.py:443` `ambiguous_band_nonempty`,
   `AMBIGUOUS_BAND_EPS`; TTL = `90 MINUTE` in the DDL `run.py:94` + `model.py` `ttl_seconds`). Under chaos a
   row parked within `eps` of its TTL boundary makes the recovery checkpoint unassertable → hard FAIL at
   ~97 min (only 1 of 81 faults exercised). Fix = downgrade the stuck-band checkpoint to INCONCLUSIVE (skip)
   under an active fault window, OR drop/lengthen the TTL for long runs. This is the immediate blocker.
2. **Reduce the fsck discovery cost or raise its bound** (B146/B154 / discovery-quadratic): at 183 GB /
   2.14 M objects the `detail=False` fsck exceeds 180 s → the `dangling==0` gate is SKIPPED, blinding the
   correctness oracle from ~t+84 min onward. Even with (1) fixed, a long run runs blind past this point.
3. **Hard-cap the pool or use a compacting store**: `_THROTTLE_MAX=1.0 s`/insert can't hold `max_pool_gb`
   because rustfs does no background compaction and merge/mutation write-amp outpaces insert-pacing
   (pool → 187 GB in the 4h run). Options: a compacting object store on the stand, a lower object-count
   scale, or a throttle that can fully stall inserts when over budget.
Also: the soak's own `pool_objects` probe returned None the entire run and `pool_bytes` None ~half the ticks
(telemetry-robustness gap; the throttle fail-closes on None, resmon du is ground truth).
Full writeup: `docs/superpowers/worklogs/2026-07-06-scenario-validation-night.md#chaos-soak-result`.
## S34-20260707T061202-1: S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRo

- **Logged (UTC):** 2026-07-07T06:14:00
- **Severity:** suspected-bug
- **Run:** 20260707T061202_S34_seed20260707
- **Observed:** S34 D1 regression: per-round GC fanout grew across create/drop iterations (CasRootGet first=0 -> last=214, root_dirs 2 -> 2) — D1 should have eliminated the monotone namespace registry; investigate dropNamespace / tombstone GC reclaim path

## S22-20260707T064805-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 246. DB::Exception: Build:

- **Logged (UTC):** 2026-07-07T06:48:32
- **Severity:** suspected-bug
- **Run:** 20260707T064805_S22_seed20260707
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 246. DB::Exception: Build: blob object soak_pool/blobs/f2/f2123bb7f1630af47810ea1a47068929 size 0 is below the pool blob header length 256. (CORRUPTED_DATA) (version 26.6.1.1) | sql=INSERT INTO s22_t0 SELECT 0 + number AS id, randomString(4096) AS payload FROM numbers(750)

## S13-20260707T071428-1: forced GC left 1 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible

- **Logged (UTC):** 2026-07-07T07:17:19
- **Severity:** suspected-bug
- **Run:** 20260707T071428_S13_seed20260707
- **Observed:** forced GC left 1 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible leak; full residual by prefix: {'_manifests': 1}. If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.

## S13-20260707T071428-2: S13 residual unreachable=5 after forced GC; classified by prefix={}

- **Logged (UTC):** 2026-07-07T07:17:19
- **Severity:** suspected-bug
- **Run:** 20260707T071428_S13_seed20260707
- **Observed:** S13 residual unreachable=5 after forced GC; classified by prefix={}

## S18-20260707T072144-1: GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['so

- **Logged (UTC):** 2026-07-07T07:22:17
- **Severity:** suspected-bug
- **Run:** 20260707T072144_S18_seed20260707
- **Observed:** GC dry-run proposed deleting 132 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/01/01072cc50e01979bd48c985b2719ee8c', 'soak_pool/blobs/01/01c16d4da5bf1ada12a2024ca8591c4c', 'soak_pool/blobs/06/06d01b256bb15321515b1c38254ff56e', 'soak_pool/blobs/06/06eebc04b7f90340adf03dbc86868b02', 'soak_pool/blobs/07/0717efb8c793beebddb325cba8d076da', 'soak_pool/blobs/0f/0fca7b1e1f16c9752ba3f714aecb3c2c', 'soak_pool/blobs/12/12d68cf72c2f6217b3ca85ffb2fae4fe', 'soak_pool/blobs/14/14efb9d2dfe01430a62cd064e40fc318', 'soak_pool/blobs/15/151ef3fcaa9bd70cf26a36132b2432a8', 'soak_pool/blobs/1d/1d60ba4b3f5540694e218b5902602f41']

## S25-20260707T072448-1: GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soa

- **Logged (UTC):** 2026-07-07T07:25:18
- **Severity:** suspected-bug
- **Run:** 20260707T072448_S25_seed20260707
- **Observed:** GC dry-run proposed deleting 10 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/00/00000000000000000000000000000000', 'soak_pool/blobs/0d/0d6b60b1f3397793f1c5f54f78326e1d', 'soak_pool/blobs/1b/1b243a06671e1270cd076b0a901ad65a', 'soak_pool/blobs/2f/2f47273814e4e7c29145a9e1543e52fa', 'soak_pool/blobs/9b/9bb486c1ee93987ad634bc7792f24bb3', 'soak_pool/blobs/c7/c7dedcceb2f845ee7ffe17e48ce96c0f', 'soak_pool/blobs/c8/c89a7d919795d0202f37af4ed5930700', 'soak_pool/blobs/d9/d94fa6eb80490d0867c25e78ee8ef02d', 'soak_pool/blobs/fb/fb85b48a48b6dbb3617b8ec2e460483b', 'soak_pool/blobs/fd/fd082a9a2007ea9bb93102b15e1a8f33']

## S26-20260707T072518-1: GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soa

- **Logged (UTC):** 2026-07-07T07:25:50
- **Severity:** suspected-bug
- **Run:** 20260707T072518_S26_seed20260707
- **Observed:** GC dry-run proposed deleting 63 key(s) NOT classified unreachable by fsck: ['soak_pool/blobs/04/040fa185ea949400f8f8f13f41e7a6eb', 'soak_pool/blobs/09/09eceded07bc90a7a9c054998a757811', 'soak_pool/blobs/0a/0a2c77daea60b234a72cd951fecf1fc3', 'soak_pool/blobs/0a/0a8b602963ffd45c68488dbe0db13ee1', 'soak_pool/blobs/0b/0b7d1c9998e7f02fcd3abb72bdf4094a', 'soak_pool/blobs/0f/0f2b0b701c916b38c32dcbd42bfd1be1', 'soak_pool/blobs/11/118b5356bdf4d4b87fc1feab72929d4a', 'soak_pool/blobs/12/121a73e8a6b09205e6fb7fa75e5bf273', 'soak_pool/blobs/16/160493b5223359bae725615333faff0e', 'soak_pool/blobs/18/182467fb900cf2494daae8acee7eab48']

## S30-20260707T072639-1: S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGe

- **Logged (UTC):** 2026-07-07T07:27:19
- **Severity:** suspected-bug
- **Run:** 20260707T072639_S30_seed20260707
- **Observed:** S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated.


## F3-single-leader-dryrun-overproposal (2026-07-07, S18/S25/S26) — RESOLVED: over-strict oracle, NOT a tool/CA defect
- **CORRECTION (evidence beat my first hypothesis):** I initially guessed `previewDeletes` reads a
  stale fold seal and over-proposes REACHABLE blobs. WRONG. A minimal repro (create/insert/DROP →
  forced-GC-to-fixpoint → fsck-detail + ca-gc-dryrun) showed: fixpoint residual=7, fsck
  classes `{reachable:6, pending-gc:7}`, dryrun proposes exactly the **7 `pending-gc`** objects — ZERO
  reachable. So `ca-gc-dryrun` is CORRECT: it previews the next round's deletes = condemned objects in
  the two-phase graduation pipeline. The candidate count equalling the reclaimable residual in the
  sweep (S25 10=10, S26 63=63) is the same signal.
- **Real root cause = the SCENARIO ORACLE.** `assert_dryrun_subset` accepted only fsck `class=="unreachable"`,
  but fsck splits not-reachable objects into `unreachable` (orphans not yet condemned) and
  `pending-gc`/`awaiting-gc` (condemned, awaiting min-ack graduation). The preview legitimately targets
  the UNION. Any create/insert/DROP scenario leaves a bounded `pending-gc` residual at the fixpoint (the
  last drop's condemned blobs), so the oracle falsely failed.
- **FIX (committed):** `scenarios/framework/assertions.py` `assert_dryrun_subset` now checks
  `dryrun ⊆ (unreachable ∪ pending-gc ∪ awaiting-gc)`, and still FAILS + notes the class if a candidate
  is genuinely `reachable`/`dangling`/absent (the real over-proposal this oracle exists to catch).
  Verified: S18/S25/S26 + S33 + S31 re-run green on this.
- **Also resolves** the paired "no unbounded leftovers" inconclusive interpretation: a `pending-gc`
  residual at the fixpoint is EXPECTED (condemned-awaiting-graduation), not a leak.
- **Distinct from:** `S31-*-dryrun-shard0` (preview-only-shard-0 under gc_shards>1 — already fixed) and
  the concurrent-leader leak (genuine orphans left forever — a real, separate liveness bug).

## NEXT-TASK-scenario-infra-and-inconclusives (2026-07-07, deferred after F3) — continue the no-vacuous-scenarios sweep
After the `Gc::previewDeletes` (F3) fix, resume closing the S13–S32 inconclusives that are still
infra/measurement gaps (not CA defects — all had dangling=0 + agreement):

**RESUME STATE (2026-07-07 night — where we stopped; continue next night):**
- **S15 gc_shards=8: INFRA BUILT + COMMITTED, clean re-run PENDING.** Done: `storage_conf_gc_shards8_ch{1,2}.xml`
  (gc_shards=8), `docker-compose-gc_shards8.yml`, variant registered in `cluster_boot`, S15 card
  `_VARIANTS` now `(("default",1),("gc_shards2",2),("gc_shards8",8))` + hardcoded inconclusive removed +
  comparison text updated. NOT yet run clean (the launch collided with another run and was killed). NEXT:
  `python3 -m scenarios.run --scenario S15 --scale dev --seed 20260707` (runs 3 variants; slow).
- **S23 (1/10-server) + S16/S20 (counters) + S21/S29 (ci-scale): NOT STARTED** (see below).
- **graduation-drain DECISION PENDING:** `gc.drain_condemned_pipeline` + class-aware `assert_no_leftovers`
  are committed and correct, but the drain adds ~110–150s per checkpoint that has a condemned residual
  (drains early if healthy). Open: run a full sweep to measure the cost, and decide whether to lower
  `mount_renew_period` on the stand (faster floor advance → faster drain; risk = affects mount-lease /
  fence-out timing). Also: the class-aware oracle now correctly FAILs S30 + recurring-hash churn cards on
  the real `RESURRECT-REUPLOAD-ORPHAN` leak (below) — expected until that product bug is fixed.
- **Chosen next work: FIX RESURRECT-REUPLOAD-ORPHAN (option 1)** — started 2026-07-07 night.

1. **Build S15 + S23 infra:**
   - **S15 (GC target-shard comparison):** DONE (infra built + committed; re-run pending — see RESUME STATE).
   - **S23 (idle shared-pool baseline):** needs a 1-server baseline and a 10-server baseline; the
     10-server case can REUSE the tenreplicas compose (`Cluster(node_count=10)`); the 1-server case
     needs a 1-node compose (or run against ch1 only). Wire both so the card measures real idle-pool
     baselines instead of recording "compose fixed at 2 servers".

2. **Chase the dev-scale inconclusives (measurement-oriented):**
   - **S16 (hot content cycle):** "resurrection counters not present in system.metrics/system.events on
     this build" — find the real counter names on 26.6.1.1 (grep system.events for resurrect/revive/
     recreate-ish CA counters) and wire them, or assert the property another way.
   - **S20 (replicated fetch and relink):** follower `CasRootCas=0` — the counter may not be scoped
     per-node; find a per-node attribution (per-node ProfileEvents query) so "follower publishes its own
     refs" is decidable.
   - **S21 (read-heavy many-ref) / S29 (large non-direct-blob memory spike):** the blob-cache /
     non-blob-footprint comparison is unmeasurable at dev scale (cache hits entirely; footprint too
     small). Re-run at `--scale ci` (or full) where the comparison is meaningful; consider a card note
     that dev is expected-inconclusive for these.
## S30-20260707T085740-1: forced GC left 3 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manif

- **Logged (UTC):** 2026-07-07T09:00:58
- **Severity:** suspected-bug
- **Run:** 20260707T085740_S30_seed20260707
- **Observed:** forced GC left 3 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'blobs': 3}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.


## RESURRECT-REUPLOAD-ORPHAN (2026-07-07, S30 churn; found via content_addressed_log) — MEDIUM, real leak (small)
- **Observed:** under rapid create/insert/DROP churn with RECURRING content hashes (a small ~300 B blob
  whose content repeats across tables), forced-GC-to-fixpoint + the graduation-drain leaves a small,
  STUCK residual (3 objects in repro; S30 real run showed 3) that fsck classes `unaccounted`
  (INV-2: "outside the whole GC view — should be impossible once GC has run") — sometimes `unreachable`.
  Does NOT drain over a long window (24+ rounds x 11s). `dangling=0` throughout (no committed ref to a
  missing object). Magnitude tiny, but it is a real orphaned physical object + an INV-2 violation.
- **Root cause (proven via `system.content_addressed_log` token trail):**
  1. `blob_put` token A → referenced → unreferenced → `blob_retire` (A condemned, in retired list).
  2. A new insert with the SAME content hash hits `blob_reuse_resurrect` ("observed token A condemned;
     caller must re-upload") and does `blob_put` token B — a NEW incarnation at the same content-hash key.
  3. GC's two-phase pipeline was tracking token A: `gc_recheck_verdict` publishes delete_pending for A,
     then `blob_delete` token A runs with `outcome=replaced` — the exact-token guard finds token B present
     and FAIL-SAFE SKIPS the delete (correct: never delete a newer incarnation you weren't told to).
  4. But incarnation B, once ITS table is dropped, is never re-entered into the condemn pipeline — GC
     never re-observes it as a zero-in-degree candidate. B is orphaned → `unaccounted`. The physical
     object survives in the store (verified: blobs/59/59fae…/xl.meta present, token B).
- **So:** the resurrect → re-upload → re-condemn path has a gap. The exact-token delete guard (fence-like)
  correctly protects the newer incarnation B; the defect is that B is not re-tracked for condemnation
  after the guarded skip, so a recurring-hash churn workload slowly orphans re-uploaded incarnations.
- **Impact:** small permanent leak of tiny recurring-hash blobs under churn; INV-2 violation. Real
  workloads with unique content hit it far less (needs the content hash to recur across the condemn window).
- **PRECISE fold-state mechanism (CasBlobInDegree.cpp `closeBlob`/`settleEntry`, ~L225-251):** in-degree
  is per-HASH (source-edges `(blob_hash, source_id)`), condemn/retire is per-TOKEN. When a hash has a prior
  retired entry (token A) AND cur_edges==0, `closeBlob` takes the `prior_retired[ri].hash == cur_blob`
  branch → `settleEntry(A, 0)` → A graduates (delete_pending → exact-token delete). It NEVER reaches the
  `else` fresh-condemn path (L238) that would `head_blob` the CURRENT token and condemn it. So the token B
  present at the key (from the resurrect re-upload) is never condemned. Under rapid churn B's transient
  source-edges (create→insert-adopt→drop within one fold window) net to ABSENT, so the hash's in-degree
  reads 0 throughout → A is never `spared` (L198-199) → A graduates → exact-token delete of A finds B →
  `outcome=replaced` skip → B orphaned. `settleEntry` settles the OLD token without re-observing that the
  physical object is now a NEWER token.
- **TLA+ STATUS (2026-07-07): reproduced + fix validated.** `docs/superpowers/models/CaGcResurrectReuploadOrphan.tla` — `NoLeakForever` VIOLATED with the shipped hash-keyed/touch-gated behavior, HOLDS with the fix (re-condemn the CURRENT token when settling a prior entry whose token differs at in-degree 0). The fix makes `closeBlob` match `CaIncarnationCore`'s already-proven `GRetire` (condemn by (hash, current token)). Root of the miss: model-vs-code faithfulness gap (the proven model encoded the correct algorithm; the C++ drifted). NEXT = implement the C++ change in `CasBlobInDegree.cpp` closeBlob + gtest + rerun S30.
- **Fix direction (design-sensitive — TLA+-gated core; do NOT rush):** in `closeBlob`, when settling a
  prior retired entry that is about to graduate/delete with cur_edges==0, `head_blob` the current token; if
  it DIFFERS from the retired entry's token (a resurrect replaced it), condemn the current token as a FRESH
  entry (condemn_round=this round) instead of graduating the stale one — so B enters the pipeline. (Alt:
  at exact-token delete, on `replaced`, re-condemn the observed token.) Touches the fold retire-merge +
  MonotoneGC/ack-floor invariants → needs a spec + TLA+ gate (CaGcRootLocalPartManifestCore.tla) before
  C++. Relates to [[feedback_ca_resurrect_invariant]] and the B140-dangle lineage.
- **Oracle:** the improved class-aware `assert_no_leftovers` + graduation-drain CORRECTLY surfaces this
  (unaccounted/unreachable ⇒ leak ⇒ FAIL) — it was previously MASKED by the "fsck detail unavailable"
  inconclusive. So S30 (and any recurring-hash churn card) now FAILs on this real residual until fixed.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`).** Fix in `CasBlobInDegree.cpp` `closeBlob`: when a hash is
  touched this fold window with net in-degree 0, HEAD the current token and ensure a condemn entry for the
  CURRENT token, superseding any stale-token retired entry — i.e. condemn/retire keyed on `(hash, current
  token)`, matching `CaIncarnationCore`'s `GRetire`. Adds `blob_retire_replaced` CA-log event +
  `CasGcRetireReplaced` counter; rides the existing round CAS (no extra write), +1 HEAD per resurrect cycle.
  TLA+-gated by `CaGcResurrectReuploadOrphan` (`_bug.cfg` violates `NoLeakForever`, `_fix.cfg` holds; see
  `docs/superpowers/cas/06-tla-models.md` §Area 12). Commits `5156d37454b`(TLA+)..`6da55fce2a0`(tests), fix
  `308360e595d`. Verified: unit `CasGcLeak.*` (RED→GREEN + idempotency + writer-side retire-view), and S30 —
  the blob residual moved from stuck `unaccounted` to a draining pipeline (the remaining S30 `_manifests`
  orphan was a DISTINCT bug, `DANGLING-PRECOMMIT`, since also fixed). Follow-up: touch-gating dimension in
  the canonical `CaIncarnationCore` model (non-blocker).
## S30-20260707T120511-1: forced GC left 1 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manif

- **Logged (UTC):** 2026-07-07T12:08:27
- **Severity:** suspected-bug
- **Run:** 20260707T120511_S30_seed1
- **Observed:** forced GC left 1 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 1}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.
- **ROOT-CAUSED 2026-07-07 (auto-diagnosis above is WRONG — not concurrent-leader):** run was single-node
  forced GC. The orphan is manifest `1:35:1` (ns `ca_soak_ch2/store/7a5/…@cas@`, part `all_0_0_0`). Decoding
  the raw root-shard journals: shards for builds 34/36 show full precommit→promote→drop (R6-deleted); shard 59
  for build 35 holds a **single `RootOwnerEvent{new_binding=Precommit, manifest_ref=1:35:1}` and NOTHING ELSE**
  — no promote, no abandon, no drop. So this is a **DANGLING PRECOMMIT manifest binding**, a distinct bug from
  the blob RESURRECT-REUPLOAD-ORPHAN (NO token-replace; all `manifest_delete` outcomes were `deleted`).
  Mechanism: `CasOrphanManifestSweep.cpp` `activeManifestKeys` puts every `new_binding` Precommit into
  `precommit_live` and only erases it on a removal event — none exists for 35 → sweep spares it EVERY round
  (proven: 15 extra forced rounds, no change; eligibility satisfied, mount `min_active=37 > 35`). R6 never
  folds a `-1` (owner never removed) → never deletes. fsck follows only COMMITTED refs → classes it
  `unreachable`. Net: sweep says "live (precommit)", fsck says "leak (unreachable)" — permanent orphan + INV-2.
  Likely trigger: a locally-inserted precommit (`all_0_0_0`, build 35) superseded by a replication-fetched
  incarnation (`tmp-fetch…`, builds 34/36 that committed+dropped) and never abandoned. Aggregate signal was
  visible (ch2: `precommit=36`, `build_publish=35`, `build_abort=0` → 1 build precommitted, never
  published/aborted). **FIX = the precommit-abandon/cleanup path** (own brainstorming→spec→plan cycle,
  started 2026-07-07). Separate from the committed blob fix.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`).** Root cause was not the writer abandon path but the
  GC-side reclaim being parked by the token-diff `Skip`: `reclaimAbandonedPrecommit` only runs on a
  fold-visit, and a content-static shard holding the abandoned precommit is Skip-parked, so reclaim never
  re-runs once the watermark proves the precommit dead. Fix (TLA+-gated by
  `SkipParksDeadPrecommit`/`LiveDeadPrecommitReclaimed`, see `docs/superpowers/cas/06-tla-models.md`
  §Area 7): `Gc::computeDiscoverDecisions` force-Reads a token-stable shard whose sealed minimal live
  precommit `isPrecommitDead` vs the mount watermark, so `reclaimAbandonedPrecommit` runs, emits the
  owner-removal, the fold folds the `-1`, and R6 deletes the manifest. Commits: TLA+ `3a836c24364` +
  sibling-cfg `5fe74bd373e`; RED test `910646891e0`; `isPrecommitDead` `180a6f2cc0e`; `ShardCoverage`
  field `10981183d19`; the fix `c1479a8553c`; guard tests `7c06bcfdde2`. Verification: unit
  `CasDanglingPrecommit.*` (deterministic), regression 170/170, and S30 — pre-fix 1 `_manifests` orphan
  (FAIL) → post-fix ×4 seeds all PASS residual 0 (`reclaimAbandonedPrecommit` seen firing live in one
  seed via the normal Read path; the parked-static-shard force-Read timing is a rare race proven
  deterministically only by the unit test). Follow-ups `PROMOTE-OVER-COMMITTED-LEAK` +
  `ABANDON-RETIRE-ORDERING` remain (below).

## INTROSPECTION-1 (2026-07-07): manifest/precommit lifecycle audit gap in `system.content_addressed_log`

- **Logged (UTC):** 2026-07-07
- **Severity:** missing-instrumentation
- **Observed:** the CA event log is only half-instrumented for the manifest/precommit lifecycle, which is why
  the DANGLING-PRECOMMIT orphan (S30 above) could not be diagnosed per-object from SQL and required hand-
  decoding raw bucket objects. Concretely: `ManifestPut` has **0 emit sites** (only the manifest DELETE is
  logged, never the body write), `PrecommitRemoved` has **0 emit sites** (the exact abandon/removal event
  whose absence IS the bug), and `ManifestExpand`/`ManifestRetire`/`ManifestStrip` are **dead enum entries**
  (declared, never emitted). This contradicts the table's design intent (every state-changing action logged
  with motivation + details).
- **Proposed action:** emit `ManifestPut` (key + token + motivation) and the owner-transition events at the
  manifest-owner level (promote, `PrecommitRemoved`/abandon); implement or delete the dead
  `ManifestExpand/Retire/Strip`. Goal: "precommit created, never removed" becomes visible per-object in
  `system.content_addressed_log`. Debuggability-first, in the spirit of B170 (blob audit that pinned the
  B140 dangle).
- **Also fold in (from the RESURRECT-REUPLOAD-ORPHAN blob-fix final review, 2026-07-08, both Minor
  observability-only):** (a) `CasBlobInDegree.cpp` `closeBlob`'s supersede reuses the `head_blob` peek,
  which is the *fresh-condemn* observation hook — so a resurrect supersede emits BOTH `blob_retire` and
  `blob_retire_replaced` for the same (hash, token, round) and double-increments
  `CasGcRetiredCondemned`+`CasGcRetireReplaced`. Give the peek a side-effect-free HEAD (or an observe-only
  `head_blob` mode) so `blob_retire_replaced` is the SOLE retire event for the supersede — this pollutes
  the very audit log used to triage such leaks. (b) `blob_retire_replaced` records only the new token; the
  spec (`…resurrect-reupload-orphan-fix-design.md` §Observability) intended `{hash, old_token, new_token,
  round}` — add the superseded token to `detail`. Neither is a data-safety issue; both belong in this
  audit-accuracy cycle.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`).** `Build::stageManifest` now emits `manifest_put` (a
  manifest's body write) and `Build::abandon` emits `precommit_removed` (the writer-side precommit removal;
  GC-side already logged `precommit_reclaim`) — so the manifest/precommit lifecycle is fully auditable
  per-object in `system.content_addressed_log` and "precommit created, never removed" is a visible gap. The
  dead `ManifestExpand`/`ManifestRetire`/`ManifestStrip` enum entries were deleted. Blob-audit fixes: the
  resurrect supersede in `closeBlob` now uses a side-effect-free peek (same single S3 HEAD) so it emits ONLY
  `blob_retire_replaced` (not also `blob_retire`) with a single `CasGcRetireReplaced` increment, and records
  the superseded `old_token` in `detail["superseded_token"]` (a review caught + fixed a size-unit regression
  in the peek — now applies `retiredLogicalSize` like `head_blob`). Commits `ab74694aaa8`, `0f539bafc0b`,
  `99cbe199580`. Unit: `CasObservability.*`.

## INTROSPECTION-2 (2026-07-07): no easy human-readable introspection of CA bucket objects

- **Logged (UTC):** 2026-07-07
- **Severity:** missing-instrumentation
- **Observed:** diagnosing the DANGLING-PRECOMMIT orphan required spinning up an ephemeral `mc` container and
  hand-decoding protobuf/custom-binary objects (root-shard journals, manifest bodies, mount leases, gc state)
  by `od -tx1`. There is no supported way to inspect a decoded CA object. The decoders already exist in the
  codebase (`decodeRootShard`, `decodePartManifest`, `decodeMountLease`, `decodeGcState`).
- **Proposed action:** expose the decoders via `clickhouse-disks` (which already hosts `ca-fsck`/
  `ca-gc-dryrun`) as e.g. `ca-inspect <key>` → human-readable JSON for any CA object; optionally extend fsck
  detail to report the "why" per key (`reachable-via` / `spared-by-precommit` / `eligible`) so leaks like this
  surface directly. Ends hand hex-decoding of the bucket.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`, commit `82bc0df7df8`).** Added `clickhouse-disks ca-inspect
  <key>` — read-only, dispatches by key layout to the existing decoders (`decodeRootShard`/
  `decodePartManifest`/`decodeMountLease`/`decodeGcState`/`decodeFoldSeal`/`decodeRetiredSet` + the
  `CasEnvelope` header for `blobs/`) and prints human-readable JSON; unknown key → `BAD_ARGUMENTS` listing
  recognized layouts (fail-closed, no guessed dump). The decode→JSON logic is a CLI-free `caInspectToJson`
  free function (`Core/CasInspect.{h,cpp}`), unit-tested against each encoder's output. The optional fsck
  "why per key" extension is NOT included (deferred). Ends the ephemeral-`mc` + `od` hand-decoding.


## PROMOTE-OVER-COMMITTED-LEAK (2026-07-08, write-path audit) — MEDIUM, real reachable leak + fail-close gap

- **Logged (UTC):** 2026-07-08
- **Severity:** suspected-bug (over-count / permanent leak + owner↔refs divergence; NOT a dangle — `INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_COMMIT_FAILCLOSED` hold). Distinct from DANGLING-PRECOMMIT.
- **Confirmed in code:** `CasBuild.cpp` `Build::promote` (~L896-907) appends a Δ=0 owner-move (`old=Precommit(R,bld,T)`, `new=Committed(R,T)`) then `root.refs[R] = RootRef{...}` UNCONDITIONALLY — never reads the prior `refs[R]`, never emits a repoint `-1` for a pre-existing committed `T_old`. The in-closure gate only proves THIS build's precommit is still live (`seen_removal && !present_in_live`), not that a different committed owner already holds `R`. So a promote over an existing committed ref leaves `Com(R,T_old)` live in the journal with no `-1` ⇒ `T_old`'s manifest body + uniquely-owned blobs pinned forever (in-degree stuck ≥1); the journal holds two live `Committed` bindings for `R`; a later `dropRef(R)` removes only `Com(T_new)`, leaving `Com(T_old)` live with no `refs[R]` entry (owner↔refs divergence).
- **Reachability (upgraded):** NOT just "unique-part-names externally violated". `republishRef` (primitive behind DETACH/ATTACH rename + RENAME TABLE, `ContentAddressedTransaction.cpp:167-174`) does `stageManifest → precommitAdd → promote(dst) → dropRef(src)` and is advertised idempotent/re-drivable. A crash/throw AFTER `promote(dst)` and BEFORE `dropRef(src)`, then re-driven, mints a FRESH `ManifestId T_b` (`stageManifest` bumps ordinal; new build ⇒ new build_seq), so the second `promote` overwrites `refs[dst]=Com(T_b)` and leaks `Com(dst,T_a)`. The `moveDirectory` idempotency claim holds only when the prior attempt did nothing or completed through `dropRef(src)`; the "dst published, src not dropped" intermediate re-drives into a leak.
- **Fix (own brainstorm→spec→plan cycle):** in `promote`, before overwriting `refs[final_ref_name]`, inspect it: if it names a DIFFERENT committed `T_old` → emit a proper repoint (`old_binding = Committed(R,T_old)`) so GC folds the `-1`, OR throw `LOGICAL_ERROR` (fail closed). Repoint event shape already defined in `CasRootShardCodec.h`. Equivalently `republishRef` could `dropRef(src)` before/atomically, or make the dst promote idempotent when `refs[dst]` already names a manifest over the same entries.
- **Interaction with DANGLING-PRECOMMIT fix (cas-gc-rebuild):** independent locus (writer-side promote vs GC-side discover/fold reclaim); does not block that fix.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`).** Chosen fix was fail-close + idempotent re-drive (NOT a
  silent repoint, and `ABORTED` not `LOGICAL_ERROR` — the latter is CI-checked and reserved for
  must-not-happen invariants). `Build::promote` throws `ABORTED` when `refs[final_ref_name]` already names a
  DIFFERENT committed `manifest_ref` (a same-manifest re-promote / absent ref proceed) — enforcing the
  model's `RefFreeFor` guard (`CasBuild.cpp`). `ContentAddressedTransaction::republishRef` is idempotent on
  the destination: if dst is already committed with the same path-sorted `entries`, skip
  stage/precommit/promote and `dropRef(src)` (finish the interrupted rename); a different-content dst throws
  `ABORTED`. So a RENAME/DETACH-ATTACH crash re-drive no longer leaks and never reaches promote's guard.
  TLA+ gate: `AtMostOneCommittedManifestPerRef` holds in `stage2` (`docs/superpowers/cas/06-tla-models.md`
  §Area 7). Commits: TLA+ `7e604ff1a2a`; RED tests `fa6b7689459`; promote guard `0c8c564f498` (+ test-fix
  `dee120cdde8`); republishRef idempotency `93e7cda1085`. Unit: `CasPromoteRepublish.*` (promote fail-close,
  same-manifest idempotent, absent-ref, re-drive idempotent, different-content conflict).

## ABANDON-RETIRE-ORDERING (2026-07-08, write-path audit) — LOW, latent robustness

- **Logged (UTC):** 2026-07-08
- **Severity:** finding (benign today). `Build::abandon` calls `retireBuildSeq(build_seq)` BEFORE appending the precommit-removal `mutateShard`, opening a window where `min_active` advances and GC's `reclaimAbandonedPrecommit` races `abandon`'s removal (double removal of the same precommit binding). Safe today ONLY because in-degree is an idempotent source-edge SET (H1b) and no committed ref is involved — but it contradicts the ordering discipline the function documents. `Build::promote` already does the safe order (retire AFTER its CAS, `CasBuild.cpp:911`).
- **Relevance to DANGLING-PRECOMMIT fix:** that fix force-Reads Skip-parked shards, INCREASING `reclaimAbandonedPrecommit` firing frequency → the abandon-vs-reclaim double-removal window is exercised more. Still safe (idempotent set), but raises the priority of moving `retireBuildSeq` to AFTER the removal `mutateShard`.
- **Fix:** move `retireBuildSeq` after the removal CAS in `Build::abandon`, mirroring `promote`.
- **RESOLVED 2026-07-08 (branch `cas-gc-rebuild`, commit `f5fd7c0ead3`).** `Build::abandon` now retires the
  build_seq only AFTER the precommit-removal `mutateShard` (unconditionally, past the `if (precommitted)`
  block; `alive = false` stays early), mirroring `promote`. The precommit becomes watermark-dead only after
  its removal is durably committed, so `reclaimAbandonedPrecommit` can never observe a live-and-dead
  precommit to double-remove. Regression test `CasPromoteRepublish.AbandonEmitsRemovalBeforeRetire`.

## STATELESS-04286-getmountpoint-eisdir: existsFile mountpoint probe throws "Is a directory" on the LOCAL CA backend

- **Logged (UTC):** 2026-07-08
- **Severity:** finding (pre-existing; NOT caused by the part-folder-cache Phase 1 work — verified the
  `existsFile` mountpoint branch + `getMountpointObject`/`casGetObject` are untouched by that change).
- **Run:** `Stateless tests (arm_binary, parallel) --test 04286_content_addressed_remote_data_paths`
  (isolated re-run reproduces).
- **Observed:** `SELECT count() >= 0 FROM system.remote_data_paths WHERE disk_name='...' SETTINGS
  traverse_shadow_remote_data_paths=1` throws `Code 74 CANNOT_READ_FROM_FILE_DESCRIPTOR: Cannot read
  from file <pool>/ca/roots/<srv>/store: Is a directory (errno 21)`. Stack:
  `existsFile` (mountpoint branch, `ContentAddressedMetadataStorage.cpp:537`) → `Store::getMountpointObject`
  → `Store::casGetObject` → `readObjectRanged` opens the pool subdir `store` as a file. The test
  explicitly pins (per B38) that this traversal must NOT throw "Is a directory".
- **Analysis:** LOCAL/Emulated object-storage backend specific — a GET on a key that collides with a
  directory prefix (`store/`) raises EISDIR instead of behaving like an absent object (S3 returns 404).
  On S3/RustFS this path likely won't reproduce. B38 fixed an earlier incarnation; it has regressed on
  the branch independently of Phase 1.
- **Proposed action:** make `Cas::readObjectRanged` / `casGetObject` treat an EISDIR open on the LOCAL
  backend as object-absent (fail-closed nullopt) for the mountpoint-probe path, OR have
  `getMountpointObject` HEAD-then-GET. Needs a fail-closed-semantics decision in Core — deferred (not
  obvious/small). Re-check on the RustFS/S3 stateless job (Task 3c) where it may not reproduce.

## STATELESS-05009-event-log-enabled: content_addressed_log enabled in local stateless env breaks the default-off test

- **Logged (UTC):** 2026-07-08
- **Severity:** finding (environment/config; NOT a code regression — Phase 1 does not touch log config).
- **Observed:** `05009_content_addressed_event_log` asserts `system.content_addressed_log` does NOT
  exist by default (stateless config doesn't enable it). In the local run the table EXISTS (returns 354
  rows), so `EXISTS TABLE` → 1 and the `serverError UNKNOWN_TABLE` assertion fails.
- **Analysis:** the local stateless server config has `<content_addressed_log>` enabled (source: the
  local `tests/config` or harness overlay), contradicting the test's default-off contract. Orthogonal
  to Phase 1 and to the file-cache work.
- **Proposed action:** confirm on the CI stateless job whether the default config enables the log; if
  the local overlay enables it, either the overlay or the test's expectation needs reconciling. Re-check
  under Task 3c. Low priority.

## GTEST-CAWIRING-3-PREEXISTING: three gtest_ca_wiring.cpp tests red on the branch (NOT part-folder-cache)

- **Logged (UTC):** 2026-07-09
- **Severity:** finding (pre-existing on the dev branch; DEFINITIVELY not caused by the part-folder-cache
  Phases 1-3 — reverted all part-folder-cache src to the pre-Phase-1 baseline `e6fa3bf16f6`, rebuilt
  `unit_tests_dbms`, and all three fail IDENTICALLY at baseline; `gtest_ca_wiring.cpp` itself is untouched
  by the work and predates the session).
- **Observed (both at HEAD and at e6fa3bf16f6):**
  - `CaWiringOps.FreezeViaHardLinksIntoShadow` (`gtest_ca_wiring.cpp:872`): after
    `removeRecursive("shadow/bk1")`, `existsDirectory("shadow/bk1")` still returns true (expected false).
    The deeper path (`shadow/bk1/store/...`, L871) correctly reports gone — only the backup-root
    intermediate marker lingers. Shadow-intermediate `existsDirectory` + `removeRecursive`/`dropNamespace`
    interaction.
  - `CaWiringGc.DroppedPartIsReclaimedByRounds`: throws `promote: blob <hash> condemned at commit
    revalidation — failing closed (INV-1)` — a promote races a GC condemn within the test's round-driving.
  - `CaWiringGc.DisplacedTreeBlobsReclaimedThroughRealPath` (`:1031`): `after.unreachable == 2`,
    expected 0 — manifestA's unique blobs not reclaimed after displacement (B199 real-path).
- **Note:** these are in the `CaWiring*` suite, which NO recent session gated (every gate used
  `--gtest_filter='Cas*'`, which does not match `CaWiring*`) — so they have been silently red. The two GC
  ones are GC/blob-lifecycle behaviors; the first is shadow-namespace removal. All three are independent
  of the read-path/observability refactor.
- **Proposed action:** triage separately (GC round/condemn timing + shadow-root removal). Add `CaWiring*`
  to a gate going forward. Deferred (not caused by current work; needs GC-domain investigation).

## PFC-PHASE4-OBSERVABILITY-MINORS: part-folder cache retention — 3 diagnostic/observability minors

- **Logged (UTC):** 2026-07-09
- **Severity:** finding (observability/diagnostic only; retention correctness reviewed clean — no
  staleness/fail-closed/deadlock issues). From the Phase-4 retention review.
- **Items:**
  1. **(Medium, deferred)** `CasPartFolderViewEvictions` ProfileEvent is DEAD — declared but never
     incremented. `CacheBase` exposes only `onEntryRemoval(weight_loss, ptr)` which fires for BOTH LRU
     evictions AND explicit `remove()`/`clear()` (write-through erases, dropNamespace sweeps, clearForTest).
     A correct eviction-only counter needs a `ViewCache` subclass overriding `onEntryRemoval` PLUS a guard
     to exclude explicit-removal paths (with a minor cross-thread imprecision if an eviction races an
     explicit remove). Non-trivial for an observability-only counter → deferred. All OTHER facade counters
     (hits/misses/refreshes/mismatches/invalidations/oversized/manifest-gets) are live.
  2. **(Low)** `explain().retained` is a LIVE `view_cache->get()` membership check (deviation from the old
     decision-journal snapshot, needed because dropNamespace skips per-key recording): (a) not an atomic
     snapshot with the `explain_map` read; (b) `get()` bumps LRU recency as a side effect of a "read-only"
     diagnostic. Test/log-only path — acceptable; worth a one-line comment.
  3. **(Low)** Single-flight followers each re-run the retain/`recordDecision`/`CasPartFolderViewMisses`
     logic after `future.get()`, so k concurrent cold waiters increment `Misses` k times (not once per cold
     key). Correct + idempotent; only inflates the miss counter under concurrency. No test asserts the
     count. Consider having only the leader record.
- **Proposed action:** address #1 if/when eviction visibility is needed (with the subclass approach above);
  #2/#3 optional polish. None block the feature.

## SOAK-TTL-BAND-abort: FIXED — checkpoint aborted spuriously on ambiguous TTL band under ttl_pressure

- **Logged (UTC):** 2026-07-09  **Status: FIXED (obvious/small harness bug, per Task 3 directive).**
- **Observed:** the 4h soak aborted at the chaos-stage entry checkpoint (~72min in) with `CHECKPOINT
  FAILURE: ambiguous TTL band still non-empty after 6 waits`. The preceding gc_checkpoint was CLEAN
  (dangling=0, unreachable=0, exact count matched) — so NOT a CA bug.
- **Root cause:** TTL=90min; at a checkpoint ~90min into the run, the warmup-stage rows (inserted over a
  wide window) all cross their `ts+5400` boundary around `now`, so the ±10s ambiguity band is never empty
  within the bounded 6×11s wait (as `now` advances past one row's boundary, another enters). The oracle
  treated this timing artifact as fatal.
- **Fix (`soak/run.py` checkpoint):** degrade like the existing FsckTimeout path instead of aborting —
  WARN, skip only the exact SUM equality (genuinely ambiguous), but STILL enforce (a) a band-tolerant
  COUNT RANGE `[count(now+eps), count(now-eps)]` (catches real data loss/dup) and (b) the GC-to-fixpoint
  + clean-pool fsck gate (the CA-integrity oracle, unaffected by TTL timing). Exact compare unchanged at
  non-ambiguous checkpoints. 22 model tests pass.

## SOAK-FREEZE_LONG-workload-fence-reroute: FIXED — workload aborted instead of rerouting fenced writes

- **Logged (UTC):** 2026-07-09  **Status: FIXED (found by the new FREEZE_LONG fault; obvious once seen).**
- **Observed:** run2 aborted at ~94% (converge) with `WORKLOAD FAILURE: Code 236 ... CAS mount lost /
  lease expired — refusing to mutate ref shard for server_root 'ca_soak_ch1'` on op 101073, during an
  83s FREEZE_LONG of ch1. The CA server behaved CORRECTLY (fail-closed ABORTED — a fenced replica must
  not mutate); the WORKLOAD retry logic was at fault.
- **Root cause:** the fence ABORTED is a retryable ABORTED, so it hit `retry_on_aborted` (6 tries over
  <1s, SAME node) then re-raised; the outer `retry_on_transport` only reroutes `is_node_down`/`is_readonly`
  (NOT ABORTED). So an 83s freeze (> the tiny same-node budget) → give up → abort. The workload never
  rerouted the write to the healthy peer (ch2, shared pool, own live lease).
- **Fix (`soak/cluster.py` + `run.py`):** classify the fence ABORTED (`QueryError.is_mount_fenced` +
  `is_mount_fenced()`), skip its same-node `retry_on_aborted` (re-raise immediately), and add it to
  `retry_on_transport`'s reroute predicate + the OPTIMIZE swallow set — so a fenced write REROUTES to the
  peer (40 attempts, capped backoff) exactly like node-down. 45 retry unit tests pass (8 new). The B137
  transient ABORTED still retries same-node.

## SOAK-REAPER-dies-on-rustfs-restart: orphan-reaper (docker exec) killed by a chaos rustfs restart

- **Logged (UTC):** 2026-07-09  **Severity:** finding (soak-operational; harness, not CA).
- **Observed:** the RustFS overwrite-leak reaper launched as `docker exec -i ... sh -s ... < orphan_reaper.sh`
  (an in-container infinite loop) is killed the moment chaos applies a `rustfs restart`/`kill` fault —
  the exec'd process dies with the container. Over a chaos soak that restarts rustfs repeatedly, the
  reaper stops after the first rustfs fault, and the leak resumes unbounded (disk pressure returns; only
  the 60G-floor disk_watchdog then protects the host).
- **Mitigation (applied this run):** run the reaper as a HOST-side loop that re-execs a single
  `orphan_reaper.sh <dir> --once` pass every 120s while the soak driver is alive, tolerating rustfs
  down/restart between passes — survives restarts. For run_24h.sh, wire the reaper this way (host loop
  with --once) rather than a single long-lived docker-exec.

## CRASH-CA-S3-staged-entries-without-Build: server LOGICAL_ERROR crash during a merge on CA-s3

- **Logged (UTC):** 2026-07-09  **Severity: CRITICAL (server crash / core dump).**  **Status: FIXED 2026-07-09.**
- **FIX (root-caused):** the INLINE write path (`ContentAddressedTransaction.cpp` `writeFile` →
  `CaInlineWriteBuffer` finalize for files <= INLINE_CAP=1MiB) staged a manifest entry via `stagingFor`
  but never called `buildFor` (the blob path does). So a part whose files are ALL inline (no
  `.bin`/`.mrk*`/`primary.idx` blob file — e.g. an EMPTY/tiny merge output) reached `publishStaging` with
  `st.entries` non-empty and `st.build == nullptr` → the invariant throw at line 222 → server abort under
  abort_on_logical_error. Fix = the inline path now calls `buildFor(route, st)` (idempotent — a no-op when
  a blob file already created the build; only an all-inline part now gets its build from the first inline
  file). **PRE-EXISTING, NOT a regression from this session** (git blame: inline-files feature
  `27c5f790d19` 2026-06-24 + `2be338197d2` 2026-06-27, both ancestors of the part-folder-cache base
  e6fa3bf16f6). Regression test `CaWiringWrite.InlineOnlyPartPublishesWithoutBuildCrash` — RED without the
  fix (reproduces the EXACT crash: `staged entries … without a Build`), GREEN with; 575 Cas*/CaWiring*
  pass (minus the 3 known-pre-existing CaWiring fails). DISK note below still applies (77GB cores → `sudo rm`).
- **Observed (pre-fix):** during the CA-s3 stateless suite (CA-s3 as the DEFAULT MergeTree policy), a background merge
  crashed the server: `<Fatal> Logical error: 'ContentAddressedTransaction: staged entries for
  stateless-ca-s3/store/2c2/<uuid>@cas@/tmp_merge_all_1_1_1 without a Build'`. Under
  abort_on_logical_error this aborts the server → SIGABRT → 57GB core (/var/lib/apport/coredump/, 12:41)
  → apport. Run result 5119 OK / 38 FAIL (many post-crash Connection-refused + the usual stderr-warning
  artifacts). ONE distinct crash type.
- **Throw site:** `ContentAddressedTransaction.cpp:222` in `publishStaging` — invariant "staged entries
  (`st.entries` non-empty) must have an associated `st.build`". A `tmp_merge_*` staging reached commit
  with entries but `st.build == nullptr`.
- **Analysis:** the invariant throw is PRE-EXISTING (part-folder-cache Phase 2 only changed the
  mutable-only branch to the facade, not the build-check). `createHardLink` DOES call `buildFor` (creates
  the Build). Suspected gap: the tmp→final move/rename build-transfer (ContentAddressedTransaction.cpp
  ~913-933) or a merge shape where entries are staged without buildFor. The 4h CA soak (real merges on
  CA-rustfs) did NOT crash and Cas* gtests (CasBuild/CasProtocolScenarios) pass — so this is a specific
  merge/staging edge case surfaced only by the CA-s3-as-default full suite (arbitrary tables incl. fuzzers
  e.g. 00746_sql_fuzzy.sh active nearby). REGRESSION-vs-PRE-EXISTING: UNDETERMINED — the CA-s3-default job
  has never been green (experimental); needs a baseline check (revert part-folder-cache src → rebuild →
  reproduce) to confirm whether Phase 1-3 introduced it. Older cores exist (23:51, 01:10) — possibly the
  same crash pre-dating parts of the work.
- **Proposed action:** (1) identify the exact reproducing test/merge shape (table <uuid>=2c2ea48b);
  (2) determine regression via baseline; (3) fix the staging→Build lifecycle so a merge can never reach
  publishStaging with entries and no Build (either ensure buildFor on every entry-staging path incl. the
  move/rename transfer, or make the mutable/hardlink-only path not leave dangling entries).
- **DISK:** 3 root-owned cores in /var/lib/apport/coredump (~77GB: 57G@12:41 + 9.7G@01:10 + 9.8G@23:51).
  No passwordless sudo — user must `sudo rm /var/lib/apport/coredump/core.*` and consider disabling core
  dumps (`sudo sysctl -w kernel.core_pattern=core` or stop apport) before re-running crash-prone suites.

## STATELESS-CA-S3-triage-2026-07-09: CA-s3-default full-suite rerun (post crash-fix) — 8424 OK / 45 distinct FAIL

- **Logged (UTC):** 2026-07-09  **Severity:** finding (triage summary).
- **Run:** `Stateless tests (arm_binary, content_addressed s3 storage, parallel)` on the crash-fix binary
  (f7539af045c). ZERO crashes / ZERO new cores (the CRASH-CA-S3 fix validated end-to-end; the pre-fix run
  crashed ~2000 tests in). 45 distinct FAILs, in TWO already-understood classes — NO new real CA regression:
  - **17 CA-test FAILs** — the CA-over-LOCAL stderr-warning artifact (CA tests define inline LOCAL disks →
    EmulatedSingleProcess `<Warning>` on stderr → the stateless harness fails any test with stderr; stdout
    matches reference, logic OK) + 04286 EISDIR (STATELESS-04286) + 05009 log-enabled (STATELESS-05009).
    → BACKLOG (suppress/downgrade the CA-over-LOCAL warning or allowlist it so it doesn't fail the check).
  - **28 non-CA FAILs** — CA-s3-as-DEFAULT baseline noise: tests not designed for CA as the global default
    policy (clickhouse-local / local-page-cache: 01146, 02841, 03456, 03536, 03793, 04039, 04266; fuzz/
    format-inference/dynamic: 03233, 03274, 03466, 04094, 03927; storage-assumption/misc: 00152, 01271,
    01516, 01854, 02224, 02479, 02784, 02878/02879, 02885, 02931, 03203, 03370, 03649, 03679). The
    CA-s3-default job is experimental and has never been green; these are not CA-write-path bugs.
- **Proposed action:** the CA-warning suppression (class 1) is the one worth fixing for CI signal; class 2
  is expected job baseline (a per-job skiplist or accept-as-noise). Not blockers.

## STATELESS-CA-S3-attribution-2026-07-09: baseline-proven — 7/38 fails are CA-caused, 31 are env

- **Logged (UTC):** 2026-07-09  **Method:** ran the 38 CA-s3 FAILs on the NORMAL (non-CA) stateless job.
  31 fail on NORMAL too → NOT CA-caused (local env: clickhouse-local persistence, no mysql, s2-geo
  precision, reference drift, loaded-box timeouts; incl. 04278-class already-fixed + 04286/05008/05009
  which are env/harness, not CA-s3-specific). 7 fail ONLY under CA-s3 = genuinely CA-attributable:
  - **01156_pcg_deserialization, 01710_projection_detach_part, 02346_exclude_materialize_skip_indexes**:
    `promote: blob <h> condemned at commit revalidation — failing closed (INV-1)` (Code 236 ABORTED). The
    resurrect-vs-GC race — a fresh INSERT dedups to a blob GC is concurrently condemning. Correct
    fail-closed; design expects a RETRY (soak retries + passes), but stateless tests don't retry and
    gc_interval_sec=5 (test config) makes it frequent. **REAL production robustness gap.**
    FIX (proper cycle): promote should resurrect-on-condemn (re-upload the condemned adopted blob) or
    retry the transient internally, so an ordinary INSERT isn't ABORTED by a GC race. Relates to
    [[project_resurrect_reupload_orphan]], [[feedback_ca_resurrect_invariant]].
  - **03582_pr_read_in_order_hits, 03800_autopr_reuse_index_analysis**: Timeout — parallel-replica tests,
    CA-s3 (remote pool) slower. FIX: raise per-test timeout under CA-s3 or investigate the slowness.
  - **00933_ttl_simple**: TTL result-diff (investigate the diff — real CA-s3 TTL behavior vs benign order).
  - **03829_insert_deduplication_info_memory**: Code 241 MEMORY_LIMIT — CA write-path memory > the test's
    bound. FIX: investigate the write-path memory (spill/hash) vs the test assertion.
- NOTE: literal 0 on the FULL CA-s3-DEFAULT suite ALSO needs handling the 31 env fails (local-env fixes or
  standard no-* storage-modifier tags for tests that don't test CA, e.g. the clickhouse-local set that
  shares the CA pool) — those are not CA-write-path bugs.

## PROMOTE-CONDEMN-TOKENLESS-2026-07-09: DETACH/freeze adopt path hits the same condemn race (not fixed by the INSERT fix)

- **Logged (UTC):** 2026-07-09. Surfaced in the full CA-s3 lane AFTER the tokened-INSERT resurrect fix
  (spec/plan 2026-07-09-cas-promote-resurrect-tokened-blob) landed + validated (01156/01710/02346 now OK
  under load).
- **Symptom:** `03283_optimize_on_insert_level` FAIL — `ALTER TABLE ... DETACH PARTITION` →
  `StorageMergeTree::dropPartition` → `makeCloneInDetached` → freeze → CAS commit → promote →
  `promote: blob <h> condemned at commit revalidation — failing closed (INV-1)` (Code 236 ABORTED,
  CasBuild.cpp:931, the `!src` tokenless backstop).
- **Root:** the clone-into-detached path references source blobs via tokenless `adoptEvidence` (no putBlob,
  no retained source), so the tokened resurrect (uploadFromSource) does NOT apply. The existing
  copy-forward pre-pass (copyForwardFromCondemned, the committed-source INV-1 exception) is SINGLE-SHOT and
  runs BEFORE the CAS closure (deliberately — GET+PUT must not run inside a retried mutateShard closure).
  The race: pre-pass copy-forwards the condemned tokenless blob → GC re-condemns the fresh incarnation →
  in-closure revalidation sees it condemned → backstop ABORTED. Architecturally flagged "rare,
  retryable-by-caller"; soak retries and passes, stateless (no retry) surfaces it.
- **NOT a regression:** old promote aborted on EVERY condemned leaf (no resurrect at all); the INSERT fix
  only added tokened resurrect — tokenless leaves abort byte-for-byte as before.
- **FIX (separate careful cycle):** make the copy-forward pre-pass a BOUNDED loop (copy-forward → re-HEAD →
  repeat until not-condemned or bound), keeping GET+PUT OUTSIDE the CAS closure — the tokenless analogue of
  the tokened bounded resurrect loop. Needs its own design + fresh-model consult + TLA+ gate (the copyForward
  reads the condemned-but-present object; verify liveness/no-dangle under bounded retry). Relates to
  [[project_ca_p31_mount_fence_recovery]] (S13 copy-forward origin), [[project_resurrect_reupload_orphan]].

## PROMOTE-REVALIDATION-MINIMIZATION-2026-07-09: the ack-floor model licenses skipping per-leaf HEADs (design pass)

- **Logged (UTC):** 2026-07-09, from a protocol review with the user ("writer acts only per its announced
  view; the floor + two-phase delete + spare protect everything newer — are the writer-side re-checks
  needed at all?"). Verified against code:
  - announce = installed-view round (`observedGcRound`, CasStore.h:288); `view_gate` drain makes it honest
    (no in-flight `mutateShard` gates on an older view, CasStore.h:622-626);
  - graduation needs `condemn_round < min_ack` (min over live + expired-unfenced, CasGc.cpp:190/341/1534);
  - two-phase delete re-judges in-degree at the delete pass (`d>0 → spared`, "recovery wins even past the
    floor", CasBlobInDegree.h:87);
  - `publishStaging` order (body → precommitAdd → putBlob → promote) makes the protecting inline-closure
    edge journal-durable BEFORE any blob adoption.
- **What stays load-bearing:** (a) the LOCAL-view publish gate (putBlob/promote condemned checks) — the
  writer's half of the ack contract (never reference a token you ACKed as condemned); costs no backend IO;
  (b) fenced/expired-writer defenses (requireAlive/epoch + promote gate backstop, P3.1) + the birth-floor
  refresh (THM-NO-RETURN create-ordering); (c) the absent-HEAD as fail-closed insurance — the
  clamp-suppression incident (CasBlobInDegree.h:91, 31 dangles live 2026-07-03) shows "fold always sees the
  durable edge" broke once in the real world.
- **Candidate simplification (needs its own design cycle):** skip the per-leaf promote HEADs when the
  installed view round is UNCHANGED since the dep was observed (`DepEntry::observed_view_round` exists for
  exactly this W-REVALIDATE bookkeeping). Soundness sketch: any token a delete pass can kill was
  delete_pending in the PREVIOUS state == condemned-visible in the writer's current view == caught at
  putBlob. Unchanged round ⟹ observed-live tokens cannot have been deleted. Would make promote O(0)
  backend calls for revalidation in the common case. Edges to design: newborn/birth-floor shards,
  out-of-band deletion, the clamp-suppression class (maybe keep HEADs under a "paranoid" setting).
- **Also noted:** accept-condemned-when-owner-live (instead of resurrect) is NEARLY provable under the
  model (min_ack passing the condemn round requires the writer's own ack, by which time its durable edge
  is folded → spared, so graduation can't fire) — but it rides entirely on (c); fail-closed keeps the
  landed resurrect/copy-forward (cost = one rare PUT).

## PROMOTE-REVALIDATION-MINIMIZATION variant B (user-proposed, 2026-07-09): per-commit pinned-round ack

- **Proposal (user):** the beat advertises `min(installed_round, min over active builds' pinned_round)`,
  where `pinned_round` is latched at `startBuild` (before the first observation) and released at build end
  — the exact pattern `min_active` already applies to build_seq on the SAME merged beat
  (CasStore.h:294-303). Then the ack floor can never pass any live commit's knowledge → the
  adopt→precommitAdd window (the one theoretical counterexample requiring the promote blob gate for a
  live writer) closes BY CONSTRUCTION.
- **Consequences:** promote's per-leaf HEAD revalidation becomes theoretically redundant → drop it
  (owner-check + pure owner-move only; O(0) backend calls) — supersedes the variant-A HEAD-skip. The
  landed resurrect/copy-forward machinery stays as the insurance path behind a "paranoid revalidation"
  setting (still catches clamp-suppression-class accounting bugs — 31 live dangles once — and any
  out-of-band deletion). Fence stays covered by the existing local write-fence deadline
  (CasStore.h:305-307) + mount-epoch check (CasBuild.cpp:125-127), independent of the blob gate.
- **Cost (liveness):** a wedged-but-alive commit (S3 retry storm; beat healthy so no fence) holds the
  POOL-WIDE graduation floor at its pinned round — condemns accumulate, retired lists grow until the
  commit finishes. Needs a per-commit pin deadline (natural hook: the same write-fence monotonic
  deadline; a commit outliving it drops its pin and must take the paranoid revalidation path).
- **Safety notes:** advertising less than installed is fail-closed (view_gate invariant holds a
  fortiori); the local view keeps advancing (syncer untouched) so putBlob only gets stricter; audit ALL
  consumers of observed_gc_round before wiring (floor R1 + srid_acks observability today).
- **TLA+:** extend the CaBuildWatermark.tla min-over-active-builds pattern to the round ack; sabotage
  config = no-pin (must reproduce the adopt-window dangle), positive = pin + no blob gate holds
  INV_NO_DANGLE/INV_NO_LOSS.
- Full design cycle required (spec → consult → TLA+ gate → impl). Complements, then retires, variant A.

## WRITER-GC-SIMPLIFICATION-2026-07-09: Tier 2 spec'd; variants A+B SUPERSEDED; Tier 3 recorded

- **Spec:** docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md (user-driven, approved).
  Core: EDGE-BEFORE-OBSERVE — publishStaging's order (body → precommitAdd → putBlob → promote) makes the
  precommit-closure edge durable BEFORE any observation; fold-activation + d>0→spared + two-phase d-recheck
  make deletion of closure-named hashes impossible. The promote freshness machinery is redundant
  defense-in-depth.
- **SUPERSEDED by that spec:** PROMOTE-REVALIDATION-MINIMIZATION variant A (HEAD-skip-on-unchanged-round)
  and variant B (per-commit pinned-round ack) above — the pin is strictly weaker than the durable edge (a
  pin bounds the floor; the edge makes deletion impossible regardless of the floor).
- **Tier 2 (the spec):** delete promote tokened revalidation + retained_sources + copy-forward pre-pass +
  view_gate drain + writer fence_round-refresh (TLA+-conditional) + dead observed_view_round; keep putBlob
  gate, owner-check, 1 HEAD/tokenless leaf + copy-forward backstop, lease/fence/epoch, syncer (no drain).
  No paranoid mode (clamp-suppression hit COMMITTED manifests — the promote gate never covered that class;
  fsck/soak/content_addressed_log remain the detection net). TLA+ phase-0: sabotage-flip redundancy proofs
  + a NEW must-stay-red order sabotage (adopt-before-precommit).
- **Tier 3 (FUTURE, after Tier 2 + one clean soak):** ack-floor graduation gating (condemn_round < min_ack —
  likely redundant given two-phase d-recheck + EDGE-BEFORE-OBSERVE), beat round-ack + syncer as its feeder,
  tokenless condemned-arm → accept (weakens the modeled ~CondemnedAtView publish gate — model change),
  failure-texture review (stale views ⇒ more loud impossible-spares). Each via its own sabotage-flip
  demolition.

- **CORRECTION to the Tier-3 item above (2026-07-09, K1 race analysis):** the ack-floor graduation gating is
  NOT "likely redundant". Post-Tier-2 it is the DELIVERY GUARANTEE of the dedup-safety list: graduation
  requires min_ack > condemn_round ⇒ every live writer's installed view covers every graduated entry ⇒
  putBlob's condemned check (K1) can always see a present-but-doomed token before adopting it. Without the
  floor (or without the check) there is a concrete dangle interleaving: entry delete_pending pre-precommit →
  pass seals before our precommitAdd → fold misses our edge → deleteExact executes after our HEAD adopted
  the token → no promote HEADs (Tier 2) → committed dangle. Exact-token displacement makes check+floor
  race-safe in both directions. Tier 3 floor removal = replace dedup-adoption safety wholesale
  (adopt-displace / no-adoption), not a deletion.

- **UPDATE 2026-07-09 (combined spec):** the WRITER-GC-SIMPLIFICATION spec was REWRITTEN as the combined
  two-phase design after further user-driven analysis: Phase A = the consulted Tier-2 deletions (findings
  A-G folded); Phase B = per-hash META-DESCRIPTOR (`blobs/xx/<hash>.meta` = {incarnation, condemned},
  INV-META-BODY "meta ⇒ body", create bottom-up / delete top-down, birth-completion + claim-first debris
  sweep) which deletes the writer-side RetireView + syncer + observed_gc_round/ack-floor gating entirely —
  the former Tier-3 arrives via point-read freshness instead of list delivery. Budget: ≈ +2 tiny PUTs per
  blob lifetime (+$10/M blobs AWS, ~0 RustFS); mass-DROP requires a parallel GC meta-op pool. Lazy-marker
  alternative REJECTED (saves 1 birth PUT, costs 2-call adopt + keeps body-token linearization).

## CA-ASAN-SUITE-2026-07-09: negative-LOGICAL_ERROR tests abort under abort-on-logical-error builds

- **Logged (UTC):** 2026-07-10, from the first ASan sweep of the CA gtest suite (build_asan was stale;
  run as the Phase-A M3 chassert gate — which came back CLEAN: 130/137 suites green, zero chassert trips).
- **Class:** CA product code uses fail-closed `LOGICAL_ERROR` for protocol violations (mount-lease foreign
  touch/hold/release, RunFile key ordering, ShardQueue partial-edit validation, moveDirectory collision,
  B122 injected publish failure, CasFormat undefined magic). The NEGATIVE tests of those paths abort the
  whole binary under abort-on-logical-error (ASan/debug) builds. 13 tests identified individually +
  5 whole-suite aborters: CasMountLease, CasMountStartup, CasRunFile, CasShardQueue, CasStoreRemount.
- **FIX (dedicated pass):** either guard those negative tests with a runtime check for
  abort-on-logical-error builds (GTEST_SKIP), or change deliberate-injection sites (B122) to a
  non-LOGICAL_ERROR code, or run them via EXPECT_DEATH. Until then ASan sweeps must exclude them (the
  exclusion list lives in the Phase-A worklog entry).
- **Fixed alongside (real bugs):** the event-sink stack-use-after-scope in 10 test sites (sink outlived
  its captured vector; syncer emits until the Store dtor). Production sink verified immune.

## GC-WEDGE-REMOVAL-FOLD-2026-07-10: a routine early DROP wedged ALL pool collection for the entire 4h soak (P1)

- **Logged (UTC):** 2026-07-10, on the Phase-A exit stand (seed 991, STILL UP — live repro preserved).
- **Symptom:** `gc_fold_clamp` reason `owner-removal: edge-bearing committed body missing at removal-fold`
  firing for the SAME 63 manifests (ONE dropped table's namespace,
  `ca_soak_ch1/store/b37/b3781ce3-...@cas@`, ~40 distinct shards) on EVERY fold pass from t+540s of the
  run to now — 56.8k clamp events over 4.6h. Effect: fail-closed clamp ⇒ **zero graduations, zero
  deletes pool-wide, forever** (ch2 leader ran 800+ Success rounds with objects_deleted=0); pool ballooned
  to 112GB / 764k unreachable; the in-run B146/B154 fsck timeouts are a downstream effect (O(pool) scans
  never shrink). Integrity held throughout (oracle + final dangling=0) — this is a LIVENESS wedge, not
  corruption.
- **Shape:** 63 owner-REMOVAL events whose committed manifest BODIES are already absent at removal-fold.
  The fold needs the body to compute the -1 edge deltas ⇒ clamps. Bodies should not be deletable before
  their removal folds — candidate causes to investigate: (a) the Phase-1d/S30 orphan-manifest sweep
  racing the drop (misjudging committed-pending-removal manifests as orphans); (b) `dropNamespace`
  wholesale ordering; (c) a chaos KILL mid-drop half-state. NOT Phase-A (fold/removal untouched by it);
  clamps began during plain steady-stage workload.
- **Forensics:** utils/ca-soak/scenarios/gc_wedge_forensics_20260710.txt (all 63 refs + shards +
  resolved_through cursors); the stand holds full state (journals, gc/state, content_addressed_log).
- **FIX DIRECTION (own cycle, spec-level):** the clamp is correct fail-closed for a TRANSIENT missing
  body, but a PERMANENTLY missing body needs a recovery rule — e.g. a removal-fold tombstone path:
  if the body is absent AND the manifest is provably beyond its lifecycle (no live owner binding),
  resolve the removal with edge deltas from the GC snap's recorded closure (the B199-S2 inline closure
  exists for precommits; committed manifests' edges are IN the snap already — the fold could subtract
  the snap-recorded edges instead of re-reading the body). Needs TLA+ (extend the fold-barrier model).
  **PRIORITY: above Phase B** — every pool that ever hits this shape stops collecting garbage forever.

- **ROOT CAUSE (2026-07-10, live-stand forensics) for GC-WEDGE-REMOVAL-FOLD:** the trigger was the
  session's OWN quarry — a 63-part INSERT commit hit the retryable promote-condemn ABORTED mid-publish
  (the soak's single "ABORTED-retried INSERT"), the B122 compensating rollback dropRef'd the
  already-published refs, and the commit retry RE-PUBLISHED THE SAME ManifestIds (re-precommit+promote
  of the same PartStaging). The GC fold then processed [rollback-removal, re-precommit(+1), promote] per
  part in one pass: `mf_cleanup` is populated by the removal-half and NOTHING erases a manifest that a
  later same-pass event re-owns → R6 post-CAS exact-token-deleted the bodies of 63 LIVE committed parts
  (event-log proof: root_add x20 @20:24:47, root_remove x20 + root_add x20 @20:24:49, manifest_delete
  @20:24:51, ref_drop @20:33:19, clamps from 20:34). TRANSIENT DANGLE window 20:24:51-20:33 (live refs
  over deleted bodies). The table's later DROP appended the final removals whose bodies are gone →
  permanent pool-wide clamp. THREE-PART FIX CANDIDATE: (W) writer — a rolled-back PartStaging must not
  be re-published under the same ManifestId (fresh ids on retry, NoManifestIdReuse) or publishStaging
  made re-drive-idempotent without the drop+re-add shape; (F) fold — mf_cleanup symmetric maintenance
  (erase on +1 re-own; R6 skips still-owned manifests) — closes the same-pass shape; (R) recovery —
  removal-fold with missing committed body resolves its -1 set from the SNAP's source edges
  (sourceEdgeId(id,path) is already recorded per blob edge; integrate into the three-cursor merge) —
  unwedges existing pools and removes the clamp class. Cross-pass note: with (W)+(F), a cross-pass
  rollback/re-own can still ABORT the writer cleanly (fail-closed, no wedge) — acceptable.

- **ROOT CAUSE CORRECTED (2026-07-10, namespace-strict re-verification — SUPERSEDES the rollback/mf_cleanup
  story above, which was built on a cross-namespace `manifest_ref_instance` string collision):** the real
  mechanism is a missing symmetry in the ORPHAN-MANIFEST SWEEP. Evidence: manifest 1:308:1 (ns b3781ce3)
  has ONE promote (20:24:45), a NORMAL table-DROP ref_drop (20:33:19), NO ns-scoped manifest_delete, NO
  build_abort, NO re-publish — yet its body `/1/308/000001.proto` is GONE and the removal-fold clamps from
  20:34:12. The deleter is `sweepManifestCursorPage` (CasOrphanManifestSweep.cpp — deletes bodies via
  `deleteExact` and emits NO event). It deletes a body iff `prefixEligible` (build_seq < min_active — TRUE
  for ANY promoted build: promote calls retireBuildSeq, so a committed manifest's build is watermark-dead)
  AND the key is not in `activeManifestKeys(ns)`. `activeManifestKeys` protects committed owners via
  `root.refs` and PENDING-PRECOMMIT removals (removal above the sealed fold cursor) — but has **NO branch
  for PENDING-COMMITTED removals**. So in the window between `dropRef` (committed-removal appended, key
  leaves root.refs) and the fold sealing that `-1` (delete-after-sealed-decrements), the sweep sees the
  body eligible+inactive and deletes it → the removal-fold then finds the committed body missing → clamps
  forever → pool-wide GC stop. NOT Phase-A (sweep predates it); requires a promoted-then-idle build + a
  DROP + sweep-timing — hit by the soak's heavy DROP/chaos, missed by gc_shards=1 short unit runs.
- **FIX (surgical, one symmetry):** in `activeManifestKeys` extend the pending-removal protection to ALL
  owner kinds — a removal event whose `transition_version > sealedFoldCursor` keeps its `old_binding`'s
  manifest body active regardless of `owner_kind` (drop the `== OwnerKind::Precommit` condition). The fold
  reads that body to emit the `-1` next round; the sweep must not delete it until the `-1` is sealed. Then:
  gtest [publish committed, drop, run sweep BEFORE the fold, assert body survives; fold; assert -1 sealed,
  then sweep deletes]; TLA+ obligation (delete-after-sealed-decrements over committed removals). Unwedge of
  the live/existing pools: `SYSTEM CONTENT ADDRESSED GC REBUILD` (rebuildBaseline) OR the fold recovery
  variant (R) — still worth evaluating so an already-orphaned committed body doesn't wedge forever.

- **ROOT CAUSE LOCKED (2026-07-10, decisive disambiguation):** confirmed THEORY 2 (orphan sweep), refuted
  the fresh-model consult's R6/NoManifestIdReuse reframe. Decisive evidence for 1:308:1 (ns b3781ce3):
  shard-20 journal has EXACTLY ONE binding event — the removal at transition_version 1487, ZERO new_binding
  re-owns (R6-reuse mechanism impossible); the fold cursor is stuck at 1486 (one BEFORE v1487) so the fold
  never reached the removal → R6 never enqueued it → and indeed ZERO ns-scoped manifest_delete for it (the
  1468 deletes at 20:33:32 are OTHER dropped manifests). The stand runs the CURRENT Phase-A binary (compose
  mounts ../../build/programs/clickhouse), not an older vintage. Only no-event manifest-body deleter with
  the demonstrated gap = the orphan sweep (deletePrefixWholesale=gen prefix, reclaimDroppedShards=ref-shard
  objects — neither touches manifest bodies). FIX = activeManifestKeys pending-committed-removal protection
  (drop the owner_kind==Precommit condition on the transition_version>cursor branch).
- **Consult's surviving findings (folded):** (R) recovery-from-snap REJECTED — source_id = one-way
  CityHash128(ns,ref,path), a missing body's edges can't be enumerated. Existing-pool UNWEDGE = SYSTEM
  CONTENT ADDRESSED GC REBUILD FORCE (rebuildBaseline reconstructs from root.refs, refuses only on a LIVE
  ref naming a missing body — dropped table's refs are gone → no refusal; FORCE needed on a healthy pool).
  R6 owner-recheck (F1) + erase-on-+1 (F2) are a REAL latent hardening (R6 deletes mf_cleanup with no
  owner re-check, relying on NoManifestIdReuse) — backlog as defense-in-depth, NOT this bug.

## INTROSPECTION-3-2026-07-10: orphan sweep deletes manifest bodies with NO audit event (diagnosis blocker)
- The orphan manifest sweep (`sweepManifestCursorPage`, CasOrphanManifestSweep.cpp) `deleteExact`s manifest
  bodies and emits NOTHING to system.content_addressed_log. This directly blocked the GC-wedge diagnosis:
  the path that deleted a LIVE committed body left zero trace; the deleter had to be inferred by elimination
  + code reading + pool `find`. INVARIANT: every manifest-body deletion must be audited. FIX: emit a
  `manifest_sweep_delete` (or reuse ManifestDelete with a distinct reason) per sweep deletion carrying
  {ns, manifest_id, eligibility, cursor}. With it, this diagnosis is a one-line SQL query.
- Relatedly (caused the FIRST wrong root cause): manifest `object_hash`/`manifest_ref_instance` in the event
  log is NOT namespace-qualified — "1:308:1" collides across tables. FIX: qualify with namespace (or add a
  manifest_id column). Both extend the INTROSPECTION-1/2 audit-completeness line.

- **FIX LANDED (c1485f52a29):** activeManifestKeys now protects pending removals of BOTH owner kinds
  (dropped the Precommit-only condition) + the orphan sweep emits a ManifestDelete audit event per
  deletion (INTROSPECTION-3). RED-proof: reverting the one condition fails the new regression test
  `CasOrphanManifestSweep.PendingCommittedRemovalBodyIsSkipped` (body deleted); GREEN with the fix.
  Full Ca*/Cas* sweep clean (only the 2 known flakes). Fresh-model consult CONFIRMED root cause + fix
  complete + no second no-event deleter + REBUILD FORCE as the unwedge.
- **TLA+ GATE — BACKLOGGED (attempted, backed out clean):** the model-faithful fix is adding
  `~HasUnfoldedRemoval(m)` to GOrphanSweep's honest guard + an invariant. BUT the invariant must scope to
  COMMITTED-owner removals only — the code clamps solely on committed removals ("a removed precommit whose
  body is absent emitted no edges — nothing to mirror"), so `HasUnfoldedRemoval => mBody` (consult) AND
  `(HasUnfoldedRemoval /\ everEdged) => mBody` (my refinement) BOTH over-catch honest missing-body /
  abandoned-precommit removals (verified: stage4 counterexample = WStageManifest→WPrecommitAdd→
  WAbandonPrecommit under EnableMissingBody=TRUE). Correct scoping needs the journal event to carry the
  removed owner_kind (Committed vs Precommit) — the model's event `.old` is a manifest-id set that drops
  it. Task: add committed-vs-precommit to the journal event (or a `HasUnfoldedCommittedRemoval` predicate),
  then invariant `HasUnfoldedCommittedRemoval(m) => mBody[m]` + a dedicated `SabotageSweepUnfoldedRemoval`
  constant (propagate FALSE to all ~47 cfgs, one negative cfg TRUE). A focused model session, not tail-work.
- **REMAINING VALIDATION:** (a) rebuild clickhouse with the fix + remount + re-run the 4h exit soak — a
  NEW wedge must NOT form (the real end-to-end gate; the stand that repro'd ran the pre-fix binary at soak
  launch); (b) unwedge/validate REBUILD FORCE on the live stand (after its dropped-table refs settle).

## STATUS ROLLUP 2026-07-10 (session close-out)
FIXED + validated this session (entries above kept for history):
- CRASH-CA-S3-staged-entries-without-Build — FIXED (inline-path buildFor).
- STATELESS-CA-S3 condemn-race (01156/01710/02346, tokened INSERT) — FIXED (promote resurrect-on-condemn),
  green under the full lane.
- PROMOTE-CONDEMN-TOKENLESS (03283, DETACH/freeze) — FIXED (in-closure copy-forward backstop), green.
- GC-WEDGE-REMOVAL-FOLD (P1) — FIXED (orphan sweep pending-committed-removal protection) + INTROSPECTION-3
  part 1 (sweep emits a ManifestDelete audit event). Validated: RED/GREEN gtest + consult + verify-soak
  (0 wedge clamps, GC reclaims) + clean fsck (dangling=0, unreachable=0).
- WRITER-GC-SIMPLIFICATION Phase A — DONE + validated (all gates, dangling=0). Phase B Gate B (raw-body +
  three-state meta) GREEN; writing-plans + impl remain.
- SOAK-TTL-BAND / SOAK-FREEZE_LONG / SOAK-REAPER — FIXED earlier this session.

STILL OPEN (debt):
- TLA+ wedge regression gate — needs committed-removal scoping / meta-model (fold into Phase-B Gate B).
- CA-ASAN-SUITE — negative-LOGICAL_ERROR tests abort under abort-on-logical-error builds (GTEST_SKIP guard).
- INTROSPECTION-3 part 2 — namespace-qualify manifest object_hash in the event log (collision footgun).
- INTROSPECTION-2 — verify done (ca-inspect exists + used); confirm/close.
- 4 non-correctness CA-s3 stateless fails: 03582/03800 (parallel-replica timeouts), 00933 (TTL timing),
  03829 (write-path memory). + ~31 local-env fails (not CA).
- PROMOTE-OVER-COMMITTED-LEAK / ABANDON-RETIRE-ORDERING (2026-07-08, prior-session audit) — status unverified.
- Gate B follow-ups: resurrect-skip-CAS + delete-meta-before-body need finer interleaving.
- Phase B implementation (writing-plans → subagent impl).

## TRIAGE SWEEP 2026-07-11 (post-campaign: RIS + deposed-leader add-only + S3-staging + pluggable-hash P1)

Campaign landed on `cas-gc-rebuild` and was validated end-to-end (soak `dangling=0`; scenario
regression set S30x2/S25/S34/S15/S33 all PASS at `eceacc2ad1d`, **0 real regressions** — see
`RUN_HISTORY.md` and the task-4 report). Status of the OPEN classes after this campaign:

- **NO new scenario regressions** from: retired-in-snapshot GC refactor (T1-T8), add-only deposed-leader
  meta fix, opt-in S3-native staging (OFF by default), pluggable-blob-hash Phase 1 (default cityHash128,
  blob path now `blobs/ch128/<shard>/<hex>` — the fsck/GC observe oracles are segment-tolerant, verified).
- **STILL OPEN (unchanged by this campaign — each a focused future effort):**
  - `NEEDS-INFRA-S12` (10-replica shared pool — template exists, see `reference_compose_multinode_template`),
    `NEEDS-INFRA-S22` (fault-injecting S3 proxy: 503/429/slow/close), `NEEDS-INFRA-S27` (instrumented store
    returning duplicate objects). Per the no-skip rule these want a per-scenario compose + N-node driver.
  - Release-gate items (ROADMAP §release-gates-2026-07-03): the three ack-floor cards (SIGSTOP floor hold,
    kill-mid-burst fence-out, request-budget guard); `B206` settle-gate tuning; `B207` fsck phantom-dangling
    race; `B3/B186` `FreezeViaHardLinksIntoShadow` red gtest (the one standing CA-battery red).
  - `S07` manifest-cap + `S01` memory-attribution: need `--scale ci/full` for a hard verdict (dev-scale gaps;
    the real memory-materialization bug is already RESOLVED via streaming `putBlob`).
- **PHASE-2 follow-on** (new, from this campaign): pluggable-hash `sha256` via a variable-length digest
  (spec `docs/superpowers/specs/2026-07-11-cas-pluggable-blob-hash-design.md` §7) — the big settlement/GC
  refactor; its own brainstorm→plan→TLA/soak.

## NEEDS-INFRA-S12 — RESOLVED 2026-07-11 (label was stale; S12 runs green on 10 replicas at HEAD)
The multi-node abstraction was completed between the 2026-07-03 "NOT RUN" note and 2026-07-07 (S12 ran
07-07). The S12 card carries `compose_variant="tenreplicas"` (no `needs_infra`); `cluster_boot` maps it to
`docker-compose-10replicas.yml` (`node_count_for=10`) and `run.py` builds `Cluster(node_count=10)` — one
command runs it: `PYTHONPATH=$(pwd) python3 -m scenarios.run --scenario S12 --seed 20260711 --duration 480s`.
Re-confirmed GREEN on `cas-gc-rebuild` @ `ffc993a1d85` (post-campaign): 11/11 verdicts, all 10 replicas
byte-identical (count=10000, matching row-hash), CA dedup fired (`CasBlobBodyPutAvoided=40`), fsck
dangling=0/unreachable=0, forced-GC residual=0, no Failed GC rounds. (A store-dependent RustFS S3
read/write error rate ~14% is recorded as info, not a CA defect.) NEEDS-INFRA remaining: S22 (fault
S3 proxy), S27 (instrumented dup-object store).

## B3/B186 FreezeViaHardLinksIntoShadow red gtest — RESOLVED 2026-07-11 (commit ecb6e1a5e58)
Root cause: CA removal is tombstone + deferred GC; the intermediate-dir `existsDirectory`
(`shadow/<bk>`) used a raw object LIST (`listMirroredChildren`) that counted tombstoned-but-not-yet-GC'd
shard/manifest objects, so a just-`UNFREEZE`d backup dir stayed "existing" until a GC round ran. Fixed by
making the intermediate branch tombstone-aware (enumerate namespaces via `listNamespaces` + consult the
tombstone-aware `listRefs`, consistent with the part-level and table-uuid-pair branches). The CA gtest
battery is now fully green (669/669, no reds). Removes the standing CA-battery red / release-gate item.

## B207 fsck phantom-dangling race — RESOLVED 2026-07-11 (commit 94970514116)
`runFsck`'s ref-walk and HEAD-confirm were minutes apart with no snapshot, so a re-published/dropped ref
plus a legitimate GC delete of the old blob manufactured a false `dangling`. Fixed: `blob_labels` is now
always-populated (was `detail`-only), and at both HEAD-absent branches (global + scoped) the fsck
RE-RESOLVES every referencing ref FRESH — a HEAD-absent blob is `dangling` ONLY if a CURRENT ref still
names it; if every label re-resolved away (re-published/dropped), it is a stale-walk artifact, not a loss.
Tests: `PhantomDanglingFromRepublishedRefIsReresolvedAway`, `...FromDroppedRef...`, and (safety companion)
`RealDanglingStillCaughtAfterReresolve`. Unblocks honest release-validation soaks (B185/B206/B144 were this
race). NOTE (unrelated test hygiene): ~5 tests (`CasInstrumentedBackend`, `CasObservability`,
`CasStoreBackpressure`, `UniqueKeyIndexCache`) fail only under a broad combined `*Ca*` gtest filter but pass
in isolation — a pre-existing order-dependent-flake / shared-state issue, not a CA correctness bug.

## S01/S07 ci/full-scale attempt 2026-07-11 — S01 memory behavior CONFIRMED good; S07 cap NOT SQL-reachable (finding)
- **S01 (memory-materialization):** the dev-scale run already shows the resolved streaming behavior clearly —
  peak MemoryResident 0.52 GB for a 64 MiB blob, RSS growth only **51 MiB** (RSS does NOT scale with blob
  size), CasBlobPut=5, multipart used, replicas byte-identical, fsck dangling=0. The "inconclusive" is purely
  the dev-scale attribution threshold (blob 64 MiB < 128 MiB), NOT a defect — `Build::putBlob` streams (fix
  `S01-PUTBLOB-MEMORY-FIXED`). A clean ci/full run to formalize the "<blob size" verdict is still nice-to-have
  but the evidence is already conclusive that RSS is bounded.
- **S07 (manifest cap) — FINDING:** at `--scale full` the probe uses a 20000-column wide insert, which is
  prohibitively slow (>20 min on the single insert, did not complete) AND — per the card's own note — the
  manifest cap is "3+ orders of magnitude above dev SQL reach." Conclusion: **the manifest-cap fail-close is
  effectively NOT exercisable via a SQL scenario even at full scale.** It should be validated by a DIRECT /
  unit-level test (a synthetic manifest that exceeds the cap → assert fail-close), not by a scenario insert.
  Recommend converting S07 from a SQL-scale probe to a gtest of the cap, or accepting it as not-scenario-testable.
- PROCESS NOTE: a subagent's helper monitor autonomously armed a `docker compose down -v` on a disk threshold
  (security-flagged). Controller policy: disk-safety teardown of the EPHEMERAL ca-soak cluster is fine and
  mandated at >85%, but the controller (not a subagent's detached monitor) manages it; never `ci/tmp/rustfs`.

## S07 manifest-cap fail-close — now covered by gtest 2026-07-11 (commit 81b40ae0df2)
Acting on the finding that S07's manifest-cap fail-close is NOT SQL-scenario-reachable: added
`CasBuild.ManifestCapEncodedBytesOverThrowsBeforeBodyWrite` + `...JustUnderStagesSuccessfully`
(`gtest_cas_build.cpp`). The over-cap manifest is built by MEASURING the actual `encodePartManifest`
output (a Blob `ManifestEntry` with a long path walked to the exact byte boundary of the 256 MiB
`kMaxManifestEncodedBytes` cap) — not a hand-derived size. Asserts `stageManifest` throws BEFORE any body
write (fail-closed), and that a just-under manifest stages. The P0 cap is now validated at unit level.
Remaining sub-gap: the ordinal cap (`kMaxManifestOrdinal`=999,999) has no test-injection point and needs
~1e6 real calls — documented, not forced into a slow test. S07-as-a-SQL-scenario stays effectively
inconclusive-by-design; the gtest is the real coverage.

## Broad-*Ca* gtest "flake" — NOT a CAS defect (diagnosed 2026-07-11)
Re-diagnosed: the CAS observability/instrumented/backpressure tests PASS as a group
(`CasObservability.*:CasInstrumentedBackend.*:CasStoreBackpressure.*` → 16/16). They ARE properly isolated
— counters are read as before/after DELTAS and events via a per-test captured vector. The 5 failures appear
ONLY under a broader `*Ca*` gtest filter that interleaves them with a NON-CAS test (e.g. `UniqueKeyIndexCache`)
which leaves shared global state dirty. So this is a pre-existing GENERAL `unit_tests_dbms` cross-test-ordering
hygiene issue (a non-CAS test not resetting global state), NOT a CAS correctness bug and NOT caused by this
campaign. The CAS gtest battery is green. No CAS action; if pursued, it belongs in general test-harness hygiene
(the offending non-CAS test's SetUp/TearDown), out of the CAS scope.
## S01-20260713T164402-1: scenario raised: cluster did not become healthy after reset

- **Logged (UTC):** 2026-07-13T16:49:15
- **Severity:** suspected-bug
- **Run:** 20260713T164402_S01_seed42
- **Observed:** scenario raised: cluster did not become healthy after reset

## S07-20260713T170326-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-13T17:08:34
- **Severity:** suspected-bug
- **Run:** 20260713T170326_S07_seed42
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S13-20260713T172032-1: forced GC left 2 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manif

- **Logged (UTC):** 2026-07-13T17:27:15
- **Severity:** suspected-bug
- **Run:** 20260713T172032_S13_seed42
- **Observed:** forced GC left 2 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 2}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S13-20260713T172032-2: S13 residual unreachable=2 after forced GC; classified by prefix={}

- **Logged (UTC):** 2026-07-13T17:27:15
- **Severity:** suspected-bug
- **Run:** 20260713T172032_S13_seed42
- **Observed:** S13 residual unreachable=2 after forced GC; classified by prefix={}

## S31-20260713T174441-1: ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under

- **Logged (UTC):** 2026-07-13T17:45:15
- **Severity:** suspected-bug
- **Run:** 20260713T174441_S31_seed42
- **Observed:** ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 23 but GC reclaimed ~78 (checklist #9). previewDeletes should iterate all target shards, not just shard 0.


## S13-20260713T172032-3: GC-side backstop for stale live precommit bindings — open spec question

- **Logged (UTC):** 2026-07-13T18:30:00
- **Severity:** design-question
- **Run:** 20260713T172032_S13_seed42 (triage: `.superpowers/sdd/s13-triage-report.md`)
- **Observed:** The S13 DANGLING-PRECOMMIT regression was fixed writer-side (the stale-precommit sweep
  now retries until verified clean and emits `precommit_reclaim` audit events), per the spec's
  §Responsibility Boundary, which assigns precommit-binding cleanup to the WRITER — GC never mutates
  another writer's ref-table state, so a GC-side reclaim would be a new protocol capability
  (leader-side writes into a writer's table), not a bugfix, and is deliberately OUT of the S13 fix
  commit. The residual exposure is a writer that dies again (or stays wedged) before ever completing a
  verified-clean sweep on any later mount: its stale precommit bindings keep protecting their manifests
  from the orphan-manifest sweep (`activeManifestKeys`, control #8) with no second line of defense.
  Two follow-up options to spec out: (a) a GC-side VISIBILITY counter — "live precommit binding with
  `writer_epoch` < mount-lease epoch" — surfaced per round in the GC log / `system.content_addressed_mounts`
  so a stuck reclaim is observable from the leader even when the writer never retries (cheap, no
  protocol change; the triage's Q4 recommendation 3); and (b) an actual GC-side reclaim of such
  bindings (requires a spec amendment to the responsibility boundary + fencing story for GC-authored
  ref-log transactions). Do (a) first; (b) only with a spec revision.
## S07-20260713T181937-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-13T18:20:34
- **Severity:** suspected-bug
- **Run:** 20260713T181937_S07_seed44
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S07-20260713T183115-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-13T18:32:12
- **Severity:** finding
- **Run:** 20260713T183115_S07_seed45
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S07-20260713T185131-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-13T19:00:12
- **Severity:** finding
- **Run:** 20260713T185131_S07_seed46
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S03-20260713T200548-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: stageM

- **Logged (UTC):** 2026-07-13T20:09:08
- **Severity:** suspected-bug
- **Run:** 20260713T200548_S03_seed46
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/89c/89ccd094-87f6-4932-94d5-cbdeef131f2a@cas@/0000000000000001-00000000000009b4/000001.proto' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId. (ABORTED) (version 26.6.1.1) | sql=INSERT INTO s03_live SELECT 12550000 + number AS id, randomString(512) AS payload FROM numbers(50000)

## S38-20260714T115429-1: quiescence failed: timed out

- **Logged (UTC):** 2026-07-14T12:20:57
- **Severity:** suspected-bug
- **Run:** 20260714T115429_S38_seed42
- **Observed:** quiescence failed: timed out

## S38-20260714T115429-2: forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-14T12:20:57
- **Severity:** suspected-bug
- **Run:** 20260714T115429_S38_seed42
- **Observed:** forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 20}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S38-20260714T131226-1: quiescence failed: timed out

- **Logged (UTC):** 2026-07-14T13:41:38
- **Severity:** suspected-bug
- **Run:** 20260714T131226_S38_seed42
- **Observed:** quiescence failed: timed out

## S38-20260714T131226-2: forced GC left 24 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-14T13:41:38
- **Severity:** suspected-bug
- **Run:** 20260714T131226_S38_seed42
- **Observed:** forced GC left 24 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 24}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S36-20260716T181544-1: scenario raised: cluster did not become healthy after reset

- **Logged (UTC):** 2026-07-16T18:20:48
- **Severity:** suspected-bug
- **Run:** 20260716T181544_S36_seed1
- **Observed:** scenario raised: cluster did not become healthy after reset

## S36-20260716T200002-1: scenario raised: cluster did not become healthy after reset

- **Logged (UTC):** 2026-07-16T20:05:05
- **Severity:** suspected-bug
- **Run:** 20260716T200002_S36_seed1
- **Observed:** scenario raised: cluster did not become healthy after reset

## S36-20260716T201906-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: promot

- **Logged (UTC):** 2026-07-16T20:19:22
- **Severity:** suspected-bug
- **Run:** 20260716T201906_S36_seed1
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 236. DB::Exception: promote: ref 'moving' already names a different committed manifest — refusing to overwrite (unique-ref invariant; use republishRef for an intended repoint). (ABORTED) (version 26.6.1.1) | sql=ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca'

## S37-20260717T005005-1: scenario raised: Node(localhost:8124) HTTP 400: Code: 36. DB::Exception: Table d

- **Logged (UTC):** 2026-07-17T00:50:22
- **Severity:** suspected-bug
- **Run:** 20260717T005005_S37_seed1
- **Observed:** scenario raised: Node(localhost:8124) HTTP 400: Code: 36. DB::Exception: Table doesn't have any table TTL expression, cannot remove. (BAD_ARGUMENTS) (version 26.6.1.1) | sql=ALTER TABLE s37_ttl REMOVE TTL

## S39-20260717T015047-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS wr

- **Logged (UTC):** 2026-07-17T01:51:43
- **Severity:** suspected-bug
- **Run:** 20260717T015047_S39_seed1
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS write could not be committed (stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/c94/c945a7a9-4578-4b78-bdd0-6ec0e42ead78@cas@/0000000000000001-000000000000000b/000001.zst' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId); retrying later. (NETWORK_ERROR) (version 26.6.1.1) | sql=INSERT INTO s39_lease SELECT 4000 + number AS id, randomString(512) AS payload FROM numbers(500)

## S39-20260717T020015-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS wr

- **Logged (UTC):** 2026-07-17T02:02:35
- **Severity:** suspected-bug
- **Run:** 20260717T020015_S39_seed1
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 210. DB::Exception: CAS write could not be committed (stageManifest: part-manifest PUT at 'soak_pool/cas/manifests/ca_soak_ch1/store/2a7/2a740e92-6007-4b67-b9f5-20062b1be4a7@cas@/0000000000000001-000000000000000d/000001.zst' is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages with a fresh ManifestId); retrying later. (NETWORK_ERROR) (version 26.6.1.1) | sql=INSERT INTO s39_lease SELECT 8000 + number AS id, randomString(512) AS payload FROM numbers(500)

## S39-20260717T020400-1: quiescence failed: Node(localhost:8124) HTTP 404: Code: 60. DB::Exception: Table

- **Logged (UTC):** 2026-07-17T02:11:50
- **Severity:** suspected-bug
- **Run:** 20260717T020400_S39_seed1
- **Observed:** quiescence failed: Node(localhost:8124) HTTP 404: Code: 60. DB::Exception: Table default.s39_lease does not exist. (UNKNOWN_TABLE) (version 26.6.1.1) | sql=SYSTEM SYNC REPLICA s39_lease

## S01-20260717T033430-1: S01 peak RSS grew 531 MiB during a 512 MiB blob upload — investigate Build::putB

- **Logged (UTC):** 2026-07-17T03:35:04
- **Severity:** suspected-bug
- **Run:** 20260717T033430_S01_seed1
- **Observed:** S01 peak RSS grew 531 MiB during a 512 MiB blob upload — investigate Build::putBlob materializing BlobSource into a String before putIfAbsentStream (README known first investigation target)

## S07-20260717T035307-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-17T03:55:49
- **Severity:** finding
- **Run:** 20260717T035307_S07_seed1
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S40-20260717T090957-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-17T09:14:10
- **Severity:** finding
- **Run:** 20260717T090957_S40_seed1
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S39-20260717T094921-1: scenario raised: leg A's fault window must be shorter than the renew period so i

- **Logged (UTC):** 2026-07-17T09:49:38
- **Severity:** suspected-bug
- **Run:** 20260717T094921_S39_seed1
- **Observed:** scenario raised: leg A's fault window must be shorter than the renew period so it can overlap AT MOST one renewal beat -- a window >= the renew period can fault two consecutive beats and (correctly) near the lease deadline, which is leg B's job, not leg A's

## S37-20260717T105322-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 384. DB::Exception: Cannot

- **Logged (UTC):** 2026-07-17T10:53:40
- **Severity:** suspected-bug
- **Run:** 20260717T105322_S37_seed1
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 384. DB::Exception: Cannot move part 'all_0_0_0' because it's participating in background process. (PART_IS_TEMPORARILY_LOCKED) (version 26.6.1.1) | sql=ALTER TABLE s37_ttl MOVE PARTITION ID 'all' TO VOLUME 'hot'

## S07-20260717T215644-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-17T22:03:09
- **Severity:** finding
- **Run:** 20260717T215644_S07_seed1
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S31-20260718T001753-1: ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under

- **Logged (UTC):** 2026-07-18T00:19:08
- **Severity:** suspected-bug
- **Run:** 20260718T001753_S31_seed1
- **Observed:** ca-gc-dryrun previews only target shard 0; subset-oracle blind to shard>=1 under gc_shards>1 — previewed 72 but GC reclaimed ~406 (checklist #9). previewDeletes should iterate all target shards, not just shard 0.

## S36-20260718T002431-1: scenario raised: Node(localhost:8123) HTTP 500: Code: 479. DB::Exception: Part '

- **Logged (UTC):** 2026-07-18T00:24:48
- **Severity:** suspected-bug
- **Run:** 20260718T002431_S36_seed1
- **Observed:** scenario raised: Node(localhost:8123) HTTP 500: Code: 479. DB::Exception: Part '0_0_0_0' is already on disk 'ca'. (UNKNOWN_DISK) (version 26.6.1.20000.altinityantalya) | sql=ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca'

## S40-20260718T003637-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-18T00:40:39
- **Severity:** finding
- **Run:** 20260718T003637_S40_seed1
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S07-20260718T212558-1: S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive 

- **Logged (UTC):** 2026-07-18T21:28:46
- **Severity:** finding
- **Run:** 20260718T212558_S07_seed1
- **Observed:** S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for the direct cap trip; the indirect fail-closed property check still runs.

## S08-20260718T223640-1: quiescence failed: quiesce initial: 1 replication-queue entries carry a real las

- **Logged (UTC):** 2026-07-18T23:01:31
- **Severity:** finding
- **Run:** 20260718T223640_S08_seed1
- **Observed:** quiescence failed: quiesce initial: 1 replication-queue entries carry a real last_exception — genuine error

## S40-20260719T000359-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-19T00:08:02
- **Severity:** finding
- **Run:** 20260719T000359_S40_seed1
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## PRODUCT BUG (correctness/availability, HIGH) — custom CAS disk objects are never torn down on DROP TABLE/DATABASE; a leaked GC/mount-lease background thread aborts the server (LOGICAL_ERROR) once the pool's backing storage is deleted

- **Logged (UTC):** 2026-07-19
- **Severity:** suspected-bug (confirmed root cause, not yet fixed)
- **Found via:** CI, PR#2073 (Altinity fork), `Stateless tests (amd_debug, sequential)`,
  sha `0d18313ddbcdc30c7a57d5183bb4894672eb4bd7`. Test:
  `tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh`. This is NOT
  a test bug and NOT a chaos-harness artifact — it reproduced on a single quiescent server,
  sequential execution, single table create/insert/mutate/drop, no chaos/multi-server
  involved.
- **Confirmed root cause (from `clickhouse-server.log` timestamps + code reading):**
  a custom disk created via the inline `disk(...)` AST function (e.g.
  `disk(object_storage_type=local, metadata_type=content_addressed,
  server_root_id='04295', name='04295_content_addressed_mut', path=...)`) is cached
  forever in `Context`'s disk selector (`src/Interpreters/Context.cpp:6584`,
  `getOrCreateDisk`) — there is no teardown hook anywhere in `DiskFromAST.cpp` or the
  `DROP TABLE`/`DROP DATABASE` path that stops or destroys the disk's underlying
  `CasPool` when the last (or only) table referencing it is dropped. The pool's
  background `ContentAddressedGC` thread and `MountLeaseKeeper` renewal thread keep
  running indefinitely against the same physical backing directory, even after every
  table using that disk is gone.
  The "no-leftovers" family of CAS tests (mirrors `04290_content_addressed_no_leftovers`)
  follows the standard pattern: `DROP TABLE ... SYNC` → poll the physical pool directory
  until background GC reclaims it to empty → `rm -rf "${POOL_DIR}"` to clean up. That
  final `rm -rf` deletes the directory out from under the still-running, leaked
  GC/mount-lease threads:
  1. GC's next tick (`Gc::acquireOrRenewLease`, `CasGc.cpp:298-340`) does
     `backend.get(gcStateKey())`; the file is gone, `has_observation` is `true` from
     prior successful rounds, so it throws `CORRUPTED_DATA "gc/state vanished after
     being observed"` every tick, forever (logged, non-fatal, "will retry next tick").
  2. `MountLeaseKeeper::renewOnce()` (`CasServerRoot.cpp:942-969`) next fires
     ~5s later: `putOverwrite(mountKey, ..., last_token)` fails (backing file absent) →
     `onRenewMismatch` (`CasServerRoot.cpp:713-784`) re-reads the key with
     `backend->get()` — also absent — so none of the three specific classification
     branches (`gc_fenced` / same-uuid-different-epoch "superseded" / foreign-uuid
     "held by a foreign server") can fire (there is nothing to classify), and it falls
     through to the generic base-class `LOGICAL_ERROR` at
     `CasServerRoot.cpp:965-967` ("was touched by a foreign writer — failing closed,
     never re-minting"). In a debug/ASan build `LOGICAL_ERROR` aborts the whole process
     (`src/Common/Exception.cpp`'s `handle_error_code`/`abortOnFailedAssertion`),
     crashing the server and failing two unrelated tests that happened to be running at
     the time ("Server died").
  Both the GC error and the mount-lease abort are the SAME root cause (the leaked
  disk object), not two independent bugs.
  Distinct from the already-tracked `CA-ASAN-SUITE-2026-07-09` backlog item (line
  ~1706 above): that item is about deliberately-injected GTEST negative tests aborting
  the *unit test binary* under abort-on-logical-error builds. This is a real crash
  during ordinary (non-adversarial) usage of a real SQL stateless test, and the same
  leak is a live production risk any time an operator deletes/reclaims a CAS pool's
  backing storage after dropping the tables that used it (e.g. after a decommission)
  without an explicit disk-teardown step.
- **Fix direction (not yet applied):** give custom CAS disks (or `CasPool` specifically)
  a lifecycle hook tied to table/database drop — either reference-count `Pool`
  instances per disk and stop background threads (`ContentAddressedGC`,
  `MountLeaseKeeper`) at zero live references, or hook `DROP TABLE`/`DROP DATABASE` to
  explicitly release the disk. As a secondary hardening (defense in depth, not a
  substitute for the lifecycle fix): `onRenewMismatch`/GC's round should distinguish
  "the key is genuinely absent because the backend itself is gone" from a real
  foreign-writer conflict, and should not escalate the former to a process-aborting
  `LOGICAL_ERROR`.

## RESOLVED (CPU) — ref-ledger JSON encoding writes byte-by-byte instead of bulk-copying safe runs

- **Logged (UTC):** 2026-07-19
- **Severity:** optimization-opportunity (confirmed cause, and now a measured delta from a
  standalone benchmark, not applied — user chose backlog-only)
- **Benchmark (2026-07-19):** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
  benchmarks/benchmark_cas_ref_protocol.cpp` (Google Benchmark, gated behind
  `-DENABLE_BENCHMARKS=ON`, never wired into `ninja test`/CI — pure measurement, no
  assertions; build+run: `cmake -S . -B build -DENABLE_BENCHMARKS=ON && ninja -C build
  benchmark_cas_ref_protocol && ./build/.../benchmark_cas_ref_protocol`):
  - `BM_WriteJSONStringSafe` (full `writeJSONString()` on a safe ~80-char ref-ledger-key-
    shaped string) vs `BM_RawBulkWriteSafe` (a raw `out.write()` of the same bytes): **177ns
    vs 23.5ns — a measured 7.5×**, not just the theoretical estimate above.
  - `BM_EncodeRefLogTxn` (the current, field-by-field `encodeRefLogTxn()` on one
    representative promote-shaped `RefLogTxn`): **753ns/call** — the absolute baseline a
    batch/template rewrite (option 4 below) would need to beat; no "after" implementation
    exists yet to diff against.
- **Found via:** `system.trace_log` `CPU` profiling of the 5h soak (v10/v11, cas-gc-rebuild
  branch). The top CPU stacks (by identical-stack sample count) were dominated by
  `CasRefLedger::flushRefBatch` → `encodeRefLogTxn`/`encodeRefTableSnapshot` →
  `writeJSONString`/`writeBindingFields`/`writeManifestRefFields` →
  `DB::WriteBuffer::write(char)`, collectively ~470 of the top-10 rows' samples (a
  modest, not dominant, share of total CPU samples — this is a constant-factor
  opportunity, not a hot-path emergency).
- **Root cause:** `writeJSONString` (`src/IO/WriteHelpers.h:182`) escapes JSON strings by
  calling `writeChar` in a plain per-character loop, even across long runs of characters
  that need no escaping. Each `WriteBuffer::write(char)` call
  (`src/IO/WriteBuffer.cpp:61-72`) pays a `finalized`/`canceled` check plus a
  `nextIfAtEnd()` call per byte, whereas the bulk `write(const char*, n)` path pays that
  overhead once per call, amortized over `n` bytes. The codebase already has a faster
  pattern for exactly this shape — `writeAnyEscapedString` (`WriteHelpers.h:267`) uses
  `find_first_symbols<...>` to vector-scan for the next special character, then bulk
  `buf.write(pos, run_length)`s the safe run in between — but `writeJSONString` does not
  use it.
- **Refinement (checked `CasTextFormat.cpp:59-92`):** the hex/numeric fields are
  ALREADY optimal — `writeHex128Value` and `writeU64StringValue` write `"` + raw bytes
  (`out.write()`/`writeIntText`) + `"` directly, bypassing `writeJSONString` entirely,
  since those values are safe by construction. The ONLY thing still going through
  `writeStringValue` → `writeJSONString` (and thus paying the byte-by-byte cost) is:
  `refOwnerKindToWord(b.kind)` (a small fixed-vocabulary enum word, e.g. "owner"/
  "tombstone" — always JSON-safe, never actually needs escaping), `b.ref_name` (a
  canonical ref name, already charset-validated by `checkCanonicalRefName` before this
  call), `t.type` (a fixed format-type tag), and `op.payload` (genuinely free-form user
  data — the one field that legitimately needs full escaping).
- **Fix directions considered (not applied):**
  1. Rewrite `writeJSONString` itself to use the `find_first_symbols` bulk-copy pattern.
     Benefits every JSON-encoding call site in ClickHouse, not just CAS, but touches a
     ubiquitous shared IO primitive — needs full format-test regression, out of
     proportion to a CAS-local profiling finding.
  2. CAS-local fast path (refined): add a raw/pre-validated string writer (bulk
     `out.write()`, no escaping loop) for `refOwnerKindToWord`'s fixed vocabulary and for
     already-charset-validated canonical names, and keep the general-purpose
     `writeJSONString` escaping path reserved for `op.payload` only (the one field that
     is genuinely free-form). Smaller blast radius than option 1, CAS-scoped, and now has
     a precise target (3 of 4 `writeStringValue` call sites, not "most strings").
  4. (User-suggested, likely the biggest single win) Batch/template assembly: the real
     overhead isn't only the escaping loop — it's the SHEER NUMBER of separate
     `WriteBuffer` calls per record (`writeChar` for each brace/comma/colon/quote,
     `writeKey` per field, `writeStringValue`/`writeU64StringValue`/`writeHex128Value`
     per value — ~9-12 calls for a single `ManifestRef`+prefix), each paying its own
     `finalized`/`canceled` check + `nextIfAtEnd()` (`WriteBuffer.cpp:38-72`). Since each
     record's key set/order/punctuation is fixed at compile time (only value bytes
     vary in length), assemble the whole small object into a local scratch buffer
     (`PODArray<char>`/`fmt::memory_buffer` with a reserved upper bound) via plain
     `memcpy`-style appends of the static literal parts (as pre-known
     `std::string_view`s) interleaved with the (already safe-by-construction, per #2)
     value bytes, then issue ONE `out.write(scratch.data(), scratch.size())` per record
     instead of ~10 individual small calls. Composes with #2 (skip escaping) but is
     independently valuable even without it — collapsing call COUNT, not just the
     per-byte escaping cost, is likely the larger win of the two.
  5. Do nothing for now (chosen): revisit if prod-scale profiling shows this becoming a
     larger share of CPU under heavier ref-ledger flush rates.
- **RESOLVED (2026-07-20):** Implemented a combined #2+#4: `CasJsonWriter`, a bulk-append
  writer (reserved scratch buffer, `memcpy`-style `append`/`appendChar` of static literal
  key/punctuation fragments interleaved with already-safe value bytes, one `out.write()` per
  record) now backs every CAS text format's encode path — ref log, ref snapshot, part
  manifest, gc state, gc outcomes, fold seal, pool meta, blob meta, blob envelope,
  server-root, and `cas_run`'s NDJSON writer. `writeJSONString`'s byte-by-byte `writeChar`
  loop and the per-record `String(prefix) + "me"`-style key allocations found during design
  are both gone from these paths. Byte-identical output throughout, verified by a
  differential gtest against the old `WriteBuffer` vocabulary (adversarial strings +
  randomized fuzz) and by the existing pins/goldens staying green.
  - **Measured:** `BM_EncodeRefLogTxn` **753ns → 333ns, a 2.26× speedup**. Floor
    `BM_MemcpyTxnBytes` (same bytes via raw fragment `memcpy`, no validation/escaping):
    **30.7ns**, so the migrated encoder sits at **~10.8× the floor** — short of the design's
    ≤3×-of-memcpy hard gate.
  - **Honest outcome on the ≤3× gate: NOT met, and a profiling investigation
    (`.superpowers/sdd/profile-encoderefllogtxn.md`) found it physically unreachable** for a
    validating, JSON-escaping encoder at this granularity. Attribution of the ~333ns:
    residual key-writing/call overhead ~47%, string escaping-scan ~23%, per-call buffer
    allocation ~16%, itoa/number formatting ~11%, validation (`checkCanonicalRefName` /
    `checkManifestRef`) ~3%. Even removing the per-call allocation ENTIRELY (reusable
    scratch buffer, no per-call malloc/free) only reaches ~278ns, still **~9.1× the floor**
    — a `memcpy` of precomputed bytes does none of the validation/formatting/escaping work a
    correct encoder must do, so the floor itself was too pure a target.
  - **Further optimization measured and REJECTED by decision** (kept the clean migration
    instead): a static-literal key-glue rung (merging `,"key":` fragments into single
    literals, extending the fast path from unprefixed to the prefixed binding keys) got to
    **240ns (7.8×)** but introduced a fragile prose-invariant separator between the
    hand-rolled literal and the value bytes. Raw-append of "safe" strings without escaping
    mostly failed as a direction because `checkCanonicalRefName` does **not** reject quotes,
    control bytes, or U+2028/U+2029 — ref names must stay through the escaping path; only 2
    fixed-vocabulary words (`refOwnerKindToWord`'s `"owner"`/`"tombstone"`) were provably safe
    to raw-append (16ns). The working tree was reverted to the clean `CasJsonWriter`
    migration; none of this rejected ladder is in the shipped code. A narrower "rung-1"
    variant was also tried and measured separately: `CasJsonWriter::keyLiteral`, merging
    the separator and a fully-rendered key literal (e.g. `"\"rn\":"`) into one append for the
    fixed unprefixed keys in `writeOp`/`writeCommittedRow`, moved `BM_EncodeRefLogTxn` only
    333ns → 325ns (~2.5%, still ~10.8× the floor) — a negligible, not-worth-a-third-key-path
    result. `keyLiteral` was removed again; the shipped encoder keeps the single `writeKey`
    path for clarity.
  - Spec: `docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md`.
  - Plan: `docs/superpowers/plans/2026-07-20-cas-json-writer-bulk-encoding.md`.

## RESOLVED (CPU, algorithmic) — admits() re-encodes the WHOLE ref table once per state-growing op in a flush batch

- **Logged (UTC):** 2026-07-19
- **Severity:** optimization-opportunity (confirmed cause via code reading AND a standalone
  micro-benchmark, not applied — user chose backlog-only)
- **Benchmark (2026-07-19):** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
  benchmarks/benchmark_cas_ref_protocol.cpp` (`BM_Admits`, Google Benchmark, gated behind
  `-DENABLE_BENCHMARKS=ON`, never wired into `ninja test`/CI — pure measurement, no
  assertions). Measured on a synthetic `RefTableState` with `N` committed rows:
  | N | time/call |
  |---:|---:|
  | 100 | 48.8 μs |
  | 1,000 | 476 μs |
  | 10,000 | 5,018 μs |
  | 100,000 | 55,976 μs |
  Google Benchmark's own complexity fit across this range: **O(N log N)**, RMS error 2% (a
  tighter, more rigorous characterization than "looks roughly linear" — the log-N component
  is consistent with `RefCowMap`'s `std::map`-backed merged iteration). The same file also
  benchmarks the byte-by-byte-escaping finding below (`BM_WriteJSONStringSafe` 177ns vs
  `BM_RawBulkWriteSafe` 23.5ns, a measured 7.5× — not just a theoretical estimate) and the
  absolute cost of the current `encodeRefLogTxn` (`BM_EncodeRefLogTxn`, 753ns/call, no "after"
  to diff against yet). Run directly: build with `-DENABLE_BENCHMARKS=ON`, then run the
  `benchmark_cas_ref_protocol` binary.
- **Found via:** following up the ref-ledger CPU findings above — `system.trace_log` `CPU`
  profiling of the 5h soak (v10/v11) showed `RefCowMap::const_iterator::operator++()`
  hot inside `admits()` (`CasRefLedger::flushRefBatch` → `admits()`), which is a bigger
  concern than the byte-level write cost above because it is O(table size), not a
  constant-factor overhead.
- **Root cause:** `admits(state, op, snapshot_budget, removal_budget)`
  (`CasRefProtocol.cpp:325-341`) answers "would applying this ONE op push the encoded
  snapshot size over budget?" by doing a full O(N) rebuild+encode from scratch on EVERY
  call, where N = the namespace's total live ref count:
  1. `snapshotOf(scratch, "")` (`CasRefProtocol.cpp:287-306`) iterates the ENTIRE merged
     `RefCowMap` (`for (const auto [name, row] : state.committed) snapshot.committed.push_back(row)`),
     then `encodeRefTableSnapshot(...)` serializes the WHOLE table just to check
     `.size() > snapshot_budget`.
  2. `buildHypotheticalRemovalTxn(scratch, ...)` (`CasRefProtocol.cpp:238-262`) does the
     same full committed+precommits iteration to build a hypothetical whole-namespace
     removal transaction, then `encodeRefLogTxn(...)` encodes THAT in full too, just to
     check its size against `removal_budget`.
  `admits()` is called ONCE PER STATE-GROWING OP inside the per-item loop in
  `CasRefLedger.cpp:1096-1116` (`flushRefBatch`'s batch loop), not once per flush or once
  per item — so a flush batch with K state-growing ops against a table with N live refs
  costs O(K×N), not O(K) or O(N).
- **Fix directions considered (not applied):**
  1. Maintain a running/incremental byte-size counter on `RefTableState`/`RefCowMap`,
     updated O(1) per applied op (add the new/changed row's encoded size, subtract the
     old one's), so `admits()` becomes `running_total + delta_for_op <= budget` — O(1)
     instead of O(N). Same idea for the removal-budget side: track a running
     committed+precommits row count (or running encoded-size total) instead of building
     the full hypothetical removal transaction from scratch. Preserves the exact
     per-op-precision semantics the current code has.
  2. Check the budget once per item (or once per whole flush batch) instead of once per
     state-growing op — fewer `admits()` calls, but per the code's own comments the
     per-op ordering exists so a LATER op in the same item is validated against a state
     that already reflects EARLIER ops in that same item; loosening this changes
     validation granularity/ordering guarantees and needs a design review, not just a
     perf patch.
  3. Do nothing for now (chosen): revisit if prod-scale profiling / a larger live ref-table
     count makes this a measurably larger share of flush latency.
- **Corroborating evidence (Real/wall-clock trace, same soak run):** `system.trace_log`
  `trace_type = 'Real'` top stacks show `pthread_cond_wait` inside
  `CasRefLedger::appendRefOps` ← `Pool::appendRefOps` ← `PartWriteTxn::promote`/
  `precommitAdd` ← `ContentAddressedTransaction::publishStaging`/`commit` across 4 near-
  identical stacks totaling ~741k samples on each replica — more than the background
  schedule pool's idle time and comparable to the query executor's own reactor loop.
  Unlike the rest of the Real-trace top stacks (query-executor async-task poll,
  schedule-pool idle, HTTP keep-alive poll, ZK/S3 network round-trips — all healthy idle
  capacity), this one is committing threads BLOCKED waiting for `appendRefOps`'s batch
  flush to complete — i.e. wall-clock evidence that the admits() O(N) cost above is
  actually queueing up concurrent committers, not just a theoretical CPU concern.
- **Decisive quantified evidence (`system.events`, same soak run, ~90min in):**
  `CasRefQueueWaitMicroseconds` (`ProfileEvents.cpp:772`, `"CA total time appendRefOps
  callers spent enqueued (sum over items)"`, incremented at `CasRefLedger.cpp:849-851`
  from `enqueued_at` to when the item's flush completes) reads **388,053,047,365 μs on
  ch1 / 387,502,949,104 μs on ch2** — i.e. ~108 cumulative hours of caller wait time.
  Dividing by `CasRefBatchedMutations` (855,272 ops on ch1, 854,840 on ch2, the count of
  ref-ops that went through this exact path) gives an **average wait of ~453.7ms per
  ref-op on ch1 and ~453.25ms on ch2** — nearly identical across both replicas,
  confirming this is a stable, reproducible effect of the flush mechanism, not noise.
  Every committed part, mutation, or removal requires at least one ref-op through this
  path, so this ~450ms average is a direct, concrete latency tax on ordinary CAS
  writes. This is the strongest evidence yet for prioritizing the admits() O(N) fix
  above — an actual named ProfileEvent built for this exact measurement, not just
  trace_log sample counts.
- **RETRACTED second corroborating signal:** an earlier pass (same day) flagged
  `ca_stress` showing 11028 outdated (`active=0`) parts against only 16 active
  (~690:1) as a suspected consequence of the admits() bottleneck starving part-removal
  via the shared `appendRefOps` path. Follow-up with `system.part_log` and a repeat
  `system.parts` check REFUTES this: `RemovePart` events (81450 total) substantially
  outnumber `NewPart` events (35349) over the run, the outdated count fell to 5787
  ~20 minutes after the 11028 snapshot (shrinking, not growing), and the single OLDEST
  outdated part was only 499s old against `old_parts_lifetime=480s` — i.e. every
  outdated part is within its normal grace window, not stuck. The 11028 figure was
  simply the pipeline depth of that grace window at the current creation rate, not a
  backlog. Do not cite the outdated-parts ratio as evidence for this bug going forward.
- **Actual part-cleanup trace_log finding (minor, separate, NOT connected to admits()):**
  `system.trace_log` `Real` shows `MergeTreeData::clearPartsFromFilesystemImplMaybeInParallel`
  blocked in `pthread_cond_wait` while scheduling removal work onto its own dedicated
  runner (~254 samples) — this pool has a fixed concurrency cap
  (`max_parts_cleaning_thread_pool_size=128`, a distinct pool from anything
  `appendRefOps`-related), so this is the removal orchestrator briefly waiting for a
  free slot during a burst of removals. Bounded, expected, and unrelated to the
  ref-ledger flush bottleneck — no fix direction.
- **RESOLVED (2026-07-20):** Implemented Approach A (incremental body-byte counters on
  `RefTableState`, maintained O(1) per op by `applyOpInPlace`, byte-exact vs the full encode,
  guarded by a debug recompute `chassert` and the existing `AdmitsExactnessPropertyTest`).
  `admits()` is now O(touched rows); a K-op flush batch against an N-ref table is O(K) not O(K×N).
  Spec: `docs/superpowers/specs/2026-07-20-cas-ref-admits-incremental-budget-design.md`.
  Plan: `docs/superpowers/plans/2026-07-20-cas-ref-admits-incremental-budget.md`.
  See the `BM_Admits` after-numbers in `benchmark_cas_ref_protocol.cpp`. Re-measured
  (2026-07-20, same synthetic `RefTableState` harness): N=100: 1842 ns, N=1,000: 1875 ns,
  N=10,000: 1864 ns, N=100,000: 1919 ns — flat, Google Benchmark complexity fit **O(1)**,
  RMS 1-2% (was O(N log N), 48.8 μs → 55,976 μs across the same N range).

## NOTE (CPU, expected, not actionable) — local blob staging round-trip: open() + CityHash128 + unlink() per blob

- **Logged (UTC):** 2026-07-19
- **Severity:** note (expected cost, no fix direction — completes the CPU profiling
  picture, not a new bug)
- **Found via:** `system.trace_log` `CPU` top-50 (5h soak v10/v11). Completes the earlier
  top-10 finding (`unlink() ← cleanupPendingTempFiles()`, the highest single CPU-sample
  stack) with its paired counterpart, both real per-blob filesystem costs of the local
  staging path (S3-native staging is opt-in, not the default here):
  - `open() ← CaContentWriteBuffer::CaContentWriteBuffer(...) ← ContentAddressedTransaction::writeFile()
    ← tryCreateWriteBuffer()` (~123 samples) — opens a fresh local staging file per blob.
  - `CityHash128BlobHashingWriteBuffer::nextImpl() ← HashingWriteBuffer::nextImpl() ← __write`
    (~188 samples) — computes the blob's content hash while flushing it to the staging
    file — this is content-addressing's own key-derivation step, fundamentally required,
    not wasted work.
  Together with the earlier `unlink()` finding, the full local-staging lifecycle per blob
  is: `open()` → hash-while-write → `unlink()` at commit. All three are genuine,
  unavoidable costs of the local-staging design (vs. opt-in S3-native staging, which
  skips the local file round-trip entirely). No fix direction — recorded for a complete
  CPU-profile picture, not because it should change.

## RESOLVED (benign, not a bug) — CANNOT_PARSE_INPUT_ASSERTION_FAILED in system.errors is ClickHouse's own VALUES-format fast/slow parser fallback, unrelated to CAS

- **Logged (UTC):** 2026-07-19
- **Severity:** resolved — confirmed benign via `system.errors.last_error_message` +
  `last_error_trace` (previously flagged as unresolved in the metrics-review commit
  above; this closes it out)
- **Root cause:** `system.errors`'s `last_error_message` for this code read: `"Cannot
  parse input: expected ',' before: 'toDateTime64(1784490339,3),1,-917,'a4de..."`. This
  is `ValuesBlockInputFormat::tryReadValue` (`src/Processors/Formats/Impl/
  ValuesBlockInputFormat.cpp:296-334`) attempting its FAST streaming-literal parser on a
  VALUES-list cell that is actually a function-call expression (`toDateTime64(...)`, from
  the soak workload's own generated `INSERT ... VALUES (...)` text) — the fast parser
  can't parse a function call, throws `CANNOT_PARSE_INPUT_ASSERTION_FAILED`, and
  `tryReadValue`'s own `catch (const Exception & e) { if (!isParseError(e.code())...)
  throw; ... }` (line 323-334) catches it, rolls back, and falls back to the full SQL
  expression parser for that column (`readRow`'s comment at line 239-242: "Parse value
  using fast streaming parser for literals and slow SQL parser for expressions... will
  be deduced [as a template], so it makes possible to parse the following rows much
  faster"). This is a deliberate, well-established ClickHouse VALUES-format optimization
  — entirely unrelated to CAS's own `CasTextFormat.cpp` parser (which was investigated
  and ruled out earlier: it uses non-throwing `checkChar` for its own control flow).
- **Not a CAS bug, no fix needed.** If the soak harness wanted to avoid the fallback's
  minor overhead, it could generate a pre-computed timestamp literal instead of a
  `toDateTime64(...)` call in its VALUES text — a harness-side micro-optimization, not a
  product concern, and not pursued.

## OPTIMIZATION OPPORTUNITY (observability, LOW/minor) — CAS conditional-PUT collisions inflate the generic S3_ERROR error counter

- **Logged (UTC):** 2026-07-19
- **Severity:** minor (user-identified; confirmed root cause, no fix applied)
- **Root cause:** `system.errors`'s `last_error_message` for `S3_ERROR` (code 499) read:
  `"Unable to parse ExceptionName: PreconditionFailed... At least one of the
  pre-conditions you specified did not hold, bucket test, key soak_pool/blobs/..."`.
  This is CAS's OWN deliberate collision-detection mechanism:
  `finalizeConditionalWriteInstrumented` (`CasObjectStorageBackend.cpp:190-270`) issues a
  conditional PUT and explicitly checks `S3Exception::isPreconditionFailed()`
  (`CasObjectStorageBackend.cpp:216-219`), mapping it to `PutOutcome::PreconditionFailed`
  — a normal, expected, fully-handled outcome of CAS's write-once/dedup-adopt semantics,
  not a real error. The underlying `S3Exception` object is still CONSTRUCTED (thrown by
  the S3 client on receiving the precondition-failed HTTP response) before CAS's code
  catches and reclassifies it, and that construction increments the generic
  `ErrorCodes::S3_ERROR` counter in `system.errors`/`system.error_log` regardless — so
  every legitimate, correctly-handled CAS collision inflates a metric that reads as if it
  were a genuine S3 error.
- **Fix direction (not applied):** avoid the throw-then-catch-and-reclassify round trip
  for this specific, expected response shape — e.g. if the S3 client API exposes the raw
  HTTP status/error code before constructing a full `Exception`, check for
  precondition-failed there and return `PutOutcome::PreconditionFailed` directly, without
  ever constructing (and thus counting) an `S3_ERROR`-coded exception for an outcome that
  isn't actually an error. Cosmetic/observability-only — does not affect correctness,
  confirmed minor priority by the user.

## RESOLVED — S3 PUT budget across query_log/part_log/blob_storage_log now ~100% attributed (was: OBSERVABILITY GAP)

- **Logged (UTC):** 2026-07-19
- **Severity:** resolved for PUT (see the correction below — ~100% attributed once matched by
  `part_name` and CAS-internal-object categories are recognized); GET/HEAD/DELETE's residual gaps
  from the `system.events` CAS-semantic-counter comparison further down were NOT re-investigated
  with this technique and stand as before (HEAD especially). No fix needed — this was purely an
  attribution/observability exercise, never a correctness issue.
- **Found via:** building a consolidated "S3 operation budget" table by aggregating
  ProfileEvents from `system.query_log` (by `query_kind`), `system.part_log` (by
  `event_type`), and `system.content_addressed_garbage_collection_log` (by `outcome`),
  then comparing the sum against a same-moment `system.events` snapshot (ch1, 5h soak
  v11). Same-moment totals:
  | op | attributed (query_log + part_log + GC-log) | system.events total | unattributed |
  |---|---:|---:|---:|
  | PUT | 567,035 | 2,045,424 | 1,478,389 (72%) |
  | GET | 4,065,877 | 6,991,516 | 2,925,639 (42%) |
  | HEAD | 7,415,849 | 13,226,618 | 5,810,769 (44%) |
  | DELETE | 1,684,312 | 2,388,055 | 703,743 (29%) |
  (`query_log`'s `Insert` and `part_log`'s `NewPart` were confirmed to be the SAME
  underlying events via `sumMap(ProfileEvents)` cross-checks — not summed together to
  avoid double-counting. `MergeParts`/`MutatePart`/`DownloadPart` are genuinely additive:
  their originating `Optimize`/`Alter` query_log rows show zero S3 ProfileEvents, meaning
  merge/mutation background execution isn't captured by query_log at all, only by
  part_log.)
- **Root cause (plausible, not fully proven):** confirmed via
  `sumMap(ProfileEvents)` across ALL `part_log` rows (every table, not just `ca_stress`)
  that PUT is 0 and DELETE is 0 in EVERY part_log row of EVERY table — i.e. no
  merge/mutation part_log event ever captures a nonzero PUT, even though merges and
  mutations clearly perform real S3 uploads for non-deduplicated content. This lines up
  with the earlier `Real`-trace finding of `WriteBufferFromS3::finalizeImpl() →
  TaskTracker::waitAll()`: CAS blob uploads during merge/mutation are dispatched to a
  separate parallel upload-worker pool (`TaskTracker`), and those workers' ProfileEvents
  apparently aren't aggregated back into the originating merge/mutation thread's
  ProfileEvents snapshot that `part_log` records — a plausible explanation for why PUT is
  the worst-attributed op (72%) while DELETE (mostly synchronous within a GC round's own
  thread) is the best-attributed (only 29% missing). GET/HEAD's partial gap (42-44%) may
  similarly trace to `MergeTreePrefetchedReadPool`'s speculative prefetch reads
  happening outside the exact window a part_log event's ProfileEvents snapshot spans.
- **Fix direction (not applied, needs more investigation before design):** if confirmed,
  ensure async upload/prefetch worker threads are properly attached to the originating
  query's/part-task's `ThreadGroup` so their ProfileEvents aggregate back correctly — or,
  if that's architecturally awkward for a detached worker pool, document the gap
  explicitly wherever `part_log`/`query_log` ProfileEvents are used for cost attribution,
  so operators don't underestimate real S3 spend from these tables alone.
- **Update — a second, much-better-covering attribution axis exists:** CAS's own
  semantic per-object-kind counters in `system.events` (`Cas{Blob,Gc,Manifest,Meta,Root,
  Other}{Put,Get,Head,Delete,List}`, incremented at the point of the S3 call itself,
  independent of query/part `ThreadGroup` attribution) cover the gap MUCH better than
  the query_log/part_log/GC-log axis above. Same-moment fresh snapshot (ch1):
  | op | sum of Cas*-kind counters | system.events total | coverage |
  |---|---:|---:|---:|
  | PUT | 1,625,628 | 2,095,330 | 77.6% (was 27.7% via query/part/gc-log) |
  | GET | 5,892,747 | 7,135,763 | 82.6% (was 58.2%) |
  | HEAD | 7,513,774 | 13,420,870 | 56.0% (was 56.0% — NOT improved) |
  | DELETE | 3,162,288 | 2,441,327 | 129.5% — CAS-sum EXCEEDS the S3 total |
  | LIST | 30,546 | 31,905 | 95.7% (was ~0%) |
  `CasBlobPut` alone (746,143) already exceeds the ENTIRE query_log-attributed PUT total
  (567,035), confirming background blob-upload workers (not query/part-scoped) are the
  dominant PUT source — consistent with the `TaskTracker`/async-upload-pool hypothesis
  above. DELETE's >100% coverage is plausibly explained by `DiskS3DeleteObjects` counting
  HTTP *requests* (each potentially a batched multi-key delete) while `Cas*Delete` counts
  logical *objects* — not yet confirmed, but a benign-looking explanation, not alarming.
  **HEAD remains the one persistently unexplained gap** (44% missing either way, and
  there is no `CasMetaHead` counter at all in the CAS-kind breakdown) — worth a follow-up
  look at whether generic `DiskObjectStorage`/`MergeTreePrefetchedReadPool` existence
  checks bypass `Cas::InstrumentedBackend`'s own instrumentation entirely.
- **Independent cross-validation, and the gap is now architecturally explained, not just
  measured:** `system.blob_storage_log` carries its own `query_id` column, and `part_log`'s
  `query_id` is documented as "Identifier of the INSERT query that created this data part".
  A direct row-level `INNER JOIN` between them (no ProfileEvents delta-summing at all)
  gives an independent count: `blob_storage_log` `Upload` rows joined to `query_log` by
  `query_id`, grouped by `query_kind`, yield 591,706 `Insert`-attributed uploads out of
  2,114,906 total `Upload` rows — **28.0% coverage**, matching the earlier
  ProfileEvents-based 27.7% (query_log's `Insert` = 567,035) within measurement-timing
  noise. Two independent methods (ProfileEvents-sum vs literal row JOIN) landing on the
  same ~28%/72% split is strong confirmation the split is real, not a quirk of either
  measurement technique.
  Crucially, `part_log`'s `query_id` is **only ever populated for `NewPart`**
  (44,624 of 59,909 rows) — `RemovePart`/`MergeParts`/`MergePartsStart`/`MutatePart`/
  `MutatePartStart`/`DownloadPart` all show `query_id = ''` for every single row, 0
  exceptions.
- **CORRECTION — the "unresolved" query_ids are NOT opaque; they decode to `table_uuid::part_name`
  (user-supplied insight) and match `part_log` by `part_name`, not by `query_id`.** An earlier
  pass in this entry checked `query_id NOT IN (SELECT query_id FROM system.part_log)` and found
  zero matches, then wrongly concluded these ids were unattributable in principle. They are not:
  merge/mutation blob uploads carry `query_id = '<table_uuid>::<part_name>'` (confirmed —
  `splitByString('::', query_id)[1]` equals `system.tables.uuid` for the table exactly). Matching
  on `splitByString('::', query_id)[2]` against `part_log.part_name` (filtered to the
  part-*creation* event types `NewPart`/`MergeParts`/`MutatePart`/`DownloadPart`, NOT `any()` over
  every event type for that part name — a part's name is reused by its later `RemovePart` row too,
  and grabbing an unfiltered `any(event_type)` silently returns the wrong stage) resolves **100%**
  of the previously "unresolved" set (21,368 of 21,368 tested).
  Final decomposition of every `Upload` event with a non-empty `query_id` (ch1, one snapshot):
  | source | uploads | share |
  |---|---:|---:|
  | `query_log` — `Insert` | 593,705 | (foreground) |
  | `part_log` — `MergeParts` | 345,457 | (background merge) |
  | `part_log` — `MutatePart` | 52,445 | (background mutation) |
  | `query_log` — `Create` | 1 | negligible |
  Zero rows remained unclassified. The remaining ~53% of ALL `Upload` events carry a **genuinely
  empty** `query_id` (not a decodable one) — these are not query/part-work at all, they're CAS's
  own internal housekeeping objects, confirmed by path-prefix breakdown of the empty-`query_id`
  rows:
  | category | uploads | avg size | note |
  |---|---:|---:|---|
  | `blobs/*.meta` | 726,527 | 72 B | per-blob meta-descriptor sidecar, written by a `ThreadPool`-named background thread alongside (not as part of) the main content PUT |
  | `cas/manifests/*` | 307,912 | 7.3 KB | ref-ledger manifest writes |
  | `cas/refs/*` | 86,818 | 935 B | ref-log/snapshot writes |
  | `gc/gen/*/attempt/*/outcomes/*` | 755 | 2.87 MB | GC round crash-recovery/idempotence records (large, but rare) |
  | `gc/other`, `gc/server-roots` | 5,585 | small | GC misc + mount-lease objects |
  Every one of these categories writes via CAS's own ref-ledger/GC machinery, which never had a
  query context to begin with — there is no "gap" left to close for PUT: **~100% of Upload volume
  is now attributed to a specific, understood source**, split roughly 28% foreground / 19%
  background merge+mutation / 53% CAS-internal housekeeping (dominated by the `.meta` sidecar,
  written once per blob almost like a second PUT for every real one).
  **Lesson for future S3-budget work**: match merge/mutation-driven activity to `part_log` by
  `(table_uuid, part_name)`, not by `query_id` string equality — the `query_id` `part_log` itself
  exposes is `NewPart`-only; the *other* event types don't carry one, but the *upstream* system
  (`blob_storage_log`) encodes the same part identity inside its own synthetic `query_id`, one
  `::`-split away. `Delete` events go further still: `query_id` is empty on **100%** of
  `blob_storage_log`'s 2,445,357 `Delete` rows, with zero exceptions — deletes (GC's bulk reclaim,
  `RemovePart`'s own cleanup) are never query-attributable by this mechanism at all, full stop; the
  CAS-semantic `Cas*Delete` counters (or the GC log directly) are the only path to DELETE
  attribution. GET/HEAD's residual gap (per the CAS-semantic-counter comparison above) was NOT
  re-investigated with this technique — HEAD in particular remains open.

## PRODUCT BUG (availability, MEDIUM-HIGH) — a transient S3-backend NETWORK_ERROR during CAS table-startup recovery permanently strands the table until server restart; ClickHouse's async table loader never retries the failed startup job on its own

- **Logged (UTC):** 2026-07-19
- **Severity:** confirmed (root cause traced from logs + code; not yet fixed)
- **Found via:** 5h soak `v11` (`utils/ca-soak/logs/soak_5h_20260719_v11.run.log`), `PHASE3 FAILED
  (rc=1)`. This ended the run — not the known "session-host `SIGTERM` burst" kill artifact
  (that one leaves no final line and a live-but-orphaned `soak.run`; this run logged a genuine
  `WORKLOAD FAILURE` and its own `PHASE3 FAILED` trailer).
- **Trigger (from the log's own chaos-fault trail, lines ~2774-3234):** an unusually dense burst
  of 5 chaos faults fired back-to-back with little/no recovery gap between them over ~430s
  (`t+13376s` to `t+13806s`): `#69 rustfs pause dur=53s`, `#70 ch1 freeze_long dur=68s`, `#71 ch1
  freeze_long dur=84s`, `#72 rustfs pause dur=50s`, `#73 ch2 kill dur=48s`. A single
  `BARRIER:DELETE op_id=128265` retried 39 consecutive times against transport failures
  (`ConnectionResetError`/`URLError: Connection refused`) across this window. `rustfs`'s own
  application log (`/logs/rustfs.log`, `rustfs::app::object_usecase`, `object_usecase.rs:1284`)
  showed `"I/O queue congestion detected"` with `queue_utilization: 100.0%`,
  `permits_in_use: 64/64` right in this window, plus `"HTTP connection is closed prematurely"`
  (`http.rs:949`) and cascading `list_path_raw`/`scan_dir` cancellations
  (`metacache_set.rs:496`, `local.rs:1528`) — `rustfs` itself was I/O-saturated, not crash-looping
  (`docker inspect`: `RestartCount=0` for all three containers throughout; the repeated
  "Initializing.../Starting rustfs server..." lines seen via `docker logs` are the CUMULATIVE
  history of every past chaos-triggered container restart across the whole run, not a live loop).
- **Root cause (confirmed by code + live re-query on the still-UP stack):** `ch1` itself came back
  from its own `freeze_long` faults (`#70`/`#71`) needing a CAS recovery pass for `ca_stress`
  (an unclean-boundary reload, per `CasRefLedger.cpp:340-345`'s `unclean_boundary_epoch()`
  check). That recovery's seal `putIfAbsentControlled` (`CasRefLedger.cpp:~404`) landed exactly
  while `rustfs` was still saturated/resetting connections from faults `#72`/`#73`, and threw
  `NETWORK_ERROR` via `throwCasWriteRetryLater` (`CasRequestControl.cpp:218-225`,
  message: "CAS write could not be committed (...); retrying later"). That exception propagated
  up through the table's async startup job as `ATTACH TABLE ... failed ... (ASYNC_LOAD_WAIT_FAILED)`.
  `CasRefLedger.cpp`'s own comment (lines ~340-345) documents that a failed seal PUT is meant to
  leave the table "unrecovered/non-writable; the next touch restarts recovery and re-seals" — but
  that promise is scoped to `CasRefLedger`'s *own* recovery entry point, one layer below
  ClickHouse's core `AsyncLoader`. Once `AsyncLoader` marks a table's startup job `FAILED`, nothing
  calls back into `CasRefLedger::recoverIfNeeded()` for that table again — confirmed empirically:
  three manual `SELECT count() FROM ca_stress` queries against the still-running, still-UP `ch1`,
  several minutes apart (well after `rustfs`'s congestion cleared at `22:15:38`, tested again at
  `22:24:47` — 9 minutes of full `rustfs` quiet), all returned the byte-identical error in ~5ms
  (`time curl ...` → `0m0.005s`) — an instant cache hit on the terminal `FAILED` job, not a fresh
  recovery attempt. So a purely *transient* infra hiccup (a handful of seconds of `rustfs`
  saturation) converts into a *permanent* (until server restart, or an explicit `DETACH`/`ATTACH`)
  table outage, which is what actually killed the soak run: the workload driver's subsequent
  queries against `ca_stress` all failed forever afterward → `WORKLOAD FAILURE` → `PHASE3 FAILED`.
- **Not itself a bug:** the fsck at shutdown found exactly 2 `"unreachable"` (not
  `"corrupt"`/`"orphaned-unaccounted"`) manifest keys under this table's namespace — consistent
  with the aborted seal attempt's own manifest write never being referenced by anything, which GC
  should reclaim normally; `fsck_status: "settled"`. `ch2 Exited (243)` was likewise expected — the
  chaos harness's own recovery-checkpoint loop restarts killed containers, but nothing does that
  once `soak.run` itself has exited.
- **Open question / fix direction (not yet applied, no fix attempted):** two independent angles,
  not mutually exclusive:
  1. Chaos-schedule angle: 5 faults with near-zero recovery gap between them (`rustfs`
     pause → `ch1` freeze ×2 → `rustfs` pause → `ch2` kill, all within ~430s) is denser than any
     single fault this suite has stress-tested individually; consider a minimum inter-fault
     recovery buffer, or explicitly keep this density as an intentional "compound fault" scenario
     (in which case CAS's behavior under it is the actual finding, not a scheduling defect).
  2. CAS-startup angle: `ATTACH TABLE`'s CAS-recovery path has no bounded internal retry of its own
     before letting a `NETWORK_ERROR` become a permanent job failure — unlike, e.g., ordinary CAS
     write/read paths which the `~90s retry envelope` mentioned in `CasRefLedger.cpp`'s comments
     already covers. Worth deciding whether table *startup* recovery should get a similar bounded
     retry-before-fail-permanently policy, or whether "transient infra blip during startup ⇒
     requires an explicit reload" is accepted as by-design ClickHouse `AsyncLoader` behavior that
     CAS inherits like any other storage engine.
- **Confirmed via `system.asynchronous_loader` (2026-07-19, ~1h10m after the failure, `ch1` still
  UP):** exactly ONE row for `job = 'load table default.ca_stress'` — `status: FAILED`,
  `schedule_time: 2026-07-19 22:14:43.433126`, `start_time: 22:14:43.433157`,
  `finish_time: 22:15:33.467675` (`elapsed: 50.03s`), plus one dependent
  `job = 'startup table default.ca_stress'` row, `status: CANCELED` (cancelled by the dependency
  failure, never itself started — `start_time: NULL`). No second row, no later `schedule_time` —
  every manual touch after `22:15:33` (several, minutes apart, up to `23:2x`) read this SAME
  terminal row rather than creating a new load job. Cross-checked against `AsyncLoader.h`/`.cpp`
  directly: `grep -i retry` over both files returns zero matches; `finish()` sets a job's
  `LoadStatus` to `FAILED` unconditionally and terminally (`AsyncLoader.h:121,471`) — there is no
  requeue/reschedule path anywhere in the class for a job already marked `FAILED`. So this is not
  "AsyncLoader retried and kept failing" — **AsyncLoader has no retry concept at all**, for any
  table, CAS or otherwise; a table whose startup job fails once stays `FAILED` until an explicit
  `DETACH`/`ATTACH` (which schedules a brand-new job) or a full server restart. This is general
  ClickHouse `AsyncLoader` behavior, not a CAS-specific defect — CAS's own ~90s retry envelope
  (`CasRequestControl`'s internal S3-request-level retries) ran to completion INSIDE that single
  50s-`elapsed` job execution and still came up empty; nothing above that layer schedules a second
  attempt. This sharpens fix-direction option 2 above: since the platform gives CAS no free retry
  at the job level, if bounded retry-before-fail-permanently is wanted for CAS table startup, CAS
  itself would need to loop inside `ensureRefTableRecovered`/the seal-PUT call, not rely on
  `AsyncLoader` ever giving it a second chance.
- **Tried the obvious in-session workaround (manual `DETACH`/`ATTACH`) — it does not work either:**
  plain `ATTACH TABLE default.ca_stress` (no prior `DETACH`) returns the identical cached error
  instantly, as expected (the table is already registered in `DatabaseOrdinary`). The prerequisite
  `DETACH TABLE default.ca_stress` was then tried on its own — it ALSO returns the byte-identical
  cached error in `0.003s`: `DETACH` itself must wait on the table's `AsyncLoader` job to obtain
  the live storage object before it can detach it, and that job is the same permanently-`FAILED`
  one. So there is no way, from SQL alone, to clear this table's stuck state — not even the
  standard `DETACH`+`ATTACH` recovery recipe works. A full server restart (which schedules a
  brand-new `AsyncLoader` job set from scratch) is, empirically, the ONLY recovery path once this
  has happened. This raises the practical severity: an operator hitting this in production cannot
  self-service a fix without a restart, which is a much bigger deal for a single-digit-second S3
  hiccup than "requires an explicit reload."
- **Proposed fix (not yet implemented — design only):** add a bounded retry-with-backoff around the
  seal `PUT` itself, inside `CasRefLedger::ensureRefTableRecovered`
  (`CasRefLedger.cpp:202-410`, the seal block at lines `392-407`):
  ```cpp
  CasWriteOutcome outcome{};
  try
  {
      outcome = ref_request_controller->putIfAbsentControlled(
          layout.refSnapshotKey(ns, seal_id), seal_bytes, fence_ok);
  }
  catch (...) { lock.lock(); throw; }
  lock.lock();
  if (outcome != CasWriteOutcome::Committed)
      throwCasWriteRetryLater(fmt::format(
          "CAS recovery seal PUT for namespace '{}' did not commit; failing recovery closed "
          "(the table stays unrecovered/non-writable; the next touch restarts recovery and "
          "re-seals)", ns.string()));
  ```
  Rationale for fixing exactly here rather than one layer up (`DatabaseOrdinary`/table-startup
  code):
  1. `ensureRefTableRecovered` is the single funnel every `CasRefLedger` touch-point calls first (13
     call sites: lines 130, 176, 641, 664, 672, 761, 794, 904, 1437, 1445, 1453, 1486, 1864, 1882) —
     a fix here covers every caller uniformly, not just table-open.
  2. This exact block already runs OUTSIDE `rt.state_mutex` (unlocked just above at ~line 379,
     relocked after) specifically so it can "run the full ~90s retry envelope" per the existing
     comment — extending that envelope with an outer retry loop doesn't newly violate anything;
     `recovery_in_progress` (not the mutex) already serializes concurrent callers for the SAME
     table across this whole unlocked window (set/cleared at lines 221-234), so a longer window is
     still safe, just longer.
  3. `putIfAbsentControlled` is an idempotent conditional write (`If-None-Match: *`) — safe to call
     again directly with no extra bookkeeping.
  4. Not upstream-coupling: the fix lives entirely inside CAS's own file, touches no generic
     `Database`/`AsyncLoader`/`Replicated` code.
  - **Open design questions before implementing (why this isn't a 2-line hack):** how many
    attempts / what backoff curve (this needs to meaningfully outlast a chaos-scale transient, e.g.
    the ~1-2 minutes `rustfs` took to drain its I/O-queue congestion in this incident, without
    blocking an `AsyncLoader` worker thread indefinitely if the backend is genuinely, durably
    down); whether retrying is worth doing given `CasRequestControl`'s OWN internal ~90s S3-retry
    envelope already ran once and came up empty (does one more layer of retry actually help, or
    does it just mean waiting ~90s longer for the same outcome when the backend is really down
    for minutes, not seconds — the answer depends on how long `rustfs`'s specific failure mode
    (I/O-queue saturation, not a hard outage) typically takes to clear, which this one incident
    doesn't fully establish); and whether this backoff-retry (against real external I/O flakiness,
    not a code race) is a legitimate use of a blocking delay in C++ here, distinct from the
    "never sleep to paper over a race condition" rule.
- **Soak stack state:** left UP per the run's own failure trap (as designed) — `ch1` up (13min at
  time of writing, `SELECT 1` healthy, `ca_stress` permanently unattachable), `ch2` still
  `Exited (243)`, `rustfs1` up and quiet since `22:15:38`. Awaiting a decision on relaunch (`v12`)
  vs. further investigation before any teardown.
## NOTE (robustness, observed while validating the stuck-table-load fix) — a lazy CAS table's first-access build blocks server-side for the whole S3 outage instead of failing fast when the mount fence is lost

- **Logged (UTC):** 2026-07-20
- **Severity:** observation (not a data-loss bug; possible UX/robustness gap)
- **Found via:** `tests/integration/test_cas_lazy_load_recovery` (the Layer 2 verification test for the
  stuck-table-load fix). Server log
  `_instances-gw0/node1/logs/clickhouse-server.log` from the first (over-reaching) run.
- **What was observed:** with `lazy_load_tables=1`, a CAS `ReplicatedMergeTree` re-attaches after a
  restart as a `StorageTableProxy`; the real storage is built on first access. When the object store
  (rustfs) is paused right as that first-access build runs, the build does NOT fail fast — it blocks
  server-side for the ENTIRE outage (observed ~14 min, `16:29:03`→`16:43:50`, ending only when rustfs
  was unpaused at test teardown). The client query times out (helper's ~600 s socket timeout) but the
  SERVER-side build keeps running, so a subsequent `DETACH TABLE` queues behind it and cannot
  interrupt it.
- **CORRECTION (where the block actually is):** an earlier draft of this note attributed the block to
  `loadDataParts`. The server log is more specific: the wait is in the CAS **ref-namespace `LIST`**
  inside `CasRefLedger::ensureRefTableRecovered` — the log shows the disk's S3 client entering its own
  transparent retry (`... retry 1/501 ...`) on that `LIST`, and only AFTER the unpause does
  `Loading data parts` begin and finish in milliseconds. So the blocking read is the recovery `LIST`,
  not the part load. This matters: it is the SAME leg as the Layer 1 review finding F1 (recovery
  LIST/GET use the disk's object-storage client, whose default `~500-attempt` transparent retry rides
  the outage), and a mitigation aimed at `loadDataParts` would miss it.
- **Why it is NOT the stuck-table bug (and the fix still holds):** the lazy load job is the proxy
  (always `OK`), so there is no permanently-cached `AsyncLoader` `FAILED` state. When S3 returns the
  blocked build completes and the table becomes usable again with NO server restart. The integration
  test asserts that self-heal (it passes in ~58 s) — specifically, no permanently-cached `FAILED`
  state after a bounded outage. It does NOT (and cannot, given block-until-recovered) assert a
  fast-fail, a `DETACH`-while-down, or a `StorageTableProxy` retry-after-a-THROWN build; the observed
  path is the ORIGINAL build continuing to completion on unpause, not the proxy re-driving a failed
  build.
- **Interaction with Layer 1's own S3 retry:** Layer 1's bounded recovery retry
  (`ensureRefTableRecovered`, `recovery_retry_budget_ms`, default 120 s) sits ABOVE the disk client's
  own ~500-attempt retry on the `LIST`. As long as the disk client keeps blocking-and-retrying the
  `LIST` internally, the outage is ridden out THERE and Layer 1's outer loop never even sees a
  transient error to bound. So today the effective first-access outage tolerance is governed by the
  disk client's retry policy, not by `recovery_retry_budget_ms`.
- **Open question / possible follow-up (not investigated):** should the recovery `LIST`/`GET` use a
  bounded, fence-aware deadline (so a long outage fail-closes and lets `StorageTableProxy` retry on
  the next access / Layer 1's budget actually bound it) instead of blocking the calling query for the
  full outage? The fix must key on the **lost mount fence** (or a deadline ≥ Layer 1's budget), NOT a
  short per-request deadline: fence loss requires an outage longer than the lease TTL, so a
  fence-keyed fail-fast cannot regress the short blips Layer 1 is designed to ride out, whereas a bare
  short deadline WOULD regress them into query failures.

## OPTIMIZATION OPPORTUNITY (write-path latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3 (170.6s vs 22.4s): three-level write-path serialization + speculative HEAD-before-PUT

- **Logged (UTC):** 2026-07-21
- **Severity:** optimization-opportunity (measured on staging from `system.query_log` ProfileEvents;
  root cause confirmed by code reading; not applied)
- **Found via:** comparative benchmark on staging (`26.6.1.20000.altinityantalya`), two 500-partition
  wide-part inserts (10M rows, 30 columns, 2 replicas). CAS table `cas_compare_wide_500p_20260720`
  (`query_id 225ec42a-94f4-41c3-989c-b316ebc50ec3`) = **170.6 s**; standard-S3 table
  `s3_compare_wide_500p_20260720` (`query_id 7295ebc8-8942-48e8-b0e8-4979449da6f5`) = **22.4 s**. CAS
  issued FEWER `PutObject` (2566 vs 6156 per node) yet was 7.6× slower — so the penalty is round-trip
  COUNT + SERIALIZATION, not bandwidth. Reads (10 `clusterAllReplicas` aggregations) were 17.6%
  FASTER on CAS, consistent with the write-only nature of the regression.
- **Measured (CAS insert ProfileEvents, one node):** `RealTimeMicroseconds` 802.6 s → avg ~4.7
  threads; `S3WriteMicroseconds` 117.5 s (2567 PUTs, ~46 ms each); `CasRefQueueWaitMicroseconds`
  **36.2 s**; `S3ReadMicroseconds` 29.1 s (1015 `HeadObject` + 514 `GetObject`, ~19 ms each);
  `OSCPUVirtualTimeMicroseconds` 16.8 s (compute). **Smoking gun:** `CasRefBatchFlushes` = 1026 ==
  `CasRefBatchedMutations` = 1026 → ref-ledger batch size EXACTLY **1.0** (1026 = 513 precommit + 513
  promote appends, each its own serial S3 round trip). `CasManifestGet` = 513 (one per part).
  `CasBlobHeadFirst` = 498 == `CasBlobHeadMiss` = 498, `CasBlobBodyPutAvoided` = 0. Standard-S3 side:
  6156 `PutObject`, 0 HEAD, 0 GET, 0 COPY.
- **Corrections to first hypotheses (from the measured data):**
  - **"Fewer PUTs is dedup" — WRONG here.** `CasBlobBodyPutAvoided` = 0: on this fresh insert dedup
    avoided ZERO PUTs. Fewer PUTs is COARSER OBJECT GRANULARITY — `CasBlobPut` = 1026 ≈ 2 blobs/part
    (content-addressed coalesced chunks) vs standard S3's ~30 separate column-file objects per part.
  - **"Double write" — ruled out.** `S3CopyObject` = 0 confirms direct PUT; S3-native staging
    (stage-PUT + server-side `CopyObject`) was OFF.
- **Root causes (prioritized by serialized-latency contribution):**
  1. **Sequential per-part commit loop** — `ContentAddressedTransaction::commit()`
     (`ContentAddressedTransaction.cpp:396-404`) drives all ~500 parts one fully after another. Because
     it is single-threaded, the ref-ledger (`CasRefLedger::flushRefBatch`, which CAN batch concurrent
     same-namespace appends) has nothing to batch — hence batch size 1.0. Each part pays its own 2
     blocking ref-lane round trips (`precommitAdd` + `promote`) ≈ 1026 serial ref-log PUTs (~47 s) +
     36 s queue wait. This is the single biggest bottleneck.
  2. **Sequential per-blob upload loop within a part** — `uploadPendingBlobs`
     (`ContentAddressedTransaction.cpp:241-283`), plain `for`, no thread pool; a part's ~20-60 column
     blobs upload one at a time. Standard `DiskObjectStorage` fans these across its I/O thread pool
     (standard ran ~12× effective write concurrency; CAS ~4.7 threads).
  3. **Unconditional manifest-body GET in `promote`** — `CasPartWriteTxn.cpp:914-924`, one
     `backend().get(manifest_key)` per part (513 GETs) to re-read + validate the manifest
     (`refMatchesBody` / `manifestNamespaceMatches`) before promoting the ref. This is a fail-closed
     invariant guard ("a committed ref must never name a missing/mismatched manifest"), NOT pure waste
     — it is load-bearing on the ADOPT (`Occupied`) path.
  4. **Speculative HEAD-before-PUT for blobs ≥ 1 MiB** — `putBlob`
     (`CasPartWriteTxn.cpp:171-198`), default `dedup_head_first_min_bytes = 1 MiB` (`CasPool.h:73`).
     On fresh data every large-blob HEAD misses (498/498) and still PUTs: pure tax (~9.5 s serial).
     Break-even hit rate ≈ HEAD/PUT ≈ 19/46 ≈ 41%; below that it loses. The size branch also defends
     against a broken-pipe / retry-storm on stores that early-close a doomed conditional PUT.
- **Proposed fixes (by risk/reward):**
  1. **Concurrent per-part commit** (biggest win, architecturally clean): drive the commit loop with
     a bounded shared thread pool so multiple parts' `precommitAdd`/`promote` arrive at the ref queue
     concurrently and the ledger's existing batching collapses ~1026 batch-size-1 flushes into a
     handful. Targets the ~47 s serial ref-PUT + 36 s `CasRefQueueWaitMicroseconds`. Correctness: the
     per-namespace mount-lease still holds and the queue/leader serializes the actual appends —
     concurrency only FILLS the batch; only the per-part `precommitAdd`-before-`promote` order must be
     preserved (different parts interleave freely). Success metric: `CasRefBatchFlushes` ≪
     `CasRefBatchedMutations`, `CasRefQueueWaitMicroseconds` drops.
  2. **Parallel per-blob upload within a part** (low risk): fan a part's blob PUTs across the SAME
     bounded pool (shared with #1 so it does not explode into parts×blobs threads). Targets the
     117.5 s `S3WriteMicroseconds` currently at ~4.7× concurrency.
  3. **Skip the promote manifest GET on the fresh-write path** (medium): when THIS txn's conditional
     manifest PUT returned `Committed` (we created it and hold the exact bytes in memory),
     `refMatchesBody`/`manifestNamespaceMatches` are provable from the in-memory manifest and durability
     from the successful PUT under S3 strong read-after-write — validate in-memory, skip the remote
     GET. KEEP the GET on the `Occupied`/adopt path (there it is a real deterministic-adoption re-read;
     removing it would be a skip-read shortcut CAS forbids). Gate on backend read-after-write
     guarantees if the GET was also meant as a durability fence for weaker stores.
  4. **Make HEAD-before-PUT adaptive** (low risk, small): track the recent head-first hit rate
     (`CasBlobBodyPutAvoided`/`CasBlobHeadFirst`) and back off speculative HEADs when it is ~0 (fresh
     bulk load), re-enabling when hits reappear — keeps the dedup win for re-inserts, drops the tax on
     first loads. Cheaper interim: raise/zero `dedup_head_first_min_bytes` on known low-dedup ingest.
     Before dropping the size branch entirely, confirm whether the target S3 endpoint actually
     early-closes doomed conditional PUTs (if not, the branch defends against a non-existent problem).
- **Confirming metrics (per INSERT `query_id`, `system.query_log` ProfileEvents / `system.events`):**
  `S3HeadObject`, `S3GetObject`, `CasBlobHeadFirst`/`CasBlobBodyPutAvoided`/`CasBlobDedupCacheHit`,
  `CasRefBatchFlushes` vs `CasRefBatchedMutations`, and `CasRefQueueWaitMicroseconds` summed vs query
  wall time (the smoking gun for #1). The full write-path code map + evidence is in the session memory
  note `project_cas_insert_slowness_writepath`.

## Mid-soak health snapshot, f1f11 attempt-4 (~85 min in, mutations stage) — Logged 2026-07-21 ~13:45, pre-stop (soak stopped early by user for parallel work)
- **Invariants**: ZERO violations both nodes (step-1 gate). Client-reached exceptions: zero. Top error counters all known-benign classes.
- **INVESTIGATE (single-shot, no client impact)**: `CORRUPTED_DATA value=1` — `RefTableState: exact committed binding 'tmp-fetch_20260721_54511_54511_0' to remove is absent`. Fetch-relink removal path; one occurrence; fsck had been clean at prior checkpoints. Needs a code-path trace (removal of a tmp-fetch ref racing rename/commit?).
- **OPTIMIZATION OPPORTUNITY (CPU)**: `RefCowMap::const_iterator::operator++` inside `applyOpInPlace` (via `applyRefLogTxn` + `admits`) sums to ~2.1k CPU samples — same order as the entire blob-hashing write path (2.4k). Ties directly to the ref-admits incremental-budget design (specs/2026-07-20-cas-ref-admits-incremental-budget-design.md).
- **NOTE (Real/wall-clock bottleneck, the round's mandate question)**: dominant CAS wait = `pthread_cond_wait` in `CasRefLedger::appendRefOps` (~97k samples across 4 stacks) — the per-namespace ref-append lane is THE serialization point under mutations churn. CPU is NOT the CAS bottleneck (global CPU top = merge insertRow/LZ4); the ref-append queue is.
- **OBSERVABILITY GAP**: GC round 3 ran 764s with all four work counters zero (deleted/marked/graduated/redeleted) — fold/edge work invisible to `ms_per_item` normalization; add fold-work counters to the Finish record or the duration reads as anomalous.
- **Budget snapshot** (cumulative, 85 min): HEAD 11.0M > GET 7.9M > DEL 2.9M > PUT 2.5M, LIST 11.6k — HEAD-dominant profile (dedup HEAD-first + validation). CAS-counter coverage vs generic: GET gap ~1.8M ≈ direct part-data reads; HEAD gap ~6.1M ≈ post-upload checks + non-CAS-counted paths. GC rounds avg 722s max 2042s (marking-proportional except round-3 above).
- **Context**: rustfs single container at ~206% CPU (ch1 149%, ch2 91%) — backend is the S3 throughput ceiling; consistent with transient retried timeouts and zero real failures.

## Investigations closed 2026-07-21 ~14:2x (user-requested, from the mid-soak health snapshot)
- **RESOLVED (was: single CORRUPTED_DATA tmp-fetch binding)**: NOT corruption, NOT data-loss. Mechanism from text_log + audit log: `CasOrphanManifestSweep` built its protection view CONCURRENTLY with the fetch-relink re-key; the view replay hit the tmp-fetch removal op against a state that lacked the binding (non-atomic snapshot+log-tail cut, same transient family as B141/B144 fsck races) and threw `CORRUPTED_DATA`; the sweep caught it and FAILED CLOSED — "protection view unavailable; skipping", nothing deleted (Warning at 10:35:04.135 UTC, 300 ms after the successful `ref_drop`). Two product improvements: (a) replay-for-VIEW should treat exact-removal-absent as an incoherent-cut signal (retry/skip) without minting `CORRUPTED_DATA` — writer-side strict throw stays; (b) the handled transient still lands in `ForcedCriticalErrorsLogger` + `system.errors[CORRUPTED_DATA]`, poisoning must-be-zero telemetry with a false alarm.
- **RESOLVED (was: GC round-3 764s zero-work anomaly)**: the round's own ProfileEvents map shows it was a fold+ref-cleanup round: `CasRefLogBodyGets` 97.5k, `CasRefEmittedEdges` 89k, `CasRefCleanupObjectsDeleted` 97.8k, `CasRootDelete` 97.8k, S3 GET 187k + HEAD 376k, ~800k ops in 764s ≈ 1 ms/op — healthy. OBSERVABILITY GAP confirmed and narrowed: the Finish row's four work columns count only blob-side outcomes; fix = surface fold/cleanup work (edges, ref objects deleted, list pages) as first-class Finish columns and include them in any ms_per_item normalization.

## OPTIMIZATION OPPORTUNITY — manifestAlreadyOwned linear value-scan is the residual admits/apply hotspot — Logged 2026-07-21 ~14:3x
The 07-20 admits() O(1) rewrite (b5f448e9b41 + 13ab814869c, IN the attempt-4 binary) killed the full re-ENCODE; the f1f11 attempt-4 CPU trace shows the residual: `RefCowMap::const_iterator::operator++` now lives in `manifestAlreadyOwned` (CasRefProtocol.cpp:24-33) — a LINEAR scan of the whole committed+precommit state per add-precommit op, guarded by a comment ("the state machine is not the hot path") that predates the encode fix and is now stale. Hit 3 ways: real apply, admits() preview (per state-growing op per flush batch), and — largest — replay paths (`applyRefLogTxn`: recovery, GC fold, orphan-sweep view build — e.g. round-3's 97k-txn fold pays the scan per historical add). Fix direction: incremental reverse index (manifest_ref -> owner) on RefTableState maintained in applyOpInPlace's four arms — the exact pattern of the body-byte counters commit; BM_Admits benchmark suite already exists to gate it. Severity: perf-only (correctness unaffected).

## KNOWN ISSUE (write-path design scope, MEDIUM) — bulk partition operations (`REPLACE`/`MOVE PARTITION`, `ATTACH`) commit part disk-transactions one at a time and get no CAS commit batching — Logged 2026-07-22
The phased multi-commit design (specs/2026-07-22, second revision after the Codex `BLOCKING FLAW`
review) deliberately scopes to the `MergeTreeData::Transaction::renameParts` seam, where the
production topology is N independent single-part disk transactions (INSERT). Bulk partition
operations do NOT benefit:
- `REPLACE PARTITION` / `MOVE PARTITION TO TABLE` pass `ClonePartParams{.txn = ...}` (a MergeTree
  transaction, NOT a shared disk transaction — `StorageMergeTree.cpp:2785`, `:2967`); each
  `cloneAndLoadDataPart`/`freeze` creates and commits its OWN disk transaction before `renameParts`
  runs (`DataPartStorageOnDiskBase.cpp:541`, `:603`), so per-part CAS commits (manifest PUT +
  precommit/promote ref round-trips) stay serial there.
- The `external_transaction` mechanism (`IDataPartStorage.h:263`) that WOULD carry one shared
  N-part disk transaction is only exercised by detached-part cloning (`IMergeTreeDataPart.cpp:2444`)
  and synthetic tests; production bulk paths never use it.
Improvement options (any future effort should re-verify these lines first):
1. **Thread a shared `external_transaction` through the bulk clone paths** so all cloned parts stage
   into ONE disk transaction committed at `renameParts` — then the multi-part path of
   `ContentAddressedTransaction::commit` (and any phased engine) batches naturally. Upstream-heavy:
   touches `cloneAndLoadDataPart`/`freeze` ownership and the caller-owns-commit contract.
2. **Defer clone-transaction commits to the seam**: keep per-part transactions but let bulk callers
   hand them uncommitted to the same batched-commit seam INSERT uses (same topology as INSERT, no
   shared-transaction rework). Requires the clone paths to stop committing inside `freeze`.
3. **CAS-internal only**: leave upstream untouched; rely on the ref-ledger's concurrency batching by
   running bulk clones on a thread pool so their per-part commits overlap (opportunistic batching
   engages with concurrency, batch size 1.0 only when serial).
Not scheduled; revisit if bulk-partition latency on CAS is ever measured as a problem (INSERT was
the measured 7.6× case).

## NEW CARD TO BUILD — S42: allocation-fault soak (exception safety of the CAS write path) (requested 2026-07-24) {#s42-allocation-fault-soak}

**Origin:** user request during the publish-confirm fetch-handoff design round. Design lives in
`docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §testing; this entry
is the suite-side registration. Next free id is **S42** (`s41_wide_insert_baseline.py` is taken).

**Why:** the suite faults nodes (kill/restart) and the object store (s3faultproxy), but never the
ALLOCATOR. The whole exception-safety class the ref-lane hardening is about — a throw landing
between a durable PUT and its in-memory apply, leaving the cache diverged from the journal — is
invisible to every existing card, and in ordinary testing allocations simply succeed. This card
makes allocation failure a first-class fault source. Blanket requirement (user): **everything must
stay consistent despite allocation errors** — queries may fail, invariants may not.

**UPDATE 2026-07-24 (review round 4 corrections — apply when building the card):** (a) the
thread-fault setting is applied on config reload in `Server.cpp:2763-2764`, NOT
`InterpreterSystemQuery.cpp:1158` (that is `SYSTEM START THREAD FUZZER`); (b) it reaches
thread-CREATING paths such as the background snapshot dispatcher (`CasRefLedger.cpp:1839`), NOT the
ref append lane — the lane runs on the CALLER thread (`CasRefLedger.cpp:980`), which is why leg A's
per-query knob is the one that reaches the CAS commit path; (c) the soundness guard must be a
TARGETED post-PUT apply failpoint or a poison-transition counter — a nonzero
`MEMORY_LIMIT_EXCEEDED` count only proves *some* allocation failed, not that the tiny post-durable
window was hit; (d) leg C's restart oracle is NOT sufficient alone: a snapshot published from a
poisoned cache makes the pre- and post-restart views identically wrong, so also replay from the
last PRE-FAULT snapshot + raw tail logs and compare against the live cache, and assert that no
snapshot advanced across a poisoned transaction.

**Mechanisms (in-server, no new compose needed):**
- `memory_tracker_fault_probability` — per-query `Float` Setting (`Core/Settings.cpp:2312`), throws
  on allocation with the given probability. NOT debug-gated (`MemoryTracker.cpp:340-342`), so it
  works on the ordinary RelWithDebInfo soak image, and it faults allocations inside the CAS commit
  path (`stageManifest` -> `precommitAdd` -> ref-lane apply -> `promote`) — exactly the window the
  hardening targets.
- `cannot_allocate_thread_fault_injection_probability` — `ServerSetting`
  (`ServerSettings.cpp:233`), applied via config + `SYSTEM RELOAD CONFIG`
  (`InterpreterSystemQuery.cpp:1158` -> `CannotAllocateThreadFaultInjector::setFaultProbability`).
  Different class: thread creation fails, reaching the background pools (GC scheduler, merges,
  ref-lane dispatch) that the query-level setting cannot.

**Legs:** A = query-thread alloc faults over a soak-shaped workload window; B = thread-allocation
faults over the same workload; C = disarm both -> quiesce -> GC to fixpoint -> fsck -> RESTART the
node and assert the post-restart view (rebuilt from the durable journal) equals the pre-restart view
part-for-part and row-for-row (the diverged-ledger-cache oracle — nothing else in the suite catches
it).

**Oracle (queries ARE allowed to fail):** zero `LOGICAL_ERROR` / zero aborts in `err.log`; every
ACKED insert's rows present (S40-shaped acked-vs-lost); replicas agree; fsck `dangling=0` /
`unaccounted=0`, unreachable settles; GC rounds succeed again after disarm; no permanently wedged
ref lane (a lane wedged by an uncertain append must resolve or provably self-heal after remount —
assert it, do not assume); no query hung past a bound.

**Soundness guard (mandatory, S39-style):** assert the faults were actually exercised (nonzero
injected-failure count) or report `inconclusive` — never a vacuous pass. Same lesson as S07's
never-triggered manifest cap and S37's vacuous TTL leg.

**Optional stronger mode:** run the same card against a DEBUG image — `DENY_ALLOCATIONS_IN_SCOPE`
regions (debug-only) are then enforced under real allocation pressure, turning the soak into a live
check of the no-throw-install invariant.

**Params:** per-scale probability (dev low enough that the workload still progresses; ci/full
higher; plus a short high-probability burst), fault-window and settle durations.

## Suite-wide GC blindness: every scenario's GC verdict was vacuous (found 2026-07-25 by S42) {#gc-observation-vacuous-2026-07-25}

FIXED for the immediate cause (`c44cb6cbe44`), but the hazard that produced it is NOT fixed.

`framework/observe.gc_log_rows` selected `min_ack` from
`system.content_addressed_garbage_collection_log`. That column no longer exists, so the query raised
`UNKNOWN_IDENTIFIER`, the function's bare `except` converted it to `[]`, and every GC observation in
every scenario card came back empty — which made every GC verdict pass vacuously, including
`assert_gc_no_failed`.

Verified independently before acting on the agent's report: `system.columns` returns 0 rows for that
column, and `SELECT min_ack FROM system.content_addressed_garbage_collection_log` errors.

This is the SECOND occurrence of this exact class; the function's own comment already records the P9-era
one. Two follow-ups, neither done:

1. **The `except` must not turn a schema mismatch into an empty result.** An unreadable observation is not
   an observation of nothing. Distinguish "table absent / not yet flushed" (legitimately empty) from
   "query failed" (must surface, and should fail the run rather than silently pass every GC assert).
2. **`assert_gc_no_failed` passes on empty.** Any assert whose subject is a row set needs an explicit
   non-vacuity guard — the same discipline the TLA+ models use `_witness_*` configs for. Sweep the other
   `assert_*` helpers in `framework/` for the same shape.

Worth generalising: this is the third distinct instance this week of a HARNESS silently under-reporting
what the product actually said (the others: the 668 mount-fence classifier that killed a soak, and the
fsck detail-class whitelist that would have dropped `stale-edge`). The pattern is always the same — the
harness pins a name or a column the product later changes, and the mismatch degrades to silence rather
than to an error.
## S42-20260726T205952-1: GC log has 2 real (non-benign) Error finish row(s)

- **Logged (UTC):** 2026-07-26T21:22:46
- **Severity:** suspected-bug
- **Run:** 20260726T205952_S42_seed42
- **Observed:** GC log has 2 real (non-benign) Error finish row(s)

## S38-20260729T104557-1: forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-29T10:49:50
- **Severity:** suspected-bug
- **Run:** 20260729T104557_S38_seed20260729
- **Observed:** forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 20}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S43-20260729T104950-1: scenario raised: 'utf-8' codec can't decode byte 0xb5 in position 1: invalid sta

- **Logged (UTC):** 2026-07-29T10:50:13
- **Severity:** suspected-bug
- **Run:** 20260729T104950_S43_seed20260729
- **Observed:** scenario raised: 'utf-8' codec can't decode byte 0xb5 in position 1: invalid start byte

## S33-20260729T105013-1: forced GC left 87 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-29T10:53:35
- **Severity:** suspected-bug
- **Run:** 20260729T105013_S33_seed20260729
- **Observed:** forced GC left 87 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 87}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S33-20260729T105013-2: S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 87 RECLAIMABLE unreac

- **Logged (UTC):** 2026-07-29T10:53:35
- **Severity:** suspected-bug
- **Run:** 20260729T105013_S33_seed20260729
- **Observed:** S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 87 RECLAIMABLE unreachable object(s) (blobs/_manifests) permanently orphaned by concurrent explicit GC leaders (safety held: dangling=0); full residual by_prefix={'_manifests': 87}. The attempt-scoped generation fix should make a deposed leader's fold seal invisible and let the next honest round drain — a nonzero reclaimable residual means that invariant broke.

## S33-20260729T105013-3: forced GC left 87 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-07-29T10:53:35
- **Severity:** suspected-bug
- **Run:** 20260729T105013_S33_seed20260729
- **Observed:** forced GC left 87 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible GC leak; full residual by prefix: {'_manifests': 87}. bookkeeping-only residual (other=0) is expected and bounded.

## S30-20260729T105335-1: S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGe

- **Logged (UTC):** 2026-07-29T10:56:58
- **Severity:** suspected-bug
- **Run:** 20260729T105335_S30_seed20260729
- **Observed:** S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated.

## S30-20260729T105335-2: forced GC left 98 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-29T10:56:58
- **Severity:** suspected-bug
- **Run:** 20260729T105335_S30_seed20260729
- **Observed:** forced GC left 98 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 98}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S38-20260729T105718-1: forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-29T11:01:17
- **Severity:** suspected-bug
- **Run:** 20260729T105718_S38_seed20260729
- **Observed:** forced GC left 20 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 20}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S43-20260729T110117-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-29T11:06:04
- **Severity:** suspected-bug
- **Run:** 20260729T110117_S43_seed20260729
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S38-20260729T110740-1: forced GC left 45 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_mani

- **Logged (UTC):** 2026-07-29T11:11:40
- **Severity:** suspected-bug
- **Run:** 20260729T110740_S38_seed20260729
- **Observed:** forced GC left 45 UNCONDEMNED orphan object(s) (unreachable/dangling blobs/_manifests): {'_manifests': 45}. These are NOT in the two-phase pipeline (that would be pending-gc). If explicit GC was driven concurrently with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK (see BACKLOG): a divergent-fold abort orphans owner-removal events.

## S38-20260729T115218-1: scenario raised: counter probe on Node(localhost:8123) did not return ['CasRefAp

- **Logged (UTC):** 2026-07-29T11:53:26
- **Severity:** suspected-bug
- **Run:** 20260729T115218_S38_seed20260729
- **Observed:** scenario raised: counter probe on Node(localhost:8123) did not return ['CasRefApplyPoisoned', 'CasGcUnappliedFoldedTxns', 'CasRefRecoveryStreamHole'] — the binary does not have these counters, or the query shape changed; refusing to treat absence as zero

## S43-20260729T115326-1: scenario raised: name '_zstd_decompress' is not defined

- **Logged (UTC):** 2026-07-29T11:53:42
- **Severity:** suspected-bug
- **Run:** 20260729T115326_S43_seed20260729
- **Observed:** scenario raised: name '_zstd_decompress' is not defined

## S33-20260729T115342-1: S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 84 RECLAIMABLE unreac

- **Logged (UTC):** 2026-07-29T11:57:08
- **Severity:** suspected-bug
- **Run:** 20260729T115342_S33_seed20260729
- **Observed:** S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 84 RECLAIMABLE unreachable object(s) (blobs/_manifests) permanently orphaned by concurrent explicit GC leaders (safety held: dangling=0); full residual by_prefix={'_manifests': 84}. The attempt-scoped generation fix should make a deposed leader's fold seal invisible and let the next honest round drain — a nonzero reclaimable residual means that invariant broke.

## S33-20260729T115342-2: forced GC left 84 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possibl

- **Logged (UTC):** 2026-07-29T11:57:08
- **Severity:** suspected-bug
- **Run:** 20260729T115342_S33_seed20260729
- **Observed:** forced GC left 84 unreachable RECLAIMABLE object(s) (blobs/_manifests) — possible GC leak; full residual by prefix: {'_manifests': 84}. bookkeeping-only residual (other=0) is expected and bounded.

## S30-20260729T115708-1: S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGe

- **Logged (UTC):** 2026-07-29T12:00:43
- **Severity:** suspected-bug
- **Run:** 20260729T115708_S30_seed20260729
- **Observed:** S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated.

## S43-20260729T120731-1: quiescence failed: <urlopen error [Errno 111] Connection refused>

- **Logged (UTC):** 2026-07-29T12:12:08
- **Severity:** suspected-bug
- **Run:** 20260729T120731_S43_seed20260729
- **Observed:** quiescence failed: <urlopen error [Errno 111] Connection refused>

## S33-20260729T121208-1: S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 84 RECLAIMABLE unreac

- **Logged (UTC):** 2026-07-29T12:15:29
- **Severity:** finding
- **Run:** 20260729T121208_S33_seed20260729
- **Observed:** S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: 84 RECLAIMABLE unreachable object(s) (blobs/_manifests) permanently orphaned by concurrent explicit GC leaders (safety held: dangling=0); full residual by_prefix={'_manifests': 84}. The attempt-scoped generation fix should make a deposed leader's fold seal invisible and let the next honest round drain — a nonzero reclaimable residual means that invariant broke.

## S30-20260729T121529-1: S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGe

- **Logged (UTC):** 2026-07-29T12:18:55
- **Severity:** finding
- **Run:** 20260729T121529_S30_seed20260729
- **Observed:** S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) grew across create/drop iterations though no table stayed live — the D1 registry-removal / dropped-shard-reclaim guarantee is violated.

## S43-20260729T123126-1: quiescence failed: Node(localhost:8123) HTTP 400: Code: 36. DB::Exception: Table

- **Logged (UTC):** 2026-07-29T12:33:31
- **Severity:** finding
- **Run:** 20260729T123126_S43_seed20260729
- **Observed:** quiescence failed: Node(localhost:8123) HTTP 400: Code: 36. DB::Exception: Table default.w3_recreated (3e1f0a2b-4c5d-4e6f-8a9b-0c1d2e3f4a5b) is not replicated. (BAD_ARGUMENTS) (version 26.6.1.20000.altinityantalya) | sql=SYSTEM SYNC REPLICA w3_recreated

