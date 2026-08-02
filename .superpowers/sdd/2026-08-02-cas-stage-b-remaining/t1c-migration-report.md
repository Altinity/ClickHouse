# CA fixture seam migration report

## Scope

Migrated the test-side `NamespaceLifeId::stageATransition` calls to
`fixture::fixtureLife`, helper `casAdmitEntry(backend, layout, ns)` calls to
`fixture::admitLive`, and helper `writeRefLogTxnRaw(backend, layout, txn)` calls to
`fixture::writeRefLogRaw`. The protected implementation in
`src/Disks/tests/cas_test_helpers.h` was not modified.

The table counts old source occurrences per touched file. The `stageATransition`
total is 284 actual `NamespaceLifeId::stageATransition(...)` occurrences plus
one separately updated comment-only `stageATransition(...)` occurrence. The helper
totals are 67 `casAdmitEntry` calls, 85 `writeRefLogTxnRaw` calls, and 14
test-side `resolveLifeOrSentinel` calls.

| File | `stageATransition` -> `fixtureLife` | `casAdmitEntry` -> `admitLive` | `writeRefLogTxnRaw` -> `writeRefLogRaw` | `resolveLifeOrSentinel` -> `lifeIfCataloged` |
|---|---:|---:|---:|---:|
| `src/Disks/tests/gtest_cas_bootstrap_ordering.cpp` | 2 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` | 3 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_fence_generation.cpp` | 11 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_fsck.cpp` | 1 | 7 | 9 | 0 |
| `src/Disks/tests/gtest_cas_gc_ack_floor.cpp` | 1 | 1 | 1 | 0 |
| `src/Disks/tests/gtest_cas_gc_arithmetic_intake.cpp` | 9 | 6 | 0 | 0 |
| `src/Disks/tests/gtest_cas_gc_bounded_walk.cpp` | 4 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_gc_fold.cpp` | 1 | 0 | 0 | 1 |
| `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp` | 9 | 13 | 11 | 6 |
| `src/Disks/tests/gtest_cas_gc_hold_grammar.cpp` | 14 | 5 | 0 | 0 |
| `src/Disks/tests/gtest_cas_gc_rebuild.cpp` | 0 | 0 | 6 | 0 |
| `src/Disks/tests/gtest_cas_gc_round_defer.cpp` | 1 | 1 | 0 | 0 |
| `src/Disks/tests/gtest_cas_gc_shard_incarnation.cpp` | 1 | 2 | 1 | 0 |
| `src/Disks/tests/gtest_cas_holey_list_detector.cpp` | 0 | 0 | 0 | 1 |
| `src/Disks/tests/gtest_cas_inspect.cpp` | 10 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_layout.cpp` | 12 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp` | 8 | 2 | 0 | 0 |
| `src/Disks/tests/gtest_cas_namespace_file_request_profile.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_namespace_janitor.cpp` | 1 | 1 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ns_file_incarnation.cpp` | 0 | 0 | 0 | 1 |
| `src/Disks/tests/gtest_cas_observability.cpp` | 4 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp` | 0 | 6 | 3 | 0 |
| `src/Disks/tests/gtest_cas_part_folder_access.cpp` | 3 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_part_write.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_pool.cpp` | 22 | 2 | 6 | 0 |
| `src/Disks/tests/gtest_cas_rebuild_condemn_nothing.cpp` | 9 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_recovery_grounding.cpp` | 1 | 0 | 19 | 0 |
| `src/Disks/tests/gtest_cas_recovery_streaming.cpp` | 11 | 0 | 4 | 0 |
| `src/Disks/tests/gtest_cas_ref_carve.cpp` | 7 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_catalog_birth_wiring.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_chunk_preparation.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp` | 7 | 1 | 1 | 1 |
| `src/Disks/tests/gtest_cas_ref_ckpt.cpp` | 20 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_ckpt_join.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_contiguous_alloc.cpp` | 0 | 0 | 0 | 1 |
| `src/Disks/tests/gtest_cas_ref_gc.cpp` | 9 | 7 | 4 | 0 |
| `src/Disks/tests/gtest_cas_ref_install_safety.cpp` | 13 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_intake.cpp` | 1 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_recovery_cas_walk.cpp` | 16 | 5 | 2 | 0 |
| `src/Disks/tests/gtest_cas_ref_wedge_every_attempt.cpp` | 24 | 0 | 0 | 0 |
| `src/Disks/tests/gtest_cas_ref_writer.cpp` | 38 | 5 | 18 | 3 |
| `src/Disks/tests/gtest_cas_retirement_sweep.cpp` | 5 | 3 | 0 | 0 |
| `src/Disks/tests/gtest_cas_writer_duties.cpp` | 1 | 0 | 0 | 0 |

