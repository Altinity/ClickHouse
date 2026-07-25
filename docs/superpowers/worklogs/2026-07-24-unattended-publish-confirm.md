# Unattended round — publish-confirm + ref-lane exception safety (2026-07-24)

Spec: `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (rev.5)
Branch: `cas-gc-rebuild`. Start HEAD: (filled below.)

**User-set program:**
1. writing-plans for the spec.
2. Subagent-driven implementation (Opus 5 for hard tasks, Sonnet 5 for simple/medium; codex
   double-check on critically hard ones).
3. Run the new fault-injection scenario (S42).
4. 4-hour soak.

**Standing rules for this round:** no handwaving on suspected bugs — systematic debugging; fix only
when the fix is obvious AND does not touch protocol/guarantees, else BACKLOG. Watch correctness,
S3 budget, CPU/memory/disk. Long tasks (>20 min) via nohup + log file + monitor; never a
self-matching pgrep. Watchdog every 20 min. Commit with pathspec ONLY (shared checkout).

## Timeline

- start HEAD: 3ddad3c6009
- 00:01 UTC — worklog created; unattended round begins
- 00:05 UTC — writing-plans started. Two read-only explore agents dispatched (exchange/fetcher
  surfaces; CAS test+bench infra). Read myself: `RefTableRuntime` layout, `commitRefChunk` +
  chunk-boundary call sites, `RefTableState` members, TLA runner conventions
  (`docs/superpowers/models/`, `run_*.sh`, jar symlink present), ca-soak `Scenario` contract.
- 00:12 UTC — SPEC REFINEMENT found while planning (committed with the plan): do NOT materialize
  the A1 candidate before the PUT. A candidate sharing its COW base with the live state cannot fold
  in place, so pre-PUT materialization would force an O(n) base rebuild per chunk; the install
  needs no materialization (moving base+overlay is equally noexcept). Correct order: COW-copy →
  apply(real id) → PUT → noexcept move install (+atomic bumps) under DENY → today's in-place
  O(overlay) fold outside the deny region. Net cost vs today ≈ one cheap COW copy; the apply merely
  MOVES to before the PUT (where a throw is a clean pre-durability failure).
- 00:14 UTC — exchange/fetcher surfaces mapped: `REPLICATION_PROTOCOL_VERSION_WITH_CA_RELINK=10`,
  `CA_POOL_UUID_PARAM`/`CA_RELINK_COOKIE`/`CA_RELINK_COOKIE_VALUE`, `tryGetContentAddressedExchange`
  (dynamic_cast on the disk's metadata storage), sender branch `DataPartsExchange.cpp:249-280`
  (releases the part by scope exit at `:276`), receiver branch `:728-771` with the
  `fall_back_to_byte_fetch` lambda, `relinkPartToDisk` `:1107-1169`, `publishEntries`
  `PartFolderAccess.cpp:338-372` (beginPartWrite → adoptEvidence* → stageManifest → precommitAdd →
  promoteBuild; catch → abandon → rethrow).
- 00:26 UTC — PLAN COMMITTED `95d4605ecf2`: 17 tasks, ledger data-loss fix first. Spec amended with
  the no-pre-PUT-materialization refinement. Execution begins.
  Sequencing decision for this round (resource discipline: never build while a soak runs):
  Tasks 1-8 (Part A: the data-loss fix) -> build+gate -> S42 card + run -> 4h soak (nohup) ->
  low-CPU work during the soak (TLA model, Task 9) -> Part B after the soak.
- 02:18 UTC — watchdog: healthy. Baseline build FINISHED clean (`NINJA_EXIT=0`, unit_tests_dbms +
  benchmark_cas_ref_protocol both linked). Task 1 (Sonnet) COMPLETE — one characterization test
  appended to `gtest_cas_blob_indegree.cpp`; the agent traced the two-cursor merge to justify the
  expectation rather than observing it (correct discipline). Task 3 (Opus, ledger no-throw install)
  still working — no Pool/ edits in the tree yet. Resources: 333G free, 65G RAM available, load 1.7.
  Note: two multi-day-old `codex` processes (~4.6d and ~3.7d elapsed, ~70MB RSS each) and one
  `tail -f` on a review log are leftovers from EARLIER sessions, not this round; harmless, left
  alone (not mine to reap).
- 02:29 UTC — watchdog: healthy, Task 3 actively writing (files touched 02:28, ~1 min before the
  check). Edits in tree: CasRefLedger.cpp +128/-, CasRefLedger.h +19, CasRefProtocol.{h,cpp} +11/+28
  (the swap), CasRefCowMap.h +15 (COW swap), plus the new 109-line
  `gtest_cas_ref_install_safety.cpp`. Nothing building/running right now (load 0.28) — expected, the
  agent is edit-only and the controller owns the build. Resources unchanged: 333G free, 65G RAM.

## Finding F1 (2026-07-24 ~03:0x UTC) — pre-existing flaky hang in `RefWriterLaneExceptionSafety.FollowerNeverRunsStrandedLeaderClosure`

Surfaced while gating Task 3: the gate stopped producing output at that test, main thread at ~90% CPU,
state R, all other threads sleeping — a SPIN, not a deadlock.

ATTRIBUTION (not this round's regression, evidence not inference):
- the spin is in the TEST's own `while (refQueuePendingForTest(ns) < 1) yield()`;
- the faulted flush throws from the PRE-CARVE hook, which fires before the carve — `commitRefChunk`,
  the only function Task 3 modified, is never reached on that path;
- `git diff` of Task 3 touches no queue/leader bookkeeping (grep for `pending`/`leader_active` in the
  diff returns comment lines only);
- the test itself landed TODAY from the parallel stage1 round (`79c07d6cc3d`, `93a0f32e669`).

MECHANISM (reproduced, not guessed): t1 enqueues → takes the baton → the hook throws →
`completeOwnedItemsAndReleaseLeadership` erases the item from `pending`, all before the main thread is
scheduled to sample. `pending` is then 0 forever and the poll spins until the harness kills it.
- idle 32-core box: 10/10 PASS (why no gate ever caught it);
- pinned to one CPU (`taskset -c 3`, maximal descheduling): **8/8 HANG**.

FIX (test-only, no protocol surface): replace the racy poll with a deterministic handshake — the
leader PARKS at the pre-carve hook, the main thread waits for that, starts the follower, waits for
`pending >= 2` (safe: a parked leader drains nothing), then releases the leader to throw. This also
removes a latent vacuity: in the interleavings that did not hang, the "follower waits behind a
throwing leader" scenario was not actually exercised.

## Finding F2 (2026-07-24 ~03:2x UTC) — PRE-EXISTING RED: `RefWriterRecoverySeal.EmptyDeadRegionCarveOutStillReportsSameProcessNamespace`

Found by the Task-3 gate (the COMPREHENSIVE filter; see the lesson below). NOT this round's regression.

SYMPTOM: `Pool::open` throws `CORRUPTED_DATA` "server-root 'test' has no durable epoch object but a
mount lease exists … refusing to re-mint epoch 1" from `allocateWriterEpoch`
(`CasServerRoot.cpp:229`), so the test cannot even open its pool.

ATTRIBUTION: the whole failing stack is `Pool::open → mountWritable → allocateWriterEpoch`
(`CasPool.cpp:461/511`), reached before any ref-log flush. Task 3 touches only `commitRefChunk`, the
`RefTableState`/COW swaps and test files — not one frame of that stack. The guard itself landed
TODAY as `6094c1473ea` ("refuse to re-mint writer_epoch 1 while a mount object exists (Phase C)"),
after the test (`83a8d9f5c22`).

RCA so far: the guard fires on {no durable epoch object} × {data subtree EMPTY} × {mount object
Present} × {normal mint policy}. Only THIS test constructs that state —
`seedUncleanPredecessorMount` writes a mount via the production `claimMount` and nothing else, and
this is the only seal test whose dead region is deliberately empty (all 11 siblings pass). In
production that state is unreachable in the other order: `mountWritable` calls `allocateWriterEpoch`
— which writes the epoch object — BEFORE `claimMount` publishes the mount. So the guard looks
RIGHT and the FIXTURE looks unrealistic (a real predecessor at epoch 1 leaves an epoch object).

NOT FIXED HERE, deliberately: the minimal fix (seed the epoch object for this one test) is probably
right, but the fixture is the parallel session's, landed hours ago together with the guard, and I
cannot yet explain the invariant well enough to be sure a "fix" would not mask a genuine
availability bug (if the state IS reachable another way, refusing it bricks a pool). Handed over
rather than guessed at — logged to `docs/superpowers/cas/BACKLOG.md`.

LESSON (already known, re-confirmed): the narrow `Cas*:CA*` gate filter EXCLUDES `RefWriter*`, which
is why this red survived its own landing commit. The comprehensive filter is the only valid gate.
