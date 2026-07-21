# `RefTableState` Closed Class + Experiment-Driven Optimization — Design

Date: 2026-07-21. Branch: `cas-gc-rebuild`.

## Problem {#problem}

Four CPU incidents in a row circled the same code without the container itself ever being the
defect: `__copy_construct_tree` on state copies (fixed by `RefCowMap`, spec 2026-07-17), full
table re-encode in `admits` (fixed by incremental byte counters, spec 2026-07-20), the
`manifestAlreadyOwned` linear value-scan (found in the 2026-07-21 soak trace), and replay paths
(GC fold, recovery, orphan-sweep protection view) paying writer-admission invariants for every
historical transaction — the round-3 fold replayed ~97k transactions, each add paying an O(N)
scan.

The systemic diagnosis (2026-07-21 session): `RefTableState` has grown into a small storage
engine — keyed point-get, ordered scan for canonical snapshot encoding, cheap snapshots, batch
apply, incremental byte accounting, value-uniqueness invariant (manifest → owner), mass replay —
but it is shaped as "plain struct + free functions". Every new contract requirement arrives as a
surprise hotspot, and derived fields (the byte counters, any future index) stay consistent only
by discipline: any code can mutate `committed` past `applyOpInPlace` and silently break them.

## Goal {#goal}

1. **Contract**: `RefTableState` becomes a closed class. Invariants hold by construction, not by
   discipline. No release-build self-re-verification.
2. **Performance**: every hot operation O(1) or as close as achievable, selected by experiment —
   several alternative internals are tried inside the closed class, compared on benchmarks *and*
   code elegance, winners kept, losers reverted but their numbers recorded.
3. **Benchmarks**: every critical operation gets a benchmark with an asymptotic-complexity read,
   so the next regression is caught by a number, not by a soak trace.

### Success criterion (final gate) {#success-criterion}

After the winning combination lands, a ~20-minute phase-3 soak must show, in the
`system.trace_log` CPU profile (per the analyzing-cas-health method): **the hottest
CAS-attributed stack family is blob hashing** (digest computation), and **every other
CAS-attributed stack family is cheaper by multiples** — operationalized as ≤ 1/3 of the hashing
family's sample count. Correctness invariants (step 1 of the health skill) must be zero-hit, and
the whole `Cas*:CA*` gtest gate plus property tests stay green throughout.

## Non-goals {#non-goals}

- No change to persisted or wire bytes: the canonical snapshot encoding (ordered rows, exact
  bytes) and the ref-log transaction format stay byte-identical. Property tests pin this.
- No change to the ref-protocol state machine: the set of legal transitions and their
  preconditions is untouched. Experiment E1 changes *where* a precondition is re-checked, never
  *whether* it holds.
- No change to the ledger concurrency design (single-leader batched lane, leader-copy-then-
  lockless-PUT isolation). Experiments live strictly inside the state container.
- No general-purpose storage-engine dependency. N is thousands-to-100k; the problem was never
  the map's asymptotics but O(N)-work-per-op patterns around it.

## Phase A — encapsulation (no behavior change) {#phase-a-encapsulation}

`RefTableState` (in `CasRefProtocol.h`, staying in place) becomes a class:

- **Private**: `lifecycle`, `remove_txn_id`, `greatest_applied`, `committed` (`RefCowMap`),
  `precommits`, `snapshot_body_bytes`, `removal_body_bytes`, and any future derived index.
- **Public reads** (cold paths — encode, fsck, snapshot construction, tests): lifecycle/txn-id
  getters, keyed lookup into `committed`, const iteration over `committed` and `precommits`,
  the two budget-size accessors.
- **Public mutations**: only the protocol entry points — `applyRefLogTxn`, `replay`,
  `stateFromSnapshot`, and the `admits` preview. Whether these remain free functions that are
  `friend`s of the class or become methods is the implementer's call; the binding requirement is
  that *no field is mutable from outside the protocol implementation*.
- All derived-field maintenance (counters, indexes) lives in the same private helpers that
  perform the primary mutation, so an experiment that adds an index cannot forget an arm.

Gate: the full existing gtest battery and property tests pass unchanged; Phase B benchmarks show
encapsulation itself is zero-cost (within noise of the pre-A baseline).

## Phase B — benchmark suite for every critical operation {#phase-b-benchmarks}

