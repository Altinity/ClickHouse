---
description: 'Phase-3 implementation plan for the CAS semantic wire-keys design: prove the cut — run the lanes the unit gate cannot see, measure what the design claims, and close revision 14'
sidebar_label: 'CAS wire keys phase 3'
sidebar_position: 22
slug: /superpowers/plans/cas-wire-keys-phase3-proof
title: 'CAS wire keys — phase 3 (proof and measurement)'
doc_type: 'guide'
---

# CAS Wire Keys Phase 3: Proof and Measurement Implementation Plan {#cas-wire-keys-phase3-proof}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish that the wire cut is correct where the unit gate cannot see, that it costs what the design says it costs, and that nothing in revision 14 remains merely asserted — so the design can be declared accepted rather than implemented.

**Architecture:** Phases 1 and 2 are landed. Phase 1 bound every codec to named carriers; phase 2 changed the vocabulary, reset the generation history, and pinned the byte deltas and closed sets. Everything phase 2 could prove with a unit gate is proven. What remains is of three kinds, and the tasks below are grouped by which: **execution** of the lanes that never run in the unit gate (integration, soak, stateless — phase 2 re-spelled their assertions but never ran them), **measurement** of the claims the design makes about cost, and **settlement** of the questions the phase-2 reviews raised but deliberately did not answer.

**Tech Stack:** C++ (ClickHouse tree), gtest, Google Benchmark (`benchmark_cas_ref_protocol`), pytest integration lanes, `utils/ca-soak` docker-compose scenarios, stateless `.sh`/`.sql` tests.

**Spec:** docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md (revision 14). Its "Implementation shape" item 3, the "Decompressed-byte accounting" paragraph, and the acceptance-criteria list are what this plan discharges.

