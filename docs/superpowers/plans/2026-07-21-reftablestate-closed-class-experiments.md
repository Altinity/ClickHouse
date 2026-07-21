# `RefTableState` Closed Class + Experiment-Driven Optimization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `RefTableState` into a closed class whose invariants hold by construction, benchmark every critical operation, then run experiments E1-E4 inside the closed class and keep the combination that makes every hot ref-table operation O(1)-or-near, judged on numbers plus elegance.

**Architecture:** Spec `docs/superpowers/specs/2026-07-21-reftablestate-closed-class-experiments-design.md`. All work in `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/` (`CasRefProtocol.{h,cpp}`, `CasRefCowMap.h` + new `.cpp` if split) plus the benchmark suite and tests. Persisted bytes and the state machine are untouchable (spec non-goals).

**Tech Stack:** C++ (Allman braces), Google Benchmark (`-DENABLE_BENCHMARKS=ON`, already ON in `build/`), gtest.

## Global Constraints

- Branch `cas-gc-rebuild`, shared with a parallel session: new commits only (no rebase/amend), verify `HEAD` after every commit, at most one `ninja` at a time (`flock /tmp/claude-1000/ninja.lock ninja -C build ...`), never push.
- Persisted/wire bytes byte-identical: `CasEncodingPins*` and all snapshot/log codec tests must stay green in every task. No change to transition legality or preconditions (E1 changes *where* a precondition is re-checked in release builds, never whether it holds).
- The CAS unit gate for EVERY task is: `build/src/unit_tests_dbms --gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'` — the definitive filter; verify coverage with `--gtest_list_tests` if any suite is renamed.
- Build outputs always redirected: `flock /tmp/claude-1000/ninja.lock ninja -C build unit_tests_dbms benchmark_cas_ref_protocol > build/build_<task>.log 2>&1`. Test/bench outputs to uniquely named logs in `build/`.
- Benchmark runs: `build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol --benchmark_repetitions=3 --benchmark_report_aggregates_only=true > build/bench_<tag>.log 2>&1`. Numbers (median) get recorded in the bench file's header history comment AND in the comparison report.
- Comparison report file: `docs/superpowers/reports/2026-07-21-reftablestate-experiments.md` — created in Task 1, appended by every task, one verdict row per experiment.
- Experiment keep/revert decisions are the CONTROLLER's, made on recorded numbers + elegance; a reverted experiment is reverted with `git revert` (no history rewriting).
- House style: member getters are `getX`; function names in prose as `f` (no parentheses); `MergeTree`-style inline code for literals.

---

### Task 1: Benchmark suite covering every critical operation + pre-encapsulation baselines

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`
- Create: `docs/superpowers/reports/2026-07-21-reftablestate-experiments.md`

**Interfaces:**
- Produces: helper `makeSyntheticSnapshot(n)` → `RefTableSnapshot`, and `makeSyntheticState(n)` rebuilt on top of `replay` (public API that survives encapsulation) — later tasks re-run this exact suite unchanged.

**Key insight to encode in comments:** the existing `BM_Admits` uses a *promote* op, which never calls `manifestAlreadyOwned` — that is why it reports O(1) while production traces still show a linear scan. The production-hotspot shape is *add-precommit* (every part publication starts with one), benchmarked here as `BM_AdmitsAddPrecommit`.

- [ ] **Step 1: Rebuild the synthetic-state helper on `replay`** (survives encapsulation):

```cpp
/// A synthetic snapshot of `n` committed rows plus one pending precommit ready to promote.
/// Built as a RefTableSnapshot and materialized via the public `replay` entry point, so this
/// helper keeps compiling unchanged when RefTableState's fields become private (Phase A).
RefTableSnapshot makeSyntheticSnapshot(size_t n)
{
    RefTableSnapshot snapshot;
    snapshot.ns = "roots/bench";
    snapshot.snapshot_id = RefTxnId{1, 1};
    snapshot.lifecycle = RefLifecycle::Live;
    for (size_t i = 0; i < n; ++i)
    {
        RefCommittedRow row;
        row.ref_name = "part_" + std::to_string(i) + "_20260719_0_1000_1";
        row.manifest_ref = ManifestRef{1, 1, static_cast<uint32_t>(i + 1)};
        snapshot.committed.push_back(row);
    }
    std::sort(snapshot.committed.begin(), snapshot.committed.end(),
              [](const auto & a, const auto & b) { return a.ref_name < b.ref_name; });
    snapshot.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "new_part_x", ManifestRef{1, 1, 999999}});
    return snapshot;
}

