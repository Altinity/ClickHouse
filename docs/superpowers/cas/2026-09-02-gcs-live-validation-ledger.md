---
description: 'Ledger of the 2026-09-02 live-GCS validation campaign for CAS: tasks, blockers, decisions, status reports.'
sidebar_label: 'GCS live validation ledger (2026-09-02)'
sidebar_position: 91
slug: /superpowers/cas/gcs-live-validation-ledger-2026-09-02
title: 'CAS live-GCS validation ledger, 2026-09-02'
doc_type: 'reference'
---

# CAS live-GCS validation ledger, 2026-09-02 {#cas-live-gcs-validation-ledger-2026-09-02}

Goal: run the three live legs on the real bucket `content-adressable-test-mfilimonov` with the
current `cas-gc-rebuild` binary. Nothing GCS-facing has run against Google since 2026-07-03, and
every GCS-touching change since (request isolation, unconditional blob publication, HEAD generation
validation, reload dialect pin, semantic wire keys) has only fake-GCS evidence.

Credentials: HMAC pair of the bucket's service account, in `utils/ca-soak/configs/gcs.env` (soak
compose) and `ci/local.env` (praktika `--env-file`). Both git-ignored. No `gcp_oauth` credential
source is available, so the `gcp_oauth` groups of the live gate will skip.

## Tasks {#tasks}

| № | Task | Status | Evidence |
|---:|---|---|---|
| 0.1 | Credentials in place, values identical in both files, `chmod 600` | DONE | sha256 compare, 2026-09-02 02:00 |
| 0.2 | Bucket smoke via `aws` CLI: list, put, head, delete | DONE | `tmp/gcs_live_20260902/` |
| 0.3 | GOOG4 object-level probe (`gcs_goog4_probe.py`) | DONE, 12/12 OK | `tmp/gcs_live_20260902/goog4_probe.log` |
| 0.4 | GOOG4 multipart probe | DONE, same as July: complete ignores `if-generation-match` | `goog4_mp_probe.log` |
| 0.5 | First mount attempt on the soak stand | FAILED: `Code: 48`, unverifiable versioning probe (`GetBucketVersioning` 403) | `logs_archive/pre_gcs_*`, ledger findings |
| 0.6 | Decision (user): unverifiable probe → warning + continue; verified `Enabled` → still refuse | DECIDED | this ledger |
| 0.7 | Code change, TDD: RED test, GREEN, docs, fresh-agent review, commit | DONE: GREEN 4/4, fresh-agent review APPROVE WITH NITS (nits applied), committed | `gtest_cas_backend_generation.cpp`, `CasObjectStorageBackend.cpp` |
| 0.8 | Rebuild `clickhouse` (release `build/`), rerun CAS gtest gate `CAS*` | DONE: rebuilt 02:18, 0 warnings; `CAS*` gate 2252/2252 OK (177 s) | `build/build_gcs_versioning_green.log`, `build/test_cas_gate_gcs_versioning.log` |
| 1 | `tests/integration/test_gcs_live` via praktika, HMAC groups | DONE: attempts 3, 4, 5, 7 each 13 passed / 0 failed / 14 skipped (9 `gcp_oauth`, 1 oauth mount, 4 ambiguity; all expected); attempt 6 lost one scenario to a connect timeout (F9); attempt 7 = final committed module `6fb06bac716`. Attempt 1 fixture (F4), attempt 2 three test assumptions (F6) | `tmp/gcs_live_20260902/attempt{3,4,5}_pytest_parallel.jsonl`, `praktika_test_gcs_live_attempt*.log` |
| 1.1 | Per-counter audit of the ordinary `system.events` assertions (docstring open question) | DONE: per-statement `ProfileEvents` (lease renewal moves Get/Head/Put process-wide); attempt 4 = 13/0/14 with the attribution | `tmp/gcs_live_20260902/attempt4_pytest_parallel.jsonl` |
| 2.1 | Soak stand up, mount succeeds, phase 1 smoke `--ops 300 --no-chaos` | stand up, both mounts `live`; smoke: model equality PASS on both replicas, fsck gate not executed (F5 harness container name); rerun after harness fix | `logs/ch1`, `system.cas_mounts` |
| 2.2 | Soak phase 3 `--duration 2h` with chaos (ch1/ch2 faults only) | PARTIAL: ran warmup, steady, mutations, ttl_pressure (t+0..2566s) with 0 failed queries, then died in gc_checkpoint on `SYNC REPLICA` timeout caused by F11; chaos/cliff/converge stages never reached. Rerun only after the F11 decision, the livelock is structural under fetch load. Started 02:39 (`--max-pool-gb 0`, env container overrides) | `utils/ca-soak/tmp/soak_gcs_phase3_2h_20260902.log`, `soak_gcs_phase3_2h_20260902.db` |
| 3 | Stateless CA lane pointed at GCS | DONE (stopped early on purpose): ~900 of 11,137 tests run, ~40% failed on `CREATE TABLE` with GCS 429 on the pool-wide `cas/ref_catalog` (F12); remaining verdicts would be noise behind that class. Config swap reverted after the stop. | `build/praktika_stateless_cas_gcs.log`, `ci/tmp/test_result.txt` |
| 4 | Record results: live-results doc, BACKLOG follow-ups item, `RUN_HISTORY.md` | PARTIAL: live-results doc and BACKLOG (`[relink-confirm-lane-livelock]`, `[gcs-hot-control-keys-429]`, `[gc-run-connect-failure-propagation]`, GCS follow-ups entry) updated; `RUN_HISTORY.md` deliberately not touched, it carries someone else's uncommitted edits, this ledger is the run record | this file |

## Blocked or out of scope {#blocked}

