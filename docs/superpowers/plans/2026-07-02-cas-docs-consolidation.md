# CAS documentation consolidation — implementation plan

> Executed via subagent-driven development (one subagent per target doc). Not TDD (docs) — each task's
> "test" is: the target doc covers its assigned sources' durable content, has anchors + frontmatter, and
> makes no unsourced claims. Spec: `docs/superpowers/specs/2026-07-02-cas-docs-consolidation-design.md`.

**Goal:** produce `docs/superpowers/cas/` (a small structured doc set) that captures every load-bearing
invariant/decision/failed-path/technical-detail from the sprawl, then delete the obsolete docs from tree + git.

**Global constraints:**
- Every technical claim MUST be traceable to an old doc or the code — no invented facts.
- Every heading gets a `{#kebab}` anchor; every new doc gets a frontmatter block (per repo doc convention).
- Synthesis, not concatenation. Detailed but managable (split a section >~600 lines; keep folder ≤ ~10 docs).
- Do NOT touch `.tla`/`.cfg` sources, the active D1 spec/plan/model, or CLAUDE.md-referenced tooling.
- Each doc records section status: DONE / TODO / REJECTED (why) / DESIRABLE (why).
- Suite/scenarios dir is the user's uncommitted tree — do not commit it here.

---

### Task 1: `01-architecture.md`
Sources: `content_addressed_mergetree.md`, `incarnation-tagged-cas.md`, `content_addressed_shared_mergetree_design.md`,
`specs/2026-06-10-ca-incarnation-store-design.md`, `specs/2026-06-11-ca-core-refactor-design.md`,
`specs/2026-06-28-cas-layout-hot-cold-split-design.md`, `specs/2026-06-07-ca-merkle-store-design.md` (REJECTED tree layer),
`specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md` (registry removal — REJECTED registry).
Write the object model, layout, incarnation identity, and the "approaches tested & REJECTED (why)" table.

### Task 2: `02-methodology.md`
Sources: the night logs + unattended worklogs + execution logs (mine for the *method* and the pivots, not the play-by-play),
`reports/2026-06-07-ca-spec-review-*` (external design reviews), `models/MODEL_CURRENCY_REVIEW_2026-06-22.md`.
Capture: TDD, subagent-driven-dev, TLA+-as-a-gate (with the pivots counterexamples forced), soak/scenario as oracle.

### Task 3: `03-writer-protocol.md`
Sources: `specs/2026-06-18-ca-build-root-precommit-design.md` (+ cpp-impl), `specs/2026-06-16-ca-build-watermark-design.md`,
`specs/2026-06-21-ca-precommit-first-design.md`, `specs/2026-06-23-ca-precommit-inline-closure-design.md`,
`specs/2026-06-16-ca-resurrect-reupload-design.md`, `specs/2026-06-21-ca-revival-consolidation-design.md`,
`specs/2026-06-14-ca-manifest-commit-lock-scope-design.md`, `specs/2026-06-16-ca-rootshard-protobuf-codec-design.md`,
plans `m2/m3/m5*`. Also read the code (`Core/CasBuild.cpp`, `CasStore.cpp`) to ground it. Include the write-path S3 budget (cross-link Task 7).

### Task 4: `04-gc-protocol.md`
Sources: `specs/2026-06-04-ca-gc-convergence-design.md`, `plans/2026-06-05-ca-gc-s1..s4*`, `plans/2026-06-11-ca-core-m-c3.md`,
`specs/2026-06-16-ca-gc-lease-heartbeat-design.md`, `specs/2026-06-17-ca-gc-snap-prune-design.md`,
`specs/2026-06-21-ca-gc-snap-generation-retention-design.md`, `specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md`,
`specs/2026-06-17-ca-b140-dangle-fix-design.md` (+v2), `specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md`,
`specs/2026-07-01-cas-gc-indegree-refold-undercount-design.md`, the D1 spec, and `project_ca_gc_root_local_redesign` context.
Ground against `Core/CasGc.cpp`. Cover lease/election, the round, orphan removal, ref removal, reclaim+incarnation+registry-removal,
attempt-scoped generations. Explicit TLA+ references (cross-link Task 6). GC S3 budget (cross-link Task 7).

### Task 5: `05-formats-and-backend.md`
Sources: `specs/2026-06-07-ca-merkle-store-design.md`, `specs/2026-06-08-ca-merkle-store-requirements.md`,
`specs/2026-06-24-cas-2b-envelope-one-header` (plan) + `2026-06-24-cas-2a-merkle-tree-id` (plan, REJECTED),
`specs/2026-06-26-cas-proto-rename-design.md`, `specs/2026-06-24-cas-schema-evolution-framework-design.md`,
`specs/2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md`, `specs/2026-06-15-ca-head-after-put-etag-design.md`,
`specs/2026-06-19-ca-vfs-contract.md`. Ground against `Core/CasEnvelope`, `CasBackend.h`, `CasLayout.h`, `CasToken.h`.
Cover object kinds/envelope/codecs/determinism, layout, Backend abstraction, exact-token delete, rustfs testbed, AWS/GCS/Azure status.

### Task 6: `06-tla-models.md`
Sources: ALL `models/*_RESULTS.md`, `models/*_README.md`, `models/INDEX.md`, `models/MODEL_CURRENCY_REVIEW_2026-06-22.md`,
and the `.tla` headers. Produce an index: each surviving model → what it proves (invariants) + the counterexamples that drove
design + currency vs code. This doc SUPERSEDES the per-model prose files (which Task 9 then deletes).

### Task 7: `07-s3-budget.md`
Sources: `specs/2026-06-08-s3-ops-cost-model.md`, `specs/2026-06-14-ca-reduce-s3-op-count-design.md`,
`specs/2026-06-20-ca-dedup-cache-head-before-put-design.md`, `reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md`,
`reports/2026-06-15-unattended-night-opcount-fixes.md`, `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md`.
Produce the per-protocol-part op-count breakdown (write / read / GC) + the reduction history. Note measured (ProfileEvents) vs modeled.

### Task 8: `08-testing-and-soak.md` + `README.md` + `ROADMAP.md`
Sources: `specs/2026-06-13-ca-soak-test-design.md`, `plans/2026-06-13-ca-soak-test.md`, `specs/2026-06-13-ca-fsck-readonly-design.md`,
`utils/ca-soak/scenarios/README.md` + the card list (S01–S35), `deferred_backlog/*`. Write the testing doc, the folder README
(index + status dashboard), and the consolidated ROADMAP (DONE/TODO/REJECTED/DESIRABLE across all areas).

### Task 9: Coverage matrix + sanity audit + deletion
1. Build `docs/superpowers/cas/CONSOLIDATION-COVERAGE.md`: for EVERY doc slated for deletion, name the new
   doc+section that captures its durable content. (Enumerate the deletion set: all top-level logs/handoffs,
   worklogs/, night logs, the per-milestone/per-feature plans+specs whose content is folded above, the
   `models/*_RESULTS.md`/`*_README.md`/INDEX. KEEP: `.tla`/`.cfg`, active D1 spec/plan/model, tooling-referenced.)
2. Dispatch a FRESH sanity-audit subagent: for the largest/most-important obsolete docs, confirm each
   load-bearing invariant/decision/failed-path is present in the new set. Any gap blocks that doc's deletion
   until filled.
3. Only after the audit passes: `git rm` the obsolete docs; `git add` the new `cas/` folder + coverage matrix;
   commit both together. Report the before/after file count + size.