RefTableState makeSyntheticState(size_t n)
{
    return replay(makeSyntheticSnapshot(n), {});
}
```

(Check `RefTableSnapshot`'s field spelling against `CasRefSnapshotFormat.h` — precommit sort order must satisfy the codec; sort if needed. Delete the old field-poking `makeSyntheticState`.)

- [ ] **Step 2: Add the seven new benchmarks** (all `->RangeMultiplier(10)->Range(100, 100000)->Complexity()` unless noted):

```cpp
/// THE production hotspot shape: add-precommit runs `manifestAlreadyOwned` (a linear value scan
/// today). Expected O(N) before the experiments, O(1) after the winning combination.
static void BM_AdmitsAddPrecommit(benchmark::State & state)
{
    const size_t n = static_cast<size_t>(state.range(0));
    const RefTableState table = makeSyntheticState(n);
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "brand_new_part", ManifestRef{2, 1, 1}};
    for (auto _ : state)
        benchmark::DoNotOptimize(admits(table, op, 1ull << 40, 1ull << 40));
    state.SetComplexityN(static_cast<int64_t>(n));
}

/// One transaction end-to-end: scratch copy + validate + apply + install (a promote of the
/// staged precommit). The copy is part of the measured cost on purpose -- it is what E3 attacks.
static void BM_ApplyRefLogTxn(benchmark::State & state)
{
    const size_t n = ...;
    const RefTableState table = makeSyntheticState(n);
    RefLogTxn txn;  // ns = "roots/bench", txn_id = {1, 2}, one promote op of ("new_part_x", {1,1,999999})
    for (auto _ : state)
    {
        RefTableState scratch = table;
        applyRefLogTxn(scratch, txn);
        benchmark::DoNotOptimize(&scratch);
    }
    state.SetComplexityN(...);
}

/// The fold/recovery profile: K transactions replayed over a size-N snapshot. Each txn creates
/// and promotes one new ref (two ops), so each add pays today's `manifestAlreadyOwned` scan.
/// K fixed at 256; complexity fit is over N.
static void BM_ReplayHistory(benchmark::State & state)
{
    const size_t n = ...;
    const RefTableSnapshot snapshot = makeSyntheticSnapshot(n);
    std::vector<RefLogTxn> tail;  // 256 txns: txn k has id {1, 2+k}, ops = [add precommit ("replay_part_<k>", {3,1,k+1}), promote same]
    for (auto _ : state)
        benchmark::DoNotOptimize(replay(snapshot, tail));
    state.SetComplexityN(...);
}

/// The isolation primitive on its own: one full state copy (COW committed + std::set precommits
/// + counters). Overlay is empty (state fresh from replay+materialize), so this is the floor.
static void BM_ScratchCopy(benchmark::State & state) { ... RefTableState copy = table; DoNotOptimize(&copy); ... }

/// Canonical snapshot encoding for size N (per-flush cost, expected O(N) -- the question is the
/// constant, which E4's contiguous scan attacks).
static void BM_SnapshotEncode(benchmark::State & state) { ... DoNotOptimize(encodeRefTableSnapshot(snapshotOf(table, "roots/bench"))); ... }

/// Full merged iteration with a 10% overlay (post-copy, pre-materialize shape).
static void BM_MergedIteration(benchmark::State & state)
{
    // table = makeSyntheticState(n); then insert n/10 extra rows through the public map API if
    // reachable, otherwise via replay of add+promote txns; iterate summing row.ref_name.size().
}

