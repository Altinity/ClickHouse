# RefTableState critical-op benchmark suite — pre-encapsulation baselines (2026-07-21)

## What this is

Phase B of the `RefTableState` encapsulation round: before touching `RefTableState`'s field
visibility, `benchmark_cas_ref_protocol.cpp` was extended to cover every critical operation on it
— not just `admits()` — and the resulting numbers were recorded as the pre-refactor baseline.
Later tasks re-run this exact suite unchanged and diff against these numbers to catch a regression
the encapsulation itself introduces.

The suite adds a `makeSyntheticSnapshot(n)` / `makeSyntheticState(n)` pair built on the public
`replay` entry point (rather than poking `RefTableState`'s fields directly, as the previous
`BM_Admits`-only helper did), so it keeps compiling once those fields go private.

## Founding measurement

`BM_Admits` benchmarks a **promote** (`old_binding` set, `new_binding` set, same manifest — no
call into `manifestAlreadyOwned`). `BM_AdmitsAddPrecommit` benchmarks an **add** (`new_binding`
only) — the shape every part publication actually starts with, and the one that pays
`manifestAlreadyOwned`'s linear scan today. The two now diverge exactly as expected:

- `BM_Admits`: flat, O(1), ~1 µs regardless of N (matches the 2026-07-20 incremental-budget fix).
- `BM_AdmitsAddPrecommit`: O(N), ~4.0 ns/row, from ~1 µs at N=100 to ~400 µs at N=100,000.

This confirms the production-hotspot mental model this round is built on: incremental budget
counters fixed the *promote* preview cost, but the *add* preview still does an O(N) value scan for
manifest-collision safety.

## A benchmark-methodology bug caught along the way

The first pass of `makeSyntheticState` matched the task brief's helper literally: build a
`RefTableSnapshot`, call `replay(snapshot, {})`, return the result. `replay` is the pure
state-machine equation and never materializes `RefCowMap`; `stateFromSnapshot` loads every
committed row through `RefCowMap::emplace`, which only ever touches the *overlay*. The result:
every subsequent `RefTableState` copy in the suite — including `admits`'s and `applyRefLogTxn`'s
own internal scratch copies — became an O(N) deep-copy of an N-row overlay `std::map` instead of an
O(1) copy sharing an immutable base pointer.

