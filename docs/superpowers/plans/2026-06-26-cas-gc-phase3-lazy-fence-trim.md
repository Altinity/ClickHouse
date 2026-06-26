---
description: "Lazy trim below sealed generation coverage for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 3 (lazy trim)"
sidebar_position: 8
slug: /superpowers/plans/2026-06-26-cas-gc-phase3-lazy-fence-trim
title: "Phase 3 — Lazy Trim — Implementation Plan"
doc_type: reference
---

# Phase 3 — Lazy Trim — Implementation Plan {#phase-3-lazy-trim-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax — each is one 2–5 minute action and every task ends with a commit. Read `2026-06-26-cas-gc-redesign-overview.md` first, then `2026-06-26-cas-gc-phase0-tla-model.md` (the model STYLE TEMPLATE and how to re-green the suite). **No code task in this phase may begin until Task 1's model extension is GREEN** — that is the R0 gate for Phase 3.

**Goal:** Make the journal trim **lazy**: trim a root journal only **below the sealed-generation cursor coverage** (the `INV_JOURNAL_COVERAGE` realization). Lazy *fence* is explicitly **dropped** — the all-shard fence from Phase 1d is unchanged, and every shard in the fence universe is still fenced every round. Task 1 adds a **permanent negative control** that *proves why* a lazy fence would be unsafe (reusing a prior fence for a shard that received a publish between discovery and recheck dangles), so the model documents the decision rather than implementing it.