- `gcp_oauth` groups of the live gate: no ADC triple and no GCE host. 10 cases skip.
- Ambiguity arms of the live gate: need the TLS-terminating fault driver with the documented control
  contract. It does not exist. 4 cases skip.
- Soft delete on the bucket: not inspectable through the XML API; not confirmed by the operator yet.

## Decision needed: F11 relink-confirm liveness {#decision-f11}

`CasRefLedger::confirmExactRef` rule 3 refuses (`Unknown`) whenever the namespace lane has any pending
append or an active leader tenure. Under sustained fetch load each replica's own fetch traffic keeps its
lane busy, so two replicas can starve each other indefinitely (F11, confirmed by experiment). Options,
none implemented, all needing the two-model concurrency consult before code:

1. **Ref-scoped rule 3.** Refuse only when a pending or in-flight op names `ref_name` (or its
   namespace-wide removal). Keeps the linearization argument for the asked ref; costs a scan of
   `pending` under the existing lock.
2. **Bounded wait for quiescence** inside the confirm (e.g. up to a few hundred ms) before answering
   `Unknown`. Cheapest change; helps only when tenures are short, which on GCS with `_ckpt` 429 backoff
   they are not.
3. **Receiver-side damping.** When a confirm answers `Unknown`, back off the *whole* fetch pool for that
   source rather than per entry, so a wedged pair stops feeding both lanes. Does not fix the rule, but
   makes the livelock self-heal; can combine with 1 or 2.
4. **Operational workaround (works today):** alternate `SYSTEM STOP FETCHES` on one replica while the
   other drains, then swap; demonstrated 01:56-02:02 UTC.

## Findings {#findings}

- **F1 (2026-09-02 00:04)** The service account lacks `storage.buckets.get`. `GetBucketVersioning`
  returns 403, and the mount refused with `NOT_IMPLEMENTED` because the probe was fail-closed on an
  unverifiable answer. User decision: warn and continue; the bucket-versioning requirement stays an
  operator precondition when it cannot be verified. Minimal IAM fix for the strict path remains
  `roles/storage.legacyBucketReader` on the bucket.
- **F2 (2026-09-02)** Multipart complete on GCS still ignores `x-goog-if-generation-match`
  (re-measured). Consistent with the unconditional blob-publication design, which no longer relies
  on a conditional multipart completion.

