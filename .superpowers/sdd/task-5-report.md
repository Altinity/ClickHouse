# Task 5 report: E3 — in-place replay apply (shipped instead of undo journal)

## Status: COMPLETE — KEEP recommended

## What shipped

The undo journal was **not** shipped. The brief's endorsed simplification was: `replay`'s state is
local and discarded on any throw, so a throw-away path needs no rollback. That variant is simpler AND
strictly faster, so it is what shipped. `applyRefLogTxn` now branches on `TxnValidation`:

- **`Full`** (writer live-state install + every trial/shape-check preview): keeps the existing
  two-phase scratch copy verbatim — "throw ⇒ state byte-for-byte unchanged". Never the regression
  source: every `Full` caller applies against a materialized (empty-overlay) or batch-bounded state,
  so the copy is O(1) shared-base pointer bumps.
- **`TrustedHistory`** (replay only, the sole caller): applies **in place, no copy**. On a throw the
  state is poisoned; sound because `replay` discards it on any throw (its result reaches a caller only
  on full success). This removes the entire per-transaction copy cost class.

Whole change: one `if/else` in `CasRefProtocol.cpp` + doc updates on `TxnValidation` and
`applyRefLogTxn` in `CasRefProtocol.h` + 5 tests. No new type, no `RefStateUndo`, no rollback, no
alloc-failure-terminate trade. `admits` left untouched (its scratch is O(batch), not a regression).

**Post-review rename note:** `TxnValidation { Full, TrustedHistory }` was subsequently renamed to
`ApplyMode { LiveAppend, TrustedReplay }` (see the Fix pass section below) — this section otherwise
keeps the original names since it describes what shipped under those names.

## Step 0 exclusivity audit — PASSED

Every production call site of `admits`/`applyRefLogTxn`/`replay` runs under the ledger `state_mutex`
or on a detached/local copy. No concurrent reader observes a state mid-mutation; in-place apply is
safe. COW writes touch only the per-copy overlay, never the shared base. `TrustedHistory`
(post-review: `ApplyMode::TrustedReplay`) is passed at exactly one place
(`CasRefProtocol.cpp:400`, inside `replay`, grep-enforced). Full table in the experiments report §E3.

| Site | Target | Protection | Verdict |
|---|---|---|---|
| `CasRefLedger.cpp:1138` `applyRefLogTxn(rt->state, wedged)` | LIVE | `state_mutex` | exclusive |
| `CasRefLedger.cpp:1243` `applyRefLogTxn(shape_check,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1255` `admits(item_scratch,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1265` `applyRefLogTxn(item_scratch,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1377` `applyRefLogTxn(rt->state, final)` | LIVE | `state_mutex` | exclusive |
| `CasRefLedger.cpp:426` `replay(…)` (recovery) | fresh local; assigned only on success | detached | exclusive |
| `CasFsck.cpp:226` `replay(…)` (oracle) | fresh local | detached | exclusive |
| `CasRefProtocol.cpp:638` `replay(snapshot, tail)` inside `recoverRefTableDetailed` | fresh local; `RecoveredRefTable` value-returned, constructed only on success | detached | exclusive |

## Numbers (baseline t4/E2; `build/bench_t5_e3.log`)

- `BM_ReplayHistory` **recovered and crushed**: 50,082 → **1,725.58 ns/row** (-96.6%); ~29× below the
  t4 regression, ~21× below t3's 36.7k success bar, ~28× below the pre-E1 48.9k baseline.
  - N=100: 7.43ms → 0.431ms; N=1k: 48.85ms → 1.776ms; N=10k: 473.76ms → 15.92ms; N=100k: 4.98s → 0.172s.
- `BM_ApplyRefLogTxn` (Full, unchanged code): +0.6…+3.5% — flat, no regression.
- `BM_Admits` / `BM_AdmitsAddPrecommit` (admits, unchanged): ±1-3% — E2's O(1) win preserved.
- `BM_ScratchCopy` (reference): flat.
- Residual O(N) in the fit is the one-time `stateFromSnapshot` base-load, NOT the per-txn cost E3
  targeted (that is now O(1)/txn, which is why the constant collapsed 29×).

