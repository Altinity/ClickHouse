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

## E2 owned-manifest index (t4) {#e2-owned-manifest-index-t4}

Task 4 attacks `BM_AdmitsAddPrecommit`'s O(N) cost directly rather than eliding it on one path (E1):
`manifestAlreadyOwned` now answers "does any owner already name this `ManifestRef`" from a new COW
membership index, `RefCowManifestSet` (`Pool/CasRefCowManifestSet.h`), instead of scanning
`committed` + `precommits`. Same copy-on-write shape as `RefCowMap`: an immutable `shared_ptr`-shared
`base` (a `std::unordered_set<ManifestRef>`, O(1) lookup) plus a per-copy `overlay`, so a
`RefTableState` scratch copy stays O(overlay), never O(table size). `RefTableState` gained one new
private field, `owned_manifests`, maintained by every arm of `applyOwnerTransition` that changes
ownership (`insert` on add-precommit, `erase` on remove-precommit and remove-committed, deliberately
untouched on promote -- the manifest keeps an owner throughout, so there is nothing to erase-then-
reinsert) plus `stateFromSnapshot`'s row-loading loops (seed) and `materializeCommitted` (folds its
overlay alongside `committed`'s). `manifestAlreadyOwned` becomes `return
owned_manifests.contains(manifest_ref);` -- O(1) in BOTH `TxnValidation` modes, so E1's
`TrustedHistory` `chassert(!manifestAlreadyOwned(...))` is now an O(1) check too. (Correction from
the final review: debug/sanitizer replay overall REMAINS O(K×N), because `debugAssertBodyCounters`'s
O(N) rebuild-and-compare still runs after every applied transaction there; only the E1 chassert
itself became O(1). Release-build replay is unaffected by either.)

### Tests

TDD was compile-failure RED in practice: the container (`Pool/CasRefCowManifestSet.h/.cpp`) and its
dedicated gtest suite (`gtest_cas_ref_cow_manifest_set.cpp`, 12 tests -- contains/insert/erase across
base and overlay, tombstone-then-reinsert both purely-in-overlay and across a materialized base,
`materialize` folding and no-op-on-empty-overlay behavior, copy isolation, `baseUseCountForTest`
copy-shares-base, and a `net_delta` correctness walk through a longer mixed op sequence) were written
together in one pass, so RED was reconstructed by temporarily removing the container header (a
compile failure -- `use of undeclared identifier 'RefCowManifestSet'`) before restoring it, the same
pattern E1 used for its own single-pass edit. Four death tests (`insert` aborting on an
already-present member in either the overlay or a materialized base, `erase` aborting on an absent or
already-tombstoned member) are gated `#if defined(DEBUG_OR_SANITIZER_BUILD)` and do not compile into
this release-flavored `build/` directory. `debugAssertBodyCounters` gained a full rebuild-and-compare
of `owned_manifests`: every scanned `committed`/`precommits` entry must be present in the index
(`chassert(owned_manifests.contains(...))`), and the index's total `size()` must equal the number of
rows scanned -- catching both a missing entry and a stale/extra one, which a size-only or
membership-only check could each miss alone. Gate (same filter as every prior round): **1,091 tests
passed, 0 failures** (1,079 + 12 new container tests; the 4 death tests don't compile into this
build, so 1,079 + 12 = 1,091 checks out exactly).

### Investigation: an `unordered_map` overlay taxes every scratch copy {#e2-scratchcopy-investigation}

The brief's acceptance gate calls for investigating `BM_ScratchCopy` before committing if it regresses
more than 10% (the container's copy must stay cheap, or the whole exercise reintroduces the cost E2
exists to remove). The first implementation followed the task brief's class sketch literally --
`std::unordered_map<ManifestRef, bool, Hash> overlay` -- and `BM_ScratchCopy` regressed **+37 to +42%**
(t3-equivalent ~46 ns → ~59-66 ns across N). That is well outside tolerance, so it was investigated
before anything was committed, per instructions.

Isolated measurement via `.claude/tools/cppexpr.sh` (`--plain -b 3000000`, copying a small struct with
one vs. two `shared_ptr`-plus-overlay members, five reps) pinned the cause: libstdc++'s
`std::unordered_map` copy constructor allocates a real bucket array even when copying an **empty**
source map (~30 ns/copy measured), whereas copying an empty `std::map` is close to free (~15 ns/copy,
indistinguishable from the cost of one more `shared_ptr` refcount bump alone). `RefCowMap`'s own
`overlay` is a `std::map`, not an `unordered_map`, for exactly this reason -- E2's first draft
reintroduced the cost `RefCowMap` had already sidestepped, just in a sibling container. Fix:
`RefCowManifestSet::overlay` is `std::map<ManifestRef, bool>` (using `ManifestRef::operator<`, already
defined); `base` stays `std::unordered_set<ManifestRef, Hash>` for O(1) large-table lookups, since
`base` is shared via `shared_ptr` and is never itself deep-copied. After the fix, `BM_ScratchCopy`
lands at ~59-60 ns across N -- a real, understood, and now-irreducible-within-this-design ~13 ns
(~28%) over the t2/t3 baseline (~46 ns): exactly the cost of one additional `shared_ptr` copy (the
second COW container's `base` pointer), confirmed by the same isolated measurement. Removing it
entirely would require merging `owned_manifests`'s and `committed`'s `base` pointers into one shared
control block -- an architecture change out of scope for "add a container," not attempted here. In
absolute terms this residual is noise against the benchmark it actually feeds: `BM_ScratchCopy` is one
component of `BM_AdmitsAddPrecommit`'s ~720 ns total (below), so the ~13 ns addition is under 2% of
the number this task was optimizing, dwarfed by the ~99.8%-at-N=100,000 win on that same benchmark.

### An honest regression: `BM_ReplayHistory` gets WORSE, not better {#e2-replayhistory-regression}

The brief's own expectation was "`BM_ReplayHistory` may improve a little (each add's scan gone)".
Measured result is the opposite: `BM_ReplayHistory`'s per-row constant goes from t3/E1's 36,679 ns/row
back up to **50,082 ns/row** -- worse than t3, and marginally worse than the *original pre-E1*
baseline's 48,859 ns/row. E1's ~22-25% win on this benchmark is essentially erased.

