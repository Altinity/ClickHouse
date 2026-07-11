# Add-only GC freshness meta (deposed-leader clearSparedMeta fix) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Close the deposed-leader stray-Clean `clearSparedMeta` data-loss hole by making GC freshness
metadata ADD-ONLY: GC never transitions `Condemned → Clean` on a spare; only a writer that displaced the
body with a fresh incarnation token publishes `Clean`.

**Architecture:** Delete the spare-side meta clear (and its helper). Keep the pre-CAS `writeCondemnedMeta`
(add-only, safe) and `deleteConfirmedMeta` (token already consumed). The writer's existing resurrect path
(`uploadFromSource` + `writeResurrectMetaClean` / `copyForwardFromCondemned`) is the ONLY `→ Clean`
transition. Spec: `docs/superpowers/specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md`.

**Tech Stack:** C++ (CAS GC), TLA+/TLC (witness gate), gtest.

## Global Constraints

- Allman braces (opening brace on its own line). Never use `sleep` to fix races. CA is pre-release → no
  compat scaffolding (no meta-format bump; `BlobMeta.condemn_round` already exists). Wrap SQL/class/
  function names in `code` in prose/comments. "exception" not "crash". GC must never throw on a
  data-plane 404.
- Commit trailers on every commit:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.
- Branch `cas-gc-rebuild`; new commits only (no amend/rebase); never commit to master.
- **Invariant being restored:** once a hash is `Condemned`, observing `Clean` ⟹ the condemned body is
  absent OR a writer already changed its incarnation token → any stale exact-token redelete finds the
  body absent or `TokenMismatch`.

---

### Task 1: TLA+ gate — add-only meta + split-action two-leader model {#task-1}

**Files:** Modify `docs/superpowers/models/CaRetiredInRunFoldAbortWitness.tla` + `.cfg` (+ sabotage
`.cfg`s), runner `docs/superpowers/models/run_foldabort_witness.sh`. This task GATES the code: prove the
add-only shape green and the clear-on-spare shapes red BEFORE removing the code.

**Interfaces produced:** the gate results the code tasks rely on (add-only = safe; any clear-on-spare =
unsafe).

**Behavioral requirements (spec §5):**
- Both `FoldRound` (adopting) and `FoldAbort` (deposed) treat GC meta writes as **add-only**: a condemn/
  pending result may set `meta[b]="cond"`; a **spare leaves `meta[b]` unchanged**; a successful exact-
  token deletion may model the meta absent/clean (body already absent); ONLY a writer fresh-upload/
  resurrection changes a present-`cond` hash to `clean` (together with a token change — `WriterStaleReuse`
  stays the only clean-read-reuse path).
- **Strengthen** against a false-green: model two bounded in-flight leaders with **split actions** —
  (capture snapshot) → (execute pre-CAS side effects incl. the exact-token `deleteExact`) → (attempt
  adoption). This exposes the ordering where a winner adopts a spare before an older leader executes its
  stale redelete.
- Add sabotage `Sabotage="gc_clear_on_spare"` (a spare sets `meta[b]="clean"` — the old behavior) and
  `Sabotage="post_adoption_clear"` (a post-CAS clear on the adopting branch — models Fix 1); extend the
  `ASSUME Sabotage \in {...}` set and the runner to cover them.

- [ ] **Step 1:** Change `FoldRound`/`FoldAbort` meta writes to add-only (spare leaves `meta[b]`
  unchanged). Add the split-action two-leader structure (capture/side-effects/adopt) sufficient to reach
  the §2 interleaving; if TLC cannot reach it in the bounded config, bump `MaxRound`/`MaxToken` by one and
  record the bound in the model header (do NOT weaken an invariant).
- [ ] **Step 2:** Add `gc_clear_on_spare` and `post_adoption_clear` to the `Sabotage` set + guards + new
  `.cfg`s + runner entries.
- [ ] **Step 3: Run the gate.** `docs/superpowers/models/run_foldabort_witness.sh <cfg>` for each config.
  Expected: honest config **GREEN** with `INV_NO_LOSS`/`INV_NO_RETURN`/`INV_COVERAGE`/`INV_ONE_PASS` all
  enabled; `inmem_token`, `attempt_reuse`, `no_pacing`, **`gc_clear_on_spare`, `post_adoption_clear`** all
  **RED**. (post_adoption_clear RED is the load-bearing proof that Fix 1 is unsafe and Fix 4 is required.)
- [ ] **Step 4: Commit** (model + cfgs + runner + report-note).

### Task 2: Invert the spare-meta unit test (add-only) {#task-2}

**Files:** Modify `src/Disks/tests/gtest_cas_gc_ack_floor.cpp` (`TEST(CasGcRetire, SpareClearsMeta)` at
`:145`). **Test:** the same file.

**Behavioral requirement:** an adopted spare (recovered in-degree) must leave the meta **`Condemned`**
(NOT clear it); a subsequent writer dedup-attempt resurrects (fresh token) and only THEN publishes
`Clean`.

- [ ] **Step 1:** Rename/rewrite `SpareClearsMeta` → `SpareLeavesMetaCondemned`: drive condemn → +1
  recovery → `runRegularRound` (spare); assert `readBlobMeta(hash).state == Condemned` (was: `Clean`).
  Add a follow-up: a `Build::putBlob` dedup-attempt on the condemned hash resurrects (new token via
  `uploadFromSource`) and `writeResurrectMetaClean` flips meta to `Clean` with a token change; assert the
  body token changed and meta is now `Clean`.
