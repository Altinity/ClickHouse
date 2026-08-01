# Task 5 checkpoint 7.5c — immutable life runtime C++ core

## Scope and outcome

The ref runtime cache now has the accepted identity shape:

`RootNamespace -> RefNameSlot -> RefTableRuntime(NamespaceLifeId, admitted_fence_generation)`.

`RefTableRuntime::life` and `RefTableRuntime::admitted_fence_generation` are construction-only
`const` values. The name slot is a non-authoritative current-pointer cache: exact predecessor
retirement or remount supersession can detach it while existing `shared_ptr` holders continue to own
the predecessor object, and a later catalog observation can publish a distinct successor. No
unbound runtime, mutable optional life, reset, reopen, rebind, or compatibility adapter remains.

This checkpoint changes only the runtime semantic core and its tests. It does not implement the
janitor or `GcMaintenanceState`.

## Production boundary

- Replaced name-keyed get-or-create with explicit resident lookup, exact-life acquire/publish,
  readable catalog acquisition, mutable catalog/birth acquisition, and exact-pointer detach.
- Catalog observation or successful birth now precedes runtime construction. Exact acquisition
  admits only the observed life and accepted mount-fence generation; identity conflict returns the
  typed retry-later outcome rather than rewriting the resident runtime.
- Catalog retirement sets a monotone per-runtime invalidation bit and erases the slot only when both
  exact life and pointer still match. A delayed predecessor invalidation therefore cannot detach a
  successor.
- Remount marks every captured predecessor superseded, drains/quiesces it, clears the name slots, and
  lets post-arm observation construct distinct runtimes. A failed remount cannot publish a runtime at
  either the fence-loss-only generation or the rejected generation.
- Read, append, recovery, wedge resolution, terminal append, and snapshot publication retain the
  captured runtime. Their admission checks include exact immutable generation plus monotone catalog
  retirement/remount supersession, so predecessor work cannot re-resolve through the current name
  slot.
- Snapshot publication carries the captured runtime through background dispatch and rechecks it
  before object PUT, immediately before each `_ckpt` CAS attempt, and before local adoption.
- `confirmExactRef` and every observational `*ForTest` accessor use resident lookup only. They perform
  no catalog/backend I/O and do not allocate, recover, or rebind a runtime. The two explicitly
  mutating test seams retain production acquisition.
- Cache eviction, shutdown, recovery assignment, Task 4b read-path assignment, and same-life
  self-remount were converted to name-slot semantics. External predecessor holders do not occupy the
  slot or delay successor publication.

The removed API/symbol audit is clean for `getRefTableRuntime`,
`pinRefTableRuntimeToLife`, `prepareInvalidatedRuntimeForReuse`,
`recoverRefTableIfNamespaceExists`, `lifeUnderLock`, mutable life assignment, and life reset.

## TDD and race evidence

The implementation was developed through retained failing-first checkpoints followed by focused
GREEN runs:

| Obligation | RED evidence | Final evidence |
| --- | --- | --- |
| Cold confirmation/read must not allocate | `build/build_task5_checkpoint75c_runtime_read_noalloc_red_build.log`, `build/test_task5_checkpoint75c_runtime_read_noalloc_red.log` | covered by focused obligations and expanded CA tests |
| Terminal append must retain the captured Removing predecessor | `build/test_task5_checkpoint75c_runtime_terminal_append_red3.log` | `build/build_task5_checkpoint75c_runtime_terminal_append_green.log` and focused obligations |
| Publisher cannot retarget reborn successor | `build/test_task5_checkpoint75c_runtime_publisher_final_red.log` | `build/test_task5_checkpoint75c_runtime_publisher_ckpt_green.log` |
| Publisher cannot advance predecessor `_ckpt` after retirement | `build/test_task5_checkpoint75c_runtime_publisher_ckpt_red.log` | `build/test_task5_checkpoint75c_runtime_publisher_ckpt_green.log` |
| Captured reader/append old-holder races | deterministic pause seams introduced before the respective state lock/enqueue | `build/test_task5_checkpoint75c_runtime_old_holder_races.log` |
| Failed remount publishes no rejected-generation runtime | failing-first runtime-generation assertions | `build/test_task5_checkpoint75c_runtime_failed_remount.log` |