## Tests: 1,096 pass, 0 failures (`build/test_gate_t5.log`)

1,091 (t4) + 5 new E3 tests in `gtest_cas_ref_statemachine.cpp` (test names shown as of this report;
post-review rename below changed 4 of the 5 — see Fix pass section):
- `E3FullLaterOpThrowLeavesPopulatedStateByteIdentical` (Full byte-identical, populated later-op throw)
- `E3FullFirstOpThrowLeavesPopulatedStateByteIdentical` (Full byte-identical, first-op throw)
- `E3AdmitsPreviewLeavesStateByteIdentical` (admits accept + reject both byte-identical)
- `E3TrustedHistoryInPlaceMatchesFullAcrossAllArms` (in-place == Full across every applyOp arm — the
  test only the new machinery can fail)
- `E3TrustedHistoryPoisonOnBadTailIsInternal` (replay throws on bad tail; poison never escapes)

The 2 disabled tests are the pre-existing pair E2 noted.

## Concern (called out for the controller)

`TrustedHistory` now carries a second meaning ("in-place, poison-on-throw") coupled onto the
validation-mode enum. Documented loudly, grep-enforced to `replay`, and a test pins poison-containment
(for the existing caller; single-caller exclusivity is enforced by grep+comment, not testable). It is a
footgun only if a future author adds a `TrustedHistory` caller that keeps state after a throw. Weighed
and judged acceptable (that contract already restricts `TrustedHistory` to already-validated durable
replay), but flagged deliberately.

## Verdict: KEEP

Regression recovery (the round's decisive requirement), ~29× win, simplest option on the table, no
regressions elsewhere, no new machinery.

## Fix pass (post-review)

Applied the reviewer's rename recommendation plus 3 Minor findings.

**1. Enum rename.** `enum class TxnValidation { Full, TrustedHistory }` →
`enum class ApplyMode { LiveAppend, TrustedReplay }` in `CasRefProtocol.h`/`.cpp`, all call sites, and
`gtest_cas_ref_statemachine.cpp` (`benchmark_cas_ref_protocol.cpp` only ever used the default argument,
so it needed no change). `LiveAppend` = "this is the first time this transaction is validated, against
a state that must survive a rejection" (two-phase scratch copy, strong exception guarantee).
`TrustedReplay` = "I am replaying already-committed, already-validated history into a local state I
own and discard on any error" (skips the O(N) cross-owner re-scan in release builds; applies in place
and poisons `state` on throw). The two axes are welded into one enum on purpose: both are derived from
the same caller intent (`replay`, and only `replay`), and welding them keeps the dangerous fourth
combination -- trusted validation applied to a state that must survive a throw -- inexpressible. The
enum's doc comment in `CasRefProtocol.h` was rewritten to state this rationale. The default argument
(`= ApplyMode::LiveAppend`) on `applyRefLogTxn` was kept.

Four of the five new E3 test names embedded the old enum-value words (`Full`/`TrustedHistory`) and were
renamed to match: `E3FullLaterOpThrowLeavesPopulatedStateByteIdentical` →
`E3LiveAppendLaterOpThrowLeavesPopulatedStateByteIdentical`,
`E3FullFirstOpThrowLeavesPopulatedStateByteIdentical` →
`E3LiveAppendFirstOpThrowLeavesPopulatedStateByteIdentical`,
`E3TrustedHistoryInPlaceMatchesFullAcrossAllArms` →
`E3TrustedReplayInPlaceMatchesLiveAppendAcrossAllArms`, `E3TrustedHistoryPoisonOnBadTailIsInternal` →
`E3TrustedReplayPoisonOnBadTailIsInternal`. Three E1-era test names (from task 3, not this task) were
also renamed for the same reason: `TrustedHistoryReplaySkipsCrossOwnerScanInRelease` →
`TrustedReplaySkipsCrossOwnerScanInRelease`, `TrustedHistoryReplayAbortsOnCrossOwnerCollision` →
`TrustedReplayAbortsOnCrossOwnerCollision`, `TrustedHistoryReplayEquivalentToFullOnValidTail` →
`TrustedReplayEquivalentToLiveAppendOnValidTail` (the blanket `TrustedHistory`→`TrustedReplay` token
rename would otherwise have produced a stuttering `TrustedReplayReplay...` name). The experiments
report and this report both keep the original names in their historical narrative sections and add
pointers to the renamed identifiers where they cite exact test names, so nothing here should be read as
rewriting history -- only as making the current source's names discoverable from the old docs.

