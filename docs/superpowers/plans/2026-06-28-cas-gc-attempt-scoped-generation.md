# CA GC Attempt-Scoped Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the GC concurrent-leader reclaim leak and divergent-run corruption by making every per-round `gc/gen/<gen>/…` write-once artifact **attempt-scoped** (keyed by the folding leader's `lease.seq`) and reader-visible only through the single `(snap_generation, snap_attempt)` pair adopted by a lease-guarded `gc/state` CAS.

**Architecture:** A deposed GC leader currently writes a `fold_seal`/`completion_seal`/in-degree-run/cleanup-bundle to a **final** `gc/gen/<gen>/…` key *before* its lease-guarded `gc/state` CAS fails, leaving an orphaned write-once artifact that wedges every future round (and a checksum-divergent run that can corrupt in-degree). The fix moves **every per-round decision-bearing artifact** — the four `gc/gen` artifacts **plus the `retired` sets and `outcomes` logs** — under `gc/gen/<gen>/attempt/<a>/…` (`a = lease.seq`, which the renew/steal paths bump every round, so it is a fresh monotonic per-round attempt id), adds a `snap_attempt` field to `GcState`, makes the fold/completion `gc/state` CASes *select* (CAS #1) then *inherit* (CAS #2) that attempt, and resolves every reader — **including `RetireView`, which writers consult on the publish gate** — through the adopted `(snap_generation, snap_attempt)`. A deposed leader's writes land under its own unadopted attempt — pure space debris reclaimed wholesale by the `gc/gen/<g>/` retention prune, invisible to every decision path (GC *and* writer). TLA+ proves adopted-only visibility (seal **and** retired set) under a second concurrent leader minting a fresh attempt id, before any code lands.

**Tech Stack:** C++ (ClickHouse `DB::Cas` namespace), Protocol Buffers (`cas_format.proto`), GoogleTest (`unit_tests_dbms`), TLA+/TLC (`tla2tools.jar`), Python (ca-soak scenario suite).

## Global Constraints

- Branch: `cas-gc-part-manifest-impl` (NOT master). Add new commits only — never rebase/amend (per `.claude/CLAUDE.md`).
- Allman braces (opening brace on its own line) for all C++ — enforced by CI style check.
- CA is **pre-release with no persisted data** ⟹ the `GcState`/`CasLayout` changes need **no migration** and **no compat scaffolding**; do not add versioned readers or dual-format fallbacks.
- **Avoid fallback paths.** On a divergent deterministic artifact, fail-closed with `ErrorCodes::CORRUPTED_DATA`; never silently substitute. GC must never throw on a benign 404 during fold/sweep (record + continue).
- Never use `sleep` in C++ to coordinate concurrency; the two-leader gtests use `InMemoryBackend` fault-injection (`failNextCasPut`, `setHoldDeletes`/`landPendingDelete`), not threads.
- Builds: run `ninja unit_tests_dbms` from inside the build dir, **no `-j`, no `nproc`**, redirect output to a log file in the build dir, and analyze the log with a subagent (return a concise summary only).
- Tests: redirect each test run to a uniquely-named log file under the build dir; analyze via subagent.
- Commit messages end with the two trailers from `.claude/CLAUDE.md` (`Co-Authored-By:` + `Claude-Session:`).
- Write names of functions/excerpts in inline code blocks in commit messages/comments; say "exception" not "crash" for logical errors.
- **Task 1 (TLA+ Gate A) MUST be green before ANY code task (Task 2+) begins.**

## Governing invariant (the whole point)

> No unadopted artifact may ever influence a retire / delete / trim / fold decision. Reader-visible generation state is exactly the pair `(snap_generation, snap_attempt)` recorded in `gc/state`.

Spec: `docs/superpowers/specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md`.

## Post-review corrections (apply in EVERY task — verified against code)

These were caught in user + adversarial plan review; they bind every task below:

1. **`Backend::head(key)` returns `HeadResult` with a `.exists` bool, NOT `std::optional`.** Every test in this plan that writes `backend->head(...).has_value()` must use `backend->head(...).exists` (see existing tests, e.g. `gtest_cas_gc_round.cpp:755`). `backend->get(...)` *does* return `std::optional` — `.has_value()` is correct there.
2. **gtest per-file helpers are NOT shared.** `ref`, `kGc`/`kGcA`, `blobExists`, `runGcToFixpoint`, `runFsck` are anonymous-namespace helpers redefined per file (and `ref` has two different signatures across files). A new test file (Task 10) must **copy in** the helpers it needs; only these come from the shared header/core: `hexToU128`, `decodeGcState`, `writeBlobBody`, `writeManifestRaw`, `publishCommittedTransition`, `dropRefTransition`, `openStoreForTest`, `blobEntryFor`, `inDegreeOf`, `RootNamespace`, `ManifestRef`. (`runGcToFixpoint`/`runFsck` live only in `gtest_cas_gc_leak.cpp`; `blobExists` in `gtest_cas_gc_round.cpp`/`gtest_cas_gc_resume.cpp`.)
3. **Never hardcode a generation number in an assertion** — derive it from `decodeGcState(...)` state (the first round's `G_f` happens to be 1, but later rounds differ). Read `snap_generation`/`snap_attempt` from `gc/state` and assert against those.
4. **The seals' divergent throws today are `ErrorCodes::ABORTED` "concurrent leader"** (`CasGc.cpp:445` fold seal, `:922` completion seal), not `CORRUPTED_DATA`. Tasks 4-6 *replace* them with `putDeterministicArtifact` (which throws `CORRUPTED_DATA` on genuine byte divergence). Do not expect to find `CORRUPTED_DATA` there pre-change.
5. **There are 42 `CaGcRootLocalPartManifestCore_*.cfg` files** (not "~38"). "Bind the new constants in every cfg" means all 42.

## File Structure

**TLA+ (Task 1):**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — add `EnableAttemptScoping` + `SabotageDeposedLeaderWritesFinalGen` constants, attempt-keyed seal slots, a deposed-writer action, a generation/attempt adopt pointer, `INV_ONLY_ADOPTED_VIEWABLE`.
- Modify: all ~38 `docs/superpowers/models/CaGcRootLocalPartManifestCore_*.cfg` — bind the two new constants (`FALSE`).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage6_attemptscoping.cfg` (positive, `EnableAttemptScoping=TRUE`, `Leaders={L1,L2}`).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_deposedleaderwritesfinalgen.cfg` (negative control).
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` — record the new stage + sabotage counts.

**C++ (Tasks 2–10)** — all under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`:
- `CasGcFormats.h` / `CasGcFormats.cpp` — `GcState::snap_attempt`.
- `Proto/cas_format.proto` — `GcStateProto.snap_attempt = 9`.
- `CasLayout.h` — attempt-scoped key derivation for **all six** `gc/gen` key families (fold_seal, completion_seal, blob_target run, part_manifest_cleanup, **retired**, **outcomes**).
- `CasBlobInDegree.cpp` / `CasBlobInDegree.h` — attempt-threaded `foldDeltasIntoGeneration` + strict `putIfAbsent`.
- `CasGcShardPlan.cpp` / `CasGcShardPlan.h` — `ShardReducer::reduce` attempt-threading.
- `CasGc.cpp` / `CasGc.h` — `fold` (CAS #1), `retire` (retired set under attempt), `recheck` (CAS #2, outcomes/retired-drop under attempt), readers, resume, pruning.
- `CasRetireView.cpp` / `CasRetireView.h` — `refresh()` resolves retired sets under the accepted `(snap_generation, snap_attempt)` instead of LISTing `gc/retired/` (writer-facing publish gate).
- A shared strict-putIfAbsent helper (added to `CasGc.cpp` anonymous namespace, or `CasCodecUtil.h` if reused by BlobInDegree).

**Tests (Tasks 2–11):** `src/Disks/tests/gtest_cas_gc_formats.cpp`, `gtest_cas_layout.cpp`, `gtest_cas_blob_indegree.cpp`, `gtest_cas_gc_fold.cpp`, `gtest_cas_gc_resume.cpp`, new `gtest_cas_gc_attempt.cpp`; `utils/ca-soak/scenarios/cards/s28_s33_corner.py` (S33).

---

### Task 1: TLA+ Gate A — prove adopted-only visibility under a second concurrent leader

**This is the gate. It must be green before any code task. Model-and-prove first, then code.**

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Modify: every `docs/superpowers/models/CaGcRootLocalPartManifestCore_*.cfg` (bind 2 new constants `FALSE`)
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage6_attemptscoping.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_deposedleaderwritesfinalgen.cfg`
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`

**Background the implementer needs (from the codebase, do not re-derive):**
- The model has **no lease/`snap_generation`/`gc/state`** record. Generation artifacts are the per-round maps `foldSeal[r] = [classified |-> SUBSET ManifestIds, foldedCursor |-> [Namespaces -> 0..MaxLog]]` and `completionSeal[r] = [fenced |-> SUBSET Namespaces, rechecked |-> SUBSET Blobs, deleted |-> SUBSET Blobs, adoptable |-> BOOLEAN]`. `adoptable` is the round-visibility flag.
- The literal stealable lease is proven separately in `CaGcLeaseCore.tla` (`RetireCommit`: a displaced leader never commits). **Do not re-model the lease here.** This model proves the orthogonal *artifact-visibility* half: a second leader that reaches the seal-write step must not make its artifact viewable.
- Feature flags are CONSTANT booleans threaded as `IF Flag THEN <arm> ELSE <arm>` and gated in `Next`; the `coordFence`/`shardIndeg`/`reducerOwner`/`storedTok` variables (lines 80–84) are the template for **inert** flag-gated variables (never written when the flag is off ⟹ zero new distinct states). `EnableSharding` (line 32) is the cleanest gating template: it routes whole action arms in `Next` (lines 1166–1171), gates action bodies (`/\ EnableSharding`), gates derived reads (`EffIndeg`, line 553), and gates invariants via `Enable* => P` (vacuously true when off).
- Sabotage idiom: a CONSTANT bool woven into an existing action as an unsafe branch, `TRUE` in exactly one `_sab_*.cfg` that asserts the invariant it must violate.
- Run command: `cd docs/superpowers/models && ./run_gc_partmanifest.sh <cfg-basename-without-.cfg>` (writes `tmp/tlc_<cfg>.log`). For big runs set `TLC_JAVA_OPTS="-Xmx48g"`.
- Inertness targets: `stage0` = 71,184 generated / **19,846 distinct**; `stage1` = 1,659,466 / **402,034 distinct** (`CaGcRootLocalPartManifestCore_RESULTS.md`).

**Modeling approach (what "attempt-scoping" means in this model):**
- Add CONSTANT `EnableAttemptScoping` (the fix) and CONSTANT `SabotageDeposedLeaderWritesFinalGen` (negative control). Add CONSTANT `MaxAttempt` (bound on the attempt counter for TLC finiteness).
- **`attempt` is a FRESH MONOTONIC id, not a leader identity** (user-review correction; matches `attempt = lease.seq`, which bumps every round). Add variable `attemptSeq \in 0..MaxAttempt` (a global monotone counter, like `gcRound`); each fold mints `attemptSeq' = attemptSeq + 1` and uses that value as its attempt id. Modeling the attempt as the leader id would be unsound: the same leader gets a *different* attempt across rounds/steals, so leader-id scoping would mask collision/resume/pruning bugs.
- Add a generation/attempt **adopt pointer** variable `adopted = [r \in 0..MaxRound |-> 0]` (`0` = none; set to the *minted attempt id* by the fold-adopt step under a guard ensuring exactly one attempt is adopted per round).
- Add attempt-keyed artifact slots covering **both** the seal **and the retired set** (the retired set is writer-visible via the RetireView LIST, so it must be in the invariant): e.g. `sealAt = [r \in 0..MaxRound |-> SUBSET (0..MaxAttempt)]` (which attempt ids wrote a seal for round `r`) and `retiredAt = [r \in 0..MaxRound |-> SUBSET (0..MaxAttempt)]` (which attempt ids wrote a retired set). When `EnableAttemptScoping = TRUE`, a leader writes under its own minted attempt id and readers (GC decisions **and** the modeled writer publish gate) consult only `adopted[r]`'s slot; when `FALSE` (baseline), there is a single shared slot and a deposed write occupies it.
- Add action `GDeposedWriteSeal(l, r, a)`: a leader `l` holding a *stale* minted attempt `a` (`a # adopted[r]`) writes a seal AND a retired set for round `r` (models the deposed leader finishing its fold + retire before its CAS fails). Scoped ⟹ writes only `sealAt[r] \cup {a}` / `retiredAt[r] \cup {a}` (invisible unless `adopted[r] = a`); under `SabotageDeposedLeaderWritesFinalGen` ⟹ writes the shared/final slot and makes `a` reader-visible even though `adopted[r] # a`.
- Model a **writer publish-gate read** of the retired set (the RetireView consumer) as part of `SealViewable`/a `RetiredViewable(r, a)` operator, so the invariant catches a stale retired set influencing a writer — not only a stale seal influencing GC.
- All new variables (`attemptSeq`, `adopted`, `sealAt`, `retiredAt`) MUST be inert when `EnableAttemptScoping = FALSE` (stay at `Init`), added to the `VARIABLES` block, `vars` tuple (line 102), `Init`, `TypeOK`, and every action's `UNCHANGED` list, with writes gated behind the flag — exactly like `coordFence`/`storedTok`.
- **M4 soundness guard (avoid a false Gate-A failure):** do NOT admit a full second-leader pipeline over the shared globals (`gcRound`/`cursor`/`blobIndeg`/`fencePos`) — this model has no lease to serialize two leaders, so a free L2 could break an R0 invariant for reasons unrelated to the fix. Instead keep the existing single-driver pipeline for the *adopted* path and model the deposed leader as the **single extra action `GDeposedWriteSeal`** that only writes the new attempt-scoped slots (`sealAt`/`retiredAt`) for a stale attempt — it touches no shared global. The two-leader concurrency that matters (one folds+adopts, a deposed one writes its own attempt) is captured by `attemptSeq` minting + this one action, without unserialized global mutation. Stage6 may keep `Leaders={L1}` if `GDeposedWriteSeal` alone produces the deposed write; only widen to `{L1,L2}` if a second *minting* leader is needed and confirm no spurious R0 violation results.

- [ ] **Step 1: Add the two CONSTANTS to the model**

In `CaGcRootLocalPartManifestCore.tla` `CONSTANTS` block (lines 8–37), add near the other `Enable*`/`Sabotage*` names:

```tla
    EnableAttemptScoping,              \* TRUE = the fix: gc/gen artifacts are attempt-scoped; only the adopted attempt is viewable
    SabotageDeposedLeaderWritesFinalGen \* TRUE = a deposed leader writes a final-gen artifact to the shared slot (reproduces the wedge/divergence)
```

- [ ] **Step 2: Bind both constants `FALSE` in every existing cfg, run stage0+stage1, confirm inertness BEFORE adding any new variable/action**

Add these three lines to **every** `CaGcRootLocalPartManifestCore_*.cfg` (all 42) in the `CONSTANTS` section:

```
EnableAttemptScoping = FALSE
SabotageDeposedLeaderWritesFinalGen = FALSE
MaxAttempt = 2
```

Run: `cd docs/superpowers/models && ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage1`
Expected: TLC completes, **402,034 distinct states** (and stage0 → 19,846). If the counts differ, a binding is wrong — fix before proceeding. (At this step there are no new variables yet, so the counts MUST match exactly.)

- [ ] **Step 3: Add the inert flag-gated variables**

Add `adopted`, `sealAttempt` (names per the modeling approach above) to the `VARIABLES` block (lines 61–84), the `vars` tuple (line 102), `Init` (initialize to the None/empty constants), `TypeOK` (lines 1013–1035), and the `UNCHANGED` list of **every** action. Gate every write behind `EnableAttemptScoping`. Re-run stage0+stage1.

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage0` and `..._stage1`
Expected: **still 19,846 / 402,034 distinct** (inertness preserved — the new vars never move when the flag is off).

- [ ] **Step 4: Add `INV_ONLY_ADOPTED_VIEWABLE` and the `GDeposedWriteSeal` action**

Add the invariant near the other `INV_*` (after line 1100), in the `Enable* => P` vacuity form so it is harmless in flag-off cfgs:

```tla
\* Only the adopted attempt's generation artifact may participate in any decision path.
\* A seal written under a non-adopted attempt is invisible to retire/recheck/delete/trim.
INV_ONLY_ADOPTED_VIEWABLE ==
    EnableAttemptScoping =>
        \A r \in 0..MaxRound :
            \A a \in Actors :
                (sealAttempt[r][a] /\ a # adopted[r]) => ~SealViewable(r, a)
```

…where `SealViewable(r, a)` is a defined operator returning TRUE iff round `r`'s decisions consult attempt `a`'s seal (define it so that, with scoping on, only `adopted[r]` is consulted; with the sabotage on, the deposed write makes a non-adopted attempt viewable). Add `GDeposedWriteSeal(l, r)` to the action list and to `Next` gated by `EnableAttemptScoping \/ SabotageDeposedLeaderWritesFinalGen`.

- [ ] **Step 5: Create the positive stage cfg and prove Gate A green**

Create `CaGcRootLocalPartManifestCore_stage6_attemptscoping.cfg` modeled on `_stage3.cfg`, with: `EnableAttemptScoping = TRUE`, `SabotageDeposedLeaderWritesFinalGen = FALSE`, `MaxAttempt = 2`, `Leaders = {L1}` (the deposed write is the `GDeposedWriteSeal` action; widen to `{L1,L2}` only if a second minting leader is needed and it does not spuriously violate R0 — see the M4 guard), all other `Enable*`/`Sabotage* = FALSE`, `CHECK_DEADLOCK FALSE`, `CONSTRAINT StateConstraint`, and:

```
INVARIANT TypeOK
INVARIANT INV_ONLY_ADOPTED_VIEWABLE
INVARIANT INV_NO_LOSS
INVARIANT INV_NO_DANGLE
INVARIANT INV_NO_RETURN
INVARIANT INV_JOURNAL_COVERAGE
```

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage6_attemptscoping`
Expected: **No invariant violation** ("Model checking completed. No error has been found."). Record the distinct-state count.

- [ ] **Step 6: Create the sabotage cfg and prove it counterexamples (the negative control)**

Create `CaGcRootLocalPartManifestCore_sab_deposedleaderwritesfinalgen.cfg`: identical scope to stage6 but `EnableAttemptScoping = FALSE`, `SabotageDeposedLeaderWritesFinalGen = TRUE`, asserting the same invariant block.

Run: `./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_deposedleaderwritesfinalgen`
Expected: **VIOLATED** — TLC reports a counterexample on `INV_ONLY_ADOPTED_VIEWABLE` (or an R0 invariant: the divergent-run corruption). This proves the sabotage actually exercises the failure the fix prevents. If it does NOT violate, the model is too weak (the deposed write isn't actually reaching a reader) — strengthen `SealViewable`/`GDeposedWriteSeal` until it counterexamples.

- [ ] **Step 7: Bounded-B liveness witnesses (evidence, not the gate)**

Add to `stage6` (or a small `_witness_attemptadopt.cfg`) two non-vacuity witnesses asserted to be VIOLATED (the TLC idiom for "this interesting state is reachable"):

```tla
\* Two leaders fold the same round; exactly one is adopted; the loser's seal stays invisible.
W_TwoLeadersOneAdopt == ~(\E r \in 0..MaxRound : \E a, b \in Actors :
    a # b /\ sealAttempt[r][a] /\ sealAttempt[r][b] /\ adopted[r] \in {a, b})
```

Run the witness cfg; expected **VIOLATED** (the state is reachable). Record it.

- [ ] **Step 8: Re-run the full sabotage/witness suite for regressions**

Re-run the existing sabotage controls that share touched actions to confirm no `UNEXPECTED PASS` was introduced. At minimum: `_sab_twoowners` (must still violate `INV_NO_LOSS`), `_sab_reducerownsfence` (must still violate `INV_NO_DANGLE`), `_stage3` (must still pass). Spot-check one large stage only if a shared ungated action was touched.

Run each via `./run_gc_partmanifest.sh <cfg>`; expected verdicts unchanged from `RESULTS.md`.

- [ ] **Step 9: Update RESULTS.md and commit**

Record stage6 distinct count, the sabotage counterexample, the witness, and the inertness confirmation (stage0=19,846 / stage1=402,034 with the flag off) in `CaGcRootLocalPartManifestCore_RESULTS.md`.

```bash
git add docs/superpowers/models/
git commit -m "CA GC TLA+ Gate A: attempt-scoped generation visibility under two leaders

Add EnableAttemptScoping + SabotageDeposedLeaderWritesFinalGen to
CaGcRootLocalPartManifestCore; prove INV_ONLY_ADOPTED_VIEWABLE under
Leaders={L1,L2} (Gate A green), the deposed-final-write sabotage
counterexamples, and inertness (stage0/stage1 counts unchanged when off).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

**Gate condition to proceed to Task 2:** stage6 green, sabotage VIOLATED, inertness counts exact, no UNEXPECTED PASS in the re-run suite.

---

### Task 2: `GcState.snap_attempt` (struct + proto + serde)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h:45-54`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp:78-82, 129-135`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto:149-158`
- Test: `src/Disks/tests/gtest_cas_gc_formats.cpp`

**Interfaces:**
- Produces: `GcState::snap_attempt` (`uint64_t`, default `0`); round-trips through `encodeGcState`/`decodeGcState`. Consumed by Tasks 5–9.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_formats.cpp`:

```cpp
TEST(CasGcFormats, SnapAttemptRoundTrips)
{
    DB::Cas::GcState s;
    s.round = 7;
    s.snap_generation = 4;
    s.snap_attempt = 42;
    const String bytes = DB::Cas::encodeGcState(s);
    const DB::Cas::GcState back = DB::Cas::decodeGcState(bytes);
    EXPECT_EQ(back.snap_attempt, 42u);
    EXPECT_EQ(back.snap_generation, 4u);
}

TEST(CasGcFormats, SnapAttemptDefaultsZero)
{
    DB::Cas::GcState s;
    EXPECT_EQ(DB::Cas::decodeGcState(DB::Cas::encodeGcState(s)).snap_attempt, 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (from build dir): `ninja unit_tests_dbms > build_t2.log 2>&1` — Expected: compile FAIL, `'struct DB::Cas::GcState' has no member named 'snap_attempt'`.

- [ ] **Step 3: Add the proto field**

In `Proto/cas_format.proto`, message `GcStateProto` (lines 149-158), add after line 157 (`fence_version = 8`):

```proto
  uint64 snap_attempt           = 9;  // adopted attempt id for the round that produced snap_generation
```

- [ ] **Step 4: Add the struct field**

In `CasGcFormats.h`, in `struct GcState` after `snap_pruned_through` (line 51):

```cpp
    uint64_t snap_attempt = 0;     /// adopted attempt id (folding leader's lease.seq) for snap_generation
```

- [ ] **Step 5: Wire encode/decode**

In `CasGcFormats.cpp` `encodeGcState` after line 82:

```cpp
    msg.set_snap_attempt(state.snap_attempt);
```

In `decodeGcState` after line 135 (`state.snap_pruned_through = msg.snap_pruned_through();`):

```cpp
    state.snap_attempt = msg.snap_attempt();
```

- [ ] **Step 6: Build and run the test**

Run: `ninja unit_tests_dbms > build_t2b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcFormats.*' > test_t2.log 2>&1`
Expected: PASS (all `CasGcFormats.*`). Analyze logs via subagent.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto \
        src/Disks/tests/gtest_cas_gc_formats.cpp
git commit -m "CA GC: add GcState.snap_attempt (proto field 9), no migration (pre-release)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 3: `CasLayout` attempt-scoped key derivation

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h:146-169`
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces (changed signatures — Task 3 lands a compile-clean tree by updating EVERY caller listed below):
  - `String foldSealKey(uint64_t generation, uint64_t attempt) const` → `<p>/gc/gen/<generation>/attempt/<attempt>/fold_seal`
  - `String completionSealKey(uint64_t generation, uint64_t attempt) const` → `…/attempt/<attempt>/completion_seal`
  - `String blobTargetRunKey(uint64_t generation, uint64_t attempt, uint64_t shard, uint64_t seq) const` → `…/attempt/<attempt>/blob_target/<shard>/<seq>`
  - `String partManifestCleanupKey(uint64_t generation, uint64_t attempt, uint64_t owner_shard, uint64_t seq) const` → `…/attempt/<attempt>/part_manifest_cleanup/<owner_shard>/<seq>`
  - `String retiredKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const` → `…/attempt/<attempt>/retired/<round>/<shard>` (was `gc/retired/<round>.<fence_seq>/<shard>`)
  - `String outcomesKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const` → `…/attempt/<attempt>/outcomes/<round>/<shard>` (was `gc/outcomes/<round>.<fence_seq>/<shard>`)
  - `String gcGenPrefix(uint64_t generation) const` → `<p>/gc/gen/<generation>/` (NEW — wholesale retention prune, Task 9; also the prefix `RetireView` LISTs)
  - `String gcGenAttemptPrefix(uint64_t generation, uint64_t attempt) const` → `<p>/gc/gen/<generation>/attempt/<attempt>/` (NEW — attempt sweep + RetireView resolution)
  - `String gcGenAttemptRetiredPrefix(uint64_t generation, uint64_t attempt) const` → `…/attempt/<attempt>/retired/` (NEW — `RetireView` LISTs this under the accepted attempt instead of the old `gcRetiredPrefix()`)
  - **REMOVE** `gcRetiredPrefix()` (was `<p>/gc/retired/`) — no reader LISTs the flat namespace anymore.

> **Why retired/outcomes move too (user review):** `retired` is writer-facing — `RetireView::refresh()` LISTs the retired namespace and writers consult `store->retireView().isCondemnedToken(...)` on the publish gate (`CasBuild.cpp:235,414,666,725`). A deposed leader's stale retired set under its own epoch would survive a flat LIST and influence live writers — violating "no unadopted artifact may influence a decision." Scoping them under the adopted attempt closes that and lets the wholesale `gc/gen/<g>/` retention prune reclaim them (no separate sweep).

**Complete caller set Task 3 must update to keep `unit_tests_dbms` compiling (M1/B1):**
- Production: `CasGc.cpp` (`foldSealKey`/`completionSealKey`/`blobTargetRunKey`/`partManifestCleanupKey`/`retiredKey`/`outcomesKey` at 440, 547, 829, 917, 949, 1093-1106, 1360, 1374, 1386 + `readFoldSeal`/`readCompletionSeal`/`readSealedCursors` 1115-1140), `CasBlobInDegree.cpp` (71-73, 171; `foldDeltasIntoGeneration`/`zeroInDegree`/`inDegreeInGeneration` signatures), `CasGcShardPlan.cpp` (`ShardReducer::reduce` 94-102), **`CasOrphanManifestSweep.cpp:43`** (`sealedFoldCursor` calls `foldSealKey(g)` — production consumer, see Task 7 for the back-scan fix), `CasRetireView.cpp:46` (replaces `gcRetiredPrefix()` LIST — see Task 7).
- Tests: `gtest_cas_layout.cpp:59-62`, `gtest_cas_blob_indegree.cpp:22,25,35,39,41,54-57,72`, `gtest_cas_gc_round.cpp:755,757,759,764,765,825,1020,1021`, `gtest_cas_gc_shard_plan.cpp:251-259,430-436,522-524,607,616,621,623,630-675`, `cas_test_helpers.h:439,458` (`inDegreeInGeneration`, `foldSealKey`, `foldCursorOf` helper).

For Task 3, give every caller a *mechanical* placeholder attempt argument so the tree compiles: pass `state.snap_attempt` at GC readers, the fold leader's `state.lease.seq` at the fold/retire writers, and a literal in tests (the existing tests assert generation-N behavior with a single implicit attempt — pass `0` or the test's chosen attempt and update the expected key strings). The *semantics* (CAS #1/#2, strict putIfAbsent, RetireView resolution, resume, pruning) are refined by Tasks 4-9, each with its own failing-test-first cycle.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_layout.cpp` (match the `Layout` ctor form the file already uses):

```cpp
TEST(CasLayout, AttemptScopedGenKeys)
{
    DB::Cas::Layout layout("p");
    EXPECT_EQ(layout.foldSealKey(4, 42), "p/gc/gen/4/attempt/42/fold_seal");
    EXPECT_EQ(layout.completionSealKey(5, 42), "p/gc/gen/5/attempt/42/completion_seal");
    EXPECT_EQ(layout.blobTargetRunKey(4, 42, 3, 0), "p/gc/gen/4/attempt/42/blob_target/3/0");
    EXPECT_EQ(layout.partManifestCleanupKey(4, 42, 0, 1), "p/gc/gen/4/attempt/42/part_manifest_cleanup/0/1");
    EXPECT_EQ(layout.retiredKey(4, 42, 7, 3), "p/gc/gen/4/attempt/42/retired/7/3");
    EXPECT_EQ(layout.outcomesKey(5, 42, 7, 3), "p/gc/gen/5/attempt/42/outcomes/7/3");
    EXPECT_EQ(layout.gcGenPrefix(4), "p/gc/gen/4/");
    EXPECT_EQ(layout.gcGenAttemptPrefix(4, 42), "p/gc/gen/4/attempt/42/");
    EXPECT_EQ(layout.gcGenAttemptRetiredPrefix(4, 42), "p/gc/gen/4/attempt/42/retired/");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t3.log 2>&1` — Expected: compile FAIL (arity mismatches across the caller set above — fix them all in Step 3).

- [ ] **Step 3: Implement the key changes + update every caller**

Replace the four `gc/gen` helpers and `retiredKey`/`outcomesKey` in `CasLayout.h` with the attempt-scoped forms below, add the three prefix helpers, and remove `gcRetiredPrefix()`:

```cpp
    String gcGenPrefix(uint64_t generation) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/";
    }

    String gcGenAttemptPrefix(uint64_t generation, uint64_t attempt) const
    {
        return gcGenPrefix(generation) + "attempt/" + std::to_string(attempt) + "/";
    }

    String foldSealKey(uint64_t generation, uint64_t attempt) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "fold_seal";
    }

    String completionSealKey(uint64_t generation, uint64_t attempt) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "completion_seal";
    }

    String blobTargetRunKey(uint64_t generation, uint64_t attempt, uint64_t shard, uint64_t seq) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "blob_target/"
               + std::to_string(shard) + "/" + std::to_string(seq);
    }

    String partManifestCleanupKey(uint64_t generation, uint64_t attempt, uint64_t owner_shard, uint64_t seq) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "part_manifest_cleanup/"
               + std::to_string(owner_shard) + "/" + std::to_string(seq);
    }

    String gcGenAttemptRetiredPrefix(uint64_t generation, uint64_t attempt) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "retired/";
    }

    String retiredKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const
    {
        return gcGenAttemptRetiredPrefix(generation, attempt) + std::to_string(round) + "/" + std::to_string(shard);
    }

    String outcomesKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "outcomes/" + std::to_string(round) + "/" + std::to_string(shard);
    }
```

Then mechanically thread the `attempt` argument through **every caller in the "Complete caller set" list above** (production with `state.snap_attempt`/`state.lease.seq` as noted; tests with the literal attempt + updated expected strings). The tree MUST compile at the end of this task.

- [ ] **Step 4: Run the layout test + full suite compiles**

Run: `ninja unit_tests_dbms > build_t3b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasLayout.*' > test_t3.log 2>&1`
Expected: build succeeds (all callers threaded), `CasLayout.*` PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "CA GC: attempt-scope all six gc/gen key families + thread callers

fold_seal/completion_seal/blob_target/part_manifest_cleanup AND retired/outcomes
now live under gc/gen/<gen>/attempt/<a>/. Add gcGenPrefix/gcGenAttemptPrefix/
gcGenAttemptRetiredPrefix; remove the flat gcRetiredPrefix. All production +
test callers threaded with the attempt arg (semantics refined next commits).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 4: Strict `putIfAbsent` for deterministic artifacts (`foldDeltasIntoGeneration` + `ShardReducer`)

**Files:**
- Create helper: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h` (add a free function) OR `CasBlobInDegree.cpp` anonymous namespace if not reused.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp:89-91, 168-173`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h:34-36`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.cpp` (`ShardReducer::reduce`)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: `Layout::blobTargetRunKey(gen, attempt, shard, seq)` (Task 3); `Backend::putIfAbsent` → `PutResult{PutOutcome::{Done,PreconditionFailed}, token}`.
- Produces:
  - `void foldDeltasIntoGeneration(Backend &, const Layout &, uint64_t prior_generation, uint64_t new_generation, uint64_t attempt, uint64_t shard, std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs)` (adds `attempt`).
  - A strict-write helper: `void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes)` — `putIfAbsent`; on `PreconditionFailed`, `get(key)` and compare bytes: byte-equal → adopt (no-op), divergent → throw `ErrorCodes::CORRUPTED_DATA`. (This centralizes the rule the spec calls "deterministic artifacts: byte-equal-or-CORRUPTED_DATA".)
  - `ShardReducer::reduce(Backend &, const Layout &, uint64_t prior_generation, uint64_t new_generation, uint64_t attempt, std::vector<BlobDelta>)` (adds `attempt`).

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_blob_indegree.cpp`:

```cpp
TEST(CasBlobInDegree, FoldDeltaByteEqualReplayAdopts)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout layout("p");
    std::vector<DB::Cas::BlobDelta> deltas{{.blob_hash = DB::UInt128(1), .delta = 1}};
    std::vector<DB::Cas::RunRef> runs1, runs2;
    DB::Cas::foldDeltasIntoGeneration(*backend, layout, 0, 1, /*attempt*/ 7, /*shard*/ 0, deltas, runs1);
    // Same inputs, same attempt => byte-identical run already present => adopt, no throw.
    EXPECT_NO_THROW(DB::Cas::foldDeltasIntoGeneration(*backend, layout, 0, 1, 7, 0, deltas, runs2));
    EXPECT_EQ(runs1, runs2);
}

TEST(CasBlobInDegree, FoldDeltaDivergentBytesThrowsCorrupted)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout layout("p");
    // Pre-occupy the run key (attempt 7) with junk, then fold => divergent => CORRUPTED_DATA.
    backend->putIfAbsent(layout.blobTargetRunKey(1, 7, 0, 0), "not-a-valid-run");
    std::vector<DB::Cas::BlobDelta> deltas{{.blob_hash = DB::UInt128(1), .delta = 1}};
    std::vector<DB::Cas::RunRef> runs;
    EXPECT_THROW(DB::Cas::foldDeltasIntoGeneration(*backend, layout, 0, 1, 7, 0, deltas, runs),
                 DB::Exception);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t4.log 2>&1` — Expected: compile FAIL (`foldDeltasIntoGeneration` arity) and, once compiling, the divergent test FAILs (old code ignores the outcome — no throw).

- [ ] **Step 3: Add the strict helper**

In `CasBlobInDegree.cpp` anonymous namespace (near `cityHash128`, lines 58-62):

```cpp
void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes)
{
    if (backend.putIfAbsent(key, bytes).outcome == PutOutcome::PreconditionFailed)
    {
        const auto existing = backend.get(key);
        if (!existing || existing->bytes != bytes)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc: deterministic artifact at {} occupied by divergent bytes (impossible under "
                "correct operation; refusing to proceed)", key);
        /// byte-equal => our own deterministic replay; adopt (no-op).
    }
}
```

- [ ] **Step 4: Thread `attempt` and use the helper**

Change `CasBlobInDegree.h:34-36` and `CasBlobInDegree.cpp:89-91` signatures to add `uint64_t attempt` after `new_generation`. Replace lines 168-173:

```cpp
    writer.finish();
    const String run_bytes = out.str();

    const String run_key = layout.blobTargetRunKey(new_generation, attempt, shard, 0);
    putDeterministicArtifact(backend, run_key, run_bytes);
    out_runs.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes)});
```

Update `ShardReducer::reduce` in `CasGcShardPlan.cpp` to accept and forward `attempt` to its `blobTargetRunKey`/`foldDeltasIntoGeneration` call. Update the read side `readGenerationRows` (CasBlobInDegree.cpp:71-73) and `zeroInDegree`/`inDegreeInGeneration` to take `attempt` (they resolve `blobTargetRunKey(generation, attempt, shard, seq)`) — these are consumed by Task 7's readers; for now give them an `attempt` parameter and thread it.

- [ ] **Step 5: Run the test**

Run: `ninja unit_tests_dbms > build_t4b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasBlobInDegree.*' > test_t4.log 2>&1`
Expected: PASS. Analyze via subagent.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "CA GC: strict putIfAbsent for deterministic in-degree runs + attempt threading

foldDeltasIntoGeneration/ShardReducer now key runs under the attempt and check
the putIfAbsent outcome: byte-equal replay adopts, divergent bytes fail-closed
with CORRUPTED_DATA (was: outcome ignored -> divergent run vs seal).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 5: `Gc::fold` — attempt-prefix writes + fold-adopt CAS #1

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:196-457` (`fold`)
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp`

**Interfaces:**
- Consumes: `state.lease.seq` (the candidate attempt); `Layout::foldSealKey(gen, attempt)`, `blobTargetRunKey(gen, attempt, …)`, `partManifestCleanupKey(gen, attempt, …)`; `putDeterministicArtifact`; `GcState::snap_attempt`.
- Produces: after a successful fold, `state.snap_generation = G_f` AND `state.snap_attempt = a` (where `a = state.lease.seq`), committed by one lease-token `gc/state` CAS (CAS #1). On lease loss the CAS fails → `ABORTED` (unchanged behavior); the fold artifacts under attempt `a` are unadopted garbage.

- [ ] **Step 1: Write the failing test (fold adopts the attempt)**

Add to `src/Disks/tests/gtest_cas_gc_fold.cpp`:

```cpp
/// After a fold, gc/state records snap_attempt == the folding leader's lease.seq, and the fold
/// seal lives under that attempt prefix.
TEST(CasGcFold, FoldAdoptsAttemptEqualsLeaseSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();

    const auto st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(st.snap_attempt, st.lease.seq);
    EXPECT_GT(st.snap_generation, 0u);
    // The fold seal is durable under (snap_generation, snap_attempt) — derive the generation from state.
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t5.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcFold.FoldAdoptsAttemptEqualsLeaseSeq' > test_t5.log 2>&1`
Expected: FAIL (`snap_attempt` is 0, fold seal not under attempt).

- [ ] **Step 3: Implement the attempt prefix + CAS #1**

In `fold` (CasGc.cpp:196-457): introduce `const uint64_t attempt = state.lease.seq;` near line 217. Pass `attempt` to every fold-artifact write: the `foldDeltasIntoGeneration`/`ShardReducer::reduce` calls (lines 384-431), `writePartManifestCleanupBundle` (433-434 — thread `attempt` through it to `partManifestCleanupKey`). Replace the fold_seal block (438-448) to write under `foldSealKey(new_generation, attempt)` via `putDeterministicArtifact` (drop the inline divergent-throw — the helper now owns it). Replace the gc/state CAS (450-456) to set BOTH:

```cpp
    state.snap_generation = new_generation;
    state.snap_attempt = attempt;
    const CasResult fold_res = backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token);
    if (fold_res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (lease lost / another leader advanced); retry next round");
    state_token = fold_res.token;
    return result;
```

- [ ] **Step 4: Run the test (+ the whole fold suite for regressions)**

Run: `ninja unit_tests_dbms > build_t5b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcFold.*' > test_t5.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "CA GC fold: write artifacts under attempt prefix; CAS #1 adopts (gen, attempt)

fold now writes fold_seal/runs/cleanup under gc/gen/<G_f>/attempt/<lease.seq>/ and
the lease-guarded gc/state CAS sets snap_generation AND snap_attempt together. A
deposed leader's fold lands under its own unadopted attempt (invisible).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 6: `Gc::retire` + `Gc::recheck` — tail artifacts (retired set, outcomes, completion seal/runs) under the adopted attempt + completion-advance CAS #2

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — `retire:459-577` (retired-set write at 547), `recheck:667-958` (outcomes at 829, completion seal at 915-925, retired-drop at 949, CAS #2 at 927-944)
- Test: `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp`

**Interfaces:**
- Consumes: `state.snap_attempt` (the attempt adopted by CAS #1 — neither retire nor recheck mints a new one); `Layout::completionSealKey(gen, attempt)`, `blobTargetRunKey(gen, attempt, …)`, `retiredKey(gen, attempt, round, shard)`, `outcomesKey(gen, attempt, round, shard)`; `putDeterministicArtifact` for the **deterministic** artifacts (runs, seals); **adopt-on-present** (read-if-present, never recompute-and-compare) for the **observation-bearing** artifacts (retired set, outcome log — these carry HEAD-observed tokens that two observers may legitimately differ on, so they keep their existing `putIfAbsent`→decode-existing path, NOT the strict byte-equal guard).
- Produces:
  - `retire` writes the retired set under `retiredKey(folded.fold_seal.generation /*G_f*/, state.snap_attempt, round, shard)` (the fold generation `G_f` + the adopted attempt).
  - `recheck` writes outcomes under `outcomesKey(completion_generation /*G_c*/, state.snap_attempt, round, shard)`, completion in-degree runs + completion_seal under `(completion_generation, state.snap_attempt)`, and drops the retired set at the same `retiredKey(G_f, snap_attempt, round, shard)`.
  - CAS #2 advances `next.snap_generation = completion_generation` while keeping `next.snap_attempt = state.snap_attempt`; gated on a complete outcome log (every retired entry of the round has a recorded outcome).
- **Artifact-class rule (spec §strict-put-if-absent):** runs/seals (deterministic) → `putDeterministicArtifact` (byte-equal-or-`CORRUPTED_DATA`); retired set + outcome log (observation-bearing) → first-durable-write-wins / read-if-present. Do NOT route retired/outcomes through `putDeterministicArtifact`.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp` a test that, after a full round, asserts the completion seal is under the inherited attempt and `snap_attempt` is unchanged across CAS #2:

```cpp
TEST(CasGcRecheck, CompletionInheritsFoldAttempt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();          // fold-adopt
    const auto after_fold = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();          // retire -> fence -> recheck -> completion-advance
    const auto after_complete = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(after_complete.snap_attempt, after_fold.snap_attempt);     // inherited, not re-minted
    EXPECT_GT(after_complete.snap_generation, after_fold.snap_generation);
    EXPECT_TRUE(backend->head(store->layout()
        .completionSealKey(after_complete.snap_generation, after_complete.snap_attempt)).exists);
}
```

Also add a test asserting the retired set is written under the fold generation + adopted attempt during the second round's retire (read it from `gc/state` after the drop-ref round but before completion, or assert it is consumed/dropped under the accepted attempt — match the file's existing fence/recheck driving pattern).

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t6.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcRecheck.CompletionInheritsFoldAttempt' > test_t6.log 2>&1`
Expected: FAIL (completion seal not under the inherited attempt / signature mismatch).

- [ ] **Step 3: Implement**

**retire (CasGc.cpp:459-577):** the retired-set write at line 547 changes from `retiredKey(round, state.fence_seq, shard)` to `retiredKey(folded.fold_seal.generation, state.snap_attempt, round, shard)` (fold generation `G_f` + adopted attempt). Keep its existing observation-bearing path (`putIfAbsent`→on `PreconditionFailed` decode the existing set), NOT `putDeterministicArtifact`.

**recheck (CasGc.cpp:667-958):** use `const uint64_t attempt = state.snap_attempt;` for all completion-generation artifact keys. Thread `attempt` into the completion `foldDeltasIntoGeneration`/`ShardReducer::reduce` calls (723, 735-743). The outcome-log writes (826-848) change to `outcomesKey(completion_generation, attempt, round, shard)` (observation-bearing — keep the existing decode-existing path). Replace the completion_seal block (915-925) to write `completionSealKey(completion_generation, attempt)` via `putDeterministicArtifact`. The retired-set drop at line 949 changes to `retiredKey(folded.fold_seal.generation, attempt, round, shard)` (must match retire's write key). In the CAS #2 block (927-944), keep `next.snap_attempt = state.snap_attempt` (copied by `GcState next = state;` — do NOT change it). Outcome-coverage gate: the completion-advance must occur only after every retired entry has a recorded outcome (the existing logic computes `computed` per shard; assert/guard it covers the retired set before the CAS — if a gap, recompute the missing outcome from the accepted retired set via the exact-token delete, then proceed). Keep `pruneSupersededGenerations` (Task 9 adjusts it).

- [ ] **Step 4: Run the test (+ fence/recheck + resume suites)**

Run: `ninja unit_tests_dbms > build_t6b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcRecheck.*:CasGcRound.*:CasGcResume.*' > test_t6.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "CA GC recheck: completion artifacts under inherited attempt; CAS #2 inherits snap_attempt

recheck writes completion runs/seal under (completion_generation, snap_attempt) and
the completion-advance CAS keeps the fold-adopted attempt; gated on full outcome-log
coverage of the round's retired set.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 7: Readers resolve via `(snap_generation, snap_attempt)` — incl. the back-scan elimination (B2) + `RetireView`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — `readSealedCursors` (1129-1140), `readFoldSeal` (~1117), `readCompletionSeal` (~1124), the **`fold` discover-ref-seal back-scan (244-251)**, `previewDeletes` (1317-1343), `trim` (960-1034), the `zeroInDegree`/`inDegreeInGeneration` call sites in `retire`/`recheck`.
- Modify: `src/Disks/.../Core/CasRetireView.cpp:46` — `refresh()` resolves retired sets under the accepted attempt (writer-facing publish gate).
- Modify: `src/Disks/.../Core/CasOrphanManifestSweep.cpp:43` — `sealedFoldCursor` reads the seal at the adopted `(snap_generation, snap_attempt)` instead of a blind back-scan (B1).
- Modify: `CasBlobInDegree.h`/`.cpp` — `zeroInDegree`/`inDegreeInGeneration` already took `attempt` from Task 4; ensure all call sites pass `state.snap_attempt`.
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

**Interfaces:**
- Consumes: `GcState::snap_attempt`, `snap_generation`; `Layout::gcGenAttemptRetiredPrefix`.
- Produces: every read of a `gc/gen` artifact resolves `(snap_generation, snap_attempt)`. `readFoldSeal`/`readCompletionSeal` gain an `attempt` parameter; `readSealedCursors(generation, attempt)`; `previewDeletes` reads `zeroInDegree(backend, layout, state.snap_generation, state.snap_attempt, 0)`; `RetireView::refresh()` reads `gc/state` then resolves retired sets under `gcGenAttemptRetiredPrefix(snap_generation, snap_attempt)` (replacing the flat `gcRetiredPrefix()` LIST).

> **B2 — eliminate the blind multi-generation back-scan (design-gap fix, verified safe).** Today `fold` (242-251) and `CasOrphanManifestSweep::sealedFoldCursor` (33-52) walk generations `g = snap_generation downto 1` looking for the latest seal. With one stored `snap_attempt` that is unsound for `g ≤ snap_generation-2` (a *prior* round's adopted attempt is a different `lease.seq`, recorded nowhere). **Verified:** in all reachable states the seal resolves at `snap_generation` (normal completed round → `readCompletionSeal(snap_generation)`, attempt `snap_attempt`) or, mid-round, at `snap_generation = G_f` (its fold seal, same `snap_attempt`); the loop never legitimately reaches a prior round. **Fix:** read the seal **directly at the adopted `(snap_generation, snap_attempt)`** — `readCompletionSeal(snap_generation, snap_attempt)` else `readFoldSeal(snap_generation, snap_attempt)` — and on absence **fail-closed to the empty seal** (all-Read / conservative, never an older generation). This removes the unsound scan and keeps the spec's single-`snap_attempt` schema. Apply the same direct-read to `sealedFoldCursor`.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_round.cpp` a test that writes a *decoy* fold seal under a non-adopted attempt and asserts `previewDeletes`/a round ignores it:

```cpp
TEST(CasGcRound, NonAdoptedAttemptSealIgnored)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    const auto st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);

    // Plant a decoy fold seal under a DIFFERENT attempt at the same generation.
    backend->putIfAbsent(store->layout().foldSealKey(st.snap_generation, st.snap_attempt + 999),
                         "decoy-seal-bytes");
    // A read path must resolve only the adopted attempt and not throw / not be influenced.
    EXPECT_NO_THROW(gc.previewDeletes());
}
```

- [ ] **Step 2: Run test to verify it fails / passes-trivially**

Run: `ninja unit_tests_dbms > build_t7.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcRound.NonAdoptedAttemptSealIgnored' > test_t7.log 2>&1`
Expected: compile FAIL until `readSealedCursors`/`previewDeletes` take the attempt; once compiling, PASS only if readers ignore the decoy.

- [ ] **Step 3: Implement**

Thread `state.snap_attempt` into every reader. `readSealedCursors(generation, attempt)`, `readFoldSeal(generation, attempt)`, `readCompletionSeal(generation, attempt)`; in `fold` the `readSealedCursors(state.snap_generation, state.snap_attempt)` call (line 215) AND replace the back-scan (242-251) with the direct adopted-attempt read + fail-closed-empty per the B2 note above; in `previewDeletes` the `zeroInDegree(backend, layout, state.snap_generation, state.snap_attempt, 0)` (line 1329); in `trim`, source from the sealed fold coverage of the adopted attempt; in `retire` `zeroInDegree(backend, layout, folded.fold_seal.generation, state.snap_attempt, 0)`; in `recheck` `inDegreeInGeneration(backend, layout, completion_generation, state.snap_attempt, entry_shard, entry.hash)`.

**`RetireView::refresh()` (`CasRetireView.cpp:46`):** replace the flat `layout.gcRetiredPrefix()` LIST with: read `gc/state`, decode `(snap_generation, snap_attempt)`, then LIST `layout.gcGenAttemptRetiredPrefix(snap_generation, snap_attempt)` and GET each retired set there. A retired set under any *other* attempt is invisible to writers — closing the writer-facing leak. (`RetireView` already GETs `gc/state`; reuse that read for the pair.)

**`CasOrphanManifestSweep::sealedFoldCursor` (`CasOrphanManifestSweep.cpp:43`):** read `gc/state` for `(snap_generation, snap_attempt)` and read `foldSealKey(snap_generation, snap_attempt)` (else completion seal at the same pair) directly; drop the `for g downto 1` scan.

Add a second test asserting a **decoy retired set** planted under a non-adopted attempt does NOT condemn a live writer token through `RetireView` (construct a fresh `RetireView`, `refresh()`, and assert `isCondemnedToken(...)` is false for a token only present in the decoy set).

- [ ] **Step 4: Run the test (+ full CasGc suite)**

Run: `ninja unit_tests_dbms > build_t7b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGc*' > test_t7.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "CA GC: all gc/gen readers resolve via (snap_generation, snap_attempt)

readSealedCursors/readFoldSeal/readCompletionSeal/previewDeletes/trim/zeroInDegree/
inDegreeInGeneration consult only the adopted attempt; a non-adopted attempt's
artifact is never read by any decision path.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 8: `tryResumeIncompleteRound` derives via the accepted attempt

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1345-1414` (`tryResumeIncompleteRound`)
- Test: `src/Disks/tests/gtest_cas_gc_resume.cpp`

**Interfaces:**
- Consumes: `state.snap_attempt`, `state.snap_generation`; `readFoldSeal(snap_generation, snap_attempt)`, `partManifestCleanupKey(snap_generation, snap_attempt, …)`, `retiredKey(snap_generation /*G_f*/, snap_attempt, round, shard)`.
- Produces: resume reads the tail of the accepted attempt — the incompleteness signal (retired sets present) is read at `retiredKey(state.snap_generation, state.snap_attempt, round, shard)`; `readFoldSeal(state.snap_generation, state.snap_attempt)`; the cleanup-bundle walk uses `partManifestCleanupKey(state.snap_generation, state.snap_attempt, 0, seq)`; re-fence + re-recheck run under the accepted attempt. No new leader's `lease.seq` is used for the tail (the tail belongs to the adopted attempt). **Note:** mid-round, `snap_generation = G_f` (fold-adopted, completion not yet advanced), so the retired set written by `retire` under `(G_f, snap_attempt)` is found at exactly this generation+attempt.

- [ ] **Step 1: Write the failing test**

Extend `src/Disks/tests/gtest_cas_gc_resume.cpp`:

```cpp
/// A fresh Gc with a DIFFERENT id resumes the accepted attempt's tail (not its own lease.seq):
/// the durable fold seal under (snap_generation, snap_attempt) drives recheck to completion.
TEST(CasGcResume, ResumeUsesAcceptedAttemptNotOwnLeaseSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc1(store, hexToU128("00000000000000000000000000000001"));
    gc1.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    // A second leader (different id) takes over and completes the round.
    Gc gc2(store, hexToU128("00000000000000000000000000000002"));
    gc2.runRegularRound();
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_NO_THROW(gc2.runRegularRound());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t8.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcResume.*' > test_t8.log 2>&1`
Expected: FAIL (resume reads fold seal at the wrong key — old single-slot signature, or the new leader uses its own seq).

- [ ] **Step 3: Implement**

In `tryResumeIncompleteRound` (1345-1414): replace the retired-set probe at line 1360 (`backend.get(layout.retiredKey(round, state.fence_seq, shard))`) with `layout.retiredKey(state.snap_generation, state.snap_attempt, round, shard)`; replace `readFoldSeal(state.snap_generation)` (1374) with `readFoldSeal(state.snap_generation, state.snap_attempt)`; replace the cleanup-bundle walk (1384-1400) to use `partManifestCleanupKey(state.snap_generation, state.snap_attempt, 0, seq)`. The re-`recheck` call already uses `state` (which carries `snap_attempt`), so it inherits correctly once Task 6 lands.

- [ ] **Step 4: Run the test**

Run: `ninja unit_tests_dbms > build_t8b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcResume.*:CasGcRound.*' > test_t8.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "CA GC resume: derive the tail from the accepted (snap_generation, snap_attempt)

tryResumeIncompleteRound reads the fold seal + cleanup bundle under the adopted
attempt; a resuming (possibly different) leader completes the accepted attempt's
tail rather than minting a new one.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 9: Pruning — generation-retention wholesale + current-gen attempt orphan sweep

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1068-1113` (`pruneSupersededGenerations`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (or a new `gtest_cas_gc_prune.cpp`)

**Interfaces:**
- Consumes: `Layout::gcGenPrefix(g)`, `gcGenAttemptPrefix(g, a)`; `Backend::list(prefix, cursor, limit)`; `GcState::snap_attempt`, `snap_generation`, `lease.seq`.
- Produces: `pruneSupersededGenerations` deletes obsolete generations wholesale by `gcGenPrefix(g)` LIST+delete (all attempts), and sweeps non-adopted attempts of the current generation with `seq < min_live_lease_seq`, never touching `snap_attempt` or generations `> snap_generation`. Bounded per round; fail-open on 404.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasGcPrune, SweepsNonAdoptedCurrentGenAttempt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    // ... drive one full round so snap_generation/snap_attempt are set ...
    const auto st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    // Plant an orphaned attempt artifact at the current generation with a lower seq.
    const uint64_t orphan_attempt = (st.snap_attempt == 0) ? 0 : st.snap_attempt - 1;
    const String orphan = store->layout().foldSealKey(st.snap_generation, orphan_attempt);
    backend->putIfAbsent(orphan, "orphan");
    Gc gc(store, kGc);
    gc.runRegularRound();   // a round should sweep the orphan (seq < min_live_lease_seq)
    EXPECT_FALSE(backend->head(orphan).exists);
    // The adopted attempt's artifact survives.
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists);
}
```

(Adjust the round-driving preamble to match the file's helpers; if `min_live_lease_seq` derivation makes the single-leader orphan ineligible, document it and assert non-deletion of `snap_attempt` plus deletion under an explicitly-below-watermark seq.)

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja unit_tests_dbms > build_t9.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcPrune.*' > test_t9.log 2>&1`
Expected: FAIL (orphan attempt survives — prune only handled attempt-0 final keys).

- [ ] **Step 3: Implement**

In `pruneSupersededGenerations` (1068-1113): (a) for `g <= prune_floor`, replace the per-key `dropExact` of `foldSealKey(g)/completionSealKey(g)/blobTargetRunKey(g,0,seq)/partManifestCleanupKey(g,0,seq)` with a wholesale LIST over `gcGenPrefix(g)` and `deleteExact` each listed key (tolerate 404 — fail-open, never throw on a benign 404 during prune, per the GC-never-throw-on-404 rule). **This wholesale `gcGenPrefix(g)` delete now also reclaims the `retired/` and `outcomes/` artifacts** (they live under `gc/gen/<g>/attempt/<a>/` after Task 3) — so there is NO separate `gc/retired/`+`gc/outcomes/` sweep, and no orphaned writer-visible retired debris (the user-review concern). (b) add a bounded current-generation attempt sweep: LIST `gcGenPrefix(snap_generation)`, parse the `attempt/<a>/` segment, and for each `a != snap_attempt` with `a < min_live_lease_seq` (a low-watermark below which no in-flight leader could still be writing — derive conservatively, e.g. from the current `lease.seq` minus an in-flight margin, documented inline), delete the attempt subtree via `gcGenAttemptPrefix(snap_generation, a)`. Never prune generations `> snap_generation`. Keep `kMaxPrunePerRound` bounding.

- [ ] **Step 4: Run the test (+ full CasGc suite)**

Run: `ninja unit_tests_dbms > build_t9b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGc*' > test_t9.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "CA GC prune: wholesale generation-retention + bounded current-gen attempt sweep

Obsolete generations are deleted by gc/gen/<g>/ prefix (all attempts); non-adopted
attempts of the current generation with seq below the in-flight watermark are swept.
snap_attempt and generations > snap_generation are never touched; fail-open on 404.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 10: Unit-level concurrent-leader regression (`gtest_cas_gc_attempt.cpp`)

**Files:**
- Create: `src/Disks/tests/gtest_cas_gc_attempt.cpp`
- (Reference) `src/Disks/tests/cas_test_helpers.h`, `CasInMemoryBackend.h` fault-injection.

**Interfaces:**
- Consumes: everything above; `InMemoryBackend::failNextCasPut(key)` to deterministically deny a leader's `gc/state` CAS (simulate a steal/loss without threads).

- [ ] **Step 1: Write the failing-then-passing regression test**

Create `src/Disks/tests/gtest_cas_gc_attempt.cpp`:

```cpp
#include "cas_test_helpers.h"
#include <Disks/.../Core/CasGc.h>          // match the include style of the sibling gtests
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
const UInt128 kGcA = hexToU128("00000000000000000000000000000001");
}

/// A leader whose fold-adopt CAS is denied (lease lost mid-fold) leaves its fold seal ONLY under its
/// own attempt; it never occupies the adopted attempt, so a subsequent honest round is not wedged and
/// drains the unreachable blob to zero. (Unit-level GC-CONCURRENT-LEADER-LEAK regression.)
TEST(CasGcAttempt, DeposedFoldAttemptDoesNotWedge)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    // Deny the next gc/state CAS => the fold-adopt fails (leader deposed mid-round).
    backend->failNextCasPut(store->layout().gcStateKey());
    Gc gc(store, kGcA);
    EXPECT_ANY_THROW(gc.runRegularRound());   // ABORTED: fold-adopt CAS denied

    // An honest round (CAS now allowed) must NOT hit a divergent/orphaned final-gen seal and must drain.
    EXPECT_NO_THROW(runGcToFixpoint(gc));
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_EQ(runFsck(*store, false).unreachable, 0u);
}
```

(If `failNextCasPut` denies the *fold* CAS specifically, confirm the fold seal it wrote is under the deposed attempt and the next round's fold uses a fresh attempt; adjust the assertion to read the planted vs adopted attempt as in Task 7. Match `runGcToFixpoint`/`runFsck`/`blobExists` to `gtest_cas_gc_leak.cpp`'s helpers.)

- [ ] **Step 2: Run test to verify it fails on a pre-fix tree / passes on the fixed tree**

Run: `ninja unit_tests_dbms > build_t10.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasGcAttempt.*' > test_t10.log 2>&1`
Expected: PASS on the post-Task-9 tree. (Sanity: temporarily reverting Task 5's attempt prefix should make it FAIL — the orphaned final-key seal wedges — confirming the test has teeth. Do not commit the revert.)

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_cas_gc_attempt.cpp
git commit -m "CA GC: unit regression for concurrent-leader leak (deposed fold attempt does not wedge)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 11: Flip S33 scenario expectation + final verification

**Files:**
- Modify: `utils/ca-soak/scenarios/cards/s28_s33_corner.py` (S33 LIVENESS verdict)
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` (mark `GC-CONCURRENT-LEADER-LEAK` fixed)
- Modify: `docs/superpowers/worklogs/2026-06-28-cas-gc-attempt-scoped-generation-worklog.md`

**Interfaces:**
- Consumes: the running server built from the fixed tree.

- [ ] **Step 1: Update S33's LIVENESS verdict**

In `s28_s33_corner.py` S33 (lines 904-922), change the LIVENESS expectation from "EXPECTED FAIL (reproduces the leak)" to "must drain `unreachable -> 0`": remove the expected-fail framing so a nonzero residual is now a real failure. Keep SAFETY (`dangling == 0`) unchanged. Update the docstring (735-754) to note the fix landed (attempt-scoped generation) and reference this plan + spec.

- [ ] **Step 2: Full unit-test gate**

Run: `ninja unit_tests_dbms > build_t11.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > test_t11.log 2>&1`
Expected: all PASS. Analyze via subagent.

- [ ] **Step 3: (If a soak host is available) run S33; else record as deferred**

Run S33 via the scenario suite per `utils/ca-soak/scenarios/` README (fresh pool). Expected: SAFETY `dangling==0` AND LIVENESS `unreachable->0`. If no soak host is available in this environment, record S33 as "to run on a soak host" in the worklog (do not fake a pass — mark inconclusive with the reason, per the suite's never-silently-skip rule).

- [ ] **Step 4: Update BACKLOG + worklog, commit**

```bash
git add utils/ca-soak/scenarios/cards/s28_s33_corner.py utils/ca-soak/scenarios/BACKLOG.md \
        docs/superpowers/worklogs/2026-06-28-cas-gc-attempt-scoped-generation-worklog.md
git commit -m "CA GC: flip S33 liveness to expect drain; mark GC-CONCURRENT-LEADER-LEAK fixed

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Self-Review

**1. Spec coverage:**
- Problem (orphaned-seal wedge + divergent run) → Tasks 4 (strict putIfAbsent), 5 (attempt-prefix fold + CAS #1). ✓
- Governing invariant (only `(snap_generation, snap_attempt)` viewable) → Task 1 (`INV_ONLY_ADOPTED_VIEWABLE`), Task 7 (readers). ✓
- `GcState.snap_attempt`, no phase field → Task 2. ✓
- Attempt namespace for **all six** gc/gen key families (incl. retired/outcomes per spec lines 102-106) → Task 3. ✓
- Round lifecycle: one attempt, CAS #1 selects / CAS #2 inherits → Tasks 5, 6. ✓
- Strict putIfAbsent by artifact class: deterministic (runs/seals) → byte-equal-or-CORRUPTED via `putDeterministicArtifact` (Task 4); observation-bearing (retired set/outcome log) → write-once/adopt-on-present, kept on their decode-existing path, NOT the strict guard (Task 6, explicit). ✓
- Readers → Task 7 (incl. the B2 back-scan elimination, `RetireView` writer-facing resolution, `CasOrphanManifestSweep`). Content deletes (exact-token, idempotent, unchanged) → untouched (documented). ✓
- Recheck ordering & outcome coverage → Task 6 (CAS #2 gated on outcome coverage). ✓
- Resume derives from durable artifacts (incl. the attempt-scoped retired-set probe) → Task 8. ✓
- Pruning (generation-retention wholesale — now also reclaims retired/outcomes — + current-gen attempt sweep, seq<min_live_lease_seq, never `>snap_generation`) → Task 9. ✓
- Distributed sharded reducer boundary → covered by the artifact-class rule (Task 4 deterministic + retired/outcomes adopt-on-present) which holds for any producer; no scheduler built (out of scope per spec). ✓
- TLA+ Gate A (`INV_ONLY_ADOPTED_VIEWABLE` over seal **and** retired-set visibility + R0 + `SabotageDeposedLeaderWritesFinalGen` + bounded-B witnesses + inertness; fresh monotonic attempt id) → Task 1. ✓
- Testing (TLA+ gate, gtest attempt-scoped fold→adopt→resume + strict putIfAbsent + deposed invisible + RetireView ignores non-adopted retired set, S33 drains) → Tasks 1, 4, 5, 6, 7, 8, 10, 11. ✓

**2. Placeholder scan:** Every code step shows real code; every TLA+ step gives concrete invariant text + run command + expected verdict + the inertness numbers (19,846 / 402,034). The one irreducible TLA+ discretion (`SealViewable`/`GDeposedWriteSeal` shape) is bounded by an objective acceptance test (Step 6 must produce a counterexample; Step 5 must be green) — not a "TODO".

**3. Type/name consistency:** `snap_attempt` (`uint64_t`) consistent Tasks 2–9. `foldSealKey(gen, attempt)`, `completionSealKey(gen, attempt)`, `blobTargetRunKey(gen, attempt, shard, seq)`, `partManifestCleanupKey(gen, attempt, owner_shard, seq)`, `gcGenPrefix(gen)`, `gcGenAttemptPrefix(gen, attempt)` consistent Tasks 3–9. `putDeterministicArtifact(backend, key, bytes)` consistent Tasks 4–6. `attempt = state.lease.seq` at fold; `attempt = state.snap_attempt` at recheck/resume/readers — consistent.

**Alignment with spec (no deviations):** an earlier draft attempt-scoped only the four `gc/gen` keys and left `retired`/`outcomes` flat. User review rejected that as unsafe — `retired` is **writer-facing** (`RetireView::refresh()` LISTs the retired namespace; writers consult `isCondemnedToken` on the publish gate, `CasBuild.cpp:235,414,666,725`), so a deposed leader's stale retired set under its own epoch would influence live writers. The plan now scopes **all six** artifact families under the adopted attempt (spec lines 102-106), `RetireView` resolves via the accepted `(snap_generation, snap_attempt)`, and the wholesale `gc/gen/<g>/` prune reclaims them (no separate sweep). **Design-gap fix (B2):** a single `snap_attempt` is unsound for blind multi-generation back-scans; verified the back-scans only ever resolve at `snap_generation` (or `G_f` mid-round, same attempt) in reachable states, so Task 7 replaces the blind scan with a direct adopted-attempt read + fail-closed-empty — keeping the spec's single-`snap_attempt` schema, no map. **TLA+ correction:** attempt modeled as a fresh monotonic id (not leader identity), and the proof covers retired-set visibility (the RetireView consumer), not only the GC-internal seal.

## Execution Handoff

This plan is executed via **superpowers:subagent-driven-development** (per the user's instruction): fresh implementer subagent per task, spec-compliance review then code-quality review between tasks, TDD throughout. **Task 1 (TLA+ Gate A) MUST be green before any code task.**