The race tests use real removal, GC drain, same-name rebirth, the same `RootNamespace`, and the same
writer epoch. They prove distinct runtime ids and exact life ids, unchanged successor content, and
the specified stale/`NotFound`/retry outcomes. The publisher `_ckpt` oracle records the existing
janitor-owned checkpoint token/body before predecessor resume and proves it does not change; absence
would be an invalid oracle because retained debris is legal.

Additional pins cover:

- nonzero predecessor cursor/coverage followed by zero-inherited successor state;
- commit-then-throw erase, replacement winner, later reconciliation, and post-LIST reconciliation;
- successor publication followed by delayed exact predecessor invalidation;
- same-life self-remount with a distinct runtime and accepted post-arm generation;
- recovery and Task 4b read-path assignment origins;
- cache eviction and shutdown with live external predecessor holders.

## Validation

| Gate | Result |
| --- | --- |
| Final release `unit_tests_dbms` build | PASS; `build/build_task5_checkpoint75c_runtime_final.log` |
| Focused immutable-runtime obligations | PASS; `build/test_task5_checkpoint75c_runtime_obligations.log` |
| Expanded runtime/CA regression filter | PASS, 96/96; `build/test_task5_checkpoint75c_runtime_expanded_green2.log` |
| Recovery-streaming follow-up | PASS, 8/8; `build/test_task5_checkpoint75c_runtime_recovery_streaming.log` |
| Completed-removal frontier follow-up | PASS, 2/2; `build/test_task5_checkpoint75c_runtime_gc_frontier_observers.log` |
| Reviewed `CaRefLaneCore` matrix | PASS, 26/26 expected verdicts; `build/test_task5_checkpoint75c_runtime_reflane_tla.log` |
| Full pre-fold TLA+ runner | PASS, 18/18 expected verdicts; `build/test_task5_checkpoint75c_runtime_prefold_tla.log` |
| Complete release-visible CA unit gate | PASS, 1,761/1,761 tests from 260 suites; 0 failed/skipped, 2 disabled; `build/test_task5_checkpoint75c_runtime_complete_ca_release.log` |
| Supplemental debug death-test gate | PASS, 30/30 tests from all 17 generated `*DeathTest` suites; 0 failed/skipped/disabled; `build_debug/test_task5_checkpoint75c_runtime_death_suites.log` |
| `git diff --check` and removed-API static audit | PASS |

## Important fix round: stale admission after retirement

Independent re-review found four remaining admission windows in the immutable-life boundary. Commit
`107eb04464c` closes all four without weakening the immutable-runtime shape:

- Recovery now treats exact catalog-life invalidation as a monotone cancellation at every unlocked
  I/O boundary, every seal attempt, the `_ckpt` contribution, final install, and both transient-retry
  gates. A real `_log` `GET` pause followed by exact predecessor deletion and same-name rebirth proves
  zero predecessor seal `PUT`, zero predecessor `_ckpt` CAS, zero recovery-result publication, and
  unchanged successor keys/checkpoint.
- Wedge retry checks exact-life invalidation both in the request controller's pre-send predicate and
  after I/O before adoption. Its deterministic pause is immediately before `slotOccupy`; retirement
  and rebirth in that pause produce zero retry send, zero resolution read, zero adoption, and no
  successor mutation.
- A cold readable catalog observation carries a process-local monotone catalog epoch through its
  object-store `GET` to slot publication. Every in-process catalog mutation advances the epoch before
  attempting durable mutation, including the GC lifecycle reconciler. A stale `Live` L1 observation
  therefore cannot occupy an empty name slot after exact L1 deletion and L2 rebirth; the stale caller
  receives retry-later and a fresh caller attaches L2. This closes the no-prior-slot ABA without a
  second catalog `GET` on every cold read.
