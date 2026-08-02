# Task 5 Step 8 C++ integration report

## Scope

- Keep the hot immutable `RefPlan` at exactly one `cas/ns/stream/` `LIST` plus one later catalog cut.
- Run exactly one independently paced `cas/ns/` janitor page and one later catalog cut on every acquired ordinary GC invocation, including `DEFER`.
- Keep uncataloged state objects inert to the hot walk while making them eligible only to the janitor.

## Defect found

The original integration returned from `DEFER` before `namespace_cleanup`. Adding the janitor call naively would have exposed a second liveness defect: advancing its cursor while deletion is globally suppressed can phase-lock page A onto every deferred invocation and page B onto every bounded forced fold, so dead objects on A are never reclaimed.

## Implemented contract

- `Gc::runNamespaceJanitorPage` owns the bounded page, post-page catalog cut, exact GC-fence check, warnings, and `namespace_cleanup` metrics. It performs no catalog lifecycle transition and never contributes to the hot walk plan.
- The folding path passes its single `FoldResult::suppress_destructive` verdict.
- `DEFER` has no complete fold verdict, so it calls the same helper with deletion suppressed.
- A valid page advances only when its catalog cut is unambiguous, deletion is globally admitted, and no candidate observes fence loss. A suppressed, ambiguous, or fence-lost page retains the old cursor for an authoritative retry. Malformed keys, `NotFound`, and exact-token mismatch remain decided per-key outcomes and may advance the page.
- Corrupt or backend-rejected cursor reset behavior is unchanged.

## Test evidence

- RED: `build/task5_step8_suppressed_cursor_red_tests.log` — the old `DEFER` early return omitted the namespace page/cut/phase, and suppressed standalone janitor minted successor progress.
- Clean production/object compilation before final link: `build/task5_step8_defer_helper_green_build.log`, `build/task5_step8_owned_objects_build.log`, and `build/task5_step8_ambiguous_cursor_object_build.log`.
- Final linked build: `build/task5_step8_final_with_foreign_compat_build.log` — `unit_tests_dbms` linked successfully.
- Final focused gate: `build/task5_step8_final_focused_21_tests.log` — 21/21 passed, zero failed/skipped/disabled.
- Final wider gate: `build/task5_step8_final_wider_tests.log` — 65/65 passed, zero failed/skipped/disabled.
- Mutation controls: removing the DEFER helper made both DEFER integration pins fail; restoring the old suppressed-cursor advance made both the standalone and integrated phase-lock pins fail. Production was restored before the final gates.

## Additional Step 8 pins

- A fresh `NamespaceJanitor` instance resumes a durable partial cursor and resets it at end-of-tree.
- `_files`-only debris omitted for a whole page/cycle is retried and reclaimed.
- Literal catalog-first `Creating` objects are retained, in addition to the existing post-LIST concurrent-creation race.
- A `_ckpt` left by public `cancelStalledCreating` is reclaimed after the row is gone.
- Suppression, catalog ambiguity, and fence loss each retain the selected page; fence loss stops subsequent deletes and does not roll back earlier exact deletes.

## Ownership exclusions

This slice does not own or stage the TLA/plan files, the protected aggregate Task 5 report, the concurrent `gc_shards` gtests, or unrelated `CasPool`/`CasRefProtocol` work.

The concurrent `gc_shards` migration changed the catalog-admission signature during this slice. Eight
call sites in the two already-owned Step 8 test files were mechanically migrated using each fixture's
actual `store->poolConfig().gc_shards`, including the multi-shard fixture. One additional compatibility
edit in `gtest_cas_ns_file_read_contract.cpp` was required to unblock the shared final link; it remains
foreign, unstaged and uncommitted.