**Prior work this plan builds on:**
- Phase-1 plan: docs/superpowers/plans/2026-08-29-cas-wire-keys-phase1-carriers.md
- Phase-2 plan: docs/superpowers/plans/2026-08-29-cas-wire-keys-phase2-cut.md
- The throughput measurement already taken at the phase-2 boundary, with its raw data: docs/superpowers/cas/2026-08-30-wire-keys-phase2-throughput.md and docs/superpowers/cas/bench-wire-keys-phase2/
- The deferred items, all in the `## Inbox` of docs/superpowers/cas/BACKLOG.md

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`, this checkout. Run `git branch --show-current` before AND after every work session; if not on `cas-gc-rebuild`, STOP. No rebase, no amend, NEVER push.
- Unit gate after every task that touches `src/`: `ninja -C build unit_tests_dbms` then `build/src/unit_tests_dbms --gtest_filter='CAS*'`, both logged. **Green means the log's `[  FAILED  ]` count is ZERO and `X tests ran` equals `[  PASSED  ] X`** — a `[  PASSED  ] N` line alone is not proof; a phase-2 batch committed a red tree that way. Baseline entering this plan: 2249.
- **Never edit any source while a build is running.** A stale object silently drops a newly added test and the gate still reads green with an unchanged count — this happened in phase 2. Build from a quiescent tree, and prove a new test exists with `--gtest_list_tests`, never by a moving count.
- **A failing external lane is a finding, not an obstacle.** This plan's whole purpose is that these lanes have never run against the cut. When one fails, diagnose it: a stale assertion this plan must fix, a real defect the cut introduced, or a pre-existing failure unrelated to the cut. Record which, with evidence. Do not "fix" a lane by weakening what it asserts.
- **Measurements are review evidence, never CI assertions.** No timing assertion enters CI. Every measured number is recorded with the conditions it was taken under, and any figure taken on a loaded machine is labelled as such — phase 2 shipped a measurement whose two sides ran under 16.09 and 3.04 load average, and only a reviewer's check caught it.
- Comments state constraints, never provenance. Allman braces. No `EXPECT_THROW` on `LOGICAL_ERROR` (death-split rule).
- Deletions, weakenings and skips of any existing test require an explicit ruling recorded in the report, naming what is lost.

---

### Task 1: Run the CAS stateless lane {#task-1}

**Files:** none expected to change; whatever the run convicts.

The 57 CAS stateless tests (`tests/queries/0_stateless/*cas*`, numbers 04278-04300 and 05000-05026) exercise the server end to end. Phase 2 updated the two that parse persisted objects by hand (`05023`'s catalog reader, and the `05010`/`05012` checks) but ran none of them.

- [ ] **Step 1: Run the lane.** Use the repository's praktika runner (`python3 -m ci.praktika run` — see the project instructions for the exact invocation and the `--test` selector syntax), selecting the CAS tests. Log to a file; have a subagent summarise it rather than reading it whole.
- [ ] **Step 2: Triage every failure into one of three classes**, with evidence for the classification: (a) an assertion still spelling a pre-cut key or word — fix it, it is this plan's work; (b) a real behaviour difference the cut introduced — that is a phase-2 defect and the most valuable thing this plan can find, so stop and report it before touching anything; (c) failing before the cut too — prove it by running the same test at `65ec8688cdb` (a worktree at that commit already exists at `/home/mfilimonov/workspace/ClickHouse/cas-p2-before`), then record it as pre-existing and out of scope.
- [ ] **Step 3:** Fix the class (a) failures; re-run until the lane is green or every remaining failure is classified (b) or (c). **Commit:** `cas: follow the wire cut into the stateless lane` (body: every test touched and why).

---

### Task 2: Run the CAS integration lane {#task-2}

**Files:** none expected to change; whatever the run convicts.

Twelve integration suites (`tests/integration/test_cas_*`). Three of them parse persisted objects directly and were re-spelled during phase 2 without ever being executed: `test_cas_gc_sharded` (reads `gc/state`'s `snap_generation`/`snap_attempt`), `test_cas_gcs` (rewrites and asserts `cas_blob_meta` bodies in its mock), `test_cas_mount_renewal_retry` (reads the lease's `seq` and `write_attempt_id`).

- [ ] **Step 1: Run the lane** per `tests/integration/README.md` (`python -m ci.praktika run "integration" --test <selectors>` from the repository root), one selector per suite so a single stuck suite does not hide the others. Log per suite.
- [ ] **Step 2: Triage** exactly as Task 1 Step 2, same three classes, same evidence rule. Pay particular attention to the three parser suites: a green run there is the first real proof that phase 2's re-spelling was correct rather than merely consistent.
- [ ] **Step 3:** Fix class (a), re-run, classify the rest. **Commit:** `cas: follow the wire cut into the integration lane`.

---

### Task 3: Run the ca-soak scenarios {#task-3}

**Files:** none expected to change; whatever the run convicts.

`utils/ca-soak` drives real servers against real object storage. Two of its cards manipulate persisted bodies directly and both were touched blind during phase 2: `cards/s38_late_put_injection.py` (restamps a ref-log meta line — it was found still using the pre-cut `ns`/`we`/`rs` because a colon-form grep could not see `meta["we"]`) and `scripts/t8_s44_stuck_removing_discrimination.py` (regex over catalog rows).

- [ ] **Step 1: Run the scenarios that touch persisted bodies first** — S38, S43 and the T8/S44 discrimination script — since those are the ones phase 2 changed. Follow the soak run conventions recorded in `utils/ca-soak/scenarios/RUN_HISTORY.md`; a fresh restart is required to get a clean mount (host logs survive teardown; `down -v` for a clean remount).
- [ ] **Step 2: Then run a representative breadth pass** across the remaining cards, enough to exercise each format the cut touched at least once. Do not run all 21 if that costs hours you can spend better; state which you ran and why that set is representative.
- [ ] **Step 3: Triage** per the three classes. A soak card that now hard-fails at its injection step is class (a) and this plan's work; a scenario whose *conclusion* changed is class (b) and stops the plan.
- [ ] **Step 4:** Fix, re-run, record. Append the run to `RUN_HISTORY.md` in the project's scenario-results table format (№ / описание / результат / артефакты / фикс). **Commit:** `cas: follow the wire cut into the soak scenarios`.

---

### Task 4: Extend the benchmark harness to the uncovered formats {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`

The measurement taken at the phase-2 boundary covers `cas_ref_log` and `RefSnapshot` only, because that is all the harness measures. The design's measurement paragraph names five inputs; three have no coverage at all, and the spec says to reach them by EXTENDING this harness rather than adding a second one.

- [ ] **Step 1: Add encode and decode benchmarks** for `PartManifest` (a realistic entry mix: blob entries plus inline entries with payload), `FoldSeal` (with a realistic distribution of `ref_life` row variants — base, hold-bearing, cleanup-evidence — since their per-row costs differ by 33 and 16 bytes), `RefCatalog`, and raw `cas_run` streaming (the format whose active row is the cheapest and whose volume is the highest). Follow the file's existing shape: `->RangeMultiplier(10)->Range(100, 100000)->Complexity()`.
- [ ] **Step 2:** Build with `-DENABLE_BENCHMARKS=ON` and run once to confirm the new benchmarks produce sane numbers and complexity fits. Do not record results yet — Task 5 takes the measurement.
- [ ] **Step 3:** Update the file's header comment: it carries a baseline table from 2026-07-21 that predates this design. Say plainly that those figures are pre-wire-keys, so nobody reads them as the before side. **Commit:** `cas: extend the ref-protocol benchmark to the formats the wire cut left unmeasured`.

---

### Task 5: Take the full before/after measurement {#task-5}

**Files:** Create `docs/superpowers/cas/2026-XX-XX-wire-keys-full-measurement.md` (date it the day it runs); add raw JSON beside the existing `docs/superpowers/cas/bench-wire-keys-phase2/`.

**Interfaces:**
- Consumes: the extended harness from Task 4; the pre-cut worktree at `/home/mfilimonov/workspace/ClickHouse/cas-p2-before` (detached at `65ec8688cdb`, `contrib` symlinked to the main checkout — verified identical, since no submodule moved across the phase).
- Produces: the measurement the spec's acceptance criteria require.

- [ ] **Step 1: Cherry-pick the harness extension onto the BEFORE side.** The new benchmarks must exist on both sides or there is nothing to compare. Apply Task 4's benchmark file to the pre-cut worktree WITHOUT any other phase-2 change — the point is to measure the old codecs through the new harness. Record exactly what you applied.
- [ ] **Step 2: Run both sides back to back on a QUIET machine**, `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`, and record `load_avg` for each run from the JSON. If the two sides' loads differ materially, the run is void — take it again. This is not pedantry: the phase-2 measurement was taken at 16.09 versus 3.04 and only a reviewer's check caught it.
- [ ] **Step 3: Byte accounting, which the throughput run does not give you.** For each of the five formats, at a realistic record count: decompressed bytes before and after, stored `.zst` bytes before and after, and the maximum record count that fits under each object cap before and after. The encoders are the oracle — measure, do not compute from the delta table.
- [ ] **Step 4: Records per second** for encode and decode of each format, derived from the timings and record counts.
- [ ] **Step 5: Write the document.** Lead with the conditions (both sides' loads, `cpu_scaling_enabled`, the commits), then the tables, then a reading that says what the numbers mean rather than restating them. State plainly what is NOT covered. **Commit:** `docs: the wire cut's full before/after measurement`.

---

### Task 6: Cross-check the constexpr descriptor bound against the real encoder {#task-6}

**Files:** Modify `src/Disks/tests/gtest_cas_blob_envelope_format.cpp` if the check is incomplete.

Phase 2 landed `static_assert(kMandatoryDescriptorWorstCase <= kMinBlobHeaderLen - 1)` and a boundary test. The spec asks for the two halves to meet: the compiler proves the formula, the test proves the formula describes the encoder.

- [ ] **Step 1: Establish what the existing test actually proves.** It pins the ENCODER-REACHABLE budget (10 bytes at the 240 floor), not the type-level bound (1 byte), because no public API can widen the version field to its `uint32` maximum. Confirm that reasoning still holds and that the gap is exactly the unused version digits.
- [ ] **Step 2: Close the remaining half if it is open.** The `<=` assert cannot catch an UNDERSTATED formula — shrinking a component would drop the sum with both the assert and the test still green. Decide whether that matters: the case that matters operationally is growth, which is caught twice. If you conclude it needs closing, the cheapest honest closure is a test that reconstructs the worst case from the encoder's own maximum-width output and compares it to the constant. Record the ruling either way.
- [ ] **Step 3:** Build + gate. **Commit** only if something changed.

---

### Task 7: Review the generated assembly of the hot paths {#task-7}

**Files:** none; the deliverable is the review, recorded in the measurement document from Task 5.

The design's stated dominant risk is the longer keys themselves. Its stated secondary risk is that the enum tables and match helpers might add to that cost — this task is what rules that out.

- [ ] **Step 1: Disassemble and review** the hot `toWord` instances (the `cas_run` marker first — it renders a word on every row) and the hot match helpers (`cas_run` token matching, `PartManifest` blob matching). Use the repository's `.claude/tools/analyze-assembly.py` (it disassembles, builds a CFG and reports spill/branch/call density; prefer it to raw `llvm-objdump`). Compare against the pre-cut binary in the worktree with the tool's `--before`/`--after` diff mode.
- [ ] **Step 2: State the verdict** in the measurement document: whether the tables compile to the indexed lookup the design assumes, whether the match helpers stayed inline with no added call or allocation, and whether anything spilled that did not before. A finding here is a design-level concern, not a nitpick.

---

### Task 8: Settle the vocabulary questions phase 2 raised {#task-8}

**Files:** the codecs named below; `docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md` if a decision changes the spec.

Three naming questions came out of the phase-2 final review. They are spec-level decisions, not implementation deviations, and phase 3 is the last cheap moment to settle them — after this the vocabulary is pinned by measurement documents and operator habit.

- [ ] **Step 1: The GC leader's two spellings.** The same identity is `lease_owner` in `cas_gc_state` and `owner` in `cas_gc_hb`. The spec's own justification for unifying `condemn_round` applies verbatim: two spellings for one concept defeat cross-format grep, and "which server holds GC" is a real operator question. Decide: unify (and which way), or record why this pair is different from `condemn_round`. Related and part of the same decision: `cas_gc_hb` applied the context rule to `by`→`owner` but not to `seq`→`hb_seq`, while `cas_mount_lease` kept a bare `seq` under that same rule.
- [ ] **Step 2: The namespace's three spellings** — `namespace` (ref log, ref snapshot), `ns` (catalog, deliberate and spec'd), `root_namespace` (part manifest). A part manifest carries exactly one namespace, so `root_` is redundant by the spec's own opening context rule. Decide and record.
- [ ] **Step 3: The `ns` identifier names three different wire keys** across three codecs, including `PartManifestWire::ns` holding `"root_namespace"`. Whatever Step 2 decides, the C++ identifier should name what it holds.
- [ ] **Step 4:** If any step changes a key, it is a wire change: update the goldens, the byte-delta pins, the closed-set pins, the README row, and the spec table in the same commit, and re-run the external lanes that read that format. If a step decides to keep what is there, record the reason in the spec so the question is not reopened. **Commit** per decision.

---

### Task 9: Discharge the remaining backlog items this campaign created {#task-9}

**Files:** per item; all are named in the `## Inbox` of `docs/superpowers/cas/BACKLOG.md`.

- [ ] **Step 1: The three hand enumerations of `BlobHashAlgo`** against one proven table: `algoFromByte` in the record stream (its byte side is now round-trip tested but still hand-written) and the candidate loop in `CasLayout.cpp` (untested). Re-anchor both on the table, or record why one cannot be.
- [ ] **Step 2: `writeWordField` and `writeStringField` have identical bodies**, and five wire-table words go through the string helper. Either give the word helper a contract the string helper does not have (it takes a table word, so it could assert that) or drop one of them. Whichever you choose, the five call sites should be consistent afterwards.
- [ ] **Step 3: The prose sweeps** the campaign accumulated: the tree-wide `this task`/`this same cut` citations, the remaining internal-reference comments, and the stale-generation narrative in `gtest_cas_namespace_life_id.cpp`. These are one mechanical pass.
- [ ] **Step 4:** Build + gate. **Commit** per group, not one commit for everything.

---

### Task 10: Close revision 14 {#task-10}

**Files:** Modify `docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md` (revision history and, if a Task-8 decision changed anything, the affected tables).

- [ ] **Step 1: Walk the acceptance-criteria list item by item** and record, for each, the artifact that discharges it — a test name, a document, a commit. An item with no artifact is not accepted, however obviously true it looks.
- [ ] **Step 2: Record the phase-3 outcome in the spec's revision history**: what was measured, what was executed, what was settled, and what — if anything — the measurements changed about the design's claims.
- [ ] **Step 3: Report** the campaign's end state: the three phases, the gate count, the measured cost, the external lanes' status, and anything deliberately left open with its reason. **Commit:** `docs: record the wire-keys campaign's acceptance evidence`.

---

## Self-Review (performed at write time) {#self-review}

- **Spec coverage.** Implementation-shape item 3 has three clauses: the constexpr cross-check → Task 6; the byte-delta and throughput measurements → Tasks 4, 5 and 7 (throughput, byte accounting, records/sec, assembly); the sweep of raw assertions in integration tests and `utils/ca-soak` → phase 2 did the SPELLING, Tasks 1-3 do the EXECUTION, which is what the acceptance criterion actually requires. Acceptance criteria 1-8 and 10 were satisfied by phase 2 (verified in its gate audit); criterion 9 is Tasks 1-3; criterion 11 is Tasks 4, 5 and 7; Task 10 walks the whole list and records the evidence. Task 8 settles what the phase-2 reviews raised and deliberately deferred; Task 9 clears the campaign's own backlog residue.
- **Placeholder scan.** No TBDs. Two tasks deliberately do not prescribe an outcome — Task 6 Step 2 and Task 8 — because they are rulings, and each says what must be recorded either way. The lane tasks name the specific tests known to parse persisted bodies, so a runner has a starting point rather than only a directory.
- **Type consistency.** No new production interfaces. Task 4 extends an existing harness in its existing shape; Task 5 consumes it on both sides; Task 7 reads binaries the earlier tasks built. The pre-cut worktree is named once and reused by Tasks 1, 5 and 7.
- **Ordering.** Tasks 1-3 are independent of 4-7 and can run in either order; 5 depends on 4, 7 depends on 5's binaries, 10 depends on everything. Task 8 should precede Task 5's final numbers if it changes a key, which is why it is placed before Task 10 but flagged as wire-affecting.