- [ ] **Step 2: Build + run — verify RED** on the current (clear-on-spare) code:
  `ninja -C build unit_tests_dbms > build/build_1a_t2.log 2>&1` then
  `build/src/unit_tests_dbms --gtest_filter='CasGcRetire.SpareLeavesMetaCondemned'`. Expected: FAIL
  (current code clears to Clean). (Use a subagent to analyze the build log.)
- [ ] **Step 3: Commit** the failing test (or hold to combine with Task 4's green — controller's choice).

### Task 3: Two-leader stale-redelete regression test {#task-3}

**Files:** add a test to `src/Disks/tests/gtest_cas_gc_ack_floor.cpp` (or the concurrent-leader test file
if one fits better — `grep -l "InterruptRoundCasBackend\|deposed\|concurrent" src/Disks/tests/`).

**Behavioral requirement (the executable form of spec §2):** a stale leader's pre-CAS `deleteExact(t1)`
must not delete a live reuse.

- [ ] **Step 1:** Construct: condemn `h` at token `t1` with a `delete_pending(t1)` adopted row; arrange an
  old leader that has PLANNED its pre-CAS `deleteExact(h,t1)` but not executed (an interrupt/injection
  backend, as `InterruptRoundCasBackend` does for the CAS); a second leader adopts a SPARE for `h` (in-
  degree recovered). Assert `readBlobMeta(h).state == Condemned` after the spare. Then a writer resurrects
  `h` to `t2` (`putBlob` → `uploadFromSource`). Resume the old leader's `deleteExact(h,t1)`; assert it
  returns `TokenMismatch`/`Replaced` (NOT `Deleted`), the body under `t2` is present, and `runFsck`
  reports `dangling==0` / no data loss.
- [ ] **Step 2: Build + run — RED** on current code (with clear-on-spare, the writer would read Clean and
  reuse `t1`, so the deleteExact deletes the live body → the test's dangling/present assertions fail).
- [ ] **Step 3: Commit** (or combine with Task 4).

### Task 4: Remove the spare-side clear (the fix) {#task-4}

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — delete
the spare-branch `scheduleMetaJob([...]{ clearSparedMeta(...); })` (~`:417`) and the now-unused
`clearSparedMeta` helper (~`:106`). Keep the pre-CAS `writeCondemnedMeta` (fresh + `ReplacedEntry`) and
`deleteConfirmedMeta` exactly as today.

- [ ] **Step 1:** Remove the spare-branch `scheduleMetaJob(... clearSparedMeta ...)` call; delete the
  `clearSparedMeta` function definition and its forward declaration/usages (compiler confirms it is
  otherwise unused). Update the fold's meta-ops comment block to state GC meta is **add-only** — GC never
  publishes `Clean` on a spare; the writer's resurrect path is the sole `→ Clean` — citing
  `reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`.
- [ ] **Step 2: Build + run — GREEN.**
  `ninja -C build unit_tests_dbms > build/build_1a_t4.log 2>&1` (subagent analyzes the log), then
  `build/src/unit_tests_dbms --gtest_filter='CasGc*:CasGcRetire*:CasBlobMeta*:CasBuild*' > build/test_1a_t4.log 2>&1`.
  Expected: Tasks 2–3 tests now PASS; no regression in the Cas* battery.
- [ ] **Step 3: Commit** the fix (with Tasks 2–3 tests if not already committed).

### Task 5: Closeout — report + ROADMAP {#task-5}

**Files:** `docs/superpowers/reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`,
`docs/superpowers/cas/ROADMAP.md`.

- [ ] **Step 1:** Flip the report **Status → FIXED** with: the chosen fix (add-only), the "Fix 1
  insufficient — stale pre-CAS `deleteExact` after adopted spare" addendum (spec §2), and the TLA gate
  results (honest green; `gc_clear_on_spare` + `post_adoption_clear` red).
- [ ] **Step 2:** ROADMAP: the `Deposed-leader stray-Clean … TODO (HARD)` row → **DONE (2026-07-11)** with
  the fix summary + gate results.
- [ ] **Step 3: Commit.**

### Task 6: Validation {#task-6}

**Files:** none (validation only).

- [ ] **Step 1:** Server build `ninja -C build clickhouse > build/build_1a_server.log 2>&1` (subagent
  analyzes; refresh `ci/tmp/clickhouse` symlink if stale).
- [ ] **Step 2:** CA-s3 stateless lane point-run (04286/05008/05009) — `05008` unmodified must pass.
- [ ] **Step 3:** 20-min soak (`utils/ca-soak` — reuse `tmp/soak_20min.sh`); expect `SOAK_20MIN OK`,
  `dangling=0`, `dryrun_subset=ok` at every checkpoint.
- [ ] **Step 4:** S33-class concurrent-leader scenario re-run (`scenarios.run --scenario S33`) — PASS.
- [ ] **Step 5:** Record results in the worklog; the RED witness is now GREEN.

## Self-Review

- Spec coverage: §3 fix → T4; §2 (fix-1-insufficient) → T3 (test) + T1 (`post_adoption_clear` sabotage);
  §5 TLA → T1; §6 units map T1–T6; §8 validation → T6. Covered.
- Type consistency: `readBlobMeta`/`writeResurrectMetaClean`/`uploadFromSource`/`deleteExact` are existing
  APIs (implementers verify exact signatures in `CasBlobMeta.*`/`CasBuild.*`/`CasBackend.*`).
- No placeholders: each task names exact files + anchors + acceptance (gate results / test names).
- Execution order: T1 gate FIRST, then T2/T3 (RED), then T4 (GREEN), then T5/T6. The TLA task (T1) is
  subtle — controller does it (or a strong subagent); the code removal (T4) is small; tests (T2/T3) are
  moderate.