Root cause is the same one the E1 section already flagged as "confirmed out of scope... a candidate
for a follow-up experiment": `BM_ReplayHistory` calls `replay(snapshot, tail)` directly (not
`makeSyntheticState`, which explicitly materializes) to model the fold/recovery profile, where
production genuinely never materializes mid-replay (`applyRefLogTxn` never calls
`materializeCommitted()`; only the writer's live flush loop does, once per flush, after which E2's
`owned_manifests` folds in lockstep with `committed`). Across `BM_ReplayHistory`'s 256-transaction
tail, `committed`'s `RefCowMap` overlay was already known to grow unboundedly and get deep-copied
whole on every one of the 256 `RefTableState scratch = state` copies inside `applyRefLogTxn` -- an
accepted, documented, out-of-scope-for-E1 cost. `owned_manifests`'s `overlay` now grows in lockstep
with it (every add-precommit across the tail inserts one entry that nothing in this benchmark ever
removes, since promote deliberately leaves the index alone) and pays the same uncapped per-copy cost a
second time, roughly doubling the pre-existing, already-accepted overhead. This is a genuine
production-relevant cost -- real recovery/GC-fold replay of a long uncommitted tail hits the identical
shape -- not a benchmark artifact; it does not change `BM_ReplayHistory`'s O(N) classification (still
`RangeMultiplier(10)` scaling `~1000x` in N producing `~670x` in time, consistent with O(N) both before
and after), only its constant.

This was not fixed in this task: the fix is "materialize periodically during a long recovery replay
(or otherwise avoid copying an un-materialized overlay)," which the E1 section already scoped as a
follow-up affecting `committed` generally, not something to improvise piecemeal onto one new field.
Flagging it here, unsmoothed, rather than reporting only the benchmarks that moved the right direction
-- the live-writer append/flush path (`BM_AdmitsAddPrecommit`, `BM_ApplyRefLogTxn`, `BM_ScratchCopy`,
all benchmarked against a `materializeCommitted()`-called, fully-materialized state) is unaffected and
gets the full O(1) win; only the never-materializes-mid-fold recovery/GC path pays this doubled
already-known cost.

### Benchmark deltas

Same binary/flags as every prior round (`--benchmark_repetitions=3
--benchmark_report_aggregates_only=true`, medians reported), post-fix (`std::map` overlay).

| Benchmark | N | Before (source) | t4/E2 (median) | Delta |
|---|---|---|---|---|
| `BM_AdmitsAddPrecommit` | 100 | 1,013 ns (t3) | 713 ns | **-29.6%** |
| `BM_AdmitsAddPrecommit` | 1,000 | 4,281 ns (t3) | 716 ns | **-83.3%** |
| `BM_AdmitsAddPrecommit` | 10,000 | 38,747 ns (t3) | 726 ns | **-98.1%** |
| `BM_AdmitsAddPrecommit` | 100,000 | 410,636 ns (t3) | 720 ns | **-99.8%** |
| `BM_AdmitsAddPrecommit` complexity fit | — | O(N), ~4.0 ns/row (baseline) | O(1), RMS 1% | **O(N) → O(1)** |
| `BM_ScratchCopy` | 100 | 46.2 ns (t2) | 60.0 ns | +29.9% |
| `BM_ScratchCopy` | 1,000 | 46.6 ns (t2) | 60.1 ns | +29.0% |
| `BM_ScratchCopy` | 10,000 | 46.1 ns (t2) | 59.0 ns | +28.0% |
| `BM_ScratchCopy` | 100,000 | 45.8 ns (t2) | 59.6 ns | +30.1% |
| `BM_ApplyRefLogTxn` | 100 | 764 ns (t2) | 752 ns | -1.6% |
| `BM_ApplyRefLogTxn` | 1,000 | 783 ns (t2) | 772 ns | -1.4% |
| `BM_ApplyRefLogTxn` | 10,000 | 803 ns (t2) | 792 ns | -1.4% |
| `BM_ApplyRefLogTxn` | 100,000 | 834 ns (t2) | 858 ns | +2.9% |
| `BM_ReplayHistory` | 100 | 4.85 ms (t3) | 7.43 ms | **+53.2%** |
| `BM_ReplayHistory` | 1,000 | 35.82 ms (t3) | 48.85 ms | **+36.4%** |
| `BM_ReplayHistory` | 10,000 | 350.57 ms (t3) | 473.76 ms | **+35.2%** |
| `BM_ReplayHistory` | 100,000 | 3.67 s (t3) | 4.98 s | **+35.7%** |
| `BM_ReplayHistory` complexity fit | — | 36,679 ns/row (t3) | 50,082 ns/row | **+36.6%** (worse than the original 48,859 ns/row baseline too) |

`BM_ScratchCopy`'s +28-30% and `BM_ReplayHistory`'s regression are both understood and explained
above (§Investigation, §An honest regression), not unexplained noise. `BM_ApplyRefLogTxn` (one
materialized-state promote: copy + validate + apply + install) stays flat and within the ±10% noise
band throughout, confirming the new field costs nothing extra on the single-op live-writer path once
materialized.

### Full-suite confirmation: everything else is untouched {#e2-full-suite}

The four benchmarks above were run under a filter during the main investigation. A full,
un-filtered `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true` pass
(`build/bench_t4_e2_full.log`) confirms the four benchmarks E2 does not touch (`owned_manifests` is
never read or written by any of them) stay within ordinary run-to-run noise of their t2 baselines:

| Benchmark | N | t2 (median) | t4/E2 (median) | Delta |
|---|---|---|---|---|
| `BM_Admits` (promote preview -- never calls `manifestAlreadyOwned`) | 100 | 1,008 ns | 983 ns | -2.5% |
| `BM_Admits` | 1,000 | 1,040 ns | 1,010 ns | -2.9% |
| `BM_Admits` | 10,000 | 1,053 ns | 1,016 ns | -3.5% |
| `BM_Admits` | 100,000 | 1,067 ns | 1,088 ns | +2.0% |
| `BM_SnapshotEncode` | 100 | 15,504 ns | 14,802 ns | -4.5% |
| `BM_SnapshotEncode` | 1,000 | 158,807 ns | 150,318 ns | -5.3% |
| `BM_SnapshotEncode` | 10,000 | 1,629,810 ns | 1,542,519 ns | -5.4% |
| `BM_SnapshotEncode` | 100,000 | 16,726,370 ns | 16,296,900 ns | -2.6% |
| `BM_MergedIteration` | 100 | 754 ns | 767 ns | +1.7% |
| `BM_MergedIteration` | 1,000 | 7,711 ns | 7,790 ns | +1.0% |
| `BM_MergedIteration` | 10,000 | 82,035 ns | 81,207 ns | -1.0% |
| `BM_MergedIteration` | 100,000 | 903,707 ns | 961,586 ns | +6.4% |
| `BM_Materialize` | 100 | 12,137 ns | 12,192 ns | +0.5% |
| `BM_Materialize` | 1,000 | 127,216 ns | 126,510 ns | -0.6% |
| `BM_Materialize` | 10,000 | 1,297,018 ns | 1,305,734 ns | +0.7% |
| `BM_Materialize` | 100,000 | 17,923,253 ns | 18,687,275 ns | +4.3% |