**Architecture:** `Gc::trim` (`CasGc.cpp:168`) erases journal records `at_version <= cursor` for every shard. Today `cursor` comes from a transient in-memory snap cursor; Phase 3 re-sources it from the **sealed** per-`(ns,shard)` coverage (`GenerationSeal.per_ns_shard.ShardCoverage`) so trim runs only below what a durable generation actually incorporated — never above it (`INV_JOURNAL_COVERAGE`). `Gc::fence` (`CasGc.cpp:819`) is **untouched**: it still fences the registry then **every** root shard of every namespace in the fence-time registry, recording `state.fence_version[round][cursorKey(ns, shard)]`. A real lazy fence is deferred (see [Deferred / Out of scope](#deferred--out-of-scope)) because it needs a proven atomic writer-visible fence authority that does not yet exist; until then GC fences every shard.

**Tech stack:** TLA+ / TLC (`tla2tools.jar` at `tmp/tla2tools.jar`, OpenJDK 21) for Task 1; C++ (ClickHouse coding standards, Allman braces) + gtest for Tasks 2–3; `ci.praktika` for the Phase exit sweep.

**Source spec:** `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` (rev. 13), §Global Fence, §Trim, §Phase Plan/Phase 3. The governing rule: *every shard in the fence universe is fenced every round (lazy fence is NOT implemented); trim only below sealed generation coverage.*

---

## Global Constraints {#global-constraints}

*Every task below implicitly includes this section (repeated verbatim from `2026-06-26-cas-gc-redesign-overview.md` so an implementer who sees only one task still has it).*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in this phase may begin until Task 1's TLA+ gate is green.
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Default `gc_shards = 1`; sharded mode is optional (Phase 4) and not touched here.
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, and trimmed — including, in this phase, **the sealed coverage cursor each shard's journal was trimmed below**.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden.

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object;
- GC must never throw/fail-closed on a 404 during fold (record what you can and continue).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (e.g. `build`, `build_debug`, `build_asan`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'`.

**TLA+ run mechanics** (exact; from `docs/superpowers/models/`)
- Run one config:
  ```bash
  cd docs/superpowers/models
  java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config <Cfg>.cfg CaGcRootLocalPartManifestCore.tla
  ```
  (For a long run set `TLC_JAVA_OPTS=-Xmx48g`.) The Phase-0 wrapper `run_gc_partmanifest.sh` already hardcodes this module; use it: `./run_gc_partmanifest.sh <cfg-basename>`.
- **PASS (HOLD config)** = exit 0 and the log contains `Model checking completed. No error has been found.`
- A **negative-control / `_sab_*`** config is correct **only when it FAILS** with `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A zero exit on a `_sab_*` config is a **suite failure** (`UNEXPECTED PASS`).

---

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- **Why lazy fence is NOT implemented (the Phase-3 decision).** A reused fence would be safe only if a skipped shard's root token were provably unchanged across the entire window the round depends on — from the prior generation's recorded fence position through this round's recheck cut. The two no-dangle horns from `Gc::fence` (`CasGc.cpp:819`, comment block at horns 1/2) both rest on a publish being *totally ordered* against a fence on the shard. Token-diff classification (Phase 2) happens at **discovery**; a publish can land between discovery and the recheck cut, and that publish would **not be writer-visible** without a fresh fence — so reusing the prior fence for that shard dangles a still-referenced manifest. Closing this would require a *proven atomic writer-visible fence authority* (a writer must be guaranteed to observe the new round / retired view before it can publish into a skipped shard), which is not modeled or proven. Until it is, GC fences **every** shard in the fence universe. Task 1 encodes this decision as a permanent negative control (`SabotageLazyFenceUnsafe`) that proves the unsafe reuse dangles `INV_NO_DANGLE`.
- **Trim coverage (spec §Trim, `INV_JOURNAL_COVERAGE`).** Trim removes journal records only below the **sealed** per-`(ns,shard)` cursor coverage carried in `GenerationSeal` (the canonical `per_ns_shard.ShardCoverage` produced by Phase 1d, persisted by Phase 2). Every shard is fenced every round, but trim is still gated on the sealed coverage cursor — never above it. This is the existing `at_version <= cursor` rule (`CasGc.cpp:168`) re-sourced from the sealed coverage rather than from a transient in-memory snap cursor.

---

## Deferred / Out of scope {#deferred--out-of-scope}

**Lazy fence is deferred — not implemented in this phase or by this plan.** A real lazy fence (reusing a prior round's fence for a token-unchanged shard instead of re-fencing it) requires a **proven atomic writer-visible fence authority**: a writer must be *guaranteed* to observe the new round / retired view before it can publish into a shard GC chose to skip. Without that primitive the ordering argument breaks — token-diff classification happens at **discovery**, but a publish can land between discovery and the recheck cut, and that publish would not be writer-visible to the skipped fence, so the recheck would never fold it and a still-referenced manifest would dangle (`INV_NO_DANGLE`). Until such a primitive is modeled and proven, **GC fences every shard in the fence universe every round** (the Phase 1d all-shard fence, unchanged). Task 1's permanent `SabotageLazyFenceUnsafe` control is the model's standing proof of this hazard. If a writer-visible fence authority is later designed and TLA+-proven, lazy fence becomes a follow-up phase.

---

## Canonical Contract (consumed in this phase) {#canonical-contract-consumed-in-this-phase}

Phase 3 consumes the contract that Phases 1d and 2 produce and **adds no new field or type**:

- `GenerationSeal` carries `fence_positions` (the per-`(ns,shard)` fence version recorded for the generation) and `per_ns_shard` entries of type `ShardCoverage` (classification, folded token, sealed cursor). These are produced by Phase 1d's `CasGenerationSeal` and persisted across rounds by Phase 2.
- `CasGc` owns `fence` / `recheck` / `trim` (the round tail). Phase 3 changes only `trim`; `fence` and `recheck` are untouched.
- Phase 2 persists the **folded tokens** + cursors and the sealed coverage cursor that `trim` reads.

Phase 3 introduces **no** contract extension: lazy fence is dropped, so there is no reuse-justification field. Only these contract type names appear in code: `GenerationSeal`, `ShardCoverage`, `CasGc`.

---

## File Structure {#file-structure}

*Paths under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/` unless noted. Exact current line anchors: `Gc::trim` at `CasGc.cpp:168` (the only code touched); `Gc::fence` at `CasGc.cpp:819` and `GcState.fence_version` at `CasGcFormats.h:53` are referenced as UNCHANGED context.*

- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — add the `EnableLazyTrim` positive stage machinery + the permanent `SabotageLazyFenceUnsafe` negative control (the latter proves WHY lazy fence is not implemented).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_lazytrim.cfg` — positive lazy-trim stage (must HOLD).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_lazyfenceunsafe.cfg` — negative control (must FAIL with `INV_NO_DANGLE`).
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` — append the two new rows to the green-suite ledger.
- Modify: `CasGc.cpp` / `CasGc.h` — `trim` is sourced from sealed coverage. `fence` and `recheck` are NOT modified.
- Create: gtest source for `CasGcRound` (the lazy-trim test; exact gtest target/file location confirmed from the C++ ground-truth report at task time; follow the existing CA gtest file the Phase 1d/2 tests were added to).

---

## Tasks {#tasks}

### Task 1: Model extension — `EnableLazyTrim` positive stage + permanent `SabotageLazyFenceUnsafe` negative control (proves WHY lazy fence is dropped) {#task-1-model-extension-enablelazytrim-positive-stage-permanent-sabotagelazyfenceunsafe-negative-control-proves-why-lazy-fence-is-dropped}

**This is the Phase 3 R0 gate. No code task starts until both configs below are correct.**

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_lazytrim.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_lazyfenceunsafe.cfg`
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`

**Interfaces produced:** `EnableLazyTrim` (positive flag gating the lazy-trim stage), `SabotageLazyFenceUnsafe` (permanent negative control); `shardUnchanged` helper and an *unsafe* reused-fence branch in `GFenceShard` reachable ONLY under the sabotage flag; the existing `INV_NO_DANGLE` proved still holding under lazy trim, and proved VIOLATED when a fence is reused for a shard that received a publish between discovery and recheck.

- [ ] **Step 1: Add the two new constants.** In `CaGcRootLocalPartManifestCore.tla`, extend the `CONSTANTS` block: add `EnableLazyTrim` to the `Enable*` group and `SabotageLazyFenceUnsafe` to the `Sabotage*` group. Both are FALSE in every positive stage and in every pre-existing `_sab_*` cfg (add the line `SabotageLazyFenceUnsafe = FALSE` / `EnableLazyTrim = FALSE` to those cfgs only if TLC reports a missing-constant error — keep edits minimal).

- [ ] **Step 2: Add a per-shard fold-token witness so "unchanged" is expressible (for the sabotage).** The model already carries `cursor[n]` (folded position) and `fencePos[n]` (recorded fence position) per namespace. Add a tiny abstraction of Phase 2's persisted folded token: a variable `foldTok` recording, per namespace, the token the last fence was recorded against, plus `prevFencePos` (the parent generation's fence position a sabotaged reuse would copy). Mirror the existing `VARIABLES` / `vars` / `Init` / `TypeOK` edit idiom in the module (follow how `fencePos`, `cursor`, `fenceVersion` are declared and initialized). These exist ONLY so the negative control can express an unsafe reuse — the production code never reuses a fence.

```tla
\* foldTok[n] is the abstract persisted folded token a fence was recorded against (Phase 2's token);
\* prevFencePos[n] is the parent generation's fence position. A publish into n changes its token
\* (modeled by bumping foldTok on a publish action), which the discovery-time classification would
\* mark as changed. The sabotage REUSES prevFencePos[n] for such a changed shard, which dangles.
shardUnchanged(n) == foldTok'[n] = foldTok[n]
```

- [ ] **Step 3: Add the UNSAFE reused-fence branch to `GFenceShard`, reachable only under the sabotage.** Keep the existing fresh-fence behavior (every shard fenced fresh) as the ONLY non-sabotage behavior — Phase 3 does NOT add a production lazy-fence arm. Add a branch gated on `SabotageLazyFenceUnsafe` that reuses `prevFencePos[n]` for a shard whose token advanced (a publish landed between discovery and the recheck cut), modeling the lazy fence we deliberately do not implement.

```tla
\* SabotageLazyFenceUnsafe demonstrates WHY a lazy fence is not implemented: it reuses a stale parent
\* fence position for a shard that received a publish between discovery and recheck. That publish is
\* not writer-visible without a fresh fence, so it lands below the (never re-advanced) fence, the
\* recheck never folds it, and a still-referenced manifest dangles => INV_NO_DANGLE breaks. There is no
\* positive (HOLD) lazy-fence branch: closing this horn needs a proven atomic writer-visible fence
\* authority that does not exist (see the plan's Deferred / Out of scope note).
UnsafeReuseFence(n) == SabotageLazyFenceUnsafe /\ ~shardUnchanged(n)
```
Write the branch inside `GFenceShard` following the module's existing action style: the default arm is the Phase-0 fresh fence (advance `fencePos[n]` to the round) and remains the only behavior with all sabotage off; the `UnsafeReuseFence(n)` arm sets `fencePos[n] = prevFencePos[n]` without advancing it. Ensure `foldTok` is bumped by the publish action(s) (`WPublishCommitted` / `WRepoint`) so a publish makes `~shardUnchanged(n)` true and the sabotage can fire on it.

- [ ] **Step 4: Re-confirm `INV_NO_DANGLE` already references the fold-through-fence cut.** No new invariant is needed — Phase 3 proves the *existing* `INV_NO_DANGLE` / `NoCommittedDangle` holds under lazy trim, and is VIOLATED by the unsafe reused fence. Confirm `GRecheckDelete` deletes only when folded through `fencePos[n]`, so the sabotaged stale `fencePos[n]` genuinely exposes the dangle.

- [ ] **Step 5: Write `CaGcRootLocalPartManifestCore_stage5_lazytrim.cfg`** — copy `CaGcRootLocalPartManifestCore_stage3.cfg`, then: set `EnableLazyTrim = TRUE`, keep `SabotageLazyFenceUnsafe = FALSE` and every other `Sabotage* = FALSE`; keep the safety invariants including `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, `BlobInDegreeMatchesActiveManifests`. The positive lazy-trim stage must HOLD (the all-shard fence is unchanged; only trim is gated on sealed coverage). Widen `Namespaces = {n1, n2}` and `MaxRound = 2` so a shard's journal accumulates records both at and above its sealed coverage cursor.

- [ ] **Step 6: Run stage5 — must HOLD.**

Run: `cd docs/superpowers/models && TLC_JAVA_OPTS=-Xmx16g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage5_lazytrim`
Expected: `Model checking completed. No error has been found.` and `exit=0`. If it finds a trace (all sabotage off), the model is wrong — lazy trim must only remove records below the sealed coverage cursor; fix the trim arm until it holds.

- [ ] **Step 7: Write `CaGcRootLocalPartManifestCore_sab_lazyfenceunsafe.cfg`** — copy `stage5_lazytrim.cfg`, set `SabotageLazyFenceUnsafe = TRUE`, and narrow to the single targeted line `INVARIANT INV_NO_DANGLE` (drop the other invariant lines to speed the counterexample, per the `CaIncarnationCore` convention).

- [ ] **Step 8: Run the negative control — must FAIL with `INV_NO_DANGLE`.**

Run: `TLC_JAVA_OPTS=-Xmx16g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_lazyfenceunsafe`
Expected: `Error: Invariant INV_NO_DANGLE is violated.` and a **nonzero** `exit=`. A zero exit is an `UNEXPECTED PASS` and a gate failure — fix the sabotage branch so a changed shard genuinely reuses a stale fence and a racing publish dangles. This control is **permanent**: it is the model's record of why lazy fence is not implemented.

- [ ] **Step 9: Regression — re-run stages 0–4 + the existing `_sab_*` suite.** The two new constants must not perturb the Phase-0 suite. Run every pre-existing stage/live/witness (must HOLD) and every pre-existing `_sab_*` (must FAIL as before). Use the analyze-via-subagent rule for any long log.

```bash
cd docs/superpowers/models
for s in stage0 stage1 stage2 stage3 stage4 live ; do ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_$s ; done
```

- [ ] **Step 10: Append the ledger rows.** In `CaGcRootLocalPartManifestCore_RESULTS.md` add two rows: `stage5_lazytrim` → HOLD; `sab_lazyfenceunsafe` → VIOLATED `INV_NO_DANGLE` (permanent negative control documenting why lazy fence is dropped). Record `states generated`, `distinct states`, and wall time for each. Mark the Phase-3 gate **GREEN** iff stage5 HOLDs and the sabotage VIOLATES with no `UNEXPECTED PASS`.

- [ ] **Step 11: Commit**

```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5_lazytrim.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_lazyfenceunsafe.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md
git commit -m "CA GC phase3: model — lazy-trim stage5 + permanent SabotageLazyFenceUnsafe negative control (gate green)"
```

---

### Task 2: Lazy trim — only below sealed generation coverage {#task-2-lazy-trim-only-below-sealed-generation-coverage}

**Gate:** Task 1 GREEN.

**Files:**
- Modify: `CasGc.cpp` (`Gc::trim`, anchor `:168`) / `CasGc.h`
- Create/Modify: the CA gtest source carrying `CasGcRound`

*Note: `Gc::fence` and `Gc::recheck` are NOT modified by Phase 3 — the all-shard fence from Phase 1d stays exactly as-is. The only behavior change is `trim`'s cursor source.*

- [ ] **Step 1 (TEST FIRST): write a failing `CasGcRound` gtest — no trim above coverage.** Add `CasGcRound.TrimOnlyBelowSealedCoverage`: build a shard whose journal has records both at and above the sealed `ShardCoverage` cursor; run `trim`; assert records `at_version <= sealed_coverage_cursor` are removed and records **above** it are retained (realizes `INV_JOURNAL_COVERAGE`). Add a second assertion: a shard whose coverage cursor was carried forward across rounds still trims correctly below that carried-forward cursor and never above it. Run — confirm it FAILS if `trim` reads a cursor source other than the sealed `GenerationSeal` coverage.

- [ ] **Step 2: source trim from sealed coverage.** In `Gc::trim` (`:168`): keep the `std::erase_if(fresh.journal, at_version <= cursor)` shape, but take `cursor` from the sealed per-`(ns,shard)` `GenerationSeal.ShardCoverage` (the canonical coverage Phase 1d produces / Phase 2 persists) instead of a transient in-memory snap cursor. Keep the peek-before-mutate no-op skip and the `GcTrim` event emit (record the coverage cursor in the event detail). The invariant comment stays: only records provably incorporated into the sealed generation may go. Build (redirect, subagent-analyze).

- [ ] **Step 3: run `CasGcRound` — must PASS.** Redirect to `<build_dir>/test_casgcround.log`, analyze with a subagent.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        <the CasGcRound gtest source>
git commit -m "CA GC phase3: lazy trim — only below sealed generation coverage (INV_JOURNAL_COVERAGE)"
```

---

### Task 3: Build + full `Cas*:Ca*` gtest sweep + Phase exit {#task-3-build-full-cas-ca-gtest-sweep-phase-exit}

**Gate:** Task 2 committed.

**Files:** none new (verification + ledger commit).

- [ ] **Step 1: clean build.** Build the chosen `build_*` directory; redirect ninja output to `<build_dir>/build.log`; **analyze the log with a subagent** and return only a concise summary. Fix any break before proceeding.

- [ ] **Step 2: full CA gtest sweep.** Run the gtest binary with `--gtest_filter='Cas*:Ca*'`, redirect to `<build_dir>/test_cas_sweep.log`; **analyze with a subagent**. The sweep — including the new `CasGcRound` lazy-trim test and every pre-existing Phase 1d/2 CA test (the unchanged `CasGcFence`/`CasGcRecheck` suites from Phase 1d still pass — Phase 3 does not touch `fence`/`recheck`) — must be green. No `no-*` tags were added.

- [ ] **Step 3: confirm the gate ledger is current.** Re-confirm `CaGcRootLocalPartManifestCore_RESULTS.md` shows `stage5_lazytrim` HOLD and `sab_lazyfenceunsafe` VIOLATED `INV_NO_DANGLE` with no `UNEXPECTED PASS` (the Task 1 gate is the safety authority for everything in this phase).

- [ ] **Step 4: Commit the phase-exit marker.** If the sweep produced any updated ledger/notes, commit them.

```bash
git commit -am "CA GC phase3: full Cas*:Ca* gtest sweep green — lazy trim complete"
```

---

## Self-Review {#self-review}

- **Spec coverage:** (a) lazy fence is **dropped** — Task 1's permanent `SabotageLazyFenceUnsafe` proves WHY (reusing a fence for a shard that received a publish between discovery and recheck VIOLATES `INV_NO_DANGLE`), and the [Deferred / Out of scope](#deferred--out-of-scope) note records the prerequisite (a proven atomic writer-visible fence authority); (b) every shard in the fence universe is still fenced every round (the Phase 1d all-shard fence, untouched); (c) trim only below sealed generation coverage = Task 2. §Global Fence (registry + all-shard fence stays as Phase 1d left it) and §Trim (`INV_JOURNAL_COVERAGE`) are both honored. ✓
- **Gate-first / TDD:** Task 1 (model) is the R0 gate and lands before any code; Task 2 writes the failing gtest *before* the implementation and ends with the test green; each task ends with a commit. The positive `EnableLazyTrim` stage HOLDs and the negative `SabotageLazyFenceUnsafe` VIOLATES `INV_NO_DANGLE`; the one new gtest is `CasGcRound.TrimOnlyBelowSealedCoverage` (`INV_JOURNAL_COVERAGE`). The Phase 1d `CasGcFence`/`CasGcRecheck` suites are unchanged and still pass (Phase 3 does not touch `fence`/`recheck`). ✓
- **Canonical contract only:** the only contract type names used are `GenerationSeal` (+ its `fence_positions` and `per_ns_shard.ShardCoverage`) and `CasGc` `trim`; Phase 3 adds **no** new field or type (lazy fence dropped ⇒ no reuse-justification field). No parallel type is invented. ✓
- **No placeholders / exact anchors:** the model edit shows the `shardUnchanged` helper and the sabotage-only `UnsafeReuseFence` arm in full; the only code anchor touched is `Gc::trim` `CasGc.cpp:168` (`Gc::fence` `CasGc.cpp:819` / `GcState.fence_version` `CasGcFormats.h:53` are cited as UNCHANGED context); the cfg files are fully specified as copy-of-stage3 + the single flag + the single invariant. The one deferred concrete is the gtest *source file path*, resolved from the C++ ground-truth report at task time — a lookup, not a vague instruction. ✓
- **Depends on Phase 2** (persisted sealed coverage cursor that `trim` reads) and **gated on Task 1** (Phase-3 model extension green). Allman braces and the prose backtick/`f`/ASan rules apply throughout. ✓