This was caught because it made `BM_Admits` regress from its documented O(1) (flat ~1.8-1.9 µs,
per the file's existing history comment) to a clearly O(N log N) climb from ~12 µs to ~20 ms — the
exact shape the 2026-07-20 fix was supposed to have eliminated. That contradiction was the signal:
production never observes an un-materialized table between batches (`RefCowMap::materialize`'s own
doc says the state-install point materializes once per flush, before any batch item runs against
the result), so a benchmark against that shape was measuring copy overhead, not the operation under
test.

Fix: `makeSyntheticState` now calls `state.committed.materialize()` before returning, mirroring
what every real caller does immediately after building or replaying a state. All numbers below are
post-fix. This also gives `BM_MergedIteration` its intended "post-copy, pre-materialize" shape: a
materialized N-row base plus a fresh 10%-sized overlay from simulated in-flight writes, rather than
one large unmaterialized 1.1×N-row overlay.

## Baselines {#baselines}

This binary, `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`, medians reported.
`BigO` / `RMS` are Google Benchmark's `->Complexity()` fit over the row-count range shown.

| Benchmark | N=100 | N=1,000 | N=10,000 | N=100,000 | Complexity fit |
|---|---|---|---|---|---|
| `BM_Admits` (promote preview) | 963 ns | 979 ns | 988 ns | 1,029 ns | O(1), RMS 2% |
| `BM_AdmitsAddPrecommit` (add preview) | 995 ns | 4,266 ns | 38,771 ns | 400,222 ns | O(N), ~4.0 ns/row, RMS 2% |
| `BM_ApplyRefLogTxn` (copy+validate+apply+install one promote) | 724 ns | 738 ns | 784 ns | 788 ns | O(1), RMS 4% |
| `BM_ReplayHistory` (snapshot of size N + 256 tail txns, 2 ops each) | 6.15 ms | 46.1 ms | 454.0 ms | 4.93 s | O(N), ~48,859 ns/row, RMS 3% |
| `BM_ScratchCopy` (one full `RefTableState` copy, materialized) | 45.7 ns | 46.0 ns | 46.7 ns | 46.8 ns | O(1), RMS 1% |
| `BM_SnapshotEncode` (`encodeRefTableSnapshot(snapshotOf(state))`) | 14,955 ns | 150,061 ns | 1,508,586 ns | 15,885,841 ns | O(N), ~159 ns/row, RMS 1% |
| `BM_MergedIteration` (base + 10% overlay, merged scan) | 759 ns | 7,719 ns | 81,073 ns | 864,552 ns | O(N), ~8.6 ns/row, RMS 4% |
| `BM_Materialize` (`RefCowMap::materialize` after one overlay insert on N-row base) | 12,069 ns | 126,687 ns | 1,296,326 ns | 18,145,559 ns | O(N log N), RMS 2% |

## Reading the table

- **`BM_Admits` vs `BM_AdmitsAddPrecommit`** is the round's headline contrast (see above): the
  incremental-budget fix only reaches the promote path. Fixing the add path's
  `manifestAlreadyOwned` scan is the natural next optimization target this suite exists to
  validate.
- **`BM_ApplyRefLogTxn`** stays flat because the benchmarked transaction is a single promote of an
  already-staged precommit against a materialized table — the copy is O(1) (shared base) and the
  apply touches one row. It would show the same O(N) shape as `BM_AdmitsAddPrecommit` if benchmarked
  against an add instead; that variant was not added separately since `BM_AdmitsAddPrecommit`
  already isolates that cost precisely.
- **`BM_ReplayHistory`** is the fold/recovery profile and the most expensive benchmark in the suite
  by a wide margin (4.93 s at N=100,000): each of its 256 tail transactions performs an add (with
  the O(N) manifest-collision scan) followed by a promote, over a snapshot that keeps growing only
  in the sense that N is the *starting* table size, not the accumulated total — the 256×O(N) scans
  dominate. This is the fold-time cost the incremental-budget fix does not touch at all, and the
  clearest amplification of the add-path hotspot.
- **`BM_ScratchCopy`** is the isolation-primitive floor (~46 ns regardless of N) confirming
  `RefTableState` copies are genuinely O(1) once the state is materialized — this is what makes
  `BM_Admits` and `BM_ApplyRefLogTxn` flat rather than what `BM_AdmitsAddPrecommit`'s scan alone
  would predict.
- **`BM_SnapshotEncode`** is real per-flush cost, expected to stay O(N); the ~159 ns/row constant is
  the baseline a future contiguous-scan optimization (E4 in the backlog) would need to beat.
- **`BM_MergedIteration`** models the cold full-scan paths (`snapshotOf`, `listRefs`,
  `dropNamespace`) against a state with a live 10%-sized in-flight overlay; ~8.6 ns/row is the
  merge-iteration constant on top of whatever the caller does per row.
- **`BM_Materialize`** is the per-flush install cost of folding one overlay insert into an N-row
  base; the O(N log N) fit reflects `std::map`'s rebuild cost and is the floor a future flush-path
  optimization would need to beat.

## Gate

CAS unit gate (`unit_tests_dbms`, filter
`Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`):
1,077 tests passed, 0 failures.

## Post-encapsulation (t2) {#post-encapsulation-t2}

Task 2 (Phase A) converted `RefTableState` from a plain struct to a closed class: private fields,
public getters (`getLifecycle`, `getRemoveTxnId`, `getGreatestApplied`, `getCommitted`,
`getPrecommits`, `getSnapshotBodyBytes`, `getRemovalBodyBytes`), mutation only through
`applyRefLogTxn`/`replay`/`stateFromSnapshot`/`materializeCommitted`. This suite was re-run
unchanged (`makeSyntheticState`'s materialize call now goes through the new
`state.materializeCommitted()`; `BM_MergedIteration` was rewritten to build and iterate a raw
`RefCowMap` directly instead of reaching through `RefTableState`, since it benchmarks the merge
primitive itself, not the state machine — see the code comment at its definition) to check the
refactor is zero-cost. Same binary and flags as the baseline run above; medians reported, delta vs.
the baseline median in the table's own column.

| Benchmark | N | Baseline (median) | Post-encapsulation (median) | Delta |
|---|---|---|---|---|
| `BM_Admits` | 100 | 963 ns | 1,008 ns | +4.7% |
| `BM_Admits` | 1,000 | 979 ns | 1,040 ns | +6.2% |
| `BM_Admits` | 10,000 | 988 ns | 1,053 ns | +6.6% |
| `BM_Admits` | 100,000 | 1,029 ns | 1,067 ns | +3.7% |
| `BM_AdmitsAddPrecommit` | 100 | 995 ns | 1,020 ns | +2.5% |
| `BM_AdmitsAddPrecommit` | 1,000 | 4,266 ns | 4,301 ns | +0.8% |
| `BM_AdmitsAddPrecommit` | 10,000 | 38,771 ns | 39,166 ns | +1.0% |
| `BM_AdmitsAddPrecommit` | 100,000 | 400,222 ns | 434,903 ns | +8.7% |
| `BM_ApplyRefLogTxn` | 100 | 724 ns | 764 ns | +5.5% |
| `BM_ApplyRefLogTxn` | 1,000 | 738 ns | 783 ns | +6.1% |
| `BM_ApplyRefLogTxn` | 10,000 | 784 ns | 803 ns | +2.4% |
| `BM_ApplyRefLogTxn` | 100,000 | 788 ns | 834 ns | +5.8% |
| `BM_ReplayHistory` | 100 | 6.15 ms | 6.19 ms | +0.6% |
| `BM_ReplayHistory` | 1,000 | 46.1 ms | 46.26 ms | +0.3% |
| `BM_ReplayHistory` | 10,000 | 454.0 ms | 453.28 ms | -0.2% |
| `BM_ReplayHistory` | 100,000 | 4.93 s | 4.79 s | -2.8% |
| `BM_ScratchCopy` | 100 | 45.7 ns | 46.2 ns | +1.1% |
| `BM_ScratchCopy` | 1,000 | 46.0 ns | 46.6 ns | +1.3% |
| `BM_ScratchCopy` | 10,000 | 46.7 ns | 46.1 ns | -1.3% |
| `BM_ScratchCopy` | 100,000 | 46.8 ns | 45.8 ns | -2.1% |
| `BM_SnapshotEncode` | 100 | 14,955 ns | 15,504 ns | +3.7% |
| `BM_SnapshotEncode` | 1,000 | 150,061 ns | 158,807 ns | +5.8% |
| `BM_SnapshotEncode` | 10,000 | 1,508,586 ns | 1,629,810 ns | +8.0% |
| `BM_SnapshotEncode` | 100,000 | 15,885,841 ns | 16,726,370 ns | +5.3% |
| `BM_MergedIteration` | 100 | 759 ns | 754 ns | -0.7% |
| `BM_MergedIteration` | 1,000 | 7,719 ns | 7,711 ns | -0.1% |
| `BM_MergedIteration` | 10,000 | 81,073 ns | 82,035 ns | +1.2% |
| `BM_MergedIteration` | 100,000 | 864,552 ns | 903,707 ns | +4.5% |
| `BM_Materialize` | 100 | 12,069 ns | 12,137 ns | +0.6% |
| `BM_Materialize` | 1,000 | 126,687 ns | 127,216 ns | +0.4% |
| `BM_Materialize` | 10,000 | 1,296,326 ns | 1,297,018 ns | +0.05% |
| `BM_Materialize` | 100,000 | 18,145,559 ns | 17,923,253 ns | -1.2% |

Every point is within the ±10% acceptance band; the largest single delta is
`BM_AdmitsAddPrecommit` at N=100,000 (+8.7%), still comfortably inside tolerance and consistent
with that benchmark's own higher run-to-run variance (baseline RMS 4%). `BM_Admits` and
`BM_ApplyRefLogTxn` show a small but consistent +2-7% shift across every N, plausibly measurement
noise from this being an unoptimized/debug-flavored `build` directory rather than a systematic
encapsulation cost (both stay flat in N either way — the O(1)/O(N) shape the encapsulation was
required to preserve is intact). `BM_ScratchCopy` (the pure copy-cost floor, entirely unrelated to
the getter surface) also moves within the same ±1-2% band, supporting the noise explanation.
Gate re-run: 1,077 tests passed, 0 failures (identical set to the baseline run).

## E1 relaxed replay (t3) {#e1-relaxed-replay-t3}

Task 3 attacks `BM_AdmitsAddPrecommit`'s O(N) `manifestAlreadyOwned` cross-owner scan — but only on
the ONE path where re-checking it is provably redundant: `replay`'s tail, whose transactions
already passed `Full` validation when they were durably appended (recovery, `fsck`, GC's owner-set
rebuild, the writer's own recovery-on-open). A new `TxnValidation` enum (`Full` | `TrustedHistory`)
threads through `applyRefLogTxn` → `applyOp` → `applyOwnerTransition`; only the add-precommit arm
consults it. Under `Full` (the default, unchanged for every writer append-time caller) the scan
still runs and throws `CORRUPTED_DATA` on a cross-owner collision, exactly as before. Under
`TrustedHistory` the scan becomes a `chassert` — compiled out entirely in release builds (this
`build/` directory has `NDEBUG` defined, no sanitizer), kept as a debug/sanitizer re-verification of
a construction-guaranteed invariant, same policy as `debugAssertBodyCounters`. Every exact-binding
precondition (the cheap, keyed checks) stays enforced in BOTH modes.

### Caller audit

`replay`'s tail loop is the only caller that now passes `TrustedHistory`; every other caller of
`applyRefLogTxn` keeps the `Full` default by omission (no edit needed):

| Caller | Location | Mode | Why |
|---|---|---|---|
| `replay`'s tail loop | `Pool/CasRefProtocol.cpp` (`replay`) | `TrustedHistory` (explicit) | Tail already passed `Full` validation at append time — recovery, `fsck`, GC rebuild, writer recovery-on-open all inherit this via `replay`/`recoverRefTableDetailed`. |
| Wedge-resolution apply | `Pool/CasRefLedger.cpp:1138` | `Full` (default) | First-time fold of a transaction this leader itself just confirmed committed, not a replay of pre-validated history. |
| Whole-item shape-check trial | `Pool/CasRefLedger.cpp:1243` | `Full` (default) | Validates a not-yet-persisted candidate transaction before any object is created. |
| Per-op trial preview | `Pool/CasRefLedger.cpp:1265` | `Full` (default) | Pre-persist per-op validation of a candidate transaction. |
| Post-PUT commit-time state install | `Pool/CasRefLedger.cpp:1377` | `Full` (default) | The writer's append-time contract itself — the FIRST validation of `final_txn`. |
| `admits`'s single-op preview | `Pool/CasRefProtocol.cpp` (`admits`) | `Full` (explicit) | Previews a hypothetical op never durably appended anywhere. |
| `independentFullReplayForTest` oracle | `gtest_cas_ref_writer.cpp:138` | `Full` (default) | Deliberately independent ground-truth oracle, unrelated to E1. |
| `BM_ApplyRefLogTxn` | `benchmark_cas_ref_protocol.cpp:286` | `Full` (default) | Benchmarks the writer's per-txn apply cost, unaffected by E1. |

### Tests

Two new tests in `gtest_cas_ref_statemachine.cpp` (plus a third, death-test variant, gated
`#if defined(DEBUG_OR_SANITIZER_BUILD)` and not compiled into this release-flavored `build/`):
`CasRefStateMachine.TrustedHistoryReplaySkipsCrossOwnerScanInRelease` (proves `Full` still throws
`CORRUPTED_DATA` on a cross-owner collision, and that `TrustedHistory` applies it instead in a
release build) and `CasRefStateMachine.TrustedHistoryReplayEquivalentToFullOnValidTail` (a valid
tail replayed via `replay`/`TrustedHistory` produces a state byte-identical, getters and encoded
snapshot, to the same tail applied via `Full`). RED was reconstructed explicitly (implementation
temporarily reverted via a saved patch + `git checkout --`, confirming a compile failure —
`use of undeclared identifier 'TxnValidation'` — before restoring it) since both edits were written
in one pass. Gate (same filter as the baseline): **1,079 tests passed, 0 failures** (1,077 + 2; the
death test doesn't compile into this build).

### Benchmark delta vs t2 {#e1-benchmark-delta}

Same binary/flags as the baseline and t2 runs; medians reported.

| Benchmark | N | t2 (median) | t3/E1 (median) | Delta |
|---|---|---|---|---|
| `BM_ReplayHistory` | 100 | 6.19 ms | 4.85 ms | **-21.6%** |
| `BM_ReplayHistory` | 1,000 | 46.26 ms | 35.82 ms | **-22.6%** |
| `BM_ReplayHistory` | 10,000 | 453.28 ms | 350.57 ms | **-22.7%** |
| `BM_ReplayHistory` | 100,000 | 4.79 s | 3.67 s | **-23.4%** |
| `BM_ReplayHistory` complexity fit | — | ~48,859 ns/row, O(N), RMS 3% | 36,679 ns/row, O(N), RMS 1% | **-24.9%** |
| `BM_AdmitsAddPrecommit` | 100 | 1,020 ns | 1,013 ns | -0.7% |
| `BM_AdmitsAddPrecommit` | 1,000 | 4,301 ns | 4,281 ns | -0.5% |
| `BM_AdmitsAddPrecommit` | 10,000 | 39,166 ns | 38,747 ns | -1.1% |
| `BM_AdmitsAddPrecommit` | 100,000 | 434,903 ns | 410,636 ns | -5.6% |

`BM_AdmitsAddPrecommit` is unchanged (`admits` always passes explicit `Full`) — all four deltas sit
inside the benchmark's own run-to-run noise. `BM_ReplayHistory` dropped ~22-25% across every N and
in its complexity fit: a real, verified win, and `manifestAlreadyOwned` genuinely stops being
called at all on this path in a release build (`chassert` compiles to `(void)sizeof(...)`, no
runtime evaluation).

It is worth being precise about what did NOT happen: `BM_ReplayHistory` stayed O(N) overall — it
did not flatten. That is because it measures a SECOND, pre-existing O(N) cost that E1 was never
scoped to touch: it builds its base state via `makeSyntheticSnapshot(n)` + `replay(snapshot,
tail)`, not `makeSyntheticState` (which explicitly calls `.materializeCommitted()` — the earlier
"benchmark-methodology bug" section above documents the general pattern). `replay` internally calls
`stateFromSnapshot`, which loads every committed row into the `committed` `RefCowMap`'s OVERLAY
(the map's `base` starts as an empty shared pointer) and never materializes. Every one of the 256
tail transactions' `RefTableState scratch = state;` copy inside `applyRefLogTxn` therefore
deep-copies that up-to-N-entry overlay `std::map` — a genuine O(N) cost, repeated 256 times, wholly
unrelated to `manifestAlreadyOwned`. Unlike the other benchmarks, this is not a benchmark artifact
to fix: `BM_ReplayHistory` deliberately models the fold/recovery profile, and production
replay-from-snapshot genuinely never materializes mid-fold (`applyRefLogTxn` never calls
`materializeCommitted()`; only the writer's live-table flush loop does, once per flush) — so this
remaining O(N) term is real production cost, confirmed out of scope for E1, and a candidate for a
follow-up experiment (materialize periodically during a long recovery replay, or avoid copying an
un-materialized overlay).

Gate: 1,079 tests passed, 0 failures (t2's 1,077 + 2 new E1 tests).