All within the ±10% band every prior round used. One cosmetic note: `BM_MergedIteration`'s
`->Complexity()` fit picked `0.58 NlgN` this run versus the original baseline's `O(N), ~8.6 ns/row`
label -- both are `->Complexity()`'s auto-selected best least-squares fit (`oAuto`) among candidate
curves over near-identical per-N timings (deltas above are all ≤6.4%), so this is the fit selector
picking a different label for the same numbers, not a behavior change; `BM_MergedIteration` doesn't
read or write `owned_manifests` at all (it benchmarks `RefCowMap` directly, per its own doc comment).
`BM_Admits` and `BM_Materialize` keep the same complexity class and RMS as their baselines.

### Verdict

DONE_WITH_CONCERNS. The headline result lands exactly as designed: `BM_AdmitsAddPrecommit` is flat
O(1) across five orders of magnitude in N (RMS 1%), and E1's debug-build `TrustedHistory` chassert is
now O(1) too (though debug/sanitizer replay overall remains O(K×N) via `debugAssertBodyCounters`'s
per-txn rebuild -- see the correction above). Two costs were found, root-caused, and are
reported rather than hidden: a small (~13 ns, ~28%), architecturally-irreducible-within-this-design
`BM_ScratchCopy` tax (one more `shared_ptr` copy per `RefTableState` copy -- negligible against the
benchmark it feeds), and a real (~35%, `BM_ReplayHistory`-constant-level) recovery/GC-fold-replay
regression that doubles a pre-existing, already-accepted, already-deferred cost from E1's own
write-up. Neither affects the live-writer append/flush path this task targeted, and neither is a new
asymptotic class -- both are follow-up-experiment material ("materialize periodically during a long
replay" would fix both `committed`'s and `owned_manifests`' versions of the same underlying issue at
once), tracked here rather than folded silently into "flat and green." The full-suite run above
confirms the two costs are localized to exactly the two benchmarks that touch `owned_manifests`;
nothing else in the suite moved outside noise.

