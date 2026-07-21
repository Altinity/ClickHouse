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
