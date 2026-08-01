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

The source inventory contains seventeen conditional death-test suites that are absent from the
release binary. The authoritative CA gate therefore runs the full release-visible suite intersection,
while a second debug invocation runs all seventeen generated death suites. This preserves both build
modes without silently excluding either side of the conditional tests.

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
logs. The authoritative complete gate is the release-visible 260-suite intersection in one invocation,
without the per-suite wrapper's artificial 60-second timeout; debug-only death suites are the separate
supplemental gate above. Nothing was excluded from either inventory.

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