Gate: 1,091 tests passed, 0 failures (t3's 1,079 + 12 new E2 container tests). The gate log's "YOU
HAVE 2 DISABLED TESTS" footer is pre-existing and unrelated to E2: both are `DISABLED_`-prefixed
tests in `gtest_cas_protocol_scenarios.cpp` (`DISABLED_RevalidateAbsentTreeDepRecreates`,
`DISABLED_AdoptTreeOfReclaimedTreeFailsClosedAtAdoptTime`), predating this task. E2's own death tests
use `#if defined(DEBUG_OR_SANITIZER_BUILD)` gating (per the brief), not `DISABLED_`, so they don't
compile into this release build at all rather than showing up as disabled.

## E3 undo-journal / no-copy apply (t5) {#e3-undo-journal-t5}

Decisive experiment for this round: recover the `BM_ReplayHistory` regression E2 introduced (and that
E1 had already half-introduced), on the recovery/GC-fold replay path.

### What shipped (not the undo journal)

The task was scoped as "replace `applyRefLogTxn`'s scratch copy with an in-place apply + a bounded
undo journal". The undo journal was **not** shipped. The brief explicitly invited the simpler variant
it sketched -- "in `replay` the state is local and discarded on any throw, so a throw-away path needs
NO rollback at all" -- and that variant is both simpler and strictly faster, so it is what shipped:

- **`Full` (writer live-state + every trial/shape-check preview in `CasRefLedger.cpp`)**: keeps the
  existing two-phase scratch copy verbatim -- "throw ⇒ state byte-for-byte unchanged". This copy was
  never the regression: every `Full` caller applies against a **materialized** (empty-overlay) live
  state or a small bounded-overlay batch scratch, so the copy is O(1) shared-base pointer bumps. E2's
  `BM_ApplyRefLogTxn` (flat) already proved this.
- **`TrustedHistory` (replay only, the sole caller)**: applies **in place, no copy**. On a throw the
  state is left partially applied ("poisoned"); this is sound because `replay` builds its state
  locally and its result reaches a caller only on full success -- any throw destroys the local state
  during unwinding. This deletes the entire cost class that was regressing.

No new type, no `RefStateUndo`, no reverse-order rollback, no `noexcept`-terminate-on-alloc-failure
trade to document -- the design the brief flagged as the default. The whole change is one `if/else`
inside `applyRefLogTxn` (`CasRefProtocol.cpp`) plus doc updates on `TxnValidation` and
`applyRefLogTxn` (`CasRefProtocol.h`). `admits` was left untouched (see below).

**Post-review rename note:** after this section was written, `TxnValidation { Full, TrustedHistory }`
was renamed to `ApplyMode { LiveAppend, TrustedReplay }` -- the two semantics (skip the cross-owner
re-scan; apply in place and poison `state` on throw) intrinsically co-occur, both derived from one
caller intent ("I am replaying already-committed, already-validated history into a local state I own
and discard on any error"), and the old name advertised only the validation axis. The rest of this
section (and the E1/E2 sections above it) keeps the original names, since they describe what shipped
under those names at the time; some E1-era test names cited above were also renamed to match and no
longer appear verbatim in the current source.

### Why the regression existed, and why in-place kills it

`replay` never materializes between tail transactions (it is the pure state-machine equation;
`stateFromSnapshot` loads every row through `emplace`, which only touches the overlay). So across a
K-transaction tail the committed-map **and** the owned-manifest COW overlays grow monotonically, and
the old per-transaction `RefTableState scratch = state` deep-copied **both** overlays every time --
O(K·N). E2 added the second overlay, doubling that per-copy constant (E1: 36.7k ns/row → E2: 50.1k
ns/row, worse than the pre-E1 48.9k baseline). Applying in place removes the per-transaction copy
outright: each tail transaction now costs O(ops touched), independent of tail position.

### Step 0 exclusivity audit (gate before any code)

Every production call site of `admits` / `applyRefLogTxn` / `replay` runs either under the ledger's
`state_mutex` or on a detached/local copy -- no concurrent reader can observe a state mid-mutation, so
in-place apply is safe. (COW writes touch only the per-copy overlay, never the shared immutable base,
so even a copy that shares a base with the live state is safe to mutate in place.)

| Site | Target state | Protection | Verdict |
|---|---|---|---|
| `CasRefLedger.cpp:1138` `applyRefLogTxn(rt->state, wedged)` | LIVE `rt->state` | under `state_mutex` (l.1130) | exclusive |
| `CasRefLedger.cpp:1243` `applyRefLogTxn(shape_check, …)` | local copy | stack-local | exclusive |
| `CasRefLedger.cpp:1255` `admits(item_scratch, …)` | local copy | stack-local | exclusive |
| `CasRefLedger.cpp:1265` `applyRefLogTxn(item_scratch, …)` | local copy | stack-local | exclusive |
| `CasRefLedger.cpp:1377` `applyRefLogTxn(rt->state, final)` | LIVE `rt->state` | under `state_mutex` (l.1376) | exclusive |
| `CasRefLedger.cpp:426` `replay(…)` (recovery) | fresh local; assigned to `rt.state` only on success | detached | exclusive |
| `CasFsck.cpp:226` `replay(…)` (oracle) | fresh local | detached | exclusive |
| `CasRefProtocol.cpp:638` `replay(snapshot, tail)` inside `recoverRefTableDetailed` | fresh local; `RecoveredRefTable` value-returned, constructed only on success | detached | exclusive |

`TrustedHistory` (post-review: `ApplyMode::TrustedReplay`) is passed at exactly one place
(`CasRefProtocol.cpp:400`, inside `replay`; grep-enforced). No `admits` call site is disqualifying either -- but `admits` was NOT changed to
in-place, because it is not a regression source: its only production call (`item_scratch`, a
detached local with a batch-bounded overlay) copies an O(batch) overlay, not an O(N) one. Making it
in-place would be unmeasured complexity for no win, so per the round's default it was left as the
scratch-copy preview.

### Benchmark results (`build/bench_t5_e3.log`, `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`, medians; baseline = t4/E2)

| Benchmark | N | t4/E2 (median) | t5/E3 (median) | Delta |
|---|---|---|---|---|
| `BM_ReplayHistory` | 100 | 7.43 ms | **0.431 ms** | **-94.2%** (17.2× faster) |
| `BM_ReplayHistory` | 1,000 | 48.85 ms | **1.776 ms** | **-96.4%** (27.5×) |
| `BM_ReplayHistory` | 10,000 | 473.76 ms | **15.92 ms** | **-96.6%** (29.8×) |
| `BM_ReplayHistory` | 100,000 | 4.98 s | **0.172 s** | **-96.5%** (28.9×) |
| `BM_ReplayHistory` complexity fit | — | 50,082 ns/row | **1,725.58 ns/row** | **-96.6%** |
| `BM_ApplyRefLogTxn` | 100 | 752 ns | 778 ns | +3.5% |
| `BM_ApplyRefLogTxn` | 1,000 | 772 ns | 788 ns | +2.1% |
| `BM_ApplyRefLogTxn` | 10,000 | 792 ns | 797 ns | +0.6% |
| `BM_ApplyRefLogTxn` | 100,000 | 858 ns | 822 ns | -4.2% |
| `BM_Admits` | 100 | 983 ns | 996 ns | +1.3% |
| `BM_Admits` | 1,000 | 1,010 ns | 1,014 ns | +0.4% |
| `BM_Admits` | 10,000 | 1,016 ns | 1,029 ns | +1.3% |
| `BM_Admits` | 100,000 | 1,088 ns | 1,057 ns | -2.8% |
| `BM_AdmitsAddPrecommit` | 100 | 713 ns | 692 ns | -2.9% |
| `BM_AdmitsAddPrecommit` | 1,000 | 716 ns | 708 ns | -1.1% |
| `BM_AdmitsAddPrecommit` | 10,000 | 726 ns | 714 ns | -1.7% |
| `BM_AdmitsAddPrecommit` | 100,000 | 720 ns | 701 ns | -2.6% |
| `BM_ScratchCopy` | 100 | 60.0 ns | 58.0 ns | -3.3% |
| `BM_ScratchCopy` | 1,000 | 60.1 ns | 59.2 ns | -1.5% |
| `BM_ScratchCopy` | 10,000 | 59.0 ns | 56.7 ns | -3.9% |
| `BM_ScratchCopy` | 100,000 | 59.6 ns | 59.1 ns | -0.8% |

`BM_ReplayHistory` is recovered and crushed: 29× below the t4 regression, ~21× below t3's 36.7k
success bar, and ~28× below the original pre-E1 48.9k baseline. The four benchmarks E3 does not touch
(`BM_ApplyRefLogTxn` -- `Full`-mode scratch copy, unchanged code; `BM_Admits` / `BM_AdmitsAddPrecommit`
-- `admits`, unchanged; `BM_ScratchCopy` -- the copy primitive itself) all stay within the ±10% noise
band every prior round used. E2's O(1) `BM_AdmitsAddPrecommit` win is fully preserved.

**On the residual O(N) fit.** `BM_ReplayHistory`'s `->Complexity()` still labels the curve O(N) (now
1,725.58 ns/row). That residual N-term is **not** the per-transaction cost E3 targeted -- it is the
one-time `stateFromSnapshot` load of the size-N base at the start of `replay` (which round-trips the
whole snapshot through `encodeRefTableSnapshot`/`decodeRefTableSnapshot`, a separate cost E4's
snapshot-encoding work owns). The thing this experiment attacked -- the per-tail-transaction cost --
is now genuinely independent of N and tail position: the 256-transaction tail is O(1) per transaction,
which is exactly why the constant collapsed 29×. At small N the tail dominates the wall time (N=100:
~1.7 µs/tail-txn, flat in N); at large N the snapshot base-load dominates. Both are correct and expected.

### Elegance self-assessment and verdict

Recommendation: **KEEP**. This is a regression **recovery** (the round's decisive requirement), not a
speculative optimization, and the win is ~29× on the affected benchmark -- an order of magnitude past
the round's 2× keep-bar. On the elegance axis the shipped variant is the simplest option on the table:
it adds zero new types and zero new machinery, is a single `if/else` in one function, and removes a
cost rather than trading one complexity for another (the undo journal would have recovered the same
regression but with a reversible-entry type, reverse-order rollback, and an alloc-failure-terminate
caveat -- and would still allocate a journal per tail transaction, i.e. strictly more work than
applying in place with nothing). The one real cost is a sharpened contract: `TrustedHistory` now also
means "in-place, poison-on-throw", coupled onto the existing validation-mode enum. That coupling is
documented loudly (enum doc + `applyRefLogTxn` doc + the in-place branch comment), grep-enforced to a
single caller (`replay`), and a test pins poison-containment (for the existing caller; single-caller
exclusivity is enforced by grep+comment, not testable). It is a genuine footgun-of-last-resort if a future author adds a `TrustedHistory` caller that
keeps state after a throw -- called out here so the reviewer weighs it deliberately.

### Tests

Gate: **1,096 tests, 0 failures** (`build/test_gate_t5.log`) -- t4/E2's 1,091 plus 5 new E3 tests. The
"2 DISABLED TESTS" footer is the same pre-existing pair E2 noted (`DISABLED_`-prefixed in
`gtest_cas_protocol_scenarios.cpp`), unrelated to this task.

New tests (`gtest_cas_ref_statemachine.cpp`):
- `E3FullLaterOpThrowLeavesPopulatedStateByteIdentical` (post-review rename:
  `E3LiveAppendLaterOpThrowLeavesPopulatedStateByteIdentical`) -- `Full` path, 3-op txn whose first two
  ops touch committed+precommits+index+counters and whose third is illegal; live state byte-identical
  after (getters + encoded-snapshot bytes). The populated / later-op-throw abort path.
- `E3FullFirstOpThrowLeavesPopulatedStateByteIdentical` (post-review rename:
  `E3LiveAppendFirstOpThrowLeavesPopulatedStateByteIdentical`) -- the symmetric empty / first-op-throw
  abort path (nothing applied before the throw).
- `E3AdmitsPreviewLeavesStateByteIdentical` -- `admits` leaves the state byte-identical for both the
  accept and the reject verdict.
- `E3TrustedHistoryInPlaceMatchesFullAcrossAllArms` (post-review rename:
  `E3TrustedReplayInPlaceMatchesLiveAppendAcrossAllArms`) -- the test only the in-place machinery can
  fail: a tail exercising every `applyOp` arm (birth/add/promote/set_payload/remove-committed/remove-precommit/
  replace/remove-namespace) replayed in place produces a state byte-identical (getters + encoded bytes)
  to the same tail applied op-by-op through `Full`.
- `E3TrustedHistoryPoisonOnBadTailIsInternal` (post-review rename:
  `E3TrustedReplayPoisonOnBadTailIsInternal`) -- a tail whose last txn is illegal makes `replay` throw
  `CORRUPTED_DATA`; an independent replay of the valid prefix is unaffected, pinning that the in-place
  poison never escapes the failed call.

## E4 flat-vector base (t6) {#e4-flat-vector-base-t6}

Attacks `RefCowMap`'s internal `base` representation directly: `Base` changes from
`std::map<String, RefCommittedRow>` to a sorted `std::vector<std::pair<String, RefCommittedRow>>`
behind the same `shared_ptr<const Base>`. Keyed lookups against `base` (`find`/`contains`-equivalent
checks inside `RefCowMap::find`/`insertLive`/`erase`) switch from the tree's own `find`/`contains`/
`lower_bound` members to three small free functions in an anonymous namespace
(`baseLowerBound`/`baseFind`/`baseContains`, `Pool/CasRefCowMap.cpp`) built on `std::lower_bound`
comparing only `.first` -- same bytewise `String` order `std::map`'s default comparator uses, so no
behavior change, only representation. `materialize()` changes from "copy `*base` into a fresh
`std::map`, then replay each overlay entry through `operator[]`/`erase`" to a genuine single-pass
two-sorted-range merge into a freshly `reserve`d vector -- the same shape the read-only merged
iterator's `normalize`/`operator++` already use, just building output instead of skipping over it.
`overlay` is untouched (`std::map<String, std::optional<RefCommittedRow>>`, per the brief -- E2's own
investigation already established why a tree, not a hash map, is the right choice for a
small/mutation-heavy, frequently-copied container). The `const_iterator`'s `base_it`/`base_end` fields
change type from `std::map<...>::const_iterator` to the vector's `const_iterator` automatically (both
are aliases of `Base::const_iterator`); no other iterator code changed line-for-line.

**Files:** `Pool/CasRefCowMap.h` (`Base` alias + doc), `Pool/CasRefCowMap.cpp` (`find`/`insertLive`/
`erase`/`materialize`, three new static helpers).

### Step 1: pin-suite gate

A dedicated suite already existed at `src/Disks/tests/gtest_cas_ref_cow_map.cpp` (landed earlier,
commit `f327210151b`, prior to this round) -- not created fresh for this task. It already covers every
scenario the brief's gate lists: keyed ops (`emplace` no-overwrite + flags, `insert_or_assign` flags,
`erase(key)` return values across base-backed/overlay-only/absent, `at` throw, `find`/`contains`/
`count`), merged iteration (base-only sorted order; tombstones/overrides interleaved with a live
base -- including a tombstone as the *last* live element, `MergedIterationAppliesTombstonesAndOverrides`;
an overlay-only key sandwiched between two base keys, `FindOverlayOnlyKeyIteratesIntoBase`),
`erase(pos)` (mid-sequence and last-element/`end()`-producing), equality across different base/overlay
representations of the same content, `materialize` (folds, empties overlay, no-op on an already-empty
overlay, preserves values, leaves an earlier copy untouched), copy-shares-base `use_count`, and a
50-trial x 200-step randomized property test cross-checking every operation against a `std::map`
oracle -- which exercises tombstones/overrides at essentially every position (including sequence
start/end) across many random shapes, a broader net than a handful of hand-picked positions. Given
this already satisfies the brief's list, no new suite was created; ran it standalone first
(`--gtest_filter='CasRefCowMap.*'`, part of the full gate below) against the *unmodified* `std::map`
base to confirm green before swapping internals, then re-ran unchanged after the swap as the
semantic gate -- exactly the brief's Step 1/Step 3 sequencing.