- `CasMountRuntime::armMountFence` now publishes the new generation before clearing `lost`. The final
  release store of `lost = false` opens the fence only after the new generation is visible, eliminating
  the loss-only-generation admission interval.

The catalog-epoch producer inventory is explicit in code: `createNamespace`, every
`completeCreation` arm, `reconcileStaleCreator`, `cancelStalledCreating`, `beginRemoving`, exact-life
invalidation, and `CatalogLifecycleReconciler` all advance it before mutation. Spurious advances are
safe retry costs; late advances would be unsafe.

### Fix-round TDD evidence

Each test was mutation-checked against only its new safety condition. The invalid initial wedge
fixture was discarded after review found its fault substring named a sentinel life that production
had not admitted; the retained `red2` run explicitly admits that incarnation and reaches the real
wedge pause.

| Obligation | Genuine RED evidence | Final GREEN evidence |
| --- | --- | --- |
| Fence re-arm opens no loss-only generation | `build/task5_checkpoint75c_fix_round1_arm_red_test2.log` | `build/task5_checkpoint75c_fix_round1_final_focused_green.log` |
| Cold L1 observation cannot publish after L2 rebirth | `build/task5_checkpoint75c_fix_round1_stale_red_build2.log`, `build/task5_checkpoint75c_fix_round1_stale_red_test.log` | `build/task5_checkpoint75c_fix_round1_final_focused_green.log` |
| Retired wedge retries send/adopt nothing | `build/task5_checkpoint75c_fix_round1_wedge_red2_build.log`, `build/task5_checkpoint75c_fix_round1_wedge_red2_test.log` | `build/task5_checkpoint75c_fix_round1_wedge_fixture_green_test.log`, `build/task5_checkpoint75c_fix_round1_final_focused_green.log` |
| Retired recovery writes/installs nothing | `build/task5_checkpoint75c_fix_round1_recovery_red4_build.log`, `build/task5_checkpoint75c_fix_round1_recovery_red4_test.log` | `build/task5_checkpoint75c_fix_round1_counter_green_test.log`, `build/task5_checkpoint75c_fix_round1_final_focused_green.log` |

The strengthened recovery mutation run is deliberately multi-boundary: removing all six retirement
gates makes the paused predecessor issue one seal `PUT`, advance `_ckpt`, and increment the detached
runtime's recovery-install counter. Restoring the gates makes every one of those oracles stay unchanged.

### Fix-round validation

| Gate | Result |
| --- | --- |
| Final release `unit_tests_dbms` build | PASS; `build/task5_checkpoint75c_fix_round1_postred_final_build.log` |
| Four deterministic race tests | PASS, 4/4; `build/task5_checkpoint75c_fix_round1_final_focused_green.log` |
| Relevant full suites | PASS, 57/57; `build/task5_checkpoint75c_fix_round1_relevant_suites.log` |
| Complete release-visible CA gate | PASS, 1,765/1,765 from the exact 260-suite intersection; 0 failed/skipped, 2 pre-existing disabled; `build/task5_checkpoint75c_fix_round1_complete_ca_release.log` |
| Final debug `unit_tests_dbms` build | PASS; `build_debug/task5_checkpoint75c_fix_round1_debug_build.log` |
| Regenerated debug-only death-test complement | PASS, 31/31 from all 18 suites; 0 failed/skipped/disabled; only the standard GoogleTest fork/thread warnings; `build_debug/task5_checkpoint75c_fix_round1_death_suites_v2.log` |
| Reviewed `CaRefLaneCore` matrix | PASS, 26/26 expected verdicts; `build/task5_checkpoint75c_fix_round1_reflane_tla.log` |
| Full pre-fold TLA+ runner | PASS, 18/18 expected verdicts; `build/task5_checkpoint75c_fix_round1_prefold_tla.log` |
| `git diff --check` | PASS |