/// RefCowMap::materialize after one overlay insert on an N-row base (per-flush install cost).
/// Benchmarks RefCowMap directly -- it is a public class.
static void BM_Materialize(benchmark::State & state) { ... RefCowMap copy = base_map; copy.insert_or_assign(...); copy.materialize(); ... }
```

For `BM_ScratchCopy`: after `makeSyntheticState`, materialize the committed map first (today: `state.committed.materialize()`; from Task 2 on: `state.materializeCommitted()` — write it with whatever compiles at this task).

- [ ] **Step 3: Build and run** (`flock /tmp/claude-1000/ninja.lock ninja -C build benchmark_cas_ref_protocol > build/build_t1.log 2>&1`; then the bench run per Global Constraints, tag `t1_pre_encapsulation`). Verify `BM_AdmitsAddPrecommit` shows O(N) growth and `BM_Admits` stays O(1) — that contrast is the round's founding measurement. If `BM_AdmitsAddPrecommit` does NOT grow with N, STOP and report BLOCKED (the mental model is wrong; controller re-triages).

- [ ] **Step 4: Record baselines.** Append the medians to the bench file's header history comment (new section `Phase B baselines, 2026-07-21, pre-encapsulation`) and create the comparison report with a `## Baselines {#baselines}` table (rows = benchmarks, columns = N).

- [ ] **Step 5: Run the CAS unit gate** (unchanged code paths, but proves the tree builds clean): filter per Global Constraints, log `build/test_gate_t1.log`. Expected: same pass count as the branch baseline, zero failures.

- [ ] **Step 6: Commit** — `cas: bench RefTableState critical ops; expose add-precommit O(N) hotspot (Phase B)`.

### Task 2: Phase A — encapsulate `RefTableState` (no behavior change)

**Files:**
- Modify: `Pool/CasRefProtocol.h` (class conversion), `Pool/CasRefProtocol.cpp` (members), `Pool/CasRefLedger.cpp`, `Pool/CasPartWriteTxn.cpp`, `Gc/CasGc.cpp`, `Gc/CasOrphanManifestSweep.cpp`, `Tools/CasFsck.cpp`, `Formats/CasFoldSealFormat.cpp` + `Formats/CasRefSnapshotFormat.cpp` (only if they touch `RefTableState` instances, not just snapshots), `src/Disks/tests/cas_test_helpers.h`, `src/Disks/tests/gtest_cas_ref_statemachine.cpp`, `gtest_cas_ref_writer.cpp`, `gtest_cas_fsck.cpp`, `gtest_cas_ref_gc.cpp`, `gtest_cas_part_write.cpp`, `gtest_cas_encoding_pins.cpp` (whichever reference state fields)

**Interfaces (produced — binding for all later tasks):**

```cpp
class RefTableState
{
public:
    RefTableState() = default;

    RefLifecycle getLifecycle() const;
    const std::optional<RefTxnId> & getRemoveTxnId() const;
    const RefTxnId & getGreatestApplied() const;
    const RefCowMap & getCommitted() const;
    const std::set<std::pair<String, ManifestRef>> & getPrecommits() const;
    uint64_t getSnapshotBodyBytes() const;
    uint64_t getRemovalBodyBytes() const;

    /// State-install point only (once per ref-log flush, never per batch item): folds the
    /// committed map's COW overlay into a fresh shared base.
    void materializeCommitted();

private:
    /* the exact fields of today's struct, same names */

    void applyOp(const RefOp & op, const RefTxnId & txn_id);          // was free applyOpInPlace
    void applyOwnerTransition(const RefOp & op);                       // was free
    void applySetPayload(const RefOp & op);                            // was free
    bool manifestAlreadyOwned(const ManifestRef & manifest_ref) const; // was free
#ifdef DEBUG_OR_SANITIZER_BUILD
    void debugAssertBodyCounters() const;                              // was free
#endif

    friend void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn);
    friend RefTableState stateFromSnapshot(const RefTableSnapshot & snapshot);
    friend bool admits(const RefTableState & state, const RefOp & op,
                       uint64_t snapshot_budget, uint64_t removal_budget);
};

/// Promoted from CasRefProtocol.cpp's anonymous namespace to the public protocol API: the ONE
/// validated way to construct a state from rows (codec round-trip validation) -- tests and
/// benchmarks use it instead of poking fields.
RefTableState stateFromSnapshot(const RefTableSnapshot & snapshot);
```