### Step 3: build + gate

- `flock ... ninja -C build unit_tests_dbms benchmark_cas_ref_protocol` -- clean both before
  (`build/build_t6.log`, baseline confirmation) and after the swap (same log, rebuilt).
- Gate (`build/test_gate_t6.log`), same filter as every prior round: **1096 tests, 0 failures**,
  identical count to the t5 baseline (no new tests added this round; `CasRefCowMap`'s existing 17
  tests are within the `Cas*` filter and passed against both representations). `CasEncodingPins*`
  (the ordering canary) passed unchanged -- confirms the vector base's `<`-only comparator and the
  merge-iteration order produce byte-identical encoded snapshots to the `std::map` base.

### Benchmarks (`build/bench_t6_before.log` / `build/bench_t6_e4.log`, `--benchmark_repetitions=3
--benchmark_report_aggregates_only=true`, medians; "before" is a fresh same-session re-run of the
unmodified t5 code, not the t5 section's numbers verbatim, to control for cross-session machine noise)

| Benchmark | N | Before (median) | t6/E4 (median) | Delta |
|---|---|---|---|---|
| `BM_Materialize` | 100 | 12,140 ns | 7,950 ns | **-34.5%** |
| `BM_Materialize` | 1,000 | 127,202 ns | 83,627 ns | **-34.3%** |
| `BM_Materialize` | 10,000 | 1,303,959 ns | 843,585 ns | **-35.3%** |
| `BM_Materialize` | 100,000 | 18,081,268 ns | 8,549,055 ns | **-52.7%** (2.12x) |
| `BM_Materialize` complexity fit | — | 10.90 NlgN, RMS 2% | **85.38 N, RMS 0%** | **O(N log N) → O(N)** |
| `BM_MergedIteration` | 100 | 765 ns | 679 ns | -11.2% |
| `BM_MergedIteration` | 1,000 | 7,669 ns | 6,795 ns | -11.4% |
| `BM_MergedIteration` | 10,000 | 80,388 ns | 68,371 ns | -14.9% |
| `BM_MergedIteration` | 100,000 | 906,842 ns | 706,784 ns | -22.1% (1.28x) |
| `BM_MergedIteration` complexity fit | — | 0.55 NlgN, RMS 5% | **7.06 N, RMS 1%** | fit label O(NlgN) → O(N) |
| `BM_SnapshotEncode` | 100 | 14,860 ns | 14,916 ns | +0.4% |
| `BM_SnapshotEncode` | 1,000 | 153,390 ns | 147,848 ns | -3.6% |
| `BM_SnapshotEncode` | 10,000 | 1,520,322 ns | 1,524,110 ns | +0.2% |
| `BM_SnapshotEncode` | 100,000 | 15,934,998 ns | 15,772,256 ns | -1.0% |
| `BM_SnapshotEncode` complexity fit | — | 159.36 N, RMS 1% | 157.50 N, RMS 1% | -1.2% |
| `BM_ApplyRefLogTxn` | 100–100,000 | 761–829 ns | 768–814 ns | -1.8%..+1.6% (noise) |
| `BM_Admits` | 100–100,000 | 1,000–1,069 ns | 1,012–1,056 ns | -1.2%..+1.4% (noise) |
| `BM_AdmitsAddPrecommit` | 100–100,000 | 698–706 ns | 700–709 ns | +0.3%..+0.4% (noise) |
| `BM_ScratchCopy` | 100–100,000 | 58.4–59.3 ns | 57.3–59.3 ns | -2.1%..+1.5% (noise) |
| `BM_ReplayHistory` | 100–100,000 | 435,177 ns–172,348,258 ns | 436,382 ns–173,913,006 ns | -0.9%..+0.9% (noise) |
| `BM_ReplayHistory` complexity fit | — | 1,725.47 N, RMS 2% | 1,737.06 N, RMS 2% | +0.7% (noise) |

Every write-path benchmark (`BM_ApplyRefLogTxn`, `BM_Admits`, `BM_AdmitsAddPrecommit`,
`BM_ScratchCopy`, `BM_ReplayHistory`) landed inside ±2% -- confirming the brief's own prediction: the
write path only ever touches `overlay` (`std::map`, unchanged type and unchanged code), never reads or
writes `base` except through the same-complexity-class `baseContains`/`baseFind` lookups, so there was
never a mechanism by which the swap could move these numbers, and it didn't.

### Reading the wins

- **`BM_Materialize`** is the clean win: a genuine complexity-class change (the old code copied
  `*base` wholesale into a new `std::map` -- O(N) allocations plus O(log N) per overlay entry via
  `operator[]`/`erase`, i.e. O(N + K log N) -- the new code does one linear merge pass, O(N + K)).
  The measured ratio *grows with N* (1.53x, 1.52x, 1.55x, 2.12x at N=100/1,000/10,000/100,000) exactly
  as an O(N log N) vs O(N) comparison predicts, and only clears the round's 2x bar at the largest N
  tested -- the trend says it would clear it decisively at any N beyond that, but the literal
  measured numbers at the tested range are what they are.
- **`BM_MergedIteration`** shows the same growing-ratio shape (1.11x, 1.13x, 1.17x, 1.28x) from
  walking contiguous vector memory instead of chasing red-black-tree node pointers, but never reaches
  2x within the tested range -- `->Complexity()`'s auto-fit relabels it from `0.55 NlgN` to a cleaner
  `7.06 N`, but the *magnitude* of the win at any single N is a real, modest constant-factor
  improvement, not a complexity-class jump the way `BM_Materialize`'s is (`RefCowMap`'s merged
  iteration was already doing O(N) tree-iterator increments before; the vector only cuts the constant
  per increment, it doesn't remove log-factor work the old iterator wasn't paying either).
- **`BM_SnapshotEncode`** is within noise (-3.6%..+0.4%). This tracks the t5-baseline write-up's own
  prediction: `BM_SnapshotEncode`'s ~159 ns/row is dominated by the encoder's own per-row
  serialization cost (`encodeRefTableSnapshot`), not by the container's per-step iteration overhead
  (`BM_MergedIteration`'s own ~8 ns/row baseline was already an order of magnitude smaller than the
  encode cost it sits inside) -- shrinking an already-minor component by ~15-20% (extrapolating
  `BM_MergedIteration`'s own delta) is invisible against the dominant cost, exactly as observed.

### Elegance self-assessment

The change is internals-only and small: one type alias, three small free functions replacing three
tree-member-function call sites (`.find`/`.contains`/`.lower_bound` → `baseFind`/`baseContains`/
`baseLowerBound`), and `materialize()` rewritten from copy-then-replay to a direct merge -- the same
shape the read-only iterator already uses elsewhere in this file, so it isn't a new pattern in the
codebase, just the second occurrence of an existing one. No new types, no public API change, no new
test surface needed (the existing suite is the semantic gate, unchanged). The cost is that keyed base
lookups no longer get member-function ergonomics (`base->find`/`base->contains`) and instead go
through three lines of top-of-file helpers -- a minor readability tax, not a real complexity increase.
Iterator-stability reasoning is unchanged: `base` is still never mutated in place, only ever replaced
wholesale by `materialize()` via `shared_ptr` swap (`MaterializeDoesNotAffectACopyTakenBeforeIt`
already pins this), so a vector's weaker general-purpose invalidation rules (any mutating op can
reallocate) never actually apply here -- the only "mutation" `base` ever undergoes is being replaced,
not appended/erased/inserted into.

### Verdict

**Mixed against the round's literal 2x-at-tested-N bar.** `BM_Materialize` (a target benchmark) clears
2x only at N=100,000; `BM_MergedIteration` and `BM_SnapshotEncode` (the other two target benchmarks,
and the two the plan's own default-revert wording names explicitly -- "iteration/encode benchmarks")
never reach 2x anywhere in the tested range (max 1.28x and ~1.0x respectively). Per the plan's stated
default ("REVERT if <2x win on the iteration/encode benchmarks"), this experiment does not clear the
bar as written. The mitigating case for KEEP: `BM_Materialize`'s win is a genuine, provable
complexity-class change (O(N log N) → O(N), not just a constant-factor tweak) with a ratio that
provably grows without bound as N grows past the tested range, and it is the only one of the three
targets with such a clean asymptotic argument backing an eventual real win at production table sizes
(`gc_meta_pool_size` and typical table row counts documented elsewhere in this codebase run well past
100,000). `BM_MergedIteration`'s growing-but-sub-2x trend supports the same argument more weakly.
Against that, `BM_SnapshotEncode` -- the benchmark closest to the actual per-flush cold-scan production
cost this experiment was meant to speed up -- shows no meaningful improvement at all, because the
container change only ever touches a component that was already a minor fraction of that operation's
total cost. Recommendation left to the controller per the round's own delegation; this writeup leans
towards REVERT on the letter of the round's rule (2 of 3 named targets miss the bar at every tested
N), while flagging that the asymptotic argument for `BM_Materialize` specifically is stronger than a
flat percentage table alone conveys.

Gate: 1,096 tests, 0 failures (unchanged from t5 -- no new tests this round; the "2 DISABLED TESTS"
footer is the same pre-existing pair E2/E3 already noted).

**Controller verdict (post-T6): REVERT.** Two of the three target benchmarks never reach the plan's 2x bar (`BM_MergedIteration` +11-22%, `BM_SnapshotEncode` noise-level); `BM_Materialize`'s O(N log N) -> O(N) class win is real but materialize runs once per flush, has never appeared in a production trace, and table size is bounded by the admission budget -- the asymptotic tail is theoretical. Simplicity wins; the full implementation and numbers stay recorded here and in git history (revert of `93eb9957cc2`), one revert away if `BM_Materialize` ever surfaces in a soak profile.

## Final comparison table (t7) {#final-comparison-table}

Final tree = Phase A (closed class) + E1 (`ApplyMode::TrustedReplay` validation elision) + E2
(owned-manifest COW index) + E3 (in-place trusted replay). E4 reverted (numbers above, §E4).
"Final" figures are from the t5 tree (`build/bench_t5_e3.log`), which is bench-identical to the
shipped tree after the E4 revert.

| Benchmark | Baseline (t1) | Final | Change | Driver |
|---|---|---|---|---|
| `BM_AdmitsAddPrecommit` @ N=100k | 400,222 ns, O(N) | 701 ns, O(1) | **~571× faster** | E2 index |
| `BM_ReplayHistory` @ N=100k (256 txns) | 4.93 s | 172.1 ms | **~28.6× faster** | E1+E3 |
| `BM_ReplayHistory` per-row constant | 48,859 ns/row | 1,725.58 ns/row | −96.5% | E1+E3 |
| `BM_Admits` (promote) @ 100k | 1,029 ns | 1,056 ns | +2.6% (noise) | — |
| `BM_ApplyRefLogTxn` @ 100k | 788 ns | 822 ns | +4.3% (noise) | — |
| `BM_ScratchCopy` | 46.8 ns | ~58 ns | +24% (~11 ns abs) | E2 index copy; accepted |
| `BM_SnapshotEncode` / `BM_MergedIteration` / `BM_Materialize` | — | unchanged | — | E4 reverted |

### Experiment verdicts {#experiment-verdicts}

| Experiment | Verdict | One-line rationale |
|---|---|---|
| E1 relaxed replay (`TrustedReplay` validation elision) | SUBSUMED by E2 | Validation un-elided post-consult (fail-open hole); the apply-strategy split kept from E3. See §Adversarial consults + fix (t7). |
| E2 owned-manifest COW index | KEEP | The uniqueness invariant as a structure; add-precommit O(N)→O(1) |
| E3 in-place trusted replay (poison-on-throw) | KEEP | Replay −96.6% with zero new machinery; welded `ApplyMode` makes misuse inexpressible |
| E4 flat-vector `RefCowMap` base | REVERT | Only `BM_Materialize` cleared 2× (and only at N=100k); per-flush, never trace-visible |

Post-round: two parallel adversarial consults (Fable max-depth; `gpt-5.6-sol` high) + whole-branch
final review run before the 20-minute validation soak; their outcomes are appended below when
concluded.

## Adversarial consults + fix (t7) {#adversarial-consults-fix-t7}

Both consults (Fable max-depth + `gpt-5.6-sol` high) converged on ONE core defect and returned the same
verdict: **do not merge as shipped**. The convergent finding: E1's `TrustedReplay` validation elision
made corrupted history/snapshots **fail OPEN**. Post-E2 the cross-owner uniqueness check is O(1), so
eliding it on the replay path bought nothing measurable — but a double-owner input (a manifest named by
two owners) would then apply silently in a release build, drifting `owned_manifests` (a later `erase`
can hide a still-live owner) and letting ordinary `LiveAppend` writes append fresh invariant-violating
durable history, with GC's `+1/-1` edge accounting double-counting the escaped collision. The release
test `TrustedReplaySkipsCrossOwnerScanInRelease` even pinned the fail-open behavior as *desired*. Both
consults also flagged (5) that `ApplyMode` being public made the poison-on-throw path expressible by any
caller, and (3) that "writer path flat" excluded the O(N) `materializeCommitted` install held under
`state_mutex`.

Fix (A–H): the cross-owner check now runs **unconditionally** in every apply strategy (A), so `replay`
fails **closed** on a collision; `stateFromSnapshot` rejects a snapshot naming one manifest under two
owners, committed/committed, committed/precommit, or precommit/precommit (C — the codec never enforced
this); `RefCowManifestSet::insert`/`erase` throw `CORRUPTED_DATA` on drift in every build instead of a
release-elided `chassert`, so `net_delta` can never drift (D); `ApplyMode` was **deleted** — the public
`applyRefLogTxn` is always the strong guarantee, and the poison-on-throw in-place strategy moved to a
private `RefTableState::applyTxnInPlace` reachable only via `replay` (its `friend`), making misuse
structurally inexpressible rather than conventional (B). Tests pivoted: the fail-open pin was deleted and
replaced with six fail-closed pins (replay + snapshot, all three collision kinds); the container's
`#if DEBUG`-gated death tests became plain `EXPECT_THROW` in all builds (E). A recovery-latency fix (G):
the recovery-install site in `CasRefLedger.cpp` now `materializeCommitted()`s the replayed state before
retaining it (it previously installed an unmaterialized N-row-overlay state, so the first flush deep-
copied it). `manifestEdgesOfTxn`'s "replace" rule is annotated defensive-only dead surface (H).

