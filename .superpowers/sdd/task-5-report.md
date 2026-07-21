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

## Step 0 exclusivity audit — PASSED

Every production call site of `admits`/`applyRefLogTxn`/`replay` runs under the ledger `state_mutex`
or on a detached/local copy. No concurrent reader observes a state mid-mutation; in-place apply is
safe. COW writes touch only the per-copy overlay, never the shared base. `TrustedHistory` is passed at
exactly one place (`CasRefProtocol.cpp:369`, inside `replay`, grep-enforced). Full table in the
experiments report §E3.

| Site | Target | Protection | Verdict |
|---|---|---|---|
| `CasRefLedger.cpp:1138` `applyRefLogTxn(rt->state, wedged)` | LIVE | `state_mutex` | exclusive |
| `CasRefLedger.cpp:1243` `applyRefLogTxn(shape_check,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1255` `admits(item_scratch,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1265` `applyRefLogTxn(item_scratch,…)` | local | stack-local | exclusive |
| `CasRefLedger.cpp:1377` `applyRefLogTxn(rt->state, final)` | LIVE | `state_mutex` | exclusive |
| `CasRefLedger.cpp:426` `replay(…)` (recovery) | fresh local; assigned only on success | detached | exclusive |
| `CasFsck.cpp:226` `replay(…)` (oracle) | fresh local | detached | exclusive |

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

1,091 (t4) + 5 new E3 tests in `gtest_cas_ref_statemachine.cpp`:
- `E3FullLaterOpThrowLeavesPopulatedStateByteIdentical` (Full byte-identical, populated later-op throw)
- `E3FullFirstOpThrowLeavesPopulatedStateByteIdentical` (Full byte-identical, first-op throw)
- `E3AdmitsPreviewLeavesStateByteIdentical` (admits accept + reject both byte-identical)
- `E3TrustedHistoryInPlaceMatchesFullAcrossAllArms` (in-place == Full across every applyOp arm — the
  test only the new machinery can fail)
- `E3TrustedHistoryPoisonOnBadTailIsInternal` (replay throws on bad tail; poison never escapes)

The 2 disabled tests are the pre-existing pair E2 noted.

## Concern (called out for the controller)

`TrustedHistory` now carries a second meaning ("in-place, poison-on-throw") coupled onto the
validation-mode enum. Documented loudly, grep-enforced to `replay`, and pinned by a test. It is a
footgun only if a future author adds a `TrustedHistory` caller that keeps state after a throw. Weighed
and judged acceptable (that contract already restricts `TrustedHistory` to already-validated durable
replay), but flagged deliberately.

## Verdict: KEEP

Regression recovery (the round's decisive requirement), ~29× win, simplest option on the table, no
regressions elsewhere, no new machinery.