Totals: 284 + 1 stage/comment, 67 `casAdmitEntry`, 85 `writeRefLogTxnRaw`, and
14 `resolveLifeOrSentinel` test sites.

## `resolveLifeOrSentinel` decision table

All 14 test sites had already admitted or created the namespace catalog entry.
Each therefore uses `CasRefCatalog::lifeIfCataloged(...).value()`, making
engagement a hard failure rather than selecting an absent-catalog fixture identity.

| Site | Rule applied |
|---|---|
| `gtest_cas_gc_fold.cpp:563` (`ns_removed` debris key) | Existing catalog entry from `appendRefLogSeed`; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2004` | Explicit `fixture::admitLive` already ran; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2012` | Migrated raw-log fixture admits the namespace; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2018` | `writeSealAt` delegates through the admitting raw fixture; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2041` | Prior raw-log fixture writes admit the namespace; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2084` | Prior raw birth/removal writes admit the namespace; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_gc_frontier_gate.cpp:2152` | Prior raw-log fixture writes admit the namespace; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_holey_list_detector.cpp:158` (`listRefKeys`) | Real writer publication created the catalog entry; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ns_file_incarnation.cpp:207` | `appendRefLogSeed` created/admitted the entry; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ref_chunked_flush.cpp:386` | Explicit fixture admission precedes the raw snapshot/log setup; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ref_contiguous_alloc.cpp:459` | Real append-lane birth created the catalog entry; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ref_writer.cpp:1192` | `publishEmptyPart` created the real catalog entry; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ref_writer.cpp:1336` | `publishEmptyPart` created the real catalog entry; use `lifeIfCataloged(...).value()`. |
| `gtest_cas_ref_writer.cpp:1368` | `publishEmptyPart` created the real catalog entry; use `lifeIfCataloged(...).value()`. |

### OPEN SITES

No ambiguous test-side sites remain. The resolver grep still reports the
three protected helper-internal calls and one explanatory comment, which were
left unchanged because the task explicitly prohibits edits to
`src/Disks/tests/cas_test_helpers.h`:

- `cas_test_helpers.h:297`, `1044`, and `1285`: helper implementation calls.
- `cas_test_helpers.h:1277`: helper implementation comment only.

These are protected-header exceptions, not unresolved test decisions.

## Acceptance grep outputs

Command:

```bash
grep -rn "stageATransition" src/Disks/tests/ | grep -v cas_test_helpers.h
```

Output: empty.

Command:

```bash
grep -rn "resolveLifeOrSentinel" src/Disks/tests/
```

Output:

```
src/Disks/tests/cas_test_helpers.h:297:    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
src/Disks/tests/cas_test_helpers.h:1044:    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
src/Disks/tests/cas_test_helpers.h:1277:/// no-op once any entry exists, so `resolveLifeOrSentinel` here resolves to whichever life is ALREADY
src/Disks/tests/cas_test_helpers.h:1285:    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(backend, layout, ns);
```

## CA gate results

Both runs used the shared lock and the generated 277-suite filter.

Pre-change baseline, `build/t1c2_gate_before.log`:

```
wrote 277 suites to /home/mfilimonov/workspace/ClickHouse/master/build/cas_suites.txt (21 excluded, 0 unclaimed)
[==========] 1976 tests from 277 test suites ran. (155902 ms total)
[  PASSED  ] 1975 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] WinnerShape/CasGcCompletedRemovalFenceRace.FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement, where GetParam() = 1-byte object <01>
 1 FAILED TEST
 YOU HAVE 2 DISABLED TESTS
```

Post-change final run, `build/t1c2_gate.log`:

```
wrote 277 suites to /home/mfilimonov/workspace/ClickHouse/master/build/cas_suites.txt (21 excluded, 0 unclaimed)
[==========] 1976 tests from 277 test suites ran. (156220 ms total)
[  PASSED  ] 1975 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] WinnerShape/CasGcCompletedRemovalFenceRace.FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement, where GetParam() = 1-byte object <01>
 1 FAILED TEST
 YOU HAVE 2 DISABLED TESTS
```

Counts did not drop: baseline 277 suites / 1,976 tests; post-change 277
suites / 1,976 tests. The same pre-existing single failure reproduced.

## Worktree scope

The task diff contains only files under `src/Disks/tests/` plus this report.
All unrelated pre-existing worktree changes remain unstaged.