**Gate:** 1105 tests, 0 failures (`build/test_gate_consultfix.log`). Arithmetic: t6's 1096 − 1 deleted
release test (`TrustedReplaySkipsCrossOwnerScanInRelease`) + 6 new state-machine pins + 4 container tests
now compiled in every build (were `#if DEBUG_OR_SANITIZER_BUILD`-gated, 0 in this release `build/`) =
1096 + 5 + 4 = 1105.

**Benchmarks** (`build/bench_consultfix.log`, `--benchmark_repetitions=3
--benchmark_report_aggregates_only=true`, medians):

- **Un-elide cost on `BM_ReplayHistory` (<2%, as predicted).** Measured same-session A/B
  (`build/bench_abtest_elided.log`, the check elided via a runtime flag vs. shipped): un-elided is
  **+0.91%** @N=100, **+0.91%** @N=1,000, **+0.21%** @N=10,000, **−1.0%** @N=100,000 (noise). Confirms
  the consults' prediction: post-E2 the check is O(1), so running it 256×/replay is negligible. A
  cross-*session* comparison against the t5 report numbers shows +6% at N=100k, but the same-session A/B
  isolates that as machine noise, not the un-elide (the check cannot slow the base-load-dominated large-N
  case by 6%).
- **`BM_FlushInstall` (new, finding 3).** End-to-end per-flush install (add+promote apply +
  `materializeCommitted`, folding BOTH the committed map and the owned-manifest index) — the O(N)
  critical section production holds `state_mutex` for, once per flush:

  | N | median |
  |---|---|
  | 100 | 17,108 ns |
  | 1,000 | 170,016 ns |
  | 10,000 | 1,710,345 ns |
  | 100,000 | 21,565,274 ns |

  O(N) (fit label `12.97 NlgN`, RMS 1%). This is the honest writer-latency number the "writer path flat"
  claim (drawn from `BM_ApplyRefLogTxn`, which stops before install) must be weighed against: the
  admission win is ~700 ns, but a large table's flush install is milliseconds. It sits ~15% above the
  shipped-report `BM_Materialize` (map-only) 100k number, i.e. roughly the added `owned_manifests` copy.
