---
description: 'Phase-3 implementation plan for the CAS semantic wire-keys design: settle the vocabulary, run the lanes the unit gate cannot see, measure what the design claims, and close revision 14'
sidebar_label: 'CAS wire keys phase 3'
sidebar_position: 22
slug: /superpowers/plans/cas-wire-keys-phase3-proof
title: 'CAS wire keys — phase 3 (proof and measurement)'
doc_type: 'guide'
---

# CAS Wire Keys Phase 3: Proof and Measurement Implementation Plan {#cas-wire-keys-phase3-proof}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish that the wire cut is correct where the unit gate cannot see, that it costs what the design says it costs, and that nothing in revision 14 remains merely asserted — so the design can be declared accepted, or explicitly not accepted, on recorded evidence.

**Architecture:** Phases 1 and 2 are landed. Everything the unit gate can prove is proven. What remains is of three kinds, and the ORDER between them is load-bearing.

**All production-changing work comes first, and ends at a named FREEZE POINT.** That is Tasks 1-3: the vocabulary rulings (which may change persisted keys), the backlog items that touch encode/decode code — including `algoFromByte` and the field-write helpers, which sit on the hot paths the assembly review inspects — and the descriptor proof (which may need a narrow test seam). The last of those commits is the **freeze commit**, and it is what "AFTER" means for the rest of the plan.

Then **execution** of the integration, soak and stateless lanes that phase 2 re-spelled but never ran, and **measurement** of the design's cost claims — both against the frozen tree. The benchmark harness is added after the freeze and changes no production code, so the measured binary is the freeze commit's production code plus benchmark-only additions; the report states both SHAs.

**Any production change after the freeze invalidates every measurement, lane run and disassembly taken before it, and they are retaken in full** — not "the external readers that read that format". If a lane finds a cut-defect, fixing it moves the freeze and restarts the evidence.

The plan ends with a durable acceptance matrix that fails closed.

**Tech Stack:** C++ (ClickHouse tree), gtest, Google Benchmark (`benchmark_cas_ref_protocol`), praktika-driven stateless and integration lanes, `utils/ca-soak` docker-compose scenarios.

**Spec:** docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md (revision 14). Its "Implementation shape" item 3, the "Decompressed-byte accounting" paragraph, and the eleven acceptance criteria are what this plan discharges.