The base-checkpoint **CA-scoped** source inventory contained seventeen conditional death-test suites
absent from the release binary. The fix round adds `CasRefCatalogRemovalDeathTest`, bringing that
CA-scoped complement to eighteen suites. The authoritative CA gate therefore runs the full
release-visible CA suite intersection, while a second debug invocation runs the regenerated
`Cas*DeathTest` complement. This preserves both build modes for the CA gate. The later full-binary
inventory in fix round 2 corrects this historical scope: two non-CA debug-only death suites were not
part of the 18-suite CA list, and the actual whole-binary complement is 20 suites / 36 tests.

The first diagnostic debug per-suite wrapper exposed six suites whose old tests treated a raw
`armMountFence` call as a usable remount, or used observational accessors to materialize state. The
production admission checks were not weakened. The tests now:

- drive a real, fast `gc_fenced` remount before retrying recovery;
- expect the typed `NETWORK_ERROR` when they intentionally keep a stale-generation runtime;
- arm the checkpoint hook only after its baseline read, proving the intended mid-attempt seam;
- treat an untouched resident-only lane observation as `Closed` and require a real operation before
  later `Ready` assertions;
- use epoch-only mutation for tests of wire linkage/self-pointer encoding rather than accidentally
  testing runtime supersession;
- use backend/wedge/tail/seal evidence after fence loss instead of issuing a production read that is
  required to refuse.

All six affected suites pass independently in the `build_debug/test_task5_checkpoint75c_runtime_*_green2.log`
logs. The authoritative complete CA gate is the release-visible 260-suite intersection in one
invocation, without the per-suite wrapper's artificial 60-second timeout; CA debug-only death suites
are the separate supplemental gate above. Nothing was excluded from either **CA-scoped** inventory;
the fix-round-2 whole-binary inventory and 20-suite run below cover the two additional non-CA suites.

## Self-review

The lock order remains `ref_queue_mutex -> state_mutex`. Catalog/backend I/O, recovery, publisher
settling, and waits occur without holding `ref_queue_mutex`. Runtime invalidation and supersession
are monotone. Exact detach compares pointer identity after checking the immutable life. All runtime
identity accessors are resident-only and the missing-name confirmation pin checks both zero slots and
zero runtimes in addition to zero backend requests.

No Step 8 janitor, maintenance cursor, `GcMaintenanceState`, or mechanical Task 13 split was added.

## Concerns

None in the implemented boundary. Independent concurrency/design review follows before checkpoint
7.5d and before Step 8.

## Important fix: catalog epoch publication serialization

The scoped review of `107eb04464c` found that `noteCatalogMutation` advanced
`catalog_lifecycle_epoch` without participating in the same `ref_queue_mutex` critical section as
`acquireRefTableRuntime`'s epoch comparison and empty-slot publication. `noteCatalogMutation` now
signals the test-only `BeforeRefQueueLock` phase, takes `ref_queue_mutex`, signals
`AfterRefQueueLock`, and advances the epoch. The critical section contains no catalog or backend I/O.

`CatalogMutationSerializesWithColdRuntimePublication` pauses a cold reader after its epoch comparison
while it holds `ref_queue_mutex`. The mutator reaches `BeforeRefQueueLock`, cannot reach
`AfterRefQueueLock` until the reader releases the mutex, then advances the epoch and exact invalidation
detaches the predecessor. The test confirms no predecessor runtime remains resident and a fresh read
attaches the successor life.

| Gate | Result |
| --- | --- |
| ASan incremental `unit_tests_dbms` build | PASS; `build_asan/build_stageb_runtime_fix2_green.log` |
| Controlled lock-removal mutation | RED, expected `mutation_after_lock` assertion; `build_asan/test_stageb_runtime_fix2_batched_red.log` |
| Byte-exact source restoration | PASS; SHA-256 check against `tmp/stageb_runtime_fix2_green_cpp.sha256` |
| Focused final cold-reader pair | PASS, 2/2; `build_asan/test_stageb_runtime_fix2_final.log` |