Free functions `applyRefLogTxn`, `replay`, `snapshotOf`, `admits`, `encodedSnapshotBudgetSize`, `encodedRemovalBudgetSize` keep their exact signatures (consumer call sites keep compiling after a read-accessor sweep). `checkRemoveNamespaceOrdering` stays a free helper (touches no fields).

- [ ] **Step 1:** Convert the struct per the surface above; move the four helpers + debug assert into private members; promote `stateFromSnapshot` to the header. Keep every doc comment, updating only what moved.
- [ ] **Step 2:** Sweep consumers: `state.lifecycle` → `state.getLifecycle()` etc.; the ledger's per-flush `working.committed.materialize()` (find the exact site in `flushRefBatch`) → `working.materializeCommitted()`. If any production site MUTATES a field directly (not via the protocol functions), STOP and report BLOCKED with the site — that is an undocumented invariant bypass the controller must triage; do not silently wrap it.
- [ ] **Step 3:** Migrate tests: field-constructed states become `stateFromSnapshot(...)` / `replay(...)` + transactions; field assertions become getter assertions. A test that deliberately builds an *unrepresentable* state (if any exists) is reported to the controller with its intent, not force-migrated.
- [ ] **Step 4:** Build + full CAS gate (log `build/test_gate_t2.log`): identical pass/fail set to Task 1's run.
- [ ] **Step 5:** Re-run the full bench suite (tag `t2_post_encapsulation`); append to report. Acceptance: every benchmark within noise (±10%) of Task 1 baselines — encapsulation must be zero-cost.
- [ ] **Step 6: Commit** — `cas: RefTableState is a closed class -- invariants by construction (Phase A)`.

### Task 3: E1 — relaxed replay of validated history

**Files:**
- Modify: `Pool/CasRefProtocol.h`, `Pool/CasRefProtocol.cpp`, `src/Disks/tests/gtest_cas_ref_statemachine.cpp` (new tests), benchmark file (no code change expected — re-run only)