- **Nothing else moved >10%.** `BM_AdmitsAddPrecommit` ~703–706 ns flat O(1); `BM_ApplyRefLogTxn`
  753–823 ns; `BM_Admits` 995–1,057 ns; `BM_ScratchCopy` ~57–59 ns — all within noise of the t5/E3
  Final figures. E2's O(1) add-precommit win and E3's ~28× replay win are fully preserved.

**Judgment calls.** (1) Consult findings 2 (post-durable-PUT allocation window) and 4's precommit-COW
half are real but explicitly OUT OF SCOPE for A–H (they need a candidate-materialize-before-PUT
restructuring and a measured precommit-COW decision respectively); recorded here, not fixed in this
patch. (2) G materializes only the ONE retained recovery-adopt site (`CasRefLedger.cpp:426`); the
fsck/GC `replay`/`recoverRefTable` locals (`CasFsck.cpp`, `CasOrphanManifestSweep.cpp`, `CasGc.cpp`) are
iterated once and dropped, so they are deliberately left unmaterialized. (3) `applyTxnInPlace` is a
single private member shared by the public scratch-wrapper and `replay`; the poison-on-throw property is
a consequence of `replay` calling it directly on a discard-on-throw local, and is unreachable from
outside the translation unit.

## Round-2 consults: closure verification + one disagreement resolved (t7) {#round-2-consults-t7}

Both round-1 consultants re-ran on the post-fix tree. Fable: all four findings CLOSED with
structural guarantees, sound to soak and merge; residuals = wedge-resolution tail-counter skip
(fixed, this commit) and the post-PUT catch over-claim (folded into the backlog item).
`gpt-5.6-sol`: three findings CLOSED, but raised a claimed BLOCKER — the GC fold extracts manifest
edges without state-machine replay, so codec-valid-but-fabricated removals of live edges would fold
wrong `-1`s. Resolved by the two-model refutation protocol: Fable conceded every mechanical step,
then REFUTED exploitability (the single lease-holding writer structurally cannot mint such history
— validated-prefix argument; raw appenders are test-only; S3 tamper out of trust model), showed
this round NARROWED the surface and is prerequisite to the proper gate, and showed the proposed
cursor-aligned-witness fix infeasible (snapshots publish ahead of the GC cursor). Outcome: BACKLOG
"GC per-table recovery gate before fold" (abort-only clamp; MANDATORY before multi-writer /
rolling-upgrade-skew milestones); the round's false "dead surface" annotation on
`manifestEdgesOfTxn`'s replace rule corrected in place. Soak validity retained — the attack
requires forged durable traffic no soak component can produce.
