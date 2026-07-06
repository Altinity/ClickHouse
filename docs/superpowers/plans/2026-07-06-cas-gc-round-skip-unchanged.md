# CAS GC Round Skip-Unchanged Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A GC round that would make no destructive decision re-adopts the sealed in-degree generation instead of rebuilding it, so idle/small-delta rounds cost O(shards+servers) instead of ~2×O(universe).

**Architecture:** Add a cheap DEFER/FOLD decision phase to `Gc::runRegularRound` computed from signals available before the snapshot merge — changed-shard count (from `computeDiscoverDecisions`, O(shards)) and "a destructive decision is due" (from the retired list vs `min_ack`, O(retired)). DEFER short-circuits the round to a no-op (no fold, no delete, no `gc/state` write); FOLD is unchanged. The delta accumulator is the journals (sealed cursor unmoved across DEFER rounds); the defer counter is leader-local in-memory. The safety invariant — no destructive decision on a not-fully-folded snapshot — is enforced by force-folding whenever a graduation is due, and is proven by a mandatory TLA+ gate before any C++.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core`), GoogleTest (`unit_tests_dbms`), TLA+/TLC (`docs/superpowers/models`), CMake/Ninja, Python soak harness (`utils/ca-soak`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-06-cas-gc-round-skip-unchanged-design.md`. This is Phase 4 **Lever A** only. Lever B (incremental in-degree) is OUT of scope.
- Branch: work on `cas-gc-rebuild`; add new commits, never rebase/amend, never commit to `master`.
- **Phase 0 (TLA+ gate) MUST be GREEN before any C++ task.** This is design-sensitive (mirror of the 2026-06-27 concurrent-leader leak).
- Safety invariant (verbatim): **no destructive decision is ever made on a not-fully-folded snapshot** — any round that would delete anything force-FOLDs first.
- DEFER decision uses only cheap pre-fold signals: changed-shard count (O(shards) LIST + token-diff) and graduation-due (O(retired) + O(servers)); the +1/−1 content is NOT needed at decision time.
- No `gc/state` schema change. Defer counter is leader-local in-memory (`rounds_since_last_fold`); a new leader starts at 0 (folds sooner, never later).
- Config defaults: `gc_fold_threshold = 1` (batching OFF = today's behavior; only zero-delta idle rounds DEFER); `gc_fold_max_defer_rounds = 8` (liveness bound, inert at threshold 1).
- No new `ErrorCodes` numbers. Allman braces (opening brace on its own line).
- Build ONLY in `build/` (SANITIZE=OFF). NEVER `build_asan` (it aborts on deliberate `LOGICAL_ERROR` throws).
- Build: `ninja -C build unit_tests_dbms > build/build_gcdefer_task<N>.log 2>&1` — NO `-j`, NO `nproc`. Check with `grep -nE "error:|FAILED|ninja: build stopped" <log>`; do not cat the whole large log.
- Test: `build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_gcdefer_task<N>.log 2>&1`; grep `PASSED|FAILED|OK`.
- Stage files explicitly by path — never `git add -A`/`-u` (many untracked files in this tree).
- No self-matching `pgrep`/wait-loops.

---

## File Structure

- `docs/superpowers/models/CaGcRoundDeferCore.tla` (+ `.cfg`s) — Create: the TLA+ gate (Task 1).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` — Modify: two `PoolConfig` fields (Task 2).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h` — Modify: the predicate decl, the `roundDecision`/`graduationDue` decls, the `RoundReport::deferred` flag, the leader-local counter member (Tasks 2–4).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — Modify: predicate + helpers + the `runRegularRound` short-circuit (Tasks 2–4).
- `src/Disks/tests/gtest_cas_gc_round_defer.cpp` — Create: the unit tests (Tasks 2–4). Register in the gtest CMake list next to the other `gtest_cas_gc_*` files.
- `utils/ca-soak/scenarios/cards/…` + `docs/superpowers/cas/ROADMAP.md` + `utils/ca-soak/scenarios/BACKLOG.md` — Modify: ops-budget assertion + record updates (Task 5).

---

## Task 1: TLA+ gate — `CaGcRoundDeferCore` (Phase 0, mandatory before code)

Model the deferred-fold interleaving and prove the two invariants. Mirror `docs/superpowers/models/CaGcAckFloorCore.tla` (same round/graduate/condemn/min_ack shape, `gcPhase` idle→running→folded, `GBegin`/`GFold`/`GComplete`) — this task EXTENDS that shape with deferral.

**Files:**
- Create: `docs/superpowers/models/CaGcRoundDeferCore.tla`
- Create: `docs/superpowers/models/CaGcRoundDeferCore_stage1.cfg`, `…_witness_deferthenfold.cfg`, `…_sab_graduate_on_stale.cfg`, `…_sab_unbounded_defer.cfg`
- Reference (read, do not modify): `docs/superpowers/models/CaGcAckFloorCore.tla`, `docs/superpowers/models/run_ackfloor.sh`

**Interfaces:**
- Consumes: nothing (model is self-contained).
- Produces: a GREEN gate the C++ tasks cite; no code interface.

- [ ] **Step 1: Author the model deltas over `CaGcAckFloorCore`**

Copy `CaGcAckFloorCore.tla` to `CaGcRoundDeferCore.tla` and add:
- A variable `unfolded` (the set of edge changes accepted by writers but not yet folded — the deferred delta). `WriterAddEdge`/`WriterRemoveEdge` add to `unfolded`; `GFold` drains `unfolded` into `folded`.
- A `DeferRound` action: advances `round` (so `min_ack` can rise via acks) and does NOT drain `unfolded`, GUARDED by `GraduationDue = FALSE` where `GraduationDue == \E e \in retired : e.delete_pending \/ e.condemn_round < MinAck`. `DeferRound` never condemns, graduates, or deletes.
- A defer bound: a constant `MaxDefer`; `DeferRound` is enabled only while `deferCount < MaxDefer`; `GFold` resets `deferCount := 0`; `DeferRound` does `deferCount' = deferCount + 1`.
- Modify `GComplete`/graduation so a physical delete of blob `b` is enabled ONLY when `unfolded` contains no edge touching `b` (i.e. a fold covering `b` ran this round) — this is the force-fold-before-graduation rule.

- [ ] **Step 2: Add the invariants**

```tla
\* No blob is physically deleted while an unfolded +1 could protect it.
NoOverDelete == \A b \in Blobs :
    (b \in deletedThisStep) => (\A e \in unfolded : e.b # b)

\* Bounded deferral: unfolded edges are always eventually folded (no permanent skip).
EventuallyFolded == (unfolded # {}) ~> (unfolded = {})
```

Add `NoOverDelete` as an INVARIANT and `EventuallyFolded` as a PROPERTY (temporal) in the stage1 cfg. Keep `CaGcAckFloorCore`'s existing `NoDangle`/`TypeOK` invariants.

- [ ] **Step 3: Write the cfgs**

`CaGcRoundDeferCore_stage1.cfg`: all sabotage constants FALSE, `MaxDefer = 3`, small `Blobs`/`Writers`/`MaxRound`; `INVARIANT NoOverDelete NoDangle TypeOK`; `PROPERTY EventuallyFolded`.
`…_witness_deferthenfold.cfg`: a reachability check (an `ALLOW_POSTCONDITION`/state-constraint or a negated invariant) that a state with `unfolded # {} /\ deferCount > 0` is reachable and later `unfolded = {}` — proves DEFER-then-FOLD actually occurs (not vacuously safe).
`…_sab_graduate_on_stale.cfg`: set `SabotageGraduateOnStale = TRUE` (a constant that drops the `unfolded`-covers-`b` guard on delete) → TLC MUST report a `NoOverDelete` counterexample.
`…_sab_unbounded_defer.cfg`: set `SabotageUnboundedDefer = TRUE` (removes the `deferCount < MaxDefer` guard) → TLC MUST report an `EventuallyFolded` violation.

- [ ] **Step 4: Run the gate**

Run (mirror `run_ackfloor.sh` invocation, adjust model name):
```
cd docs/superpowers/models && bash run_ackfloor.sh CaGcRoundDeferCore stage1
```
(or the project's standard `tlc` invocation for the four cfgs). Redirect output to `docs/superpowers/models/CaGcRoundDeferCore_RESULTS.md`.
Expected: `stage1` and `witness_deferthenfold` PASS (no error); `sab_graduate_on_stale` produces a `NoOverDelete` counterexample; `sab_unbounded_defer` produces an `EventuallyFolded` counterexample.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaGcRoundDeferCore.tla docs/superpowers/models/CaGcRoundDeferCore_*.cfg docs/superpowers/models/CaGcRoundDeferCore_RESULTS.md
git commit -m "CAS TLA+: CaGcRoundDeferCore gate — NoOverDelete + EventuallyFolded for GC round defer"
```

---

## Task 2: Config knobs + the pure DEFER predicate

**Files:**
- Modify: `CasStore.h` (PoolConfig, after `gc_trim_body_soft_limit` ~`:157`)
- Modify: `CasGc.h` (free-function or static decl), `CasGc.cpp` (definition)
- Create: `src/Disks/tests/gtest_cas_gc_round_defer.cpp` (+ register in the gtest CMake list)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `PoolConfig::gc_fold_threshold` (`uint64_t`, default 1), `PoolConfig::gc_fold_max_defer_rounds` (`uint64_t`, default 8).
  - `bool DB::Cas::shouldDeferRound(size_t changed_shards, bool graduation_due, uint64_t rounds_since_last_fold, uint64_t fold_threshold, uint64_t fold_max_defer_rounds);` — pure; returns true iff the round may be deferred.

- [ ] **Step 1: Write the failing test**

In `src/Disks/tests/gtest_cas_gc_round_defer.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>

using DB::Cas::shouldDeferRound;

TEST(CasGcRoundDefer, PredicateTruthTable)
{
    /// threshold=1 (default): defer ONLY when zero shards changed AND no graduation due AND within bound.
    EXPECT_TRUE (shouldDeferRound(/*changed*/0, /*grad_due*/false, /*since*/0, /*threshold*/1, /*max*/8));
    EXPECT_FALSE(shouldDeferRound(1, false, 0, 1, 8));   // a shard changed => fold
    EXPECT_FALSE(shouldDeferRound(0, true,  0, 1, 8));   // graduation due => force fold
    EXPECT_FALSE(shouldDeferRound(0, false, 8, 1, 8));   // defer bound reached => force fold

    /// threshold=3 (batching): defer while accumulated changed shards < threshold, no grad, within bound.
    EXPECT_TRUE (shouldDeferRound(2, false, 0, 3, 8));
    EXPECT_FALSE(shouldDeferRound(3, false, 0, 3, 8));   // reached threshold => fold
    EXPECT_FALSE(shouldDeferRound(2, true,  0, 3, 8));   // graduation due => force fold regardless of size
    EXPECT_FALSE(shouldDeferRound(2, false, 8, 3, 8));   // bound reached => force fold
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.PredicateTruthTable' > build/test_gcdefer_task2.log 2>&1`
Expected: FAIL to compile — `shouldDeferRound` undefined (and the test file not yet in the build).

- [ ] **Step 3: Add the config fields**

In `CasStore.h`, in `struct PoolConfig` after the `gc_trim_body_soft_limit` field (~`:157`):

```cpp
    /// Phase-4 skip-unchanged (spec 2026-07-06-cas-gc-round-skip-unchanged): a GC round may DEFER
    /// (re-adopt the sealed in-degree generation instead of rebuilding it) when fewer than this many
    /// shards changed since the last fold AND no destructive decision is due. Default 1 = fold as soon
    /// as anything changed (batching off; only idle rounds defer). > 1 batches small deltas.
    uint64_t gc_fold_threshold = 1;
    /// Liveness bound for batching: force a FOLD after this many consecutive DEFER rounds even below
    /// the threshold. Inert at gc_fold_threshold == 1 (an idle defer has nothing to fold). Default 8.
    uint64_t gc_fold_max_defer_rounds = 8;
```

- [ ] **Step 4: Add the predicate**

In `CasGc.h` (namespace `DB::Cas`, near the top-level declarations, not inside the `Gc` class):

```cpp
/// Phase-4 skip-unchanged decision (spec 2026-07-06). Pure. Returns true iff the current round may be
/// DEFERRED (re-adopt the sealed generation, no fold/delete). A round MUST fold when: enough shards
/// changed (>= fold_threshold), OR a destructive decision is due (graduation_due), OR the defer bound
/// is reached (rounds_since_last_fold >= fold_max_defer_rounds). The graduation_due term is the
/// load-bearing safety guard: no destructive decision ever runs on a not-fully-folded snapshot.
bool shouldDeferRound(size_t changed_shards, bool graduation_due, uint64_t rounds_since_last_fold,
                      uint64_t fold_threshold, uint64_t fold_max_defer_rounds);
```

In `CasGc.cpp` (near the other free helpers):

```cpp
bool shouldDeferRound(size_t changed_shards, bool graduation_due, uint64_t rounds_since_last_fold,
                      uint64_t fold_threshold, uint64_t fold_max_defer_rounds)
{
    if (graduation_due)
        return false;
    if (changed_shards >= fold_threshold)
        return false;
    if (rounds_since_last_fold >= fold_max_defer_rounds)
        return false;
    return true;
}
```

- [ ] **Step 5: Register the test file + build**

Add `gtest_cas_gc_round_defer.cpp` to the unit-test source list (the same CMake list holding `gtest_cas_gc_round.cpp` etc.; grep `gtest_cas_gc_round` under `src/Disks` CMake to find it).
Run: `ninja -C build unit_tests_dbms > build/build_gcdefer_task2.log 2>&1`
Expected: builds clean.

- [ ] **Step 6: Run the test to verify it passes**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.PredicateTruthTable' > build/test_gcdefer_task2.log 2>&1`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round_defer.cpp <the CMake file>
git commit -m "CAS: gc_fold_threshold/gc_fold_max_defer_rounds config + shouldDeferRound predicate"
```

---

## Task 3: The cheap decision signals — `graduationDue` and `changedShardCount`

Both are computed from state already reachable before the fold's snapshot merge.

**Files:**
- Modify: `CasGc.h` (two private method decls), `CasGc.cpp` (definitions)
- Modify: `src/Disks/tests/gtest_cas_gc_round_defer.cpp` (add tests)

**Interfaces:**
- Consumes: `computeDiscoverDecisions` (`CasGc.cpp:1306`), `readFoldSeal`, `GcState`, `DiscoverDecision`.
- Produces (private `Gc` methods):
  - `bool graduationDue(const GcState & state, uint64_t min_ack);` — true iff any current retired entry is due to delete/graduate this round (`delete_pending` OR `condemn_round < min_ack`). Reads the retired lists referenced by `state.retired_refs`.
  - `size_t changedShardCount(const GcState & state);` — the number of present shards whose token differs from the sealed generation (= count of non-`Skip` `DiscoverDecision`s from `computeDiscoverDecisions` over the fold seal at `state.snap_generation`/`snap_attempt`).

- [ ] **Step 1: Write the failing tests**

Add to `gtest_cas_gc_round_defer.cpp` (use the existing GC test scaffolding — an `InMemoryBackend`, a `Store`, a `Gc`, and the `cas_test_helpers.h` seeding helpers; mirror the setup in `gtest_cas_gc_round.cpp`):

```cpp
// graduationDue: a delete_pending entry, and an entry whose condemn_round < min_ack, each force it true;
// an entry with condemn_round >= min_ack and not delete_pending leaves it false.
TEST(CasGcRoundDefer, GraduationDueDetectsDuePendingAndFloorCrossing)
{
    // ... seed a pool; publish a retired list with one entry {condemn_round=2, delete_pending=false};
    // assert graduationDue(state, /*min_ack=*/2) == false  (condemn_round not < min_ack)
    // assert graduationDue(state, /*min_ack=*/3) == true   (2 < 3 => due to graduate)
    // re-seed the entry with delete_pending=true; assert graduationDue(state, /*min_ack=*/0) == true
}

// changedShardCount: with the fold seal covering shard s at its current token, a quiescent pool reports 0;
// after one publish to a ref in shard s, it reports 1.
TEST(CasGcRoundDefer, ChangedShardCountIsZeroWhenQuiescent)
{
    // ... build+publish a ref (shard s populated), run a full GC round so the fold seal covers s;
    // assert changedShardCount(current_state) == 0;
    // publish another ref into s (token changes); assert changedShardCount(current_state) == 1;
}
```

Fill the `...` with the concrete seeding from `gtest_cas_gc_round.cpp` (same `openStoreForTest` + `Gc gc{store}` + `runRegularRound()` + `cas_test_helpers.h` retired-list seeding). Keep assertions exactly as above.

- [ ] **Step 2: Run to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.GraduationDue*:CasGcRoundDefer.ChangedShardCount*' > build/test_gcdefer_task3.log 2>&1`
Expected: FAIL to compile — methods undefined.

- [ ] **Step 3: Implement `graduationDue`**

In `CasGc.cpp` (private method). It reads the current retired lists the same way `fold` does (`state.retired_refs` → `backend.get(retired_key)` → `decodeRetiredSet`), and returns true on the first qualifying entry:

```cpp
bool Gc::graduationDue(const GcState & state, uint64_t min_ack)
{
    Backend & backend = store->backend();
    for (const auto & [retired_shard, retired_key] : state.retired_refs)
    {
        const auto got = backend.get(retired_key);
        if (!got)
            /// Missing retired list = corrupt destructive bookkeeping (same fail-closed rule as fold).
            /// Force a fold so the round's existing fail-closed path surfaces it, never silently defer.
            return true;
        for (const RetiredEntry & e : decodeRetiredSet(got->bytes).entries)
            if (e.delete_pending || e.condemn_round < min_ack)
                return true;
    }
    return false;
}
```

- [ ] **Step 4: Implement `changedShardCount`**

In `CasGc.cpp`. Resolve the same discover reference seal `fold` uses (`readFoldSeal(state.snap_generation, state.snap_attempt)`, else empty) and count non-`Skip` decisions:

```cpp
size_t Gc::changedShardCount(const GcState & state)
{
    CasFoldSeal ref_seal;
    if (const auto seal = readFoldSeal(state.snap_generation, state.snap_attempt))
        ref_seal = *seal;
    const std::map<String, DiscoverDecision> decisions = computeDiscoverDecisions(ref_seal);
    size_t changed = 0;
    for (const auto & [ck, dec] : decisions)
        if (dec != DiscoverDecision::Skip)
            ++changed;
    return changed;
}
```

- [ ] **Step 5: Add the decls to `CasGc.h`**

In the `Gc` class private section (near `computeDiscoverDecisions`):

```cpp
    bool graduationDue(const GcState & state, uint64_t min_ack);
    size_t changedShardCount(const GcState & state);
```

- [ ] **Step 6: Build + run**

Run: `ninja -C build unit_tests_dbms > build/build_gcdefer_task3.log 2>&1` then
`build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.*' > build/test_gcdefer_task3.log 2>&1`
Expected: builds clean; the two new tests PASS (plus the predicate test still passes).

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round_defer.cpp
git commit -m "CAS: graduationDue + changedShardCount — cheap pre-fold GC round-defer signals"
```

---

## Task 4: Wire the DEFER short-circuit into `runRegularRound`

**Files:**
- Modify: `CasGc.h` (`RoundReport::deferred` flag; the leader-local counter member; a `CasEventType::GcRoundDeferred` if the event enum is where round events live — otherwise reuse a `deferred` outcome on the round log)
- Modify: `CasGc.cpp` (`runRegularRound`: insert the decision + short-circuit between the ack-floor and the fold)
- Modify: `src/Disks/tests/gtest_cas_gc_round_defer.cpp` (integration tests)

**Interfaces:**
- Consumes: `shouldDeferRound` (Task 2), `graduationDue`/`changedShardCount` (Task 3), `PoolConfig::gc_fold_threshold`/`gc_fold_max_defer_rounds` (Task 2).
- Produces: `RoundReport::deferred` (`bool`, default false) set true on a deferred round; `Gc::rounds_since_last_fold_` (`uint64_t`, default 0) leader-local counter.

- [ ] **Step 1: Write the failing integration tests**

Add to `gtest_cas_gc_round_defer.cpp`. Use an op-counting/instrumented backend (reuse the pattern from an existing GC test that counts `get`s, or the `CasGc*` ProfileEvents such as `CasGcGet`) to assert an idle round does NO generation-run reads.

```cpp
// Idle round re-adopts: after a settled round, a subsequent round with zero changed shards and no
// graduation due sets report.deferred=true and performs ZERO generation-run GETs (snapshot untouched).
TEST(CasGcRoundDefer, IdleRoundDefersAndReadsNoGeneration)
{
    // build+publish, run one full round (report.deferred==false); quiesce.
    // record the CasGcGet ProfileEvent (or instrumented get-count) baseline.
    // run another round; assert report.deferred == true AND the CasGcGet delta == 0
    // (no foldDeltasIntoGeneration run reads); assert snap_generation/attempt unchanged.
}

// The +1 guard (mirror of the 2026-06-27 leak): a blob condemned + delete_pending, then re-referenced
// while deferred, must NOT be over-deleted — the due graduation forces a fold that sees the +1.
TEST(CasGcRoundDefer, DueGraduationForcesFoldAndSparesReReferencedBlob)
{
    // drive a blob B to delete_pending at condemn_round K (run rounds until it is pending).
    // add a new manifest referencing B (a +1) while B is pending.
    // advance min_ack past K (mount ack) so B is due to graduate.
    // run a round: assert report.deferred == false (force-folded), B is NOT deleted
    //   (blobAbsent(...) == false), and fsck dangling == 0.
}

// Bounded deferral: with gc_fold_threshold large and a small standing delta, at most
// gc_fold_max_defer_rounds consecutive rounds defer, then one folds.
TEST(CasGcRoundDefer, BoundedDeferralForcesFoldWithinWindow)
{
    // set gc_fold_threshold=100, gc_fold_max_defer_rounds=3; create a 1-shard standing delta.
    // run 4 rounds; assert the first 3 defer (report.deferred==true) and the 4th folds (false).
}
```

Fill the scaffolding from `gtest_cas_gc_round.cpp` + `cas_test_helpers.h` (`runRoundsUntilAbsent`, retired seeding, `blobAbsent`, `assert_fsck` equivalent). Config knobs are set on the `PoolConfig` passed to `openStoreForTest`/`Store::open` (add an overload or set fields directly as the existing GC tests do).

- [ ] **Step 2: Run to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.IdleRound*:CasGcRoundDefer.DueGraduation*:CasGcRoundDefer.BoundedDeferral*' > build/test_gcdefer_task4.log 2>&1`
Expected: FAIL — `report.deferred` does not exist / rounds never defer.

- [ ] **Step 3: Add `RoundReport::deferred` + the counter member**

In `CasGc.h`: add `bool deferred = false;` to `struct RoundReport`; add `uint64_t rounds_since_last_fold_ = 0;` to the `Gc` class private members.

- [ ] **Step 4: Insert the decision + short-circuit in `runRegularRound`**

In `CasGc.cpp`, after the ack-floor block (`min_ack` known — after `report.min_ack = floor.min_ack;` at `:122` and the stale-ack watchdog / fence-out events, i.e. immediately BEFORE the `GcFoldBegin` event at `:178`), insert:

```cpp
    /// Phase-4 skip-unchanged (spec 2026-07-06): decide DEFER vs FOLD from cheap pre-fold signals.
    /// A DEFER round re-adopts the sealed generation — no fold, no delete, no gc/state write — so a
    /// slow idle/small-delta round no longer rebuilds the whole in-degree snapshot. Safety: a due
    /// graduation forces a FOLD (graduationDue), so no destructive decision runs on a stale snapshot.
    {
        const bool graduation_due = graduationDue(state, floor.min_ack);
        const size_t changed = changedShardCount(state);
        if (shouldDeferRound(changed, graduation_due, rounds_since_last_fold_,
                             store->poolConfig().gc_fold_threshold,
                             store->poolConfig().gc_fold_max_defer_rounds))
        {
            ++rounds_since_last_fold_;
            report.deferred = true;
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcFence;   /// reuse the Snap round-event channel; outcome = "deferred"
                e.object_kind = CasEventObjectKind::Snap;
                e.round = state.round;
                e.gen = state.snap_generation;
                e.outcome = "deferred";
                e.reason = "skip-unchanged: no changed shard reached the fold threshold and no graduation "
                           "is due; re-adopting the sealed generation (snapshot rebuild elided)";
                e.detail = {{"changed_shards", std::to_string(changed)},
                            {"rounds_since_last_fold", std::to_string(rounds_since_last_fold_)}};
            });
            return report;   /// no fold, no pre-CAS deletes, no gc/state CAS — sealed generation stays pinned
        }
        rounds_since_last_fold_ = 0;   /// this round folds
    }
```

Leave the entire fold/delete/CAS path below unchanged.

Note for the reviewer: `changedShardCount` and `fold` both call `computeDiscoverDecisions` (one extra O(shards) LIST on FOLD rounds only). That is intentional and negligible against the snapshot merge the FOLD then does; it keeps `fold` unchanged (no signature churn). Do not thread the decisions into `fold` in this task.

- [ ] **Step 5: Build + run the integration tests**

Run: `ninja -C build unit_tests_dbms > build/build_gcdefer_task4.log 2>&1` then
`build/src/unit_tests_dbms --gtest_filter='CasGcRoundDefer.*' > build/test_gcdefer_task4.log 2>&1`
Expected: builds clean; all `CasGcRoundDefer.*` PASS.

- [ ] **Step 6: Run the full GC suite (no regression)**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_gcdefer_task4_full.log 2>&1`
Expected: the full `CasGc*` suite stays green. Because the default `gc_fold_threshold = 1` makes every round with any change FOLD exactly as before, existing GC tests (which change state every round and/or drive `runRoundsUntilAbsent`) are unaffected; only genuinely-idle rounds defer.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round_defer.cpp
git commit -m "CAS: GC round DEFER short-circuit — re-adopt the sealed generation on idle/small-delta rounds"
```

---

## Task 5: Harness ops-budget assertion + record updates

**Files:**
- Modify: `utils/ca-soak/scenarios/cards/…` (the S03 idle-cost card) — add a per-round S3-ops budget assertion
- Modify: `docs/superpowers/cas/ROADMAP.md` (the Phase-4 row → DONE), `utils/ca-soak/scenarios/BACKLOG.md` (the two S3-BUDGET idle/O(universe) entries → RESOLVED with the commit range)

**Interfaces:**
- Consumes: the landed C++ (Tasks 2–4).
- Produces: no code interface; a harness assertion + updated records.

- [ ] **Step 1: Add the S03 ops/round budget assertion**

Locate the S03 idle-GC card under `utils/ca-soak/scenarios/cards/` (grep for the `S3-BUDGET` idle-cost scenario or `S03`). Add an assertion that, after quiescence, an idle GC round's `CasGcGet` delta (from `system.events` / the instrumented counter the harness already reads) is near-zero (e.g. `< 50`, well below the pre-fix ~1362), and `dangling == 0`. Follow the card's existing assertion style (`framework/assertions.py`). If no S03 card exists yet, add the assertion to the closest idle-GC card and note it.

- [ ] **Step 2: Run the harness unit tests**

Run: `cd utils/ca-soak && python3 -m pytest scenarios/tests/ -q > /tmp/gcdefer_harness.log 2>&1` (redirect under the repo `tmp/` if preferred)
Expected: green (the new assertion has unit coverage or at least imports cleanly).

- [ ] **Step 3: Update the records**

In `docs/superpowers/cas/ROADMAP.md`, change the row `| GC round is O(universe) not O(delta) — fold/discover skip-unchanged | **TODO** (Phase 4 …)` to `**DONE**` with the commit range and a one-line note (Lever A: DEFER short-circuit; Lever B still open).
In `utils/ca-soak/scenarios/BACKLOG.md`, mark the `S3-BUDGET — idle GC …` and `S3-BUDGET/SCALABILITY — GC round duration is O(ref universe)` entries RESOLVED for the idle/small-delta case (Lever A), noting Lever B (incremental in-degree) remains for the large-delta O(universe) case.

- [ ] **Step 4: Commit**

```bash
git add utils/ca-soak/scenarios/cards/<the card> docs/superpowers/cas/ROADMAP.md utils/ca-soak/scenarios/BACKLOG.md
git commit -m "soak+docs(cas): S03 idle-round ops budget assertion; Phase-4 Lever A records updated"
```

---

## Self-Review

**1. Spec coverage:**
- §3 DEFER/FOLD decision + journals-as-accumulator → Tasks 2–4. ✓
- §4 safety (force-fold-before-graduation; −1 conservative; +1 hazard) → `graduationDue` (Task 3) + the short-circuit guard (Task 4) + the `DueGraduationForcesFoldAndSparesReReferencedBlob` test. ✓
- §4.3 bounded deferral → `gc_fold_max_defer_rounds` + `BoundedDeferralForcesFoldWithinWindow`. ✓
- §4.4 concurrent leaders + §5 no schema change / leader-local counter → Task 4 counter member; the TLA+ gate models concurrent defer/fold. ✓
- §6 config defaults (1, 8) → Task 2. ✓
- §7 TLA+ gate (NoOverDelete + EventuallyFolded; sab_graduate_on_stale + sab_unbounded_defer) → Task 1. ✓
- §8 tests (idle re-adopts / +1 guard / bounded defer / gc_shards>1) → Task 4 (idle, +1, bounded); gc_shards>1 — ADD to Task 4 Step 1 as a variant of the idle test with `gc_shards=2` (note below). ✓
- §9 acceptance (idle ops near-zero; dangling=0) → Task 5 budget assertion + Task 4 tests. ✓

Gap fixed inline: Task 4 Step 1 should include a `gc_shards=2` variant of `IdleRoundDefersAndReadsNoGeneration` (the spec §8 asks for gc_shards>1 coverage) — the implementer adds `IdleRoundDefersUnderShardedGc` with `gc_shards=2` seeded, same assertion (deferred + zero generation reads).

**2. Placeholder scan:** No TBD/TODO. Test bodies with `...` scaffolding (Task 3/4) name the exact source to copy from (`gtest_cas_gc_round.cpp` + `cas_test_helpers.h`) and pin the exact assertions — the framework setup is transcription from a named sibling, not invention. The one genuinely open lookup ("the S03 card path", "the CMake test list file") is a grep the implementer runs; both are named with the grep target.

**3. Type consistency:** `shouldDeferRound(size_t, bool, uint64_t, uint64_t, uint64_t)` identical in Task 2 decl/def and Task 4 call site. `graduationDue(const GcState&, uint64_t)`, `changedShardCount(const GcState&)`, `RoundReport::deferred`, `rounds_since_last_fold_`, `gc_fold_threshold`, `gc_fold_max_defer_rounds` used identically across tasks. `DiscoverDecision::Skip` matches `CasGc.cpp:650`.