**Design.** `replay` (and only `replay` — the writer's direct `applyRefLogTxn` appends stay strict) replays history that was already validated at append time and CAS-committed. The one O(N) admission re-check, `manifestAlreadyOwned`, becomes debug-only on that path:

```cpp
/// How much admission re-checking a transaction application performs. `Full` is the writer's
/// append-time contract. `TrustedHistory` is for replaying transactions that already passed
/// `Full` validation when they were durably appended (recovery, GC fold, fsck, protection
/// views): the O(N) cross-owner uniqueness scan is elided in release builds and kept as a
/// `chassert` in debug/sanitizer builds (same policy as `debugAssertBodyCounters`). Every
/// exact-binding precondition (cheap, keyed) is enforced in BOTH modes -- a corrupted log
/// object still fails closed in either mode.
enum class TxnValidation : uint8_t { Full, TrustedHistory };

void applyRefLogTxn(RefTableState & state, const RefLogTxn & txn, TxnValidation validation = TxnValidation::Full);
```

In `applyOwnerTransition`'s add-precommit arm (threading `validation` down through `applyOp`):

```cpp
if (validation == TxnValidation::Full)
{
    if (manifestAlreadyOwned(b.manifest_ref))
        throw Exception(ErrorCodes::CORRUPTED_DATA, ...);   // unchanged message
}
else
{
    /// Trusted replay: the append-time Full validation already proved uniqueness; re-prove it
    /// only where chassert is active.
    chassert(!manifestAlreadyOwned(b.manifest_ref));
}
```

`replay` passes `TxnValidation::TrustedHistory`. Audit every OTHER caller of `applyRefLogTxn` (grep; expected: `CasRefLedger` append/flush paths — those stay `Full` by default-argument) and record the audit in the report.

- [ ] **Step 1: Failing test first.** New gtest `CasRefStateMachine.TrustedHistoryReplaySkipsCrossOwnerScanInRelease`: hand-build (via the codec, since `stateFromSnapshot` validates) a tail whose add-precommit WOULD collide cross-owner — assert `Full` throws `CORRUPTED_DATA`; assert `TrustedHistory` behavior: under `DEBUG_OR_SANITIZER_BUILD` it still aborts (death test or skip, matching `CasBlobDigestDeathTest` convention), in release it applies. Plus a positive test: `replay` of a valid tail produces a state identical (getters + `snapshotOf` bytes) to `Full` replay.
- [ ] **Step 2:** Implement; build; CAS gate (`build/test_gate_t3.log`).
- [ ] **Step 3:** Re-run benches (tag `t3_e1`). Expected: `BM_ReplayHistory` drops its O(N) term (release build); `BM_AdmitsAddPrecommit` unchanged (writer path untouched). Append numbers + verdict row to the report.
- [ ] **Step 4: Commit** — `cas: E1 relaxed replay -- trusted history skips the O(N) cross-owner re-scan`.

### Task 4: E2 — COW owned-manifest index (the uniqueness invariant as a structure)

**Files:**
- Create: `Pool/CasRefCowManifestSet.h` (+ `.cpp` if non-trivial)
- Modify: `Pool/CasRefProtocol.h` (new private field), `Pool/CasRefProtocol.cpp` (maintenance in the four arms + `stateFromSnapshot` + `debugAssertBodyCounters` extension), new gtest file `src/Disks/tests/gtest_cas_ref_cow_manifest_set.cpp`

**Design.** A copy-cheap membership set of every `ManifestRef` that currently has an owner (committed row or precommit). Copy-cheapness is MANDATORY: a plain `std::set` would make every scratch copy O(N) again — the exact regression `RefCowMap` was built to kill. Same COW pattern, membership-only (no ordered iteration needed):

```cpp
/// A value-semantic membership set of ManifestRef with O(overlay) copies: shared immutable base
/// + per-copy overlay (true = present, false = tombstone). The ref table uses it to hold the
/// add-precommit uniqueness invariant ("no conflicting owner may name the same manifest") as a
/// structure instead of a linear scan. Not thread-safe; same ownership rules as RefCowMap.
class RefCowManifestSet
{
public:
    bool contains(const ManifestRef & m) const;
    void insert(const ManifestRef & m);    /// must be absent (chassert) -- the invariant guarantees it
    void erase(const ManifestRef & m);     /// must be present (chassert)
    size_t size() const;                    /// base + net_delta, O(1)
    void materialize();                     /// fold overlay into a fresh shared base
private:
    struct Hash { size_t operator()(const ManifestRef & m) const; };  /// combine the three ints
    std::shared_ptr<const std::unordered_set<ManifestRef, Hash>> base;
    std::unordered_map<ManifestRef, bool, Hash> overlay;
    int64_t net_delta = 0;
};
```

Maintenance (all inside the existing private arms — the closed class makes forgetting an arm impossible to miss in review):
- add precommit → `insert`; remove precommit → `erase`; remove committed → `erase`; promote → NO-OP (same manifest keeps an owner throughout); `set_payload` → no-op; `stateFromSnapshot` → seed; `remove_namespace` → `chassert(owned_manifests.size() == 0)`; `materializeCommitted` → also `owned_manifests.materialize()`.
- `manifestAlreadyOwned` body becomes `return owned_manifests.contains(manifest_ref);` — O(1) for writer AND strict replay. `debugAssertBodyCounters` gains a full rebuild-and-compare of the set (the old linear scan lives on as the debug cross-check).

- [ ] **Step 1:** TDD the container: dedicated gtest suite `CasRefCowManifestSet` (insert/erase/contains across base+overlay, tombstone re-insert, materialize, copy-shares-base via a `use_count` test hook, chasserted misuse as death tests under sanitizers).
- [ ] **Step 2:** Wire into `RefTableState`; extend `debugAssertBodyCounters`; the E1 `chassert(!manifestAlreadyOwned(...))` now costs O(1) — note in the report that E1+E2 compose (debug replay stops being O(K×N) too).
- [ ] **Step 3:** Build; CAS gate (`build/test_gate_t4.log`); state-machine property tests unchanged and green.
- [ ] **Step 4:** Re-run benches (tag `t4_e2`). Expected: `BM_AdmitsAddPrecommit` flat O(1) across the range; `BM_ScratchCopy` unchanged (copy stays O(overlay)); `BM_ReplayHistory` debug≈release now. Append + verdict row.
- [ ] **Step 5: Commit** — `cas: E2 COW owned-manifest index -- add-precommit uniqueness O(1)`.

### Task 5: E3 — transaction undo-journal instead of scratch copy (experiment; keep-or-revert)

**Files:**
- Modify: `Pool/CasRefProtocol.{h,cpp}`, `src/Disks/tests/gtest_cas_ref_statemachine.cpp` (abort-path tests)

**Design.** Replace `applyRefLogTxn`'s and `admits`'s whole-state scratch copy with in-place apply + a bounded undo journal (Keeper `UncommittedState`-delta spirit): O(ops touched) per transaction instead of O(overlay + precommits) copy.

```cpp
/// One reversible step of an in-place transaction application. Rollback applies inverses in
/// reverse order inside a noexcept context: the only possible failure is allocation, and an
/// allocation failure mid-rollback is unrecoverable by design (std::terminate) -- documented
/// as this experiment's trade against the copy-based strong guarantee.
struct RefStateUndo
{
    struct RestoreCommitted { String ref_name; std::optional<RefCommittedRow> old_row; };  // nullopt = erase
    struct RestorePrecommit { std::pair<String, ManifestRef> binding; bool was_present; };
    struct RestoreMeta { RefLifecycle lifecycle; std::optional<RefTxnId> remove_txn_id; };
    /* counters + owned-manifest index snapshot: two uint64 + the index's own undo entries */
};
```

`applyRefLogTxn`: record undo per mutation; on throw, roll back and rethrow (state byte-identical — the existing two-phase contract); on success, clear journal + set `greatest_applied`. `admits`: apply the single op in place, read the two budget sizes, ALWAYS roll back — zero copies, but this mutates a `const`-today state internally, so:

- [ ] **Step 0 (gate before any code):** Audit every `admits` and `applyRefLogTxn` call site for exclusivity — both must only ever run under the ledger's state lock or on a detached copy (read `CasRefLedger.cpp`'s `flushRefBatch`/batch-builder locking; write the finding into the report). If any concurrent-reader call site exists, E3 is DISQUALIFIED for `admits` (keep scratch-copy `admits`, journal only `applyRefLogTxn`) — record and proceed with the reduced scope.
- [ ] **Step 1:** Failing tests: mid-transaction throw leaves state byte-identical (compare `snapshotOf` encodings + all getters before/after a 3-op txn whose 3rd op is illegal — both with an empty and a populated undo path); `admits` preview leaves state byte-identical.
- [ ] **Step 2:** Implement; build; CAS gate (`build/test_gate_t5.log`).
- [ ] **Step 3:** Benches (tag `t5_e3`): `BM_ApplyRefLogTxn` and `BM_Admits`/`BM_AdmitsAddPrecommit` should shed the copy cost; `BM_ScratchCopy` becomes reference-only. Append + verdict row with BOTH numbers AND an elegance judgment (undo-journal complexity vs measured win) — the controller decides keep vs revert; if the win over post-E2 numbers is marginal (<2× on the affected benchmarks at N=100k), the default is REVERT (the COW copy is already cheap; simplicity wins per the user's stated preference).
- [ ] **Step 4: Commit** (`cas: E3 experiment -- txn undo-journal replaces scratch copy`), or if reverted after review: commit, record numbers, then `git revert` with a message pointing at the report.

### Task 6: E4 — flat sorted-vector base inside `RefCowMap` (experiment; keep-or-revert)

**Files:**
- Modify: `Pool/CasRefCowMap.h` (+ its `.cpp` if implementation is split; find it via `grep -rn "RefCowMap::" src/Disks`), RefCowMap-focused tests

**Design.** Internals-only (the public `RefCowMap` API and its ordered-iteration contract are unchanged — this is exactly the seam the closed class + Phase A preserved): `Base` changes from `std::map` to a sorted `std::vector<std::pair<String, RefCommittedRow>>` behind the same `shared_ptr<const Base>`; keyed find = `std::lower_bound`; merged iteration walks contiguous memory; `materialize` = two-range merge into a fresh reserved vector. Overlay stays `std::map` (small, mutation-heavy). Iterator internals change (`Base::const_iterator` becomes a vector iterator) but the read-only pair-proxy surface stays.

- [ ] **Step 1:** Ensure container test coverage exists first: if no dedicated `RefCowMap` gtest suite exists, extract one (`gtest_cas_ref_cow_map.cpp`) pinning the CURRENT behavior — merged order, tombstones, `emplace` no-overwrite, `erase(pos)`, equality, `materialize`, `size` — and land it green BEFORE touching internals.
- [ ] **Step 2:** Swap the base representation; keep `overlayEntriesForTest`/`baseUseCountForTest` semantics.
- [ ] **Step 3:** Build; CAS gate (`build/test_gate_t6.log`) — snapshot-encoding byte-identity (`CasEncodingPins*`) is the canary for any ordering slip.
- [ ] **Step 4:** Benches (tag `t6_e4`): expect `BM_MergedIteration`, `BM_SnapshotEncode`, `BM_Materialize` constant-factor wins; watch `BM_ApplyRefLogTxn` for regressions (vector base changes nothing on the write path — overlay-only — but verify). Append + verdict row; controller keep/revert (same <2× default-revert rule, judged on the iteration/encode benchmarks).
- [ ] **Step 5: Commit** or commit+revert, as in Task 5.

### Task 7: Selection, comparison table, whole-branch review

- [ ] **Step 1:** Controller finalizes the report: full comparison table (every benchmark × every tag: `t1_pre`, `t2_post`, `t3_e1`, `t4_e2`, `t5_e3`, `t6_e4`, final HEAD), asymptotic classes, keep/revert decisions with one-line rationales, the E3 call-site exclusivity audit, and the bench-file header history updated to the final shipped numbers.
- [ ] **Step 2:** Confirm losers are reverted and the tree state = winners only; full CAS gate again on final HEAD (`build/test_gate_t7.log`).
- [ ] **Step 3:** Dispatch the final whole-branch code review (most capable model) over `scripts/review-package <T1-BASE> HEAD`; fix Critical/Important via one fix subagent; re-review.
- [ ] **Step 4: Commit** report + any fixes — `cas: RefTableState experiments -- comparison table + selection`.

### Task 8: Final gate — 20-minute soak + success criterion (controller-run)

- [ ] **Step 1:** Rebuild the soak image/binary from final HEAD per `reference_ca_soak_fresh_restart` (clean remount, `down -v`); start phase-3 soak: `python3 -m soak.run --seed 1 --phase 3 --duration 20m --metrics <db>` (nohup, log under `utils/ca-soak`; watchdog stays armed).
- [ ] **Step 2:** After completion run the analyzing-cas-health pass: step 1 correctness invariants MUST be zero-hit; then the CPU `trace_log` profile.
- [ ] **Step 3:** Evaluate the spec's success criterion: hottest CAS-attributed stack family = blob hashing; every other CAS family ≤ 1/3 of its samples. PASS → record the profile table in the report + close the worklog. FAIL → the residual hotspot becomes a new experiment row: systematic debugging, triage (quick fix inline, design-heavy → `docs/superpowers/cas/BACKLOG.md`), and the round is not done until re-measured.
- [ ] **Step 4:** Final summary to the user (findings, comparison table, remarks, any new bugs + triage outcomes); stop the watchdog cron.

## Self-Review Notes

- Spec coverage: Phase A → T2, Phase B → T1, Phase C (E1-E4) → T3-T6, Phase D → T7-T8. Success criterion → T8. Non-goals guarded by `CasEncodingPins*` in every task's gate.
- Type consistency: `TxnValidation` (T3) is threaded through `applyOp`; T4's `manifestAlreadyOwned` O(1) makes T3's `chassert` cheap — order T3 before T4 is deliberate (measures E1's win in isolation first).
- T1's helpers deliberately use only `replay`/free functions so T2 does not have to touch the bench file except the one `materializeCommitted` call.