**Prior work:**
- Phase-1 plan: docs/superpowers/plans/2026-08-29-cas-wire-keys-phase1-carriers.md; phase-2 plan: docs/superpowers/plans/2026-08-29-cas-wire-keys-phase2-cut.md
- Phase-2 range in git: `65ec8688cdb..874ca4a491e`. **Cite commits, tests and docs — never SDD workspace paths: `874ca4a491e` deleted them.**
- Boundary measurement already taken (narrow: `cas_ref_log` + `RefSnapshot` timings only): docs/superpowers/cas/2026-08-30-wire-keys-phase2-throughput.md with raw JSON in docs/superpowers/cas/bench-wire-keys-phase2/
- Deferred items: the `## Inbox` of docs/superpowers/cas/BACKLOG.md

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`, this checkout. Check `git branch --show-current` before and after every session. No rebase, no amend, NEVER push.
- Unit gate after every task touching `src/`: `ninja -C build unit_tests_dbms`, then `build/src/unit_tests_dbms --gtest_filter='CAS*'`, both logged, both analysed by a subagent. **Green means `[  FAILED  ]` is ZERO AND `X tests ran` equals `[  PASSED  ] X`.** Baseline entering this plan: 2249.
- **Never edit a source while a build runs** — a stale object drops a new test and the gate still reads green. Prove a new test exists with `--gtest_list_tests`, never by a moving count.
- **A failing external lane is a finding, not an obstacle**, and never fixed by weakening an assertion. Every failure gets one of SIX outcomes (Task 2 defines them); `unclassified` blocks closure.
- **Measurements are review evidence, never CI assertions.** Every number carries the conditions it was taken under. A run whose two sides' 1-minute load averages differ by more than 1.0, or whose absolute loads exceed 2.0, is VOID — preserve it under `void/` with a manifest and take it again. Phase 2 shipped a pair taken at 16.09 versus 3.04 and only a reviewer caught it.
- **The freeze point governs all evidence.** Every task from Task 4 on records the freeze commit it ran against. Any production change after the freeze — a vocabulary ruling revisited, a backlog fix, a cut-defect repair — moves the freeze and invalidates every measurement, lane run and disassembly taken before it; they are retaken in full. Benchmark-only additions do not move the freeze, and the measurement report states the harness commit alongside it.
- New files under `docs/` need frontmatter (`description`, `sidebar_label`, `sidebar_position`, `slug`, `title`, `doc_type`) and `{#anchor}` on every heading.
- Comments state constraints, never provenance. Allman braces. No `EXPECT_THROW` on `LOGICAL_ERROR`. Weakening or skipping any existing test requires a ruling in the report naming what is lost.

---

### Task 1: Settle the vocabulary — FOUR separate rulings {#task-1}

**Files:** the codecs each ruling touches; `docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md` when a ruling changes or confirms a table.

This is FIRST because it is the last task that may change a persisted key. Phase 2 already pinned literal goldens, sixteen byte deltas, closed sets, canonical examples, README rows and external parsers — so a change here is another small wire cut, not a prose ruling, and it must land before anything is measured or executed. Four rulings, each recorded separately even when the outcome is "keep what is there"; bundling them is how a multi-part item gets half-discharged.

**Interfaces:**
- Produces: the FINAL vocabulary. Its commit is not itself the AFTER side — the freeze commit declared at the end of Task 3 is, since Tasks 2 and 3 also change production code.

- [ ] **Step 1: Ruling A — the GC leader's spelling.** The same identity is `lease_owner` in `cas_gc_state` and `owner` in `cas_gc_hb`. Decision criteria, all four to be answered in the record: semantic precision when the object is read alone; redundancy against the containing object's own context; cross-format grep value for the operator question "which server holds GC"; repeated-row byte cost (nil here — both are singletons). Note the nuance before deciding: the heartbeat's owner may name a DEPOSED leader while `gc/state.lease_owner` names the current lease holder (`CasGc.cpp` compares them deliberately), so "same identity type" is not "same current value" — that may argue for or against one spelling, but it must be addressed.
- [ ] **Step 2: Ruling B — the sequence context.** `cas_gc_hb` took `by`→`owner` but kept `seq`→`hb_seq`, while `cas_mount_lease` kept a bare `seq` under the same context rule. Answer the same four criteria (precision alone, redundancy against the container, cross-format grep value, repeated-row cost), decide the rule, and apply it consistently. This is NOT part of Ruling A: bundling them is how a two-clause item gets half-discharged.
- [ ] **Step 3: Ruling C — the namespace's three spellings**: `namespace` (ref log, ref snapshot), `ns` (catalog, deliberate and spec'd), `root_namespace` (part manifest, where the object carries exactly one namespace so `root_` is redundant by the spec's own context rule). Same four criteria; note that `namespace` is a repeated-row key in neither format, so byte cost does not decide this one — say what does.
- [ ] **Step 4: Ruling D — the C++ carrier identifiers.** `ns` names three different wire keys across three codecs, including `PartManifestWire::ns` holding `"root_namespace"`. This one is governed by the spec's asymmetric member rule (a member may be fuller than its key, never more cryptic) rather than by byte cost; whatever C decides, an identifier should name what it holds.
- [ ] **Step 5: If any ruling changes a key, treat it as a wire cut** — goldens, the affected byte-delta pins, closed-set pins, canonical examples, README row, spec table, external parsers, and a fresh dead-spelling sweep in ALL forms (raw, escaped `rg -F`, bare token, index form `meta["k"]`, and backticked prose — each of those forms caught something phase 2's other forms missed). Unit gate green.
- [ ] **Step 6: Record all four rulings in the spec**, including the ones that changed nothing, so they are not reopened. **Commit** per ruling. Record each SHA; Task 3 Step 4 folds them into the freeze point.

---

### Task 2: Discharge the backlog items that touch production code {#task-2}

**Files:** per item, all named in the `## Inbox` of `docs/superpowers/cas/BACKLOG.md`.

These come BEFORE the measurement because two of them change code the measurement and the assembly review look at: `algoFromByte` sits in the record-stream decode path and the field-write helpers are on every encode path. Each item is discharged only when EVERY clause of its bullet is done — phase 2 struck a two-clause item having done one clause, and the surviving half was a wrong number in a production header that nothing then tracked. Re-read each bullet before striking it, and strike it in the same commit as the fix.

- [ ] **Step 1: The three hand enumerations of `BlobHashAlgo`** against one proven table: `algoFromByte` in the record stream (byte side now round-trip tested but still hand-written) and the candidate loop in `CasLayout.cpp` (untested). Re-anchor both on the table, or record why one cannot be.
- [ ] **Step 2: `writeWordField` and `writeStringField` have identical bodies** and five wire-table words go through the string helper. Give the word helper a contract the string helper lacks, or drop one; leave the five call sites consistent either way.
- [ ] **Step 3: The prose sweeps**: the tree-wide `this task`/`this same cut` citations, the remaining internal-reference comments, and the stale-generation narrative in `gtest_cas_namespace_life_id.cpp`.
- [ ] **Step 4:** Build + gate. **Commit** per group.

---

---

### Task 3: Close the descriptor proof — and set the freeze point {#task-3}

**Files:** `src/Disks/tests/gtest_cas_blob_envelope_format.cpp`, and whatever narrow seam Step 2 needs.

The spec requires two independent halves: the compiler proves the formula, and a test proves the formula describes the ENCODER. Today the `static_assert` proves the bound, and the boundary test pins the encoder-REACHABLE budget (10 bytes at the floor) — which is a different quantity from the type-level 239-byte worst case, because no public API can widen the version field to its `uint32` maximum. The remaining gap is real and asymmetric: the `<=` assert cannot catch an UNDERSTATED formula, so a shrunken component would pass both halves.

- [ ] **Step 1: Build the independent oracle.** Reconstruct the mandatory worst case from the REAL encoder's maximum-width output — not by re-deriving the key-cost arithmetic, which would compare the formula to a copy of itself. That requires rendering a descriptor whose version field is at its type maximum; if the version stamp is file-local, add the narrowest possible test seam and say plainly in the comment that it exists for this proof.
- [ ] **Step 2: Pin what the spec names**: the 239-byte mandatory shape, the one-byte `ref` budget at the 240 floor and the 17-byte budget at the 256 default, both at type maxima. Keep the existing reachable-budget test — the two measure different things and the file must say which is which.
- [ ] **Step 3:** Build + gate. **Commit:** `cas: prove the descriptor formula against the encoder, not against itself`. A documentation-only outcome is NOT acceptable here; the spec's acceptance text requires the independent confirmation.
- [ ] **Step 4: Declare the FREEZE POINT.** This task's last commit — or Task 1's or Task 2's, whichever landed last — is the freeze commit. Record its SHA in the report; every task below cites it. From here on, no production change without moving the freeze and retaking the evidence.

---

---

### Task 4: Define the lane-failure taxonomy, then run the CAS stateless lane {#task-4}

**Files:** whatever the run convicts; the taxonomy lives in this task's report and binds Tasks 3 and 4.

**Interfaces:**
- Produces: the six-outcome taxonomy Tasks 5 and 6 reuse.

- [ ] **Step 1: Fix the taxonomy** (three classes are not enough — the soak history already contains inconclusive, connection-refused, missing-helper and resource failures):
  - **stale-assertion** — the test spells a pre-cut key or word. This plan fixes it. A repair must name the exact discriminator it now asserts (message, code, field) and show the intended fence was reached, with a control proving no OTHER fence satisfies the assertion. That control is mandatory: a negative test another fence also satisfies is the defect class this campaign hit three times.
  - **cut-defect** — a real behaviour difference the cut introduced. STOP and report before touching anything; this is the most valuable thing phase 3 can find.
  - **pre-existing** — fails before the cut too. Proof requires the OLD test code, the OLD binary and the OLD config against a clean pool; running post-cut test code against pre-cut bytes manufactures a failure and proves nothing.
  - **environment/harness** — docker cleanup, missing service, ports, disk, credentials, timeout. Retryable; record the retry.
  - **inconclusive** — reproduced neither way. Retryable a bounded number of times, then escalated.
  - **unclassified** — anything not yet in the five above. **Blocks closure.**
- [ ] **Step 2: Run the CAS stateless lane.** **41** tests, matched by `_cas[_.]` and NOT by a bare `cas` substring — that pattern also catches `case` and `cast` and inflates the set to 348, which is how an earlier draft of this plan claimed 57. Confirm the set before running: `ls tests/queries/0_stateless/ | grep -E "_cas[_.]" | grep -E "\.(sh|sql)$"`, numbers 04278-04300 and 05000-05026. Also check for CAS tests that do NOT carry `cas` in the name (grep the bodies for `content_addressed`) and say whether any exist.
  The repository has a DEDICATED CAS stateless lane — `ci/defs/altinity_jobs.py` parametrises "Stateless tests" with `cas s3 storage`, which runs the suite with a content-addressed disk as the default MergeTree storage. Run it from the repository root:

```bash
python3 -m ci.praktika run "Stateless tests (amd_binary, cas s3 storage, parallel)" \
  --test "04278_cas_disk 04279_cas_gc ..." > build/test_stateless_cas.log 2>&1
```

  Pass the whole set in ONE `--test` argument (the flag is a single space-separated string and repeats collapse to the last). Confirm the exact job name against `ci/defs/altinity_jobs.py` before running — it is parametrised, so the string above must match the parametrisation in the tree at that moment. Note that this lane runs the WHOLE stateless suite over a CAS disk, which is stronger evidence than the 41 CAS-named tests alone; run the named set first for a fast signal, then the full lane. Have a subagent summarise the log rather than reading it whole. Do not overlap with a soak run: praktika's post-hooks prune docker containers and volumes.
- [ ] **Step 3: Classify every failure** per Step 1, fix the stale-assertion ones, re-run. **Commit:** `cas: follow the wire cut into the stateless lane` (body: every test touched and its outcome class).

---

### Task 5: Run the CAS integration lane {#task-5}

**Files:** whatever the run convicts.

Twelve suites match `tests/integration/test_cas_*`. Three parse persisted bodies and were re-spelled in phase 2 without ever being executed: `test_cas_gc_sharded` (reads `gc/state`'s `snap_generation`/`snap_attempt` — phase 2 also made its reader raise on an unreadable-but-present object rather than returning the absent sentinel), `test_cas_gcs` (its mock rewrites and asserts `cas_blob_meta` bodies), `test_cas_mount_renewal_retry` (reads the lease's `seq` and `write_attempt_id`, both deliberately unchanged by the cut).

- [ ] **Step 1: Run per suite** from the repository root: `python -m ci.praktika run "integration" --test <suite>` (see `tests/integration/README.md`), one suite per invocation so a stuck suite does not mask the others, logging each to `build/test_integration_<suite>.log`. The twelve suites are the directories matching `tests/integration/test_cas_*`; list them first and record the list, so a suite added since this plan was written is not silently skipped.
- [ ] **Step 2: Classify** with Task 4's six outcomes. The three parser suites are the point: a green run there is the first evidence that phase 2's re-spelling was correct rather than merely self-consistent.
- [ ] **Step 3:** Fix stale assertions, re-run, record. **Commit:** `cas: follow the wire cut into the integration lane`.

---

### Task 6: Run the ca-soak scenarios {#task-6}

**Files:** whatever the run convicts; append the run to `utils/ca-soak/scenarios/RUN_HISTORY.md`.

Two cards manipulate persisted bodies and were edited blind in phase 2: `cards/s38_late_put_injection.py` (restamps a ref-log meta line; it was found still on the pre-cut `ns`/`we`/`rs` because a colon-form grep cannot see `meta["we"]`) and `scripts/t8_s44_stuck_removing_discrimination.py` (regex over catalog rows; phase 2 also made it raise instead of returning an empty catalog when the pattern no longer matches).

- [ ] **Step 1: Read `RUN_HISTORY.md` first** for the run conventions, the status vocabulary and the compose file each scenario needs. Real soak runs use the duration-driven phase, not the quick `--ops` phase; a fresh `down -v` remount is required for a clean mount.
- [ ] **Step 2: Run the body-touching scenarios first** — S38, S43, and the T8/S44 discrimination script with the sequence `RUN_HISTORY.md` records for it. If that sequence is not recorded there, reconstruct it from the script and WRITE IT DOWN before running, so the run is repeatable.
- [ ] **Step 3: Then a breadth pass.** Build the mapping FIRST, as a table: one row per scenario card under `utils/ca-soak/scenarios/cards/`, the compose file it needs, and which of the seventeen formats it exercises (derive this from what the card does, not from its name). Choose the smallest set of cards that covers every format the cut touched, run those, and record the table with a Ran/Skipped column and a reason per skip. A breadth claim without that table is not a claim, and the table is also what tells the next campaign which formats no soak card reaches at all.
- [ ] **Step 4: Classify** with Task 4's taxonomy. A card that hard-fails at its injection step is stale-assertion; a scenario whose CONCLUSION changed is cut-defect and stops the plan.
- [ ] **Step 5:** Fix, re-run, append the run to `RUN_HISTORY.md` in its table format (№ / описание / результат / артефакты / фикс). **Commit:** `cas: follow the wire cut into the soak scenarios`.

---

### Task 7: Extend the benchmark harness to all five required formats, on both sides {#task-7}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`
- Create: a recorded patch file for the BEFORE side (see Step 4)

The spec names five inputs and both directions. The harness today measures `cas_ref_log` and `RefSnapshot` encode plus some state-machine paths; `PartManifest`, `FoldSeal`, `RefCatalog` and raw `cas_run` streaming have no coverage at all, and DECODE is missing for most of what exists — while the design says decode throughput is the primary evidence.

**Interfaces:**
- Produces: encode AND decode benchmarks for `cas_run`, `RefSnapshot`, `PartManifest`, `FoldSeal`, `RefCatalog`, plus the fixture layer and the byte/cap oracle Task 8 measures through. This task changes NO production code, so it does not move the freeze.

- [ ] **Step 1: Define the workloads explicitly in code comments**, because a measurement whose input shape is undocumented cannot be repeated: for each format, the record count range, the field-value widths, and — for `FoldSeal` — the proportion of base, hold-bearing and cleanup-evidence `ref_life` rows, since those three shapes differ by 33 and 16 bytes and a seal made only of base rows would understate the cost. Follow the file's existing `->RangeMultiplier(10)->Range(100, 100000)->Complexity()` shape.
- [ ] **Step 2: Add encode and decode benchmarks** for all five. Decode must consume real encoder output, not a hand-built string.
- [ ] **Step 3: The byte and cap oracle.** Add a small non-benchmark harness (or a gtest in the benchmark's own file guarded off the timing path) that, per format at a stated record count, reports: decompressed bytes, stored bytes after the format's real compression (and explicitly `n/a` for formats stored raw — say which those are rather than reporting a zero), and the maximum record count that fits under the format's object cap, found by the real encoder rather than computed from the delta table. State the search algorithm (binary search on record count against the cap, encoder as oracle).
- [ ] **Step 4: Make the harness build on BOTH sides.** A straight copy will NOT compile at `65ec8688cdb`: `RefCoverage::classification` is `uint8_t` there and `CoverageClass` here, and the pre-cut worktree is not clean (its `contrib` is a symlink and tracked entries read as deleted, so `git cherry-pick` has no clean precondition). Choose one and record it: (a) keep the benchmark bodies identical and put the two public APIs behind a tiny side-local fixture adapter, applying the harness to the BEFORE side with `git apply` of a recorded patch; or (b) write the fixtures against only the API both sides share. Build BOTH sides before any measurement and record compiler, flags, patch hash and binary hashes.
- [ ] **Step 5:** Correct the file's header: its baseline table is from 2026-07-21 and predates this design, so it is not the before side. **Commit:** `cas: extend the ref-protocol benchmark to every format and direction the design measures`.

---

### Task 8: Take the full before/after measurement {#task-8}

**Files:** Create `docs/superpowers/cas/<date>-wire-keys-full-measurement.md` (frontmatter + anchored headings); raw data under `docs/superpowers/cas/bench-wire-keys-phase3/`.

**Interfaces:**
- Consumes: Task 7's harness on both sides; the FREEZE COMMIT from Task 3 as the AFTER side (the measured binary is that commit's production code plus Task 7's benchmark-only additions — record both SHAs); `/home/mfilimonov/workspace/ClickHouse/cas-p2-before` (detached at `65ec8688cdb`) as the BEFORE side.

- [ ] **Step 1: Validity protocol, fixed before running.** Both sides: same compiler, same flags, same machine, back to back, `--benchmark_repetitions=3 --benchmark_report_aggregates_only=true`. Record `load_avg` and `cpu_scaling_enabled` from each JSON. VOID conditions (from the Global Constraints): sides' 1-minute loads differing by more than 1.0, or either above 2.0. A void run is preserved under `bench-wire-keys-phase3/void/` with a manifest saying why — never overwritten, so the record shows what was rejected.
- [ ] **Step 2: Throughput**, encode and decode, all five formats, both sides.
- [ ] **Step 3: Byte accounting** through Task 7's oracle: decompressed bytes, stored bytes (or explicit `n/a` for a format stored raw), and maximum records under each cap — also `n/a` where a format has no object cap to search against. Say which formats fall into each `n/a` case and why, rather than reporting a zero that reads as a measurement.
- [ ] **Step 4: Records per second** for encode and decode of each format.
- [ ] **Step 5: Write the document**: conditions first (commits, loads, scaling, flags, binary hashes), then the tables, then a reading that says what the numbers mean. State what is not covered.
- [ ] **Step 6: If a measurement CONTRADICTS a design claim** — a cost materially larger than the accepted single-digit percent, a cap maximum that falls further than the design's capacity note allows, a complexity class that changed — STOP. That is a finding for the spec, not a number to record and move past. Say so in the report and escalate. **Commit:** `docs: the wire cut's full before/after measurement`.

---

### Task 9: Review the generated assembly of the hot paths {#task-9}

**Files:** the verdict goes into Task 8's measurement document; raw tool output under `bench-wire-keys-phase3/asm/`.

The design's dominant risk is the longer keys themselves; its secondary risk is that the enum tables and match helpers add to that. This task rules the secondary risk in or out.

- [ ] **Step 1: Locate the symbols.** The helpers are `inline` and will usually not survive as standalone symbols, so disassemble the ENCLOSING functions: `DB::Cas::SourceEdgeRunWriter::append` and `DB::Cas::SourceEdgeRunView::next` (the `cas_run` row writer and reader — the marker `toWord` and the token match live here), and `encodePartManifest` / `decodePartManifest` (the blob match). Resolve them with `analyze-assembly.py <binary> "<name>" --search` and, when a templated symbol defeats the regex, by address; record the exact symbol each number came from. If a helper DID survive as its own symbol, say so — that is itself a finding about inlining.
- [ ] **Step 2: Compare both sides** (the freeze-commit binary and the pre-cut one) with `.claude/tools/analyze-assembly.py` in `--before`/`--after` mode (it builds a CFG and reports spill/branch/call density; prefer it to raw `llvm-objdump`).
- [ ] **Step 3: State the verdict against three explicit conditions**: the `toWord` instances compile to the indexed lookup the design assumes (not a scan); the match helpers added NO call, NO allocation and NO branch beyond the comparisons the pre-cut chains made; nothing spills that did not spill before. A failure of any one is a design-level finding.
- [ ] **Step 4: Disposition.** If the verdict is negative, it blocks acceptance until either the code is fixed or the spec's claim is amended — record which. Do not record a negative verdict as a caveat and continue.

---

### Task 10: The acceptance matrix {#task-10}

**Files:** Create `docs/superpowers/cas/<date>-wire-keys-acceptance.md` (frontmatter + anchored headings); modify the spec's revision history to link it.

- [ ] **Step 1: Build the matrix — one row per acceptance criterion** (eleven), each with: the exact requirement, the artifact that discharges it (a commit SHA, a test name, a document path, a log path, a raw-data path — never a deleted workspace path), the result, any caveat, and the disposition. A criterion with no artifact is not discharged, however obviously true it looks.
- [ ] **Step 2: Two supporting matrices** — one row per external lane with its outcome classes and counts, and one row per format per measured metric.
- [ ] **Step 3: The closure rule, stated and applied.** ACCEPTED requires every criterion row to be PASS, or an explicit spec revision that changes the requirement. Any of the following leaves revision 14 NOT ACCEPTED: an OPEN row, an `unclassified` lane failure, a VOID measurement with no valid replacement, a negative assembly verdict, or a measurement that contradicts a design claim. Say which state the campaign is in and why.
- [ ] **Step 4: Record the outcome in the spec's revision history** — a summary and a link to the matrix, not the matrix itself. **Commit:** `docs: the wire-keys campaign's acceptance evidence`.

---

## Self-Review (performed at write time; revision 2 after external review) {#self-review}

- **Ordering is the substantive change, twice over.** Revision 2 moved the vocabulary rulings from last to first. Revision 3 went further: EVERY production-changing task (vocabulary, the backlog items on the encode/decode paths, the descriptor proof's possible test seam) now precedes the evidence and ends at a named freeze commit, because `algoFromByte` and the field-write helpers are exactly the code the assembly review inspects. The benchmark harness lands after the freeze and changes no production code; the measurement report states both SHAs.
- **Spec coverage.** Implementation-shape item 3: the constexpr/encoder cross-check → Task 3, now mandatory with a named oracle (revision 1 allowed a documentation-only ruling, which the acceptance text does not); byte-delta and throughput → Tasks 7-8, now covering all five formats in BOTH directions with a defined workload, a stated byte/cap oracle, and quantitative void conditions; the raw-assertion sweep → phase 2 discharged the SPELLING, which is criterion 9's literal text, and Task 10 records that as the criterion-9 row citing the phase-2 artifact; the lane EXECUTION in Tasks 4-6 is additional confidence evidence and lives in Task 10's separate lane matrix, not in the criterion row. Assembly review → Task 9, now with three explicit conditions and a blocking disposition.
- **Placeholder scan.** No TBDs. Dates in output paths are written as `<date>` deliberately — they are stamped when the task runs. Task 1's four rulings leave OUTCOMES open by design; each states the criteria to decide on and requires the reasoning to be recorded either way.
- **What could still be missed.** The lane tasks cannot prove absence of a defect in a format no lane exercises; Task 6 Step 3 requires the scenario-to-format mapping precisely so that gap is visible rather than assumed. The measurement cannot see costs outside encode/decode (object-store round trips, GC scan time); that is stated in Task 8 Step 5 as not covered rather than left to a reader's assumption.
- **Type consistency.** No new production interfaces except the narrowest possible test seam Task 3 may need, which that task must justify in a comment. Task 7 produces the fixture layer and the byte/cap oracle; Task 8 consumes both; Task 9 reads the binaries Task 8 measured.