Extend `benchmarks/benchmark_cas_ref_protocol.cpp` (built with `-DENABLE_BENCHMARKS=ON`;
baseline numbers recorded in the file-header comment, the file's existing convention). Target
list, each over `Range(100, 100000)` table sizes with `->Complexity()`:

| Benchmark | Measures | Hot caller |
|---|---|---|
| `BM_Admits` (exists) | single-op preview | writer per-op |
| `BM_ApplyRefLogTxn` | one transaction apply (validate + install) | writer append, replay |
| `BM_ReplayHistory` | K transactions over a size-N table | GC fold, recovery, protection view |
| `BM_ScratchCopy` | full state copy (the isolation primitive) | `admits`, `applyRefLogTxn` |
| `BM_ManifestAlreadyOwned` | value-uniqueness check in isolation | add-precommit arm |
| `BM_Materialize` | overlay fold into a new base | state install per flush |
| `BM_SnapshotEncode` | canonical snapshot bytes for size N | snapshot publish |
| `BM_MergedIteration` | full merged scan, varying overlay fraction | encode, fsck |

Baselines are captured **before** Phase A (current struct), re-captured after Phase A
(zero-cost check), and then after every experiment.

## Phase C — experiment matrix {#phase-c-experiments}

Each experiment: implement inside the closed class → full gtest + property gate green → bench
run → verdict recorded (numbers + an explicit elegance judgment). Winners stay; losers are
reverted, but every verdict row survives in the comparison table (Phase D). The matrix is open:
new candidates discovered mid-round join the table instead of derailing the round.

- **E1 — relaxed replay of validated history.** Replay-for-view paths re-prove writer
  invariants on history the writer already validated and CAS-committed. Add a replay mode that
  skips admission re-checks (`manifestAlreadyOwned`) on committed history, keeping them as
  `chassert` in debug/sanitizer builds (the existing pattern of `admits`'s drift check). ~O(K)
  fold instead of O(K×N). Cheapest change; philosophically aligned: trust construction instead
  of re-verifying it.
- **E2 — COW owned-manifest index.** A copy-cheap set of manifests that currently have an
  owner: the value-uniqueness invariant *as a structure*. Membership check O(log N)/O(1) for
  all paths (writer and strict replay). Caveat driving the "experiment" framing: a plain
  `std::set` would make every scratch copy O(N) again — the index must itself be COW
  (generalize `RefCowMap` or add a sibling), which is real machinery (~150-200 lines + tests).
- **E3 — transaction layer instead of scratch copy.** A transaction applies into its own
  overlay level: commit folds the level, abort discards it (Keeper `UncommittedState`-style
  deltas). Removes full-state copying from `applyRefLogTxn`/`admits` entirely: O(ops) per
  transaction. Must preserve the two-phase guarantee — no intra-transaction state observable,
  strong exception safety on throw.
- **E4 — flat sorted vector base for `RefCowMap`.** Immutable shared sorted vector + overlay:
  point-get by binary search, merged scan over contiguous memory, materialize as a two-range
  merge into a fresh vector. Attacks the constants of `BM_MergedIteration`,
  `BM_SnapshotEncode`, `BM_Materialize` (node-based `std::map` pointer-chasing).

Interaction note: E3 and E2/E4 compose (a txn layer works over either base); E1 reduces the
weight of what E2 must speed up. The comparison table judges combinations, not only singles —
the expected end state is a winning *combination*.

## Phase D — selection and final validation {#phase-d-selection}

1. Comparison table — one row per experiment/combination: benchmark deltas per operation,
   asymptotic class, gates status, elegance verdict, keep/revert decision. Recorded in a
   companion report `docs/superpowers/reports/2026-07-XX-reftablestate-experiments.md` and
   linked from this spec once written.
2. Losers reverted from the branch; the report keeps their numbers so the decision never needs
   re-deriving.
3. Final gate: the ~20-minute phase-3 soak and the success criterion above
   ([Success criterion](#success-criterion)). If the criterion fails, the round is not done —
   the residual hotspot becomes the next experiment row.

## Testing strategy {#testing-strategy}

- Existing `Cas*:CA*` gtest battery + property tests are the semantic gate for every phase and
  every experiment (byte-identical snapshot encoding pinned by property tests).
- Phase A adds targeted gtests only where the class surface makes new behavior reachable
  (e.g. E3's abort path discarding a partially applied level).
- Benchmarks are the performance instrument; no timing asserts in unit tests.
- Final: 20-minute soak + trace-profile check per the success criterion.

## Risks {#risks}

- **E3 atomicity**: replacing scratch-copy two-phase with overlay levels moves the strong
  exception guarantee into new code — the property "state byte-for-byte unchanged after a
  throwing transaction" must be explicitly tested.
- **E4 iteration/invalidation**: flat-base iterators have different invalidation rules; the
  read API of the closed class must not leak iterator stability promises the old base gave.
- **Shared branch**: `cas-gc-rebuild` is concurrently worked by another session — commit
  discipline per the shared-worktree rules (verify `HEAD` after commit, one ninja at a time,
  never rebase/amend).