## Important fix round 2: backend-authoritative cold runtime admission

The process-local `catalog_lifecycle_epoch` and its producer inventory were insufficient authority:
another `Pool` sharing the backend can replace catalog life `L1` with `L2` without advancing state in
the reader's process. The local epoch, `noteCatalogMutation`, its `Pool` forwarder, every producer
call, and the serialization-only test seams are deleted.

A cold readable empty slot now captures mount-fence generation `G`, reads catalog cut `C1`, requires
an unambiguous exact `Live` life `L`, and immediately before publication reads `C2`. Publication is
allowed only when both the backend token and the decoded canonical catalog value are unchanged. The
existing queue-locked acquire then rechecks `G` and the exact name slot before publishing immutable
`(L, G)`. The warm resident path returns before either catalog read and therefore remains zero-GET.

### Fix-round-2 TDD evidence

- `ColdReadRejectsCatalogLifeReplacedWithoutLocalInvalidation` was genuine RED: after a paused `C1`,
  exact durable `L1 -> L2` replacement and an `L2` checkpoint, the old code returned normally,
  installed runtime id 1 for `L1`, and prevented a fresh read from attaching `L2`.
- `ColdReadRejectsReplacementByExternalPoolActor` and
  `ColdReadRejectsUnrelatedCatalogMutationBetweenObservations` were independently genuine RED: the
  old local epoch observed neither an external actor's replacement nor an unrelated shared-catalog
  mutation, and published a runtime from stale `C1` in both cases.
- `WarmReadableRuntimeDoesNotReadCatalog` was a non-vacuous green performance guard before and after
  the fix: it primes a resident runtime, resets backend counters, repeats the read, and observes zero
  `ref_catalog` GETs.
- The exact mutation control retained `C2` and `catalog_changed` but omitted only the retry branch.
  It compiled without warnings; all three safety tests went RED while the warm zero-GET guard stayed
  green. Restoring only that branch returned the focused set to 4/4 green.

### Fix-round-2 validation

| Gate | Result |
| --- | --- |
| Final release `unit_tests_dbms` build | PASS; `build/task5_runtime_cold_catalog_final_build.log` |
| Focused catalog-admission obligations | PASS, 4/4; `build/task5_runtime_cold_catalog_final_tests.log` |
| Full runtime-identity suite | PASS, 7/7; `build/task5_runtime_cold_catalog_runtime_identity_suite.log` |
| Full `RefWriter*` suite | PASS, 91/91; `build/task5_runtime_cold_catalog_ref_writer_suite.log` |
| Warning-free exact retry-branch mutation | expected RED for all 3 safety tests, warm guard PASS; `build/task5_runtime_cold_catalog_mutation_red_build_exact.log`, `build/task5_runtime_cold_catalog_mutation_red_tests.log` |
| Reviewed `CaRefLaneCore` matrix | PASS, 26/26 expected verdicts; `build/task5_runtime_cold_catalog_reflane_tla.log` |
| Full pre-fold TLA+ runner | PASS, 18/18 expected verdicts; `build/task5_runtime_cold_catalog_prefold_tla.log` |
| CA debug-only death complement | PASS, 31/31 from 18 suites; `build_debug/task5_runtime_cold_catalog_death_suites.log` |
| Full current debug-only death complement | PASS, 36/36 from 20 suites; `build_debug/task5_runtime_cold_catalog_full_debug_only_death_suites.log` |
| Complete release-visible CA gate | BLOCKED by the concurrent Step 8 janitor integration: 1,782/1,786 pass and the 4 failures are its extra catalog GET, broadened hole accounting, and intended dead-life checkpoint deletion; `build/task5_runtime_cold_catalog_complete_ca_release.log` |
| `git diff --check` and removed-local-epoch static audit | PASS |