**2. Audit-table row.** Both exclusivity-audit tables (this report's Step 0 section and the
experiments report §E3) were missing the third production `replay` caller:
`CasRefProtocol.cpp:638`, `return RecoveredRefTable{replay(snapshot, tail), ...}` inside
`recoverRefTableDetailed`. Added as a row: target is a fresh local state, `RecoveredRefTable` is
value-returned and constructed only on success (a throw inside `replay` propagates before that `return`
statement ever executes, so no partially-built `RecoveredRefTable` is ever observable) -- detached,
exclusive, same verdict as the other two `replay` call sites.

**3. Stale line refs.** Both docs cited the `TrustedReplay` (then `TrustedHistory`) pass site as
`CasRefProtocol.cpp:369`. After the rename, the actual site is `CasRefProtocol.cpp:400`
(`applyRefLogTxn(state, txn, ApplyMode::TrustedReplay);` inside `replay`'s tail loop) -- the line
number had already drifted from the doc-writing pass to now for unrelated reasons; re-checked and
re-cited in both docs.

**4. Wording.** Changed "pinned by a test" / "pinned by a test that the poisoned internal state never
escapes" to "a test pins poison-containment (for the existing caller; single-caller exclusivity is
enforced by grep+comment, not testable)" in both docs -- the original phrasing could be read as
claiming the single-caller exclusivity itself is test-covered, which it is not (no test can observe "no
other call site exists"; that's a grep+comment invariant). Only the poison-containment behavior
(`replay` throws → no caller ever observes the poisoned state) is actually exercised by a test.

**5. Death-test gating vs. build type -- verified, no code change needed.** `build/` is an NDEBUG
release build (`DEBUG_OR_SANITIZER_BUILD` is not defined, so `chassert` compiles out). None of T5's 5
new tests use `EXPECT_DEATH`/`ASSERT_DEATH` -- checked the full source range of the 5 new tests
(`gtest_cas_ref_statemachine.cpp` lines 918-1075 pre-rename numbering); all 5 are plain `TEST(...)`
cases. The one death test the reviewer likely noticed in the gate log,
`TrustedReplayAbortsOnCrossOwnerCollision` (renamed from `TrustedHistoryReplayAbortsOnCrossOwnerCollision`),
predates this task -- it was added in E1/task 3, is wrapped in
`#if defined(DEBUG_OR_SANITIZER_BUILD)`, and does not compile into this release build at all. Confirmed
by grep on the gate log: zero `DeathTest` suite names ran under the T5 filter. The arithmetic reconciles
cleanly: 1,096 = 1,091 (t4 baseline, which already excludes that gated-out E1 death test) + 5 new
T5/E3 tests, all non-death. The stray "Death tests use fork()" warnings visible in the gate log come
from unrelated pre-existing death tests elsewhere in the binary that the broad `Cas*`/`CaWiring*`
filter also matches (e.g. `CasWiringOpsDeathTest.MoveDirectoryMutableCollisionPolicyAborts`), not from
anything in this task.

**Build:** `flock /tmp/claude-1000/ninja.lock ninja -C build unit_tests_dbms benchmark_cas_ref_protocol
> build/build_t5fix.log 2>&1` -- clean, 0 errors.

**Gate:** `build/src/unit_tests_dbms
--gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'
> build/test_gate_t5fix.log 2>&1` → **1,096 tests passed, 0 failures** (identical count to T5's original
gate). No bench re-run: the rename is a pure token substitution plus a doc-comment rewrite, codegen-neutral.

**Files touched:** `CasRefProtocol.h`, `CasRefProtocol.cpp`, `gtest_cas_ref_statemachine.cpp`,
`.superpowers/sdd/task-5-report.md` (this file), `docs/superpowers/reports/2026-07-21-reftablestate-experiments.md`.
