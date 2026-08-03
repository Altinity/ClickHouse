# Fix round: TEST-1/TEST-2 (t6-review.md) + T5-2/T5-3 (codex review)

Four small code/test fixes on `cas-gc-rebuild`, MAIN worktree
(`/home/mfilimonov/workspace/ClickHouse/master`). Ordinary commits, no rebase/amend, no push.

## Fix 1 — ground the list_liar kill shot (TEST-1)

`src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp`

**Defect.** `buildKillShot` wrote no `_ckpt` for either `hidden` or `visible`. Both hit
`chooseRecoveryGrounding`'s `!ckpt` throw and recorded the `checkpoint_unusable` anomaly, which
suppressed the round on the anomaly term alone — before this fix, `visible`'s own drop never even
folded. The test's comment claimed the survival mechanism was "the CATALOG names `hidden` ...
`frontier_proven == frontier_namespaces` fails on member count alone", a claim the fixture could not
produce (confirmed by running it pre-fix: log showed `checkpoint_unusable=2`, both namespaces
unproven). The test would have passed unchanged if the round's universe/count terms were deleted
outright.

**Fix.** `buildKillShot` now grounds both namespaces with `writeRecoverableCkptForRawFixture`,
reflecting exactly what was published (`hidden`'s `committed_through = RefTxnId{1,1}`, `visible`'s
`committed_through = RefTxnId{1,2}`). Renamed the test from
`AnEntirelyHiddenNamespacesEdgeIsRefusedByTheProductionDefault` to
`AHiddenNamespacesBirthIsFoundByExactKeyAndSavesTheBlobOnACompleteFrontier`, extended the file's
shared `RoundEvidence`/`runRoundCapturing` helper to also read `frontier_complete` and
`suppress_destructive` off the `fold_reduce` phase (read, not recomputed — same idiom as
`gtest_cas_gc_frontier_gate.cpp`'s `GateVerdict`), and rewrote the assertions to check the blob's
survival (`head().exists`, `deleteCount(blob key) == 0`) together with the gate's own verdict
(`frontier_complete == true`, `suppress_destructive == false`). Reworded the section header and the
file's own D44-flagged framing ("the one shape arithmetic intake cannot save") to state what the
fixture actually proves.

**Scope question raised and resolved mid-round.** The brief's suggested mutation targets
(`universe_authoritative`, `frontier_namespaces > 0`) are **vacuous against this grounded fixture by
construction**: with both namespaces truthfully grounded, `frontier_namespaces = 2 > 0` and, under
`UniversePolicy::kDefault` (`= Authoritative` post-flip), `universe_authoritative = true`
unconditionally — neither term can be made to flip this specific scenario's `frontier_complete` from
true to false, since both are already satisfied independent of any correctness of the term itself.
I traced the formula in `CasGc.cpp:2972-2984` to confirm this before raising it, rather than guessing.
Flagged to the team lead before finalizing; team lead confirmed (independently corroborated by a
second review, t6-review-fable.md's T-3) that the universe/count terms are the property of
`gtest_cas_gc_frontier_gate.cpp`'s (3a)/(3b)/(3c) suppressor arms, not this fixture, and directed:
ground the fixture as planned, and demonstrate the mutation this test *does* exercise — the
arithmetic-intake exact-key read itself. The final header comment states this explicitly so a future
reader does not re-derive the same question: "This is NOT a duplicate of
`gtest_cas_gc_frontier_gate.cpp`'s twin ... `gtest_cas_gc_frontier_gate.cpp` owns those terms ...
terms this fixture cannot exercise, because grounding both namespaces here makes
`frontier_namespaces > 0` and `universe_authoritative` true unconditionally."

**Mutation demonstration.** Weakened the arithmetic walk's per-record read
(`CasGc.cpp:2372`, inside `Gc::fold`) so a record whose key is not in the round's own LIST hint is
treated as absent — i.e. the mechanism this test's docstring says the walk must NOT do:

```diff
-            const auto got = backend.get(layout.refLogKey(life, *expected));
+            const auto got = std::binary_search(listing.logs.begin(), listing.logs.end(), *expected)
+                ? backend.get(layout.refLogKey(life, *expected))
+                : std::nullopt;   /// MUTATION-DEMO: treat a LIST omission as absence
```

Rebuilt release, ran `CasListLiarEndToEnd.*` — the target test failed exactly as predicted (and four
other tests in the same file, whose whole point is the same arithmetic-vs-listing property, failed
alongside it as corroboration, not noise):

```
[ RUN      ] CasListLiarEndToEnd.AHiddenNamespacesBirthIsFoundByExactKeyAndSavesTheBlobOnACompleteFrontier
CAS GC ref intake: namespace 00/hidden@cas@ HELD at 0000000000000001-0000000000000001 -- ref intake: a
checkpoint-committed ref log is absent -- its authoritative frontier cannot be complete. The cursor
stays at none and this namespace folds nothing further this round.
CAS GC fold: destructive work SUPPRESSED this pass — 1 anomaly(ies), 1 held namespace(s), frontier
INCOMPLETE (1 of 2 namespace(s) proven; unproven: held=1). ...
src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp:467: Failure
Value of: evidence.frontier_complete
  Actual: false
Expected: true
`hidden`'s own `_ckpt` is read by exact key, so its frontier is provable despite the listing
omission -- if this is false the blob above survived on suppression instead of on its own
in-degree, which proves nothing about the edge

src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp:471: Failure
Value of: evidence.suppress_destructive
  Actual: true
Expected: false
[  FAILED  ] CasListLiarEndToEnd.AHiddenNamespacesBirthIsFoundByExactKeyAndSavesTheBlobOnACompleteFrontier (0 ms)
```

Full failing run: `build/fixround_mutation_gate.log`. **Mutation reverted** (`git checkout --` on
`CasGc.cpp`; verified clean with `git diff --stat` producing no output before rebuilding). Both
release and ASan were rebuilt again after the revert and re-gated green (see below) before committing.

## Fix 2 — rename the misdescribing frontier-gate test (TEST-2)

`src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`

`HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` asserted
`frontier_complete == true` and `suppress_destructive == false` — the round is not refused; the blob
survives on its own folded in-degree via the exact-key probe (the stronger outcome). Renamed to
`AHiddenEdgeIsFoundByTheExactKeyProbeAndSavesTheBlobOnACompleteFrontier` and rewrote its own comment
and the "three-arm summary" section header above it, which still said "the round refuses because the
CATALOG names `hidden` and its own frontier is unproven" for this arm. Corrected to: the exact-key
probe finds `hidden`'s edge and saves the blob on a complete frontier (arm 1); the blob still drains
once `hidden` also honestly folds its own removal (arm 2, unchanged); a namespace with a sealed
cursor has its hidden `+1` found the same way (arm 3, unchanged).

## Fix 3 — retirement-sweep cadence (T5-2, codex review)

`src/Disks/tests/gtest_cas_retirement_sweep.cpp`

`TheRoundEnumeratesTheRefPrefixExactlyOnce` ran 5 rounds; the deleted detector this regression test
guards against had a cadence of every 16th folding round, so a regression reintroducing a second
enumeration on that cadence could pass a 5-round check. Raised to 32 rounds (exercises the old
cadence twice over) and added the reasoning to the test's comment.

## Fix 4 — GC log phase-list drift (T5-3, codex review)

`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`

The `phase` column description advertised a nonexistent `fold_ns_cleanup_scan` phase and omitted the
real `pre_fold_ref_drain` phase. Replaced with the exact 18-phase list, in execution order, and
cross-checked character-for-character against:
- `src/Disks/tests/gtest_cas_gc_log.cpp`'s runtime-order test (`expected` vector, ~lines 385-390);
- `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md`'s phase table
  (already correct, no change needed there).

All three now list: `lease, pre_fold_ref_drain, heartbeat_floor, defer_decision, parent_seal_read,
fold_ref_group, fold_seal_read, fold_ref_intake, fold_reduce, fold_seal_write, pending_deletes,
meta_pool_wait, round_commit, handoff_reclaim, manifest_deletes, namespace_cleanup,
ref_object_cleanup, orphan_sweep`. No gtest pins the column *comment string* itself (checked via
`git grep`), so no additional test coupling was found or needed.

## Gates

Suites: `CasListLiarEndToEnd.*:CasGcFrontierGate.*:CasRetirementSweep.*:CasGcLog.*` (58 tests total:
8 + 41 + 4 + 5), verified present via the filter's own `tests from N test suites` header. Wrapped in
`flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`. Only `unit_tests_dbms` rebuilt in each
of `build` and `build_asan`; build logs confirmed successful completion (`Linking CXX executable
src/unit_tests_dbms` as the final line) before trusting the test run.

Final state (mutation reverted, all four fixes present):
- Release: build log `build/fixround_final_build.log`, gate log
  `build/fixround_gate_final_release.log` — `[  PASSED  ] 58 tests.`, exit 0.
- ASan: build log `build_asan/fixround_final_build.log`, gate log
  `build_asan/fixround_gate_final_asan.log` — `[  PASSED  ] 58 tests.`, exit 0.

(Earlier intermediate gate/build logs from the iterative development of Fix 1 — including one run
that caught a wrong assertion, `EXPECT_EQ(deleteTotal(), 0u)`, firing on legitimate manifest-body and
generation-prune deletes unrelated to the blob under test — are also under `build/` and
`build_asan/` with distinct names; the final logs above are the ones that matter.)

## Commits

- `7b9a8fc8f2d` — Fix 1 + Fix 2 (`gtest_cas_list_liar_end_to_end.cpp`,
  `gtest_cas_gc_frontier_gate.cpp`)
- `075b2ed5f01` — Fix 3 + Fix 4 (`gtest_cas_retirement_sweep.cpp`,
  `ContentAddressedGarbageCollectionLog.cpp`)

Both verified with `git show --stat` to contain exactly the intended files (no `-A`, no stray
worktree debris; `CasGc.cpp` confirmed clean — `git status --short` produces no output for it — at
HEAD after the mutation-demo revert).

## Deviations from the brief

- Removed `deletedKeysMessage` (an anonymous-namespace helper in
  `gtest_cas_list_liar_end_to_end.cpp`) after replacing its only call site — the original
  `EXPECT_EQ(backend->deleteTotal(), 0u)` assertion in Fix 1's test was too strong (it fires on
  legitimate manifest-body/generation-prune deletes that a real fold now performs once the fixture
  is grounded) and was replaced with a blob-specific `deleteCount` check; this left the helper
  unused, which would have been a compile warning/error.
- Raised a scope question with the team lead before finalizing Fix 1's mutation demonstration
  (see above) rather than forcing a mutation the formula could not make fail. This was **not** a
  deviation from the final delivered shape — the team lead confirmed the analysis and directed the
  approach actually implemented — but the initial attempt (matching the frontier_gate twin
  one-for-one, asserting only gate-level survival) is preserved in git history as an intermediate
  commit-message draft was not made; no code deviation remains in the final diff.