- **F3 (2026-09-02 00:23 UTC, soak smoke)** GCS returns `429 SlowDown` ("The object exceeded the rate
  limit for object mutation operations") on the per-namespace checkpoint object
  `cas/ns/state/<ns>/_ckpt`: 106 hits on ch1 and 126 on ch2 within the first six minutes of a
  4-worker phase-1 smoke, all on that one key per namespace. GCS allows about one mutation per second
  per object name; every commit rewrites the same checkpoint key, so write throughput per namespace is
  capped at roughly 1 commit/s on GCS and each excess commit costs an SDK retry with backoff. Inserts
  still succeed (retries absorb it) but this is a GCS-specific hot-key hazard for every single-key
  control object. Follow-up belongs in the BACKLOG: rate-aware checkpoint coalescing or a
  generation-suffixed key for the GCS dialect.

- **F4 (2026-09-02 00:21 UTC, phase 1 attempt 1)** All 13 `gcs_hmac` cases failed at fixture setup: the
  node never started, `Code: 499 ... InvalidSecurity: The provided security credentials are not valid
  ... While checking access for disk live_hmac_plain`. Cause is the harness, not Google: the values in
  `ci/local.env` were wrapped in single quotes, docker `--env-file` passes quotes through literally
  (unlike a shell), and the suite's `html.escape` then wrote `&#x27;` into the generated config. Fixed
  by unquoting the file. Lesson: `ci/local.env` values must be bare `KEY=VALUE`; a hash check that
  strips quotes before comparing hides exactly this.

- **F5 (2026-09-02 00:31 UTC, soak smoke)** Phase-1 smoke on GCS: 300 ops, both replicas equal the
  model exactly at the first checkpoint (5928 rows, identical fingerprints), no operation failed, no
  lease loss. The checkpoint still exited 1 because `soak/run.py` hardcodes the fsck container
  `ca-soak-ch1-1`, which does not exist on the `ca-live-gcs` stand (`docker exec` → "container is not
  running", reported as `persistent-dangling`). Harness defect, not a CAS finding: the scenario
  framework already honours `CA_SOAK_FSCK_CONTAINER`, `soak.run` does not. Same hardcoding in
  `soak/chaos.py` (fault targets) and `soak/pool.py` (RustFS `du`).

- **F6 (2026-09-02, phase 1 attempt 2)** Three live-gate tests encoded assumptions the real service
  did not meet, none of them a product defect: (a) `system.cas_log.token` carries the 128-bit build id
  for `build_start`/`precommit`/`build_publish`/`build_abort` (`object_kind = none`), so the
  "every token is a numeric generation" check must be scoped to object rows; every manifest/blob token
  on live GCS was numeric. (b) A second `SELECT count()` over a Parquet object is answered from the
  per-file row-count cache and never reaches the Parquet metadata cache; reading a column does.
  (c) On a disk over an absent bucket the typed `S3_ERROR` arrives at `CREATE TABLE` (per-table
  access check), not at the first `INSERT`. Fixed in the test; attempt 3 green.

- **F7 (2026-09-02, review of the harness fix)** Pre-existing misclassification in the soak checkpoint: a
  `docker exec` that never ran the fsck applet returned a dict without `dangling`, and the checkpoint read
  `None != 0` as `INV-NO-LOSS` data loss before its `exit_code` gate. Third instance of this shape in the
  waiter's own history. Fixed: `run_fsck`/`run_dryrun` raise `FsckUnavailable` when nothing ran; the
  checkpoint degrades exactly as for `FsckTimeout` (loud skip, no fabricated verdict). Deferred from the
  same review: duplicated default container names across modules (7), the same import-time constant in
  four scenario cards (8), no local coverage for the `object_kind` filter (11).

- **F8 (2026-09-02 03:07, soak phase 3)** Read latency on a cacheless CAS disk over the WAN to GCS:
  `system.query_log` on ch1 for the soak's non-system SELECTs over 40 minutes shows n=537, p50 3.6 s,
  p99 15.7 s, max 30.8 s, and about 725 `S3GetObject` requests per SELECT (one ranged GET per column
  file per part, each paying WAN latency). Four soak SELECT workers hit their client timeout so far,
  non-fatal. Not a correctness finding; it is the read-side twin of the insert-path cost and the
  reason the stateless lane keeps a `cache` disk policy available. Consider a cache disk in front of
  `ca` for any WAN deployment.

- **F9 (2026-09-02 01:15 UTC, phase 1 attempt 6)** Two TCP connect timeouts to
  `storage.googleapis.com` within 13 s. One hit the `PUT` of a GC control object
  (`gc/gen/1/attempt/1/blob_target/0/0`) inside a synchronous `SYSTEM CAS GC RUN`, which surfaced it as
  `S3_ERROR` and failed the condemned-staged-retag scenario (green in attempts 2-5 and 7 if it passes).
  The other hit the mount-lease renewal of the same disk, which "entered retry after physical attempt 1"
  with 19 s of confirmed budget left and kept the lease. Environment, not Google and not a product
  defect. Two follow-ups for the BACKLOG: whether a manual `GC RUN` should absorb a connect-level
  failure the way the background scheduler does (next round retries), and that the live test's
  manual-round loop is strict by design and will report such blips as failures.

- **F10 (2026-09-02 01:12-01:22 UTC, soak phase 3)** A ten-minute storm of TCP connect timeouts to
  every Google front-end IP the client resolved (five addresses, 500-2900 log lines per minute on ch1,
  about 35k lines across both replicas), overlapping the `ttl_pressure` and `gc_checkpoint` stages and
  the live-gate attempt-6 failure (F9). Host-side causes ruled out: conntrack 519/262144, 8 sockets in
  TIME_WAIT, a fresh connect from the host takes 43 ms. Provider or path degradation, not the product.
  What the product did with it: zero failed INSERT/SELECT on either replica in that window (643 and
  623 finished, 0 exceptions), both mount leases held (renewal retried and succeeded), no `Code: 210`
  refusals, no 5xx. The cost was throughput and the 429 count on `_ckpt` climbing to ~1.5k per node.

- **F11 (2026-09-02, soak phase 3, FIXED (pending live gate), product)** Replication between the two GCS replicas wedged
  from 01:12 UTC until the soak died at ~01:53 on `SYSTEM SYNC REPLICA` (Code 159): ch1 queue 1397
  `GET_PART` + 175 `MERGE_PARTS`, ch2 906 `GET_PART` + 830 `MUTATE_PART` + 71 `MERGE_PARTS`, every entry
  failing with `NO_REPLICA_HAS_PART: Source ... did not prove it still holds the manifest it offered ...
  by relink`; the source side logged `Relink confirm is unproven (unknown)` 17,975 times on ch2 and
  17,299 on ch1, steadily ~4.5k per ten minutes long after the connect storm (F10) that started it.
  Replicas diverged to 123,258 vs 165,615 rows against a model of 247,461; 27,647 `build_abort
  abandoned` events since 01:10 (each retry writes a precommit-removal record and leaves debris for GC).
  Mechanism (hypothesis under test): `CasRefLedger::confirmExactRef` rule 3 answers `Unknown` whenever
  the namespace's ref lane has ANY pending append or an active leader tenure. Each replica is also a
  receiver, and its own failing fetches keep appending precommit-removals to its own lane, so neither
  side ever observes the other's lane quiescent: a symmetric livelock, widened on GCS by slow `_ckpt`
  writes (429 + WAN). No `Unknown` came from the exception path (0 "unanswerable" lines), both mounts
  stayed `live`, `wedged_namespace_count` 0. Experiment in progress: `SYSTEM STOP FETCHES` on ch1 to
  quiesce its lane and watch whether ch2's queue drains. RESULT (01:56-01:58 UTC): with ch1's fetches
  stopped, ch1 as a source answered zero `unproven` confirms and served 260 relink offers in two
  minutes; ch2's queue fell from 1701 to 50 and ch2 caught up from 165,615 to 260,895 rows. Mechanism
  CONFIRMED. Manual recovery procedure: alternate `SYSTEM STOP FETCHES` on one replica while the other
  drains, then swap. Candidate fix for decision: make rule 3
  ref-scoped (refuse only when a pending/in-flight op names `ref_name`) or let a busy lane answer after a
  bounded wait; needs the two-model consult the concurrency policy requires.

- **F12 (2026-09-02 ~02:00 UTC, stateless CAS lane over GCS, product)** About 40% of the first 900
  stateless tests failed at `CREATE TABLE ... MergeTree` with `Code: 499 ... The object exceeded the rate
  limit for object mutation operations`. The hot object is the pool-wide `cas/ref_catalog` (1,694
  `SlowDown` hits in the server log, plus 167 on per-namespace `_ckpt`): every table create or drop
  rewrites that one key, a parallel test suite does that many times per second, GCS allows about one
  mutation per second per object, and the conditional write is not retried on `SlowDown`, so DDL fails
  outright. Same root as F3 (single hot control keys on GCS), different key and a harder consequence
  (failed statements rather than throughput). The lane was stopped after ~900 tests: the failure class
  is established and the remaining verdicts would be noise behind it. Fix belongs with F3:
  rate-aware retry with backoff on `SlowDown` for conditional control writes, and/or fewer mutations
  of pool-wide keys (batch or shard the catalog).
- **F11 addendum (02:04 UTC)** The lane is fed by more than fetches: with ch2's fetches stopped but 13
  merges and 8 mutations executing, ch2 still answered `Unknown` to 891 of 898 confirms in two minutes
  and ch1's queue grew back from 605 to 1066. Any sustained local write activity on a replica blocks
  confirms for its peer under rule 3; the alternate-STOP-FETCHES workaround only helps while the
  quiesced side is otherwise idle.
  Once ch2's merges finished (02:08 UTC) ch1 drained to 22, ch2's fetches were re-enabled, and two
  minutes later both queues were empty (2 and 0), both replicas held exactly 244,024 rows and no confirm
  answered `Unknown`: the livelock is a liveness defect, not a data-loss one, and the stand converged
  with no manual repair beyond alternating `STOP FETCHES`.
- **F13 (2026-09-02 22:33 to 2026-09-03 00:50, F11 live gate, harness)** The phase-3 checkpoint's
  leak assert is unsatisfiable on a pool prefix that carries inherited debris. The bound is
  `50*reachable + 5000`; when the workload's last op empties the table `reachable` falls toward zero
  and the bound collapses to about 5000, while the residual stays where it is. The gate run failed on
  `unreachable=138462 did not converge after quiesce (reachable=1)`. The residual was not this run's:
  it read 138330 at the first checkpoint about 200 seconds in, and rose by only a few thousand over
  two hours, with `dangling=0` at every checkpoint. The assert therefore fires on pool history rather
  than on the code under test, and will fire on any future run against a used prefix. Options: run the
  live gate on a fresh prefix, or scale the bound by the pool's entry residual instead of by
  `reachable`.
- **F14 (2026-09-02, F11 live gate, harness)** A long checkpoint silently consumes the phase-3 chaos
  window. The stage timeline is wall-clock while checkpoint cost grows with pool size, so on this pool
  the first checkpoint outlasted the whole ten-minute timeline. `ChaosRunner` fires overdue faults
  rather than skipping them, so the failure is quiet: the first fault fired late and the other three
  were never reached, and nothing in the final report says three quarters of the schedule did not
  happen. The gate run got one graceful `ch1 restart` out of four scheduled faults, losing the long
  freeze that is the only scheduled fault which makes a mount lose its fence. Options: pause the stage
  clock while a checkpoint runs, or report fired-versus-scheduled faults in the gate report so a run
  that lost its chaos coverage cannot read as a clean pass.
- **F15 (2026-09-02, F11 live gate, pre-existing, product)** A single fold round holds the GC lease for
  hours on this bucket, so any stage that waits for a collection fixpoint cannot converge inside a
  ten-minute soak. Read from `system.cas_gc_log` on both nodes at 22:57 UTC. `ca_live_node2` started a
  scheduled round at 21:19:49, emitted its phase rows through `fold_seal_read` by 21:20:00, and 97
  minutes later had still written no `fold_ref_intake` row, no `Finish` row, and no later `Start` row.
  A `Phase` row carries that phase's duration and so is written on completion; `fold_ref_intake` is the
  phase after `fold_seal_read` in the order the `phase` column documents. So the round had not finished
  reading its ref-log intake, which is the O(pool) leg, against a backlog of about 138 thousand
  unreachable objects.
  The peer behaved correctly throughout: since its restart at 21:19:44, `ca_live_node1` ran 558 rounds,
  every one `NotALeader` with `error_code = 0`. That is a clean, cheap "someone else holds it", not
  churn and not a failed write.
  Duration evidence, all-time: `ca_live_node1` has 7 `Success` rounds with a median of 66.0 s and a
  maximum of 4384.7 s, and one `Aborted` round that ran 33379.6 s (9 h 16 m) before ending with error
  code 236, `gc/state moved during the round (another leader advanced it)`. `ca_live_node2` has never
  completed a successful round: all 3782 of its `Finish` rows are `NotALeader`.
  Nothing here is caused by the relink change and nothing is new to this run: `ca_live_node2` also
  carries an unfinished round from 10:40:02 and the 9-hour aborted round is from 11:16, both before the
  stand was recreated at 20:33. Two of the three unfinished rounds are simply rounds whose process was
  replaced mid-round, at the recreate and at the chaos restart.

## Status reports {#status-reports}

Appended by the 20-minute watchdog. Format: time, what is running, progress, anomalies.

- 02:05 Campaign started. RED test written and verified failing; GREEN applied; unit build next.
- 02:16 GREEN code applied; header comments, user docs, spec and BACKLOG note updated; `ninja unit_tests_dbms clickhouse` running (`build/build_gcs_versioning_green.log`). Next: GREEN filter run, `CAS*` gate, fresh-agent review, commit, stand up.
- 02:21 Rebuilt binary mounts on the real bucket: `GetBucketVersioning` 403 logged as WARNING by `S3ObjectStorage`, CAS warning logged, both replicas `live`, ch1 leader. `CAS*` gate at 1691 OK / 0 FAIL and running. Phase 1 relaunched under the `integration` alias.
- 02:23 `CAS*` gate green 2252/2252. Soak smoke first attempt died at argparse (`--checkpoint-every` is required for phase 1), relaunched with `--checkpoint-every 100 --max-pool-gb 0` (no RustFS to measure, budget must be off or the throttle fails closed to max). Praktika `test_gcs_live` container up. Review agent still running.
- 02:26 Soak smoke running (3.4k rows on ch1, both replicas active). Finding F3: GCS 429 SlowDown on `_ckpt` hot key, retries absorb it so far. Praktika pytest session in progress (no outcomes yet). Review agent still running.
- 02:29 Phase 1 attempt 1: node failed to start (F4, quoted values in ci/local.env). Unquoted, relaunching. Soak smoke passed its first checkpoint at 100 ops; F3 429s continue on `_ckpt`.
- 02:32 Soak smoke: data model PASS, fsck gate blocked by hardcoded container name (F5). Fixing the harness (env-overridable containers, TDD + review), then rerun. Phase 1 attempt 2 running.
- 02:35 Phase 1 attempt 2 done: 10/3/14. All CAS publication scenarios on real GCS passed (fresh streaming, duplicate adoption, concurrent publishers, blob above former cap, multipart, native staged copy, condemned retag). Analysing the 3 failures. Harness container-name fix: 6 new tests + 305 existing green.
- 02:38 C++ review verdict APPROVE WITH NITS; P1-P3 prose nits applied; committed. Live-gate test fixes (build ids in `cas_log.token`, count-from-files cache, refusal at CREATE) applied; phase 1 attempt 3 running. fsck pre-flight on the GCS pool running in ch1.
- 02:39 fsck pre-flight on the GCS pool OK (`dangling=0 stale_edge=0`, 1981 unreachable in the GC pipeline after the smoke). Soak phase 3 (2h, chaos) started with the env-overridable container names. Fresh-agent review of the harness fix and live-gate test fixes running. Phase 1 attempt 3 running.
- 02:41 Watchdog: phase 1 attempt 3 running (praktika container up, tests executing); soak phase 3 in warmup, metrics tick #2, both replicas up, process alive; harness/live-gate review agent still working (7 min). No hangs.
- 02:42 Phase 1 attempt 3: 13/0/14, exit 0. Every `gcs_hmac` live case Google-accepted: ordinary PUT/HEAD/GET/LIST/copy/delete/multipart, typed refusal, ETag-keyed Parquet cache, CAS mount with numeric generations, all seven publication scenarios. Next: 1.1 counter audit, review-2 verdict, commits.
- 02:44 Task 1.1 settled by construction: lease renewal moves S3Get/Head/PutObject process-wide (`CasServerRoot.cpp` get/head/putIfAbsent), so the ordinary test now attributes every counter to its statement via `query_log.ProfileEvents`; deletes stay proven by prefix-filtered log lines. Attempt 4 running to validate. Soak phase 3 warmup, 16.8k rows.
- 02:46 Soak phase 3 warmup at ~25k rows, tick #6; leases healthy (renewal 163/162, both live); one SELECT-worker HTTP timeout (non-fatal, server side saw a broken pipe at 00:44:11 UTC), 429 SlowDown on `_ckpt` continues (226/240) with no failed operation; no 5xx. Attempt 4 of the live gate running.
- 02:49 Phase 1 attempt 4 (per-statement counters): 13/0/14, exit 0. Task 1 and 1.1 closed. Awaiting the harness/live-gate review verdict, then a fresh review of the attribution change, then commits. Soak phase 3 continues.
- 02:54 Review 2: harness APPROVE WITH NITS, live-gate fixes REQUEST CHANGES (dead fall-through in the refused-request test, false comment). Fixed both, plus F7 and nits; harness suite 311 green. Attempt 5 running with the final test module; fresh review round 3 spawned. Soak phase 3 continues.
- 02:58 Phase 1 attempt 5 (final test module, review-2 fixes in): 13/0/14, exit 0. Waiting for review round 3 to commit the harness and live-gate changes.
- 03:07 Watchdog: soak phase 3 in `mutations` stage (t+1080s), 95.8k rows, 30 ticks, process alive, both replicas up; 4 SELECT-worker client timeouts so far (non-fatal). Review round 3 report written at 03:06, reading it. No hangs.
- 03:11 Review round 3: A/B APPROVE WITH NITS, C APPROVE. Applied A1-A6 (probe failures no longer swallowed in the first fixpoint read; no-summary run is unavailable regardless of exit code; wording; dryrun skip recorded) and B1-B5 (lease-renewal description corrected to conditional overwrite + get; stricter refusal discriminator; whitespace-safe fall-through; synchronous MOVE pinned; enumeration wording). Attempt 6 running. F8 read-latency finding recorded.
- 03:17 Phase 1 attempt 6 (round-3 nits): 12 passed / 1 failed / 14 skipped; the condemned-staged-retag scenario failed inside `SYSTEM CAS GC RUN` with `S3_ERROR` (green in attempts 2-5). Investigating the server log before classifying.
- 03:18 Attempt 6 failure classified (F9, connect timeout to Google). Live-gate test corrections and the `CasEvent.h` contract comment committed; attempt 7 running for a green final run. Harness committed earlier as `5404e06d9b9`.
- 03:22 Phase 1 attempt 7 (final committed module): 13/0/14, exit 0. Phase 1 closed. Soak phase 3 entering its chaos window; stateless lane on GCS queued behind it.
- 03:27 Watchdog: soak phase 3 in the `gc_checkpoint` stage since t+2566s (~03:17), checkpoint running (fixpoint wait + fsck over WAN), metrics ticks continuing (51), process alive, both replicas live, 5 SELECT-worker timeouts total. Phase 1 closed (attempt 7 green). No hangs.
- 03:49 Watchdog: soak checkpoint stage still running (detail fsck in ch1 ~6 min); metrics ticks paused since 03:39 during the checkpoint. ANOMALY under investigation: ~35k `connect timed out` lines to storage.googleapis.com across both replica logs and 429s up to ~1.5k per node; leases still live (renewal 531). Checking whether the connect storm is host-side (conntrack/ports) or provider-side.
- 03:50 Connect-timeout anomaly classified as F10 (provider/path, 01:12-01:22 UTC, no failed queries, leases held). Soak checkpoint stage still running its detail fsck; chaos window opens at t+2880s.
- 03:56 Soak phase 3 died at the gc_checkpoint stage: SYNC REPLICA timeout caused by a replication livelock (F11). Stand kept alive as the reproduction; experiment running (fetches stopped on ch1).
- 03:57 Phase 3 started (stateless CAS lane over GCS). F11 evidence grows: ch2 appended 12.4k precommit + 11.4k precommit-removal records in 17 min from failing fetches alone, with ~50 `_ckpt` 429s/min. Experiment (fetches stopped on ch1) result due.
- 03:59 F11 livelock confirmed experimentally (ch2 drained 1701→50 once ch1 stopped fetching). Step 2 running with the sides swapped to drain ch1 and restore the stand. Phase 3 stateless lane running.
- 04:02 F11 step 2: with ch2 quiesced, ch1 drained 1582→605 in 3 min (rows 123k→205k); waiting for ch1 < 30 to re-enable ch2 and check both converge. Stateless lane at ~800/11137 tests.
- 04:03 BACKLOG and live-results doc updated with today's results and the three new items; F11 decision options written into the ledger. Waiting for the stand to converge (ch2 fetches to be re-enabled once ch1 < 30) and for the stateless lane.
- 04:06 Stateless lane over GCS: F12 (pool-wide `ref_catalog` hot key → 429 → failed DDL) established after ~900 tests; stopping the lane. Stand: ch1 queue grew back once ch2 merges/mutations resumed (F11 addendum). Phases 1-3 all executed; ledger and BACKLOG carry the findings.
- 04:11 Stand converged: queues 2/0, rows 244,024 = 244,024, zero unproven confirms. Campaign closed; watchdog removed; stand left up as the F11 reproduction.
- 07:51 All GCS findings and fix options consolidated into `docs/superpowers/cas/BACKLOG/gcs.md` (index row in BACKLOG.md; the three items moved out of formats-and-storage.md leave a pointer).
- 09:46 F11 design brainstormed and approved: rule 3 keeps only the broken-lane states; safety rests on gate 0 and the `asMutableDeletingPart` ordering. Spec: `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md`. Next: TLA variant, two-model consult, plan.
- 10:05 User decision: per-task live gate on GCS is a ten-minute phase-3 soak; the two-hour soak runs once after all fixes as the campaign's closing gate. Spec and backlog order updated. Codex (gpt-5.6-sol, high) review of the F11 spec running.
- 10:53 Codex (gpt-5.6-sol, high) review of F11 spec rev 1: REQUEST CHANGES, blocker = live repoint retires an edge with the part still Active (`_sab_stalecache`). Rev 2 written: rule 3 ref-scoped on the sent `append_attempt`. Sending rev 2 to codex round 2 and to an independent Claude consult.
- 11:08 F11 spec revision 3: folds in codex round 2 (4 prose findings) and the Claude consult (APPROVE WITH NITS, 9 findings, 7 simplifications): model extension specified (sArmed, sTouches, witness), the four existing rule-3 tests named with reversed phase expectations, four OwnerTransition shapes, single fill site, docs list. Sending rev 3 to codex round 3.
- 11:21 F11 spec rev 3: Claude consult APPROVE (two editorial nits), codex round 3 REQUEST CHANGES on prose only (model extension still under-specified; the companion lane models `CaRefLaneCore`/`CaRelinkLaneComposition` encode the Ready-only seam and must change too; three more comments/results). Rev 4 written with both folded in; consult reports committed as `docs/superpowers/cas/2026-09-02-f11-spec-consults.md`.
- 11:26 Claude consult on rev 4: APPROVE WITH NITS (README targets, lane-model seam identifiers, RESULTS:43, sPending owner). Applied in place. Codex round 4 still running.
- 11:30 Codex round 4 on rev 4: REQUEST CHANGES, prose only (ConfirmCore transitions under-specified; lane Certify must keep CurrentRuntime and per-identity currency; composition observable modelled independently; three modules). Rev 5 written with the transition table. Codex round 5 next.
- 11:38 Claude consult on rev 5 §model: REQUEST CHANGES scoped to §model (guards admitted certification in broken lane states; witnesses for certification outside Ready; nextId, poison, sTouches operator, derived attempt_touches_identity, inert composition flag). Rev 6 applies all seven. Codex round 5 still running on rev 5.
- 13:52 Codex round 5 on rev 5: REQUEST CHANGES, four prose items; three coincide with the Claude consult's R5-1/R5-2/R5-4 already in rev 6, the fourth (lane-core propagation details) is delegated to the model author with the battery as the check; fence-loss phase mapping fixed. Claude consultant hit its session limit before checking rev 6. Design unchanged since rev 2.
- 14:22 F11 spec revision 7 (the simplification the user asked for): rule 3 reads the `MutationScope` every queued/carved lane item already carries; no attempt phases, no new API. Consults doc keeps the history of revisions 1-6.
- 14:23 Codex review of rev 7 could not run: OpenAI usage limit reached (resets 15:04 local); one-shot reminder set for 15:12 to rerun it. Independent Claude consult (fresh agent) on rev 7 running.
- 14:40 Fresh Claude consult on rev 7: REQUEST CHANGES, one real hole (a wedged chunk's items complete before the tenure ends, so exempting `Wedged` reopened the stale-row hazard) and one safety-bearing gap (declared scope never checked against ops). Rev 8: `Wedged` refuses table-wide, scope validated pre-durability, prose fixed.
- 14:46 Consult on rev 8: APPROVE WITH NITS (safety argument closed). Nits applied: full inventory of test items with a Ref{X} scope over many refs (6 sites), debug/sanitizer death split for the LOGICAL_ERROR test, two citations. Codex rerun scheduled 15:12.
- 16:09 Codex review of spec rev 8 (gpt-5.6-sol high, 265k tokens): REQUEST CHANGES on three prose/model-plan points (shape-aware `SenderPoison` and a post-durability witness in the confirm-core model, binding-equality currency in the lane core, test inventory = 11 append expressions not 6 items, `carved` lifetime wording); the design's code paths confirmed ("Closed"). All three verified and folded into spec revision 9 and the implementation plan `docs/superpowers/plans/2026-09-02-cas-relink-confirm-liveness.md` (plan committed as `713c3be6b50`, eight tasks: models, `rt.carved`, scope validation, fake-GCS liveness test RED-first, rule 3 + ProfileEvents, backlog, 10-min live gate). Report in the consults doc, round 6.
- 17:32 Plan execution started at 16:55. Task 1: `CaRelinkConfirmCore.tla` gets a second admitted shape (`noop`, a mutation of another ref), `SabotageTouchBlind`, and the ref-scoped rule 3, plus a review fix round. Confirm-core battery (`run_relinkconfirm.sh`) green: 15/15 rows, `ALL EXPECTATIONS MET` (7 sabotages, 3 green runs — `main`, `main2r`, `empty_receivers` — and 5 witnesses).
- 18:16 Task 2: the lane-core and composition models (`CaRefLaneCore`, `CaRelinkLaneComposition`) get the matching ref-scoped guards; a review fix round splits the certify-blocked sabotage into two independent arms (`sab_certifyblocked`, the new `sab_certifytouching`) after TLC's "Successor state is not completely specified" caught a modelling gap. Final batteries: `run_reflane.sh` 28/28 (13 sabotages, 1 honest, 14 witnesses), `run_relinklane.sh` 10/10, both `ALL EXPECTATIONS MET`.
- 18:53 Task 3: `RefTableRuntime::carved` now mirrors the tenure's carved items, so a confirm mid-carve still sees the mutation instead of losing it between `pending` and completion.
- 19:36 Task 4: a `Ref`-scoped lane item's ops are validated against its declared `MutationScope` before durability (`LOGICAL_ERROR` on a mismatch, split into a debug-build throw test and a `Cas*` death test for sanitizer builds).
- 20:39 Task 5: the two-node fake-GCS liveness case (`tests/integration/test_cas_gcs_relink_liveness`) reproduces the starvation, RED against the table-wide rule 3. At its original parameters (`CKPT_DELAY_MS=250`, `INSERTS_PER_NODE=40`) the run passed and proved nothing; escalated to `CKPT_DELAY_MS=1000`, `INSERTS_PER_NODE=80` per the plan's own escape hatch, both replication queues stuck at 79 and 80 entries past the 180s drain deadline, and each node logged the confirm refusal over 1,500 times (1501 and 1516) in the run.
- 22:07 Task 6, plus a review fix round: rule 3 now reads each pending or carved item's `MutationScope` — only a mutation of the exact asked-about ref, `Wedged`, or another broken lane state refuses. Five ProfileEvents counters attribute every refusal (`RefMutationInFlight`, `LaneWedged`, `LaneBroken`, `StateLockBusy`, and a fifth added in the fix round, `MountCannotSpeak`, for the two arms meaning this mount cannot speak for the namespace at all); design spec bumped to revision 10 to record the fifth counter. Confirm gtest suite 25/25 (`CASConfirmExactRef*`); full `CAS*` gate 2259 tests from 285 suites, 177700 ms, green. Integration `test_cas_gcs` 34 passed/0 failed (55.30s) and `test_cas_replicated_relink` 11 passed/0 failed (46.83s) were run against the PRE-fix-round tree (before `f4c87d0a6d8`), not re-verified after it: that commit touches `CasRefLedger.cpp`/`.h`, the file implementing `confirmExactRef` which `test_cas_replicated_relink` exercises end to end, but its diff to the two changed arms only adds a counter increment and a log line to a path that already returned `Unknown` under the same condition, so no behavioural change is expected, though this is not itself re-verification. Its bare `--test test_cas_gcs` selector does NOT also run the liveness suite, it matches only its own directory by exact name; the plan's claim to the contrary is corrected. `test_cas_gcs_relink_liveness` 1 passed/0 failed (386.72s) WAS run against the post-fix-round tree (`build/itest_cas_gcs_liveness_task6_fix1.log`), and its captured stdout in `ci/tmp/pytest_parallel.jsonl` prints `node1 refusal counters: ` and `node2 refusal counters: ` both with an empty result: on the five-counter build, BOTH nodes show zero refusals of any kind — not one of the five counters at zero, but no `system.events` row for any of them — because the ref-scoped rule only refuses when a queued or carved mutation names the exact ref being asked about, and in this workload each node's lane stayed busy with its own newer parts while its peer asked about a part committed seconds earlier. Open items carried into the live gate: `CASRelinkConfirmRefusedStateLockBusy` has no test at all (its non-blocking lock acquisition has no seam that forces a miss); three of the five counters (`RefMutationInFlight`, `MountCannotSpeak`, `StateLockBusy`) have unit-level fences only, so the live gate is the first place a break between the `ProfileEvents` increment and the `system.events` row would show for them; and the liveness case's two escalated constants were raised together, so whether the 1000ms delay alone reproduces at the original 40-insert count (roughly halving the ~6.5-minute suite) is unmeasured. BACKLOG (`[relink-confirm-lane-livelock]`) and this ledger updated; F11 status FIXED (pending live gate). Live gate next (Task 8).
- 00:50 (2026-09-03) Task 8, the F11 live gate: a ten-minute phase-3 soak on the real GCS stand. Stand first put on the fixed binary — the containers had been running a pre-fix build because a rebuild replaces the host file's inode and a running container keeps the mapping it started with; `ch1` and `ch2` recreated (Keeper untouched), three matching checksums `32701ffacdb49b53d10497ba21d789f9`, both CAS identities preserved, both mounts `live`. Command: `python3 -m soak.run --seed 20260902 --phase 3 --duration 10m --workers 6 --max-pool-gb 0 --metrics soak_gcs_f11_gate_10m.db` with the stand's container names in the environment and the RustFS fault slot pointed at `ch2`. **The gate does not pass as specified: criterion 1 fails on the exit code (1), not on divergence.** The soak died on `CHECKPOINT FAILURE: unreachable=138462 did not converge after quiesce (reachable=1, bound 50*reachable + 5000)`; the same failure record shows the model expecting an empty table and both replicas empty, and the two checkpoints that completed both agreed with the model at 21273 rows with `dangling=0 stale_edge=0`. Recorded as F13. **The behaviour under test passed every direct measurement.** In the run's window (container clocks are UTC, two hours behind the host, and the server logs are host-mounted and carry the previous day's pre-fix runs — both have to be handled before any log count means anything) `ch1` performed 134 fetch-by-relink operations and `ch2` 96, and `Relink confirm is unproven` — which the answering peer logs for every non-`Yes` answer, including the two arms of `confirmExactRef` that return `Unknown` without incrementing a counter — occurs zero times on either node, as do `did not prove it still holds` and `NO_REPLICA_HAS_PART`. So 230 confirms ran and all answered yes, with no refusal of any class, counted or uncounted; that log evidence also covers `ch1`'s life before its chaos restart, which its profile-event counters do not. Criteria 2 and 4 pass: both queues empty and non-readonly at the end with zero `did not prove` entries, and all five refusal counters zero across 7681 samples taken every ten seconds on both nodes (`build/soak_gcs_f11_counters.tsv`), the only non-values being `ch1` unreachable for five samples inside its restart window. Criterion 3 passes degenerately — all five tie at zero, so nothing dominates. Contrast on the same stand and pool: the pre-fix binary logged `Relink confirm is unproven` 11 times in container hour 01 and 2988 in hour 02, the second containing no completed relink at all. Two gate limitations, both harness-side. F13 above. F14: the phase-3 stage clock is wall-clock while each checkpoint drives the GC to a fixpoint through `cas-fsck` folds that ran to their 580s cap on this pool, so one checkpoint cost about half an hour, the timeline expired during the first one, and three of the four scheduled faults were never reached — including the long freeze, the only one that makes a mount lose its fence and therefore the one that would exercise `CASRelinkConfirmRefusedMountCannotSpeak`. Partial progress on the plan's open item about the counters reaching `system.events`: all five are registered under exactly their names on both live servers (`system.metric_log` carries a column per registered event), which rules out a break between declaration and the event registry, but no increment path fired, so the increment-to-`system.events` step is still unproven live for all five. `[relink-confirm-lane-livelock]` stays OPEN naming F13 and F14; what would close it is a re-run against a fresh pool prefix, which removes both the failing assert and the checkpoint cost that ate the chaos window. Report: `.superpowers/sdd/2026-09-02-cas-relink-confirm-liveness/task-8-report.md`; log `build/soak_gcs_f11_gate_10m.log`; metrics db `utils/ca-soak/soak_gcs_f11_gate_10m.db` (not committed).
- 01:00 (2026-09-03) Task 8 follow-up: criterion 3 re-read under the controller's ruling, and the `gc_checkpoint` stall diagnosed from the product's own log. **Criterion 3 now reads PASS on the stronger result, not "degenerate".** No confirm was refused for any reason: all five counters are zero on both nodes, and the answering peer's `Relink confirm is unproven`, which it logs for every non-`Yes` answer including the arms that refuse without incrementing a counter, occurs zero times in the run's window against 230 completed fetch-by-relink operations. Criteria 2 and 4 re-confirmed from a final sample at 00:57 local: both queues empty and non-readonly, zero delay, 2 of 2 replicas active, zero `did not prove` entries, all five counters zero. **Criterion 1 was NOT cut short by the controller** and is recorded as it happened: the soak ran to its own conclusion at 00:35 and exited 1 on the F13 leak assert, before the stop instruction arrived. Nothing was stopped, restarted or removed; all three containers are still up. New finding F15, checked against `system.cas_gc_log` on both nodes rather than taken from the harness aggregate: a single fold round holds the lease for hours, so a fixpoint-waiting stage cannot converge inside a ten-minute soak. Where my reading differs from the diagnosis I was handed, and the log is what I trusted: the soak-window `Finish` counts are 554 and 264, not 415 and 264; all-time they are 3785 and 3782, not 3280 and 3782; the seven successful rounds are `ca_live_node1`'s alone, while `ca_live_node2` has never completed one; there IS one error, an `Aborted` round of 9 h 16 m with code 236; `ca_live_node2` has two unfinished rounds, not one; and the round that opened at 20:33:22 is `ca_live_node1`'s and is a zombie its own chaos restart orphaned, so the lease is actually held by `ca_live_node2`'s round from 21:19:49. The corrected conclusion stands and is confirmed: this is round duration, not leadership churn, every `NotALeader` round is clean with `error_code = 0`, and none of it is caused by the relink change or new to this run. Ledger, backlog and gate report updated; no code and no harness parameter changed.
